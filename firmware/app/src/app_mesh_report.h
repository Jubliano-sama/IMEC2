#ifndef APP_MESH_REPORT_H
#define APP_MESH_REPORT_H

#include "dwm3000_driver.h"
#include "app_mesh_route_wait_tx.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "uwb_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
struct k_work_delayable;
#endif
struct app_mesh_command_orchestrator;
struct app_mesh_flood_progress;
struct app_mesh_flood_result;
struct app_node_comm_route_refresh_event;

struct uwb_range_schedule_frame;

enum app_gateway_semantic_acceptance {
    APP_GATEWAY_SEMANTIC_ACCEPT_NEW = 0,
    APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE = 1,
    /*
     * The payload is structurally valid, but reset or terminal operation
     * cleanup removed the volatile state needed to apply it. Preserve the
     * exact raw delivery item and retire sender custody without claiming a
     * semantic state transition.
     */
    APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW = 2,
    /*
     * Collection state already owns this result and no new BLE host item is
     * needed. Reapply only the live collection/EACK transition; do not
     * enqueue a duplicate host record.
     */
    APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE = 3,
    /*
     * A structurally valid collection result/bundle reached a gateway with no
     * live RAM ledger. The raw host record is retained until its exact GUI
     * receipt; that receipt emits a CLOSED one-packet recovery EACK instead
     * of the ordinary gateway ACK path.
     */
    APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY = 4,
};

struct mesh_delivery_health {
    uint32_t ack_retry_admission_failures;
    uint32_t oldest_ack_pending_age_ms;
    uint32_t permanent_report_failures;
    uint32_t last_permanent_error;
    uint32_t last_permanent_session_id;
    uint16_t last_permanent_seq;
    uint8_t last_permanent_msg_type;
};

