#ifndef APP_ANCHOR_H
#define APP_ANCHOR_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_report_callbacks;
struct app_durable_state_gateway_assignment_identity;
struct gateway_membership_snapshot;
struct gateway_membership_publication;

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void);
int app_anchor_init(void);
int app_anchor_start_anchor_role(void);
int app_anchor_start_gateway_role(void);
/* Retain one widened Channel-5 scan after an anchor releases Channel-9. */
void app_anchor_note_channel9_window_released(void);
/*
 * Reconstruct an exact durable enumeration publication after reset.  Returns
 * 1 while publication debt remains, 0 when there is none, or a negative errno
 * when retry is required.
 */
int gateway_resume_pending_assignment_publication(void);
int app_anchor_gateway_assignment_resume_pending_table(
    const struct gateway_membership_snapshot *snapshot,
    const struct gateway_membership_publication *publication);
/* Release the exact assignment lease only after publisher state is terminal. */
int app_anchor_gateway_assignment_publication_complete(void);
/* Same-boot ambiguous durable-save adoption retains this RAM-only token.  It
 * may be converted once only after the exact durable identity is read back. */
int app_anchor_gateway_assignment_adopted_result_commit(
    const struct app_durable_state_gateway_assignment_identity *identity,
    uint16_t published_count);
int gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            uint64_t received_at_ms,
                                            uint64_t previous_hop_id);
/*
 * Returns 1 when ordinary semantic data may commit, 0 when an active
 * enumeration temporarily isolates an unregistered source, or a negative
 * errno when admission state is unavailable.
 */
int gateway_discovery_assignment_admit_nonassignment_source(uint64_t src_id);
/*
 * Validate a host-visible survey delivery without changing survey state.
 * The mesh RX owner must remain held from a nonnegative result through the
 * matching semantic commit. Returns APP_GATEWAY_SEMANTIC_ACCEPT_NEW or
 * APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE when commit is guaranteed admissible.
 */
int gateway_survey_preflight_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t received_at_ms,
    uint64_t previous_hop_id,
    uint8_t radio_channel,
    uint8_t link_quality);
enum gateway_survey_result_preflight_outcome {
    GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED = 0,
    GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED,
    GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED,
};

enum gateway_survey_result_preflight_outcome
gateway_survey_preflight_result(const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t received_at_ms);
void gateway_survey_commit_preflight_result(void);
void gateway_survey_discard_preflight_result(void);
bool gateway_survey_owns_pending_control(
    const struct proto_packet *command,
    enum command_id command_id);
/* Return zero when gateway Channel-5 RF may start now, otherwise the exact
 * delay until the active pair's immutable ranging window has ended. */
uint32_t app_anchor_gateway_survey_c5_quiet_delay_ms(uint32_t now_ms,
                                                     uint32_t tx_span_ms);
#if defined(CONFIG_IMEC_GATEWAY_BLE)
bool gateway_handle_ble_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t result_reservation_token);
#endif

#endif
