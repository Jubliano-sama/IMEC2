#ifndef APP_ANCHOR_ACTION_RUNTIME_H
#define APP_ANCHOR_ACTION_RUNTIME_H

#include "app_anchor_actions.h"
#include "app_node_comm.h"

int app_anchor_action_handle(const struct proto_packet *command,
    const uint8_t *payload, size_t payload_len,
    const struct app_node_comm_reservation_lease *reservation);

#endif
