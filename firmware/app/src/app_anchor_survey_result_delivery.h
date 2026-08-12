#ifndef APP_ANCHOR_SURVEY_RESULT_DELIVERY_H
#define APP_ANCHOR_SURVEY_RESULT_DELIVERY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mesh_outbound;
struct survey_pair;
struct app_node_comm_reservation_lease;

/* Fixed number of in-RAM survey pair-result delivery slots. */
#define APP_MESH_SURVEY_PAIR_RESULT_DELIVERY_SLOTS 5u

struct app_anchor_survey_result_delivery_ops {
    int (*schedule_work_ms)(uint32_t delay_ms);
    int (*active_owner_matches_outbound)(
        const struct mesh_outbound *outbound);
    void (*wake_active_outbox)(const char *reason);
};

int app_anchor_survey_result_delivery_init(
    const struct app_anchor_survey_result_delivery_ops *ops);

/*
 * Stage the exact sample in one bounded RAM slot before consuming the
 * pre-reserved communication record. A negative return leaves any
 * successfully staged record under the delivery worker's custody; the caller
 * must stop producing further samples and cancel its unused reservations.
 */
int app_anchor_survey_result_delivery_stage_reserved(
    const struct app_node_comm_reservation_lease *delivery_reservation,
    const struct mesh_outbound *outbound,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN]);
/*
 * Release every still-owned burst reservation even if one cancellation
 * fails. An expired lease has already been reclaimed by node communication,
 * so it is safe cleanup rather than a watchdog condition.
 */
int app_anchor_survey_result_delivery_cancel_reservations(
    struct app_node_comm_reservation_lease *delivery_reservations,
    size_t delivery_reservation_count,
    const char *reason);

/*
 * Retire only responder records bound to the exact ABORT operation, survey,
 * pair, sample-count, and round tuple. The control path validates the ABORT's
 * full-round commitment; pair-result records do not carry that commitment.
 * A duplicate after all matching records retired is an idempotent success.
 */
int app_anchor_survey_result_delivery_abort_round(
    const struct survey_pair *pair,
    uint32_t session_id,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN],
    bool producer_active,
    size_t *retired_count);
void app_anchor_survey_result_delivery_producer_finished(
    const struct survey_pair *pair,
    uint32_t session_id,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN]);

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