struct anchor_range_window_report {
    struct dwm3000_range_result result;
    int32_t distance_samples_mm[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t sample_sequence_start_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    uint8_t range_round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t distance_sum_mm;
    int64_t first_exchange_start_ms;
    int64_t click_timestamp_ms;
    uint32_t quality_sum;
    uint16_t sample_count;
    bool have_result;
    bool have_exchange_start_ms;
    bool have_click_timestamp_ms;
    bool rsl_sampled;
    bool cir_sampled;
    bool anchor_full_cir_sampled;
};

struct app_mesh_report_callbacks {
    void (*anchor_note_uwb_awake_since)(int64_t start_ms,
                                        uint32_t already_counted_us);
    bool (*anchor_handle_click_wake_claim)(
        const struct uwb_wake_claim_frame *claim,
        uint8_t link_quality,
        int64_t received_at_ms);
    int (*anchor_handle_local_command)(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t ingress_hop_id);
    int (*anchor_enumeration_prearm)(uint32_t epoch,
                                     uint32_t hold_ms,
                                     uint32_t operation_budget_ms,
                                     bool survey_follows,
                                     uint32_t local_depth_block_start_ms,
                                     uint8_t observed_hop_count);
    void (*gateway_note_anchor_boot_observation)(
        const struct proto_packet *packet,
        const uint8_t *payload,
        size_t payload_len,
        uint64_t first_received_at_ms);
    int (*gateway_note_ack_confirm)(
        const struct proto_packet *confirm_packet,
        const struct mesh_gateway_ack_confirm_identity *identity,
        uint64_t first_received_at_ms);
    int (*anchor_delivery_gateway_accepted)(
        const struct proto_packet *packet,
        const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
    int (*anchor_delivery_gateway_confirmed)(
        const struct proto_packet *packet,
        const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
    void (*gateway_route_refresh_event)(
        const struct app_node_comm_route_refresh_event *event);
    bool (*gateway_route_refresh_prearm)(uint32_t *epoch,
                                         uint32_t *hold_ms,
                                         struct operation_policy_set *policy,
                                         bool *survey_follows);
    void (*gateway_enumeration_pipeline_start)(uint32_t epoch,
                                               uint32_t gateway_wake_start_ms);
};

struct app_mesh_outbound_view {
    const struct proto_packet *packet;
    const uint8_t *payload;
    uint16_t payload_len;
    uint64_t absolute_deadline_ms;
    uint8_t radio_channel;
    uint64_t next_hop_id;
    uint32_t queued_at_ms;
    uint32_t earliest_tx_ms;
    /* Exact facade owner for a deferred node-communication route request. */
    uint32_t delivery_generation;
    uint8_t flood_retry_count;
    bool queued_at_valid;
    bool earliest_tx_valid;
};

struct app_mesh_tx_observation {
    uint64_t rf_started_at_ms;
    uint64_t tx_completed_at_ms;
    uint64_t gateway_confirmed_at_ms;
    uint64_t result_at_ms;
    bool rf_started;
    bool tx_completed;
    bool gateway_confirmed;
};

enum mesh_c5_control_send_mode {
    MESH_C5_CONTROL_WAKE_IF_NEEDED = 0,
    MESH_C5_CONTROL_ACCEPTED_EXCHANGE = 1,
};

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks);
int app_mesh_report_attach_gateway_ack_store(void);
bool app_mesh_report_gateway_delivery_confirmation_pending(
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms);
bool app_mesh_report_gateway_identity_confirmation_pending(
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint16_t seq,
    uint32_t now_ms);
bool app_mesh_report_gateway_operation_confirmation_pending(
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms);
bool app_mesh_report_gateway_origin_confirmation_pending(uint64_t src_id,
                                                         uint32_t now_ms);
int app_mesh_report_attach_anchor_downlink_store(void);
/* Returns one-based RF hops to the gateway, or zero when no route is known. */
uint8_t app_mesh_report_selected_gateway_hop_count(void);
int anchor_append_sequence_time_tlvs(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len,
                                     int64_t local_ms);
int append_range_result_timing_tlvs(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    const struct dwm3000_range_result *result);
int append_anchor_status_tlvs(uint8_t *payload,
                              size_t payload_cap,
                              size_t *payload_len);
int build_uwb_schedule_report_if_relevant(
    const struct uwb_anchor_session *session,
    const struct uwb_range_schedule_frame *schedule,
    uint8_t schedule_flags,
    const struct anchor_range_window_report *report);
uint8_t *mesh_anchor_click_cir_capture_begin(size_t *capacity);
void mesh_stop_role_scan(void);
void mesh_restart_role_scan(void);
int mesh_transport_pause_preserving_queued(void);
bool mesh_transport_pause_active(void);
bool mesh_transport_quiesced(void);
void mesh_transport_resume(void);
bool mesh_c5_flood_work_pending(void);
bool mesh_c5_protocol_flood_work_pending(void);
int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
int mesh_send_c5_control(const struct mesh_outbound *out,
                         uint8_t purpose,
                         enum mesh_c5_control_send_mode mode,
                         const char *reason);
int mesh_send_c5_flood(const struct mesh_outbound *out,
                       uint8_t purpose,
                       const char *reason,
                       bool *sent_now);
int mesh_try_send_c5_flood(const struct mesh_outbound *out,
                           uint8_t purpose,
                           const char *reason,
                           bool *rf_started);
int mesh_try_send_c5_flood_resume(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    struct app_mesh_flood_progress *progress,
    struct app_mesh_flood_result *result,
    bool *rf_started);
int mesh_try_send_c5_flood_view(const struct app_mesh_outbound_view *view,
                                uint8_t purpose,
                                const char *reason,
                                bool send_wake_train,
                                struct app_mesh_tx_observation *observation,
                                uint32_t *scheduled_retry_delay_ms);
int mesh_try_send_control_response_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation);
int mesh_try_send_reliable_uplink_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation,
    uint32_t *scheduled_retry_delay_ms);
/*
 * Reliable-backend release is asynchronous. The request freezes the exact
 * semantic packet identity and hands cancellation to the mesh-route owner;
 * callers must retain their terminal record until the matching result is
 * available through mesh_take_reliable_uplink_cancel_result().
 */
int mesh_request_reliable_uplink_cancel(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t delivery_handle,
    uint32_t delivery_generation,
    uint32_t request_token);
int mesh_take_reliable_uplink_cancel_result(uint32_t delivery_handle,
                                            uint32_t request_token,
                                            int *cancel_result);
int mesh_send_gateway_command_flood(
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    bool *sent_now);
struct app_mesh_command_orchestrator *mesh_gateway_command_orchestrator_context(void);
void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
int mesh_prepare_channel9_outbound(struct mesh_outbound *out,
                                   const struct mesh_event_plan *plan,
                                   uint32_t now_ms,
                                   uint32_t *required_ms);
int mesh_request_route(uint64_t target_id, const char *reason);
void mesh_clear_route_waiting_tx(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum app_mesh_route_wait_tx_owner expected_owner);
int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason);
int mesh_start_owned_tracked_tx(const struct mesh_outbound *out,
                                const char *reason,
                                bool *rf_sent);
int mesh_owned_tracked_tx_preflight(const struct mesh_outbound *out,
                                    const char *reason,
                                    enum app_mesh_route_wait_tx_owner owner,
                                    uint32_t generation);
