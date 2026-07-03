#ifndef APP_GATEWAY_EACK_POLICY_H
#define APP_GATEWAY_EACK_POLICY_H

#include "mesh.h"
#include "mesh_relay.h"

#include <stdint.h>
#include <stddef.h>

enum app_gateway_eack_send_mode {
    APP_GATEWAY_EACK_SEND_NONE = 0,
    APP_GATEWAY_EACK_SEND_CHANNEL9 = 1,
    APP_GATEWAY_EACK_SEND_C5_FLOOD = 2,
    APP_GATEWAY_EACK_SEND_CURRENT_CHANNEL9 = 3,
};

struct app_gateway_eack_policy_ops {
    int (*plan_channel9)(uint64_t next_hop_id,
                         struct mesh_event_plan *plan,
                         void *ctx);
    int (*prepare_channel9)(struct mesh_outbound *out,
                            const struct mesh_event_plan *plan,
                            void *ctx);
    int (*send_channel9)(const struct mesh_outbound *out, void *ctx);
    int (*send_c5_flood)(const struct mesh_outbound *out, void *ctx);
    void (*note_tx_sent)(const struct mesh_outbound *out, void *ctx);
    void (*note_channel9_tx)(uint64_t next_hop_id,
                             uint32_t event_start_ms,
                             void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void *ctx;
};

struct app_gateway_eack_policy_result {
    enum app_gateway_eack_send_mode mode;
    int channel9_plan_ret;
    int channel9_prepare_ret;
    int channel9_send_ret;
    int c5_send_ret;
    uint64_t channel9_next_hop_id;
    uint8_t channel9_candidate_count;
    uint8_t channel9_attempt_count;
};

int app_gateway_eack_send_to_candidates(struct mesh_outbound *eack,
                                        const uint64_t *return_next_hop_ids,
                                        size_t return_next_hop_count,
                                        const struct app_gateway_eack_policy_ops *ops,
                                        struct app_gateway_eack_policy_result *result);
int app_gateway_eack_send_to_candidates_with_current_channel9(
    struct mesh_outbound *eack,
    uint64_t current_channel9_next_hop_id,
    const struct mesh_event_plan *current_channel9_plan,
    const uint64_t *return_next_hop_ids,
    size_t return_next_hop_count,
    const struct app_gateway_eack_policy_ops *ops,
    struct app_gateway_eack_policy_result *result);

int app_gateway_eack_send(struct mesh_outbound *eack,
                          uint64_t return_next_hop_id,
                          const struct app_gateway_eack_policy_ops *ops,
                          struct app_gateway_eack_policy_result *result);

#endif
