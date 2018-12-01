#pragma once

#include "msg_interface.hpp"
#include "rdma_ctrl.hpp"
#include "ralloc/ralloc.h"

/**
 * The Adapter use UD QP is based on FaSST RPC.
 */
namespace rdmaio {

class UDRecvManager {
 public:
  UDRecvManager(UDQP *qp,int max_recv_num,MemoryAttr local_mr):
      qp_(qp),max_recv_num_(max_recv_num)
  {
    RDMA_ASSERT(max_recv_num_ <= UDQPImpl::MAX_RECV_SIZE)
        << "UD can register at most " << UDQPImpl::MAX_RECV_SIZE << "buffers.";
    // allocate local heap
    RThreadLocalInit();

    // int recv structures
    /*
     *  The recv_buf_size must be smaller than *Real packet size* - *sizeof(GRH_header)*
     *  otherwise the msg cannot be received
     */
    int recv_buf_size = MAX_PACKET_SIZE;
    RDMA_ASSERT(recv_buf_size <= MAX_PACKET_SIZE);

    // init receive related structures
    for(uint i = 0;i < max_recv_num_;++i) {
      struct ibv_sge sge {
        .addr   = (uintptr_t)(Rmalloc(recv_buf_size)),
        .length = (uint32_t)recv_buf_size,
        .lkey   = local_mr.key
      };
      RDMA_ASSERT(sge.addr != 0) << "failed to allocate recv buffer.";
      sges_[i] = sge;

      rrs_[i].wr_id  = sges_[i].addr;
      rrs_[i].sg_list = &sges_[i];
      rrs_[i].num_sge = 1;

      rrs_[i].next    = (i < (max_recv_num_ - 1)) ? &rrs_[i + 1] : &rrs_[0];
    }

    post_recvs(max_recv_num_);

    // now the qp can receive connection requests
    qp_->set_ready();
  }
 public:
  // the size of global routing header
  static const int GRH_SIZE = 40;
  static const int MAX_PACKET_SIZE = 4096 - GRH_SIZE;

 protected:

  UDQP *qp_ = nullptr;

  int recv_head_ = 0;
  int idle_recv_num_ = 0;
  int max_idle_recv_num_ = 1;
  int max_recv_num_ = 0;

  struct ibv_recv_wr rrs_[UDQPImpl::MAX_RECV_SIZE];
  struct ibv_sge sges_[UDQPImpl::MAX_RECV_SIZE];
  struct ibv_wc wcs_[UDQPImpl::MAX_RECV_SIZE];
  struct ibv_recv_wr *bad_rr_;

  void post_recvs(int recv_num) {

    if(recv_num <= 0) {
      return;
    }

    int tail = recv_head_ + recv_num - 1;
    if(tail >= max_recv_num_)
      tail -= max_recv_num_;

    ibv_recv_wr  *head_rr = rrs_ + recv_head_;
    ibv_recv_wr  *tail_rr = rrs_ + tail;
    ibv_recv_wr  *temp = tail_rr->next;
    tail_rr->next = NULL;

    int rc = ibv_post_recv(qp_->qp_,head_rr,&bad_rr_);
    if(rc != 0) {
      RDMA_LOG(ERROR) << "post recv " << recv_num << "; w error: " << strerror(errno) ;
    }
    recv_head_ = tail;
    tail_rr->next = temp;
    recv_head_ = (recv_head_ + 1) % max_recv_num_;
  }
  // class specific constants
};

class UDAdapter : public MsgAdapter, public UDRecvManager {
  static const int MAX_UD_SEND_DOORBELL = 16;
 public:
  UDAdapter(std::shared_ptr<RdmaCtrl> cm, RNicHandler *rnic, MemoryAttr local_mr,
        int w_id, int max_recv_num):
      node_id_(cm->current_node_id()),
      worker_id_(w_id),
      UDRecvManager(cm->create_ud_qp(create_ud_idx(w_id,RECV_QP_IDX),rnic,&local_mr),max_recv_num,local_mr),
      send_qp_(cm->create_ud_qp(create_ud_idx(w_id,SEND_QP_IDX),rnic,&local_mr))
  {
    // init send structures
    for(uint i = 0;i < MAX_UD_SEND_DOORBELL;++i) {
      srs_[i].opcode = IBV_WR_SEND_WITH_IMM;
      srs_[i].num_sge = 1;
      srs_[i].imm_data = ::rdmaio::encode_qp_id(node_id_,worker_id_);
      RDMA_ASSERT(::rdmaio::decode_qp_mac(srs_[i].imm_data) == node_id_);
      srs_[i].next = &srs_[i+1];
      srs_[i].sg_list = &ssges_[i];

      ssges_[i].lkey = local_mr.key;
    }
  }

