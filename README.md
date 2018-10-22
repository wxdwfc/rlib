## RLib

### Intro

RLib is a header-only library for **easier** use of RDMA. It is basically a wrapper of the interfaces of libibverbs, 
while it additionally handles some tedius things, such as establishing connections between servers.

------

### To use

`#include "rdma_ctrl.hpp"` is all you need.

------

### Example

Usually very few lines of code is needed to use RDMA with RLib. Below is a snippet of using RLib to implement a 
pingpong application using one-sided RDMA primitive.

Server side
```c++
int server_node_id = 1;
int tcp_port       = 8888;

RdmaCtrl *c = new RdmaCtrl(server_node_id,tcp_port);
RdmaCtrl::DevIdx idx {.dev_id = 0,.port_id = 1 }; // using the first RNIC's first port
c->open_device(idx);

// register a buffer to the previous opened device, using id = 73
char *buffer = (char *)malloc(4096);
RDMA_ASSERT(c->register_memory(73,buffer,4096,c->get_device()) == true);

RCQP *qp = c->create_rc_qp(create_rc_idx(1,0),c->get_device(),c->get_local_mr(73));

while(true) {
    // This is RDMA, server does not need to do anything :)
    sleep(1);
}
```

Client side
```c++
int client_node_id = 0;
int tcp_port       = 8888;

RdmaCtrl *c = new RdmaCtrl(client_node_id,tcp_port);
RdmaCtrl::DevIdx idx {.dev_id = 0,.port_id = 1 }; // using the first RNIC's first port
c->open_device(idx);

// register a buffer to the previous opened device, using id = 73
char *buffer = (char *)malloc(4096);
RDMA_ASSERT(c->register_memory(73,buffer,4096,c->get_device()) == true);

// get remote server's memory information
MemoryAttr mr;
while(QP::get_remote_mr(remote_ip,tcp_port,73,&mr) != SUCC) {
    usleep(2000);
}

// create the RC qp to access remote server's memory, using the previous registered memory
RCQP *qp = c->create_rc_qp(create_rc_idx(1,0),c->get_device(),c->get_local_mr(73));
qp->bind_remote_mr(mr); // bind to the previous allocated mr

while(qp->connect(remote_ip,tcp_port) != SUCC)  {
    usleep(2000);
}

// main pingpong loop
while(true) {
    char *local_buf  = buffer;
    uint64_t address = 0;
    // read an uint64_t from the server
    auto rc = qp->post_send(IBV_WR_RDMA_READ,local_buf,sizeof(uint64_t),address,IBV_SEND_SIGNALED);
    qp->poll_till_completion();
    // then get the results, stored in the local_buffer
}

```
