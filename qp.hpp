#pragma once

#include "common.hpp"
#include "qp_impl.hpp" // hide the implementation

namespace rdmaio {

class QP {
 public:
  QP(RNicHandler *rnic,int n_id,int w_id,int idx = 0):
      node_id_(n_id),
      worker_id_(w_id),
      idx_(idx),
      rnic_(rnic) {
  }

  ~QP() {
    if(qp_ != nullptr)
      ibv_destroy_qp(qp_);
    if(cq_ != nullptr)
      ibv_destroy_cq(cq_);
  }
  /**
   * Connect to remote QP
   * Note, we leverage TCP for a pre connect.
   * So the IP/Hostname and a TCP port must be given.
   *
   * WARNING:
   * This function actually should contains two functions, connect + change QP status
   * maybe split to connect + change status for more flexibility?
   */
  /**
   * connect to the specific QP at remote, specificed by the nid and wid
   * return SUCC if QP are ready.
   * return TIMEOUT if there is network error.
   * return NOT_READY if remote server fails to find the connected QP
   */
  virtual ConnStatus connect(std::string ip,int port,int nid,int wid) = 0;

  // return until the completion events
  // this call will block until a timeout
  ConnStatus poll_till_completion(ibv_wc &wc, struct timeval timeout = default_timeout) {
    return QPImpl::poll_till_completion(cq_,wc,timeout);
  }

  void bind_local_mr(MemoryAttr attr) {
    memcpy(&local_mr_,&attr,sizeof(MemoryAttr));
  }

  QPAttr get_attr() {
    QPAttr res = {
      .addr = rnic_->query_addr(),
      .lid  = rnic_->lid,
      .qpn  = (qp_ != nullptr)?qp_->qp_num:0,
      .psn  = DEFAULT_PSN,
      .node_id = 0, // a place holder
      .port_id = rnic_->port_id
    };
    return res;
  }

  /**
   * Get remote MR attribute
   */
  static ConnStatus get_remote_mr(std::string ip,int port,int mr_id,MemoryAttr *attr) {
    return QPImpl::get_remote_mr(ip,port,mr_id,attr);
  }

  // QP identifiers
  const int worker_id_; // thread id of the QP
  const int node_id_;   // remote id the QP connect to
  const int idx_;       // which QP connect to this mac, at this worker

 public:
  // internal verbs structure
  struct ibv_qp *qp_ = NULL;
  struct ibv_cq *cq_ = NULL;

  // local MR used to post reqs
  MemoryAttr local_mr_;
  RNicHandler *rnic_;

 protected:
  ConnStatus get_remote_helper(ConnArg *arg, ConnReply *reply,std::string ip,int port) {
    return QPImpl::get_remote_helper(arg,reply,ip,port);
  }
};

#define DEFAULT_RC_INIT_FLAGS  (IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC)
class RCQP : public QP {
 public:
  //
  RCQP(RNicHandler *rnic,int n_id,int w_id,int idx,
       MemoryAttr local_mr,MemoryAttr remote_mr,int access_flags = DEFAULT_RC_INIT_FLAGS)
      :RCQP(rnic,n_id,w_id,idx,access_flags) {
    bind_local_mr(local_mr);
    bind_remote_mr(remote_mr);
  }

  RCQP(RNicHandler *rnic,int n_id,int w_id,int idx,MemoryAttr local_mr,int access_flags = DEFAULT_RC_INIT_FLAGS)
      :RCQP(rnic,n_id,w_id,idx,access_flags) {
    bind_local_mr(local_mr);
  }

  RCQP(RNicHandler *rnic,int n_id,int w_id,int idx,int access_flags = DEFAULT_RC_INIT_FLAGS)
      :QP(rnic,n_id,w_id,idx)
  {
    RCQPImpl::init(qp_,cq_,rnic_,access_flags);
  }

  ConnStatus connect(std::string ip,int port) {
    return connect(ip,port,node_id_,worker_id_);
  }

  ConnStatus connect(std::string ip,int port,int nid,int wid) {

    ConnArg arg; ConnReply reply;
    arg.type = ConnArg::QP;
    arg.payload.qp.from_node = nid;
    arg.payload.qp.from_worker = wid;
    arg.payload.qp.qp_type = IBV_QPT_RC;

    auto ret = QPImpl::get_remote_helper(&arg,&reply,ip,port);
    if(ret == SUCC) {
      // change QP status
      if(!RCQPImpl::ready2rcv(qp_,reply.payload.qp,rnic_)) {
        RDMA_LOG(LOG_WARNING) << "change qp status to ready to receive error: " << strerror(errno);
        ret = ERR; goto CONN_END;
      }

      if(!RCQPImpl::ready2send(qp_)) {
        RDMA_LOG(LOG_WARNING) << "change qp status to ready to send error: " << strerror(errno);
        ret = ERR; goto CONN_END;
      }
    }
 CONN_END:
    return ret;
  }

  /**
   * Bind this QP's operation to a remote memory region according to the MemoryAttr.
   * Since usually one QP access *one memory region* almost all the time,
   * so it is more convenient to use a bind-post;bind-post-post fashion.
   */
  void bind_remote_mr(MemoryAttr attr) {
    memcpy(&remote_mr_,&attr,sizeof(MemoryAttr));
  }