  ConnStatus connect(std::string ip,int port) {
    return send_qp_->connect(ip,port,create_ud_idx(worker_id_,RECV_QP_IDX));
  }

  ConnStatus send_to(int node_id,const char *msg,int len) {

    RDMA_ASSERT(current_idx_ == 0) << "There is pending reqs in the msg queue.";
    srs_[0].wr.ud.ah = send_qp_->ahs_[node_id];
    srs_[0].wr.ud.remote_qpn  = send_qp_->attrs_[node_id].qpn;
    srs_[0].wr.ud.remote_qkey = DEFAULT_QKEY;
    srs_[0].sg_list = &ssges_[0];
    srs_[0].next = NULL;

    srs_[0].send_flags = ((send_qp_->queue_empty()) ? IBV_SEND_SIGNALED : 0)
                         | ((len < MAX_INLINE_SIZE) ? IBV_SEND_INLINE : 0);

    ssges_[0].addr = (uint64_t)msg;
    ssges_[0].length = len;


    if(send_qp_->need_poll()) {
      ibv_wc wc; auto ret = send_qp_->poll_till_completion(wc);
      RDMA_ASSERT(ret == SUCC) << "poll UD completion reply error: " << ret;
      send_qp_->pendings = 0;
    } else
      send_qp_->pendings += 1;

    int rc = ibv_post_send(send_qp_->qp_, &srs_[0], &bad_sr_);
    //reset next ptr
    srs_[0].next = &srs_[1];
    return (rc == 0)?SUCC:ERR;
  }

  void prepare_pending() {
    RDMA_ASSERT(current_idx_ == 0);
  }

  ConnStatus send_pending(int node_id,const char *msg,int len) {

    auto i = current_idx_++;
    srs_[i].wr.ud.ah = send_qp_->ahs_[node_id];
    srs_[i].wr.ud.remote_qpn  = send_qp_->attrs_[node_id].qpn;
    srs_[i].wr.ud.remote_qkey = DEFAULT_QKEY;

    srs_[i].send_flags = ((send_qp_->queue_empty()) ? IBV_SEND_SIGNALED : 0)
                         | ((len < MAX_INLINE_SIZE) ? IBV_SEND_INLINE : 0);

    if(send_qp_->need_poll()) {
      ibv_wc wc;auto ret = send_qp_->poll_till_completion(wc);
      RDMA_ASSERT(ret == SUCC) << "poll UD completion reply error: " << ret;
      send_qp_->pendings = 0;
    } else {
      send_qp_->pendings += 1;
    }

    ssges_[i].addr = (uintptr_t)msg;
    ssges_[i].length = len;

    if(current_idx_ >= MAX_UD_SEND_DOORBELL)
      flush_pending();
  }

  ConnStatus flush_pending() {
    if(current_idx_ > 0) {
      srs_[current_idx_ - 1].next = NULL;
      auto ret = ibv_post_send(send_qp_->qp_, &srs_[0], &bad_sr_);
      srs_[current_idx_ - 1].next = &srs_[current_idx_];
      current_idx_ = 0;
      return (ret == 0)?SUCC:ERR;
    }
    return SUCC;
  }

  void poll_comps() {

    int poll_result = ibv_poll_cq(qp_->recv_cq_,UDQPImpl::MAX_RECV_SIZE,wcs_);
    /**
     * The reply messages are batched in this call
     */
    prepare_pending();
    for(uint i = 0;i < poll_result;++i) { // poll_result: number of results
      RDMA_ASSERT(wcs_[i].status == IBV_WC_SUCCESS)
          << "error wc status " << wcs_[i].status << " at " << worker_id_;
      callback_((const char *)(wcs_[i].wr_id + GRH_SIZE),::rdmaio::decode_qp_mac(wcs_[i].imm_data),
                ::rdmaio::decode_qp_index(wcs_[i].imm_data));
    }
    flush_pending(); // send the batched replies
    idle_recv_num_ += poll_result;
    if(idle_recv_num_ > max_idle_recv_num_) {
      // re-post recvs to the QP
      post_recvs(idle_recv_num_);
      idle_recv_num_ = 0;
    }
  }

 private:
  const int node_id_;   // my node id
  const int worker_id_; // my thread id
  /**
   * sender structures
   */
  UDQP *send_qp_ = nullptr;
  ibv_send_wr srs_[MAX_UD_SEND_DOORBELL];
  ibv_sge     ssges_[MAX_UD_SEND_DOORBELL];
  struct ibv_send_wr *bad_sr_ = nullptr;

  int current_idx_ = 0;

  static const int RECV_QP_IDX = 1;
  static const int SEND_QP_IDX = 0;
};

} // namespace rdmaio