void mesh_report_wake_active_outbox(const char *reason);
/* Complete custody preemption before this absolute physical handoff deadline. */
int mesh_preempt_for_click_event_until(uint32_t physical_deadline_ms);
void report_tx_schedule(uint32_t delay_ms);
uint32_t report_tx_queue_used(void);
bool mesh_report_tx_backlog_active(void);
bool mesh_report_ch9_ack_wait_active(void);
bool mesh_report_local_protocol_response_active(void);
bool mesh_report_next_channel9_activity_prepare_delay_ms(uint32_t now_ms,
                                                         uint32_t *delay_ms);
int mesh_range_report_batch_reserve(uint64_t clicker_id,
                                    uint32_t event_seq,
                                    uint8_t attempt_index);
int mesh_range_report_batch_reserve_capacity(uint64_t clicker_id,
                                    uint32_t event_seq,
                                    uint8_t attempt_index,
                                    uint8_t fragment_capacity);
int mesh_range_report_batch_rebind(uint64_t current_clicker_id,
                                   uint32_t current_event_seq,
                                   uint8_t current_attempt_index,
                                   uint64_t replacement_clicker_id,
                                   uint32_t replacement_event_seq,
                                   uint8_t replacement_attempt_index);
void mesh_range_report_batch_abort(uint64_t clicker_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index);
int queue_anchor_range_report_fragment(
    const struct mesh_outbound *outbound,
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    bool final_fragment);
/*
 * Return 1 when the active relay RAM owner is either the exact raw outbound
 * or its retained in-RAM gateway-ACK confirmation state, 0 when unrelated/idle.
 * Malformed active confirmation state fails closed with a negative errno.
 */
int mesh_report_active_owner_matches_outbound(
    const struct mesh_outbound *outbound);
int mesh_anchor_range_report_note_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
int mesh_anchor_range_report_note_terminal_release(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int queue_anchor_report(const struct mesh_outbound *outbound);
bool mesh_queue_from_frame(const uint8_t *frame,
                           size_t frame_len,
                           uint8_t link_quality,
                           uint8_t radio_channel,
                           bool *valid_mesh_frame,
                           uint64_t *previous_hop_id);
bool mesh_queue_from_frame_deferred(const uint8_t *frame,
                                    size_t frame_len,
                                    uint8_t link_quality,
                                    uint8_t radio_channel,
                                    bool *valid_mesh_frame,
                                    uint64_t *previous_hop_id);
bool mesh_queue_from_frame_deferred_radio(const uint8_t *frame,
                                          size_t frame_len,
                                          uint8_t link_quality,
                                          int8_t link_rsl_dbm,
                                          bool link_rsl_valid,
                                          uint8_t radio_channel,
                                          bool *valid_mesh_frame,
                                          uint64_t *previous_hop_id);
/* BLE completion boundary for a gateway-local ACK-required host item. */
void mesh_gateway_host_receipt_ingress_queued(void);
void mesh_gateway_host_receipt_ready(void);
bool mesh_anchor_handoff_route_wake_frame(const uint8_t *frame,
                                          size_t frame_len,
                                          uint8_t link_quality);
void mesh_submit_queued_rx(void);
bool mesh_process_queued_rx_now(const char *reason);
int mesh_start_uwb_rx(const char *reason);
void mesh_delivery_health_get(struct mesh_delivery_health *health);
bool mesh_route_waiting_tx_active(void);
uint32_t mesh_rx_pending_count(void);
bool mesh_rx_response_active(void);
bool mesh_anchor_low_duty_scan_should_defer(uint32_t *retry_ms);
bool mesh_anchor_connected_radio_active(void);
int mesh_gateway_command_priority_submit(struct k_work_delayable *work);
int mesh_gateway_command_priority_safe_boundary(void);
#if defined(__ZEPHYR__)
int mesh_route_owner_work_reschedule_timeout(struct k_work_delayable *work,
                                             k_timeout_t delay);
#endif
int mesh_route_owner_work_reschedule(struct k_work_delayable *work,
                                     uint32_t delay_ms);
int mesh_route_work_reschedule(struct k_work_delayable *work, uint32_t delay_ms);
int mesh_schedule_route_request(uint64_t target_id, const char *reason);
int mesh_node_comm_gateway_delivery_due_begin(bool *wait_for_scan_boundary);
bool mesh_node_comm_gateway_delivery_due_pending(void);
bool mesh_node_comm_gateway_delivery_due_ready(void);
bool mesh_node_comm_gateway_delivery_due_end(void);
void app_mesh_report_close_channel9_idle_parent(const char *reason);
#endif
