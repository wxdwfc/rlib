## RLib

### Intro

RLib is a header-only library for **easier** use of RDMA using C++. Basically it is a set of wrappers of the interfaces of `libibverbs`, 
yet it additionally handles many tedius things, such as establishing connections between RDMA QPs, and simplifies many configurations.

------

### To use

`#include "rdma_ctrl.hpp"` is all you need.

------

### Example

Usually very few lines of code are needed to use RDMA with RLib. Below is a snippet of using RLib to implement a 
simple pingpong application using one-sided RDMA primitive.

Server side
```c++
/**
 * Note, RDMA usually uses some other communication method (e.g. TCP/IP) to exchange QP informations.
 * RLib uses TCP for the pre-communication.
 */
int server_node_id = 1;
int tcp_port       = 8888;
int client_port    = 8000;

using namespace rdmaio;

RdmaCtrl *c = new RdmaCtrl(server_node_id,tcp_port);
RdmaCtrl::DevIdx idx {.dev_id = 0,.port_id = 1 }; // using the first RNIC's first port
c->open_thread_local_device(idx);

// register a buffer to the previous opened device, using id = 73
char *buffer = (char *)malloc(4096);
memset(buffer, 0, 4096);
RDMA_ASSERT(c->register_memory(73,buffer,4096,c->get_device()) == true);

char s[] = "hello world";
memcpy(buffer, s, strlen(s));

RCQP *qp = c->create_rc_qp(create_rc_idx(1,0),c->get_device(),c->get_local_mr(73));

// server also needs to "connect" clinet.
while(qp->connect(client_ip,client_port) != SUCC)  {
    usleep(2000);
}

while(true) {
    // This is RDMA, server does not need to do anything :)
    sleep(1);
}
```

Client side
```c++
int client_node_id = 0;
int tcp_port       = 8000;
int server_port    = 8888;

using namespace rdmaio;

RdmaCtrl *c = new RdmaCtrl(client_node_id,tcp_port);
RdmaCtrl::DevIdx idx {.dev_id = 0,.port_id = 1 }; // using the first RNIC's first port
c->open_thread_local_device(idx);

// register a buffer to the previous opened device, using id = 73
char *buffer = (char *)malloc(4096);
RDMA_ASSERT(c->register_memory(73,buffer,4096,c->get_device()) == true);

// get remote server's memory information
MemoryAttr mr;
while(QP::get_remote_mr(server_ip,server_port,73,&mr) != SUCC) {
    usleep(2000);
}

// create the RC qp to access remote server's memory, using the previous registered memory
RCQP *qp = c->create_rc_qp(create_rc_idx(1,0),c->get_device(),c->get_local_mr(73));
qp->bind_remote_mr(mr); // bind to the previous allocated mr

while(qp->connect(server_ip,server_port) != SUCC)  {
    usleep(2000);
}

// main pingpong loop

ibv_wc wc;
while(true) {
    char *local_buf  = buffer;
    uint64_t address = 0;
    int msg_len = 11;   // length of "hello world"
    // read an uint64_t from the server
    auto rc = qp->post_send(IBV_WR_RDMA_READ,local_buf,msg_len,address,IBV_SEND_SIGNALED);
    qp->poll_till_completion();
    // then get the results, stored in the local_buffer
}

```

### Acknowledgments
TODO