  /**
   * Post request(s) to the sending QP.
   * This is just a wrapper of ibv_post_send
   */
  ConnStatus post_send(ibv_wr_opcode op,char *local_buf,
                       uint32_t len,uint64_t off,int flags,uint64_t wr_id = 0, uint32_t imm = 0) {
    ConnStatus ret = SUCC;
    struct ibv_send_wr *bad_sr;

    // setting the SGE
    struct ibv_sge sge {
      .addr = (uint64_t)local_buf,
          .length = len,
          .lkey   = local_mr_.key
          };

    // setting sr, sr has to be initilized in this style
    struct ibv_send_wr sr;
    sr.wr_id = wr_id;
    sr.opcode = op;
    sr.num_sge = 1;
    sr.next = NULL;
    sr.sg_list = &sge;
    sr.send_flags = flags;
    sr.imm_data = imm;
    sr.wr.rdma.remote_addr = (off + remote_mr_.buf);
    sr.wr.rdma.rkey = remote_mr_.key;

    auto rc = ibv_post_send(qp_,&sr,&bad_sr);
    return rc == 0 ? SUCC : ERR;
  }

  // one-sided compare and swap
  ConnStatus post_cas(char *local_buf,uint64_t off,
                      uint64_t compare,uint64_t swap,int flags,uint64_t wr_id = 0) {
    ConnStatus ret = SUCC;
    struct ibv_send_wr *bad_sr;

    // setting the SGE
    struct ibv_sge sge {
      .addr = (uint64_t)local_buf,
          .length = sizeof(uint64_t), // atomic only supports 8-byte operation
          .lkey   = local_mr_.key
          };

    struct ibv_send_wr sr;
    sr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
    sr.num_sge = 1;
    sr.next = NULL;
    sr.sg_list = &sge;
    sr.send_flags = flags;
    // remote memory
    sr.wr.rdma.rkey = remote_mr_.key;
    sr.wr.rdma.remote_addr = (off + remote_mr_.buf);
	sr.wr.atomic.compare_add = compare;
	sr.wr.atomic.swap = swap;

    auto rc = ibv_post_send(qp_,&sr,&bad_sr);
    return rc == 0 ? SUCC : ERR;
  }

  ConnStatus post_batch(struct ibv_send_wr *send_sr,ibv_send_wr **bad_sr_addr,int num = 0) {
    auto rc = ibv_post_send(qp_,send_sr,bad_sr_addr);
    return rc == 0 ? SUCC : ERR;
  }

  /**
   * Poll completions. These are just wrappers of ibv_poll_cq
   */
  int poll_send_completion(ibv_wc &wc) {
    return ibv_poll_cq(cq_,1,&wc);
  }

  /**
   * Used to count pending reqs
   * XD: current we use 64 as default, but it is rather application defined,
   * which is related to how the QP's send to are created, etc
   */
  bool need_poll(int threshold = (RCQPImpl::RC_MAX_SEND_SIZE / 2)) {
    return (high_watermark_ - low_watermark_) >= threshold;
  }

  uint64_t high_watermark_ = 0;
  uint64_t low_watermark_  = 0;

  MemoryAttr remote_mr_;
};

template <int MAX_SERVER_NUM = 16>
class UDQP : public QP {
  // the QKEY is used to identify UD QP requests
  static const int DEFAULT_QKEY = 0xdeadbeaf;
 public:
  UDQP(RNicHandler *rnic,int w_id,int idx,MemoryAttr local_mr)
      :UDQP(rnic,w_id,idx) {
    bind_local_mr(local_mr);
  }

  UDQP(RNicHandler *rnic,int w_id,int idx)
      :QP(rnic,0 /* a dummy nid*/,w_id,idx) {
    UDQPImpl::init(qp_,cq_,recv_cq_,rnic_);
    std::fill_n(ahs_,MAX_SERVER_NUM,nullptr);
  }

  bool queue_empty() {
    return pendings == 0;
  }

  bool need_poll(int threshold = UDQPImpl::UD_MAX_SEND_SIZE / 2) {
    return pendings >= threshold;
  }

  /**
   * Simple wrapper to expose underlying QP structures
   */
  inline __attribute__ ((always_inline))
  ibv_cq *recv_queue() {
    return recv_cq_;
  }

  inline __attribute__ ((always_inline))
  ibv_qp *send_qp() {
    return qp_;
  }

  ConnStatus connect(std::string ip,int port) {
    // UD QP is not bounded to a mac, so use idx to index
    return connect(ip,port,node_id_,idx_);
  }

  ConnStatus connect(std::string ip,int port,int wid,int idx) {

    ConnArg arg; ConnReply reply;
    arg.type = ConnArg::QP;
    arg.payload.qp.from_node = wid;
    arg.payload.qp.from_worker = idx;
    arg.payload.qp.qp_type = IBV_QPT_UD;

    auto ret = QPImpl::get_remote_helper(&arg,&reply,ip,port);

    if(ret == SUCC) {
      // create the ah, and store the address handler
      auto ah = UDQPImpl::create_ah(rnic_,reply.payload.qp);
      if(ah == nullptr) {
        RDMA_LOG(LOG_WARNING) << "create address handler error: " << strerror(errno);
        ret = ERR;
      } else {
        ahs_[reply.payload.qp.node_id]   = ah;
        attrs_[reply.payload.qp.node_id] = reply.payload.qp;
      }
    }
 CONN_END:
    return ret;
  }

  /**
   * whether this UD QP has been post recved
   * a UD QP should be first been post_recved; then it can be connected w others
   */
  bool ready() {
    return ready_;
  }

  void set_ready() {
    ready_ = true;
  }

  friend class UDAdapter;
 private:
  /**
   * FIXME: curretly we have limited servers, so we use an array.
   * using a map will affect the perfomrance in microbenchmarks.
   * remove it, and merge this in UDAdapter?
   */
  struct ibv_ah *ahs_[MAX_SERVER_NUM];
  struct QPAttr  attrs_[MAX_SERVER_NUM];

  // current outstanding requests which have not been polled
  int pendings = 0;

  struct ibv_cq *recv_cq_ = NULL;
  bool ready_ = false;
};

}; // end namespace rdmaio
