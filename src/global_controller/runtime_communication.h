#ifndef RUNTIME_COMMUNICATION_H__
#define RUNTIME_COMMUNICATION_H__
#include "ctrl_runtime_messages.h"

int send_to_runtime(unsigned int runtime_id, struct ctrl_runtime_msg_hdr *hdr,
                    void *payload);
int runtime_communication_loop(int listen_port);

#endif
