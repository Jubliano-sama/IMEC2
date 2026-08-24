#ifndef APP_ANCHOR_H
#define APP_ANCHOR_H

#include "protocol.h"
#include "uwb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_report_callbacks;
struct app_durable_state_gateway_assignment_identity;
struct gateway_membership_snapshot;
struct gateway_membership_publication;

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void);
int app_anchor_init(void);
bool app_gateway_enumeration_response_window(
    uint64_t now_ms,
    uint64_t *round_deadline_ms);
bool app_gateway_enumeration_response_pending_wait_ms(
    uint64_t now_ms,
    uint32_t *wait_ms);
int app_gateway_enumeration_response_handle_bundle(
    const struct uwb_enumeration_bundle_frame *bundle,
    uint64_t received_at_ms,
    struct uwb_enumeration_hop_ack_frame *ack);
void app_gateway_enumeration_response_publish_pending(void);
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
#if defined(CONFIG_IMEC_GATEWAY_BLE)
bool gateway_handle_ble_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t result_reservation_token);
#endif

#endif
