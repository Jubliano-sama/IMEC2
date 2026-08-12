#ifndef APP_MESH_C5_REPAIR_AUTHORIZATION_H
#define APP_MESH_C5_REPAIR_AUTHORIZATION_H

#include <stdbool.h>
#include <stdint.h>

#define APP_MESH_C5_REPAIR_DIGEST_LEN 32u

enum app_mesh_c5_tx_authorization {
    APP_MESH_C5_TX_AUTH_NONE = 0,
    APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR,
    APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
    APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR,
};

struct app_mesh_c5_tx_authorization_token {
    enum app_mesh_c5_tx_authorization kind;
    uint64_t peer_id;
    uint32_t pending_session_id;
    uint16_t pending_seq;
    uint8_t pending_msg_type;
    uint8_t pending_digest[APP_MESH_C5_REPAIR_DIGEST_LEN];
    uint32_t retained_ack_session_id;
    uint16_t retained_ack_seq;
    uint8_t retained_ack_digest[APP_MESH_C5_REPAIR_DIGEST_LEN];
    bool retained_ack_valid;
    bool valid;
};

#endif
