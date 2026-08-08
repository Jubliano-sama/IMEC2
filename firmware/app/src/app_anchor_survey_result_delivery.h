#ifndef APP_ANCHOR_SURVEY_RESULT_DELIVERY_H
#define APP_ANCHOR_SURVEY_RESULT_DELIVERY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mesh_outbound;

/* Fixed number of in-RAM survey pair-result delivery slots. */
#define APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS 5u

struct app_anchor_survey_result_delivery_ops {
    int (*schedule_work_ms)(uint32_t delay_ms);
    int (*active_owner_matches_outbound)(
        const struct mesh_outbound *outbound);
    void (*resume_restored_outbox)(const char *reason);
};

int app_anchor_survey_result_delivery_init(
    const struct app_anchor_survey_result_delivery_ops *ops);
int app_anchor_survey_result_delivery_restore(bool *restored);
int app_anchor_survey_result_delivery_start(void);

/*
 * Stage the exact sample in NVS before consuming the pre-reserved
 * communication record. A negative return leaves any successfully staged
 * record under the delivery worker's custody; the caller must stop producing
 * further samples and cancel its unused communication reservations.
 */
int app_anchor_survey_result_delivery_stage_reserved(
    uint32_t delivery_reservation_token,
    const struct mesh_outbound *outbound);
/*
 * Release every still-owned burst reservation even if one cancellation
 * fails. Any failure loses the caller's only durable-capacity token and
 * therefore forces watchdog recovery after the complete cleanup pass.
 */
int app_anchor_survey_result_delivery_cancel_reservations(
    uint32_t *delivery_reservation_tokens,
    size_t delivery_reservation_count,
    const char *reason);

int app_anchor_survey_result_delivery_service(void);
size_t app_anchor_survey_result_delivery_occupied_count(void);
int app_anchor_survey_result_delivery_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
int app_anchor_survey_result_delivery_transport_released(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool preempted);

#endif
