#ifndef RUNTIME_COMMUNICATION_H_
#define RUNTIME_COMMUNICATION_H_
#include <unistd.h>
#include <stdbool.h>

enum peer_msg_type {
    NIL = 0,
    PEER_MSU_MSG
};

struct runtime_peer_msg_hdr {
    enum peer_msg_type type;
    size_t data_size;
};

struct add_runtime_msg {
    unsigned int runtime_id;
    int fd;
};

int send_to_peer(unsigned int runtime_id, size_t data_size, void *data);
int add_runtime_peer(unsigned int runtime_id, int fd);
int init_runtime_socket(int listen_port);
int handle_runtime_communication(int fd);

// TODO: is_runtime_fd()
bool is_runtime_fd(int fd);

#endif
