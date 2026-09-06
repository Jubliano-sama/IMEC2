#ifndef APP_ANCHOR_ACTIONS_H
#define APP_ANCHOR_ACTIONS_H

#include "protocol.h"

#define ANCHOR_ACTION_MAX_HOPS 8u
/* Preserve the existing 12-second reliable delivery allowance per hop. */
#define ANCHOR_ACTION_PER_HOP_MS 12000u
#define ANCHOR_ACTION_MAX_AGE_MS (2u * ANCHOR_ACTION_MAX_HOPS * ANCHOR_ACTION_PER_HOP_MS)
#define ANCHOR_ACTION_REQUEST_MAX_LEN 13u

struct app_anchor_action_result {
    enum command_status status;
    uint8_t reason;
    uint16_t battery_mv;
    uint32_t boot_counter;
    uint64_t sampled_at_ms;
};

/* One current command/result, serialized by the existing command owner.
 * The radio packet's age carries elapsed sample age on every retransmission. */
struct app_anchor_actions {
    struct app_anchor_action_result result;
    uint64_t accepted_at_ms;
    uint32_t session_id;
    uint16_t sequence;
    uint16_t command_id;
    uint8_t request[ANCHOR_ACTION_REQUEST_MAX_LEN];
    uint8_t request_len;
    bool valid;
};

struct app_anchor_action_ops {
    int (*identify)(void *ctx);
    int (*read_battery)(void *ctx, uint16_t *mv);
    void *ctx;
};

bool app_anchor_action_command(enum command_id command_id);
int app_anchor_action_request(const uint8_t *payload, size_t payload_len,
    uint32_t *assignment_epoch, uint8_t *hop_count);
uint32_t app_anchor_action_delivery_ms(uint8_t hop_count);
int app_anchor_action_result_payload(enum command_id command_id,
    uint64_t anchor_id, uint32_t assignment_epoch,
    const struct app_anchor_action_result *result,
    uint8_t *payload, size_t capacity, size_t *length);
uint8_t app_anchor_identify_color(uint32_t elapsed_ms, uint32_t *next_ms);
int app_anchor_action_execute(struct app_anchor_actions *state,
    const struct proto_packet *command, const uint8_t *payload, size_t payload_len,
    uint64_t anchor_id, uint64_t gateway_id, uint32_t boot_counter,
    uint32_t assignment_epoch,
    uint64_t now_ms, const struct app_anchor_action_ops *ops,
    struct app_anchor_action_result *result);

#endif
