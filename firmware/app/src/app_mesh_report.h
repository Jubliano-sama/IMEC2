#ifndef APP_MESH_REPORT_H
#define APP_MESH_REPORT_H

#include "dwm3000_driver.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "uwb_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct k_work_delayable;

struct uwb_range_schedule_frame;

struct anchor_range_window_report {
    struct dwm3000_range_result result;
    int32_t distance_samples_mm[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t sample_sequence_start_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    uint8_t range_round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t distance_sum_mm;
    int64_t first_exchange_start_ms;
    uint32_t quality_sum;
    uint16_t sample_count;
    bool have_result;
    bool have_exchange_start_ms;
    bool rsl_sampled;
    bool cir_sampled;
    bool anchor_full_cir_sampled;
};

struct app_mesh_report_callbacks {
    bool (*anchor_survey_discovery_is_pending)(void);
    void (*anchor_note_uwb_awake_since)(int64_t start_ms,
                                        uint32_t already_counted_us);
    bool (*anchor_handle_click_wake_claim)(
        const struct uwb_wake_claim_frame *claim,
        uint8_t link_quality,
        int64_t received_at_ms);
    void (*anchor_handle_local_command)(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len);
    void (*anchor_handle_survey_discovery_start)(const struct proto_packet *packet,
                                                 const uint8_t *payload,
                                                 size_t payload_len);
    void (*anchor_handle_survey_pair_prepare)(const struct proto_packet *packet,
                                              const uint8_t *payload,
                                              size_t payload_len);
    void (*gateway_handle_survey_discovery_report)(const struct proto_packet *packet,
                                                   const uint8_t *payload,
                                                   size_t payload_len);
};

enum mesh_c5_control_send_mode {
    MESH_C5_CONTROL_WAKE_IF_NEEDED = 0,
    MESH_C5_CONTROL_ACCEPTED_EXCHANGE = 1,
};

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks);
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
void build_uwb_schedule_report_if_relevant(
    const struct uwb_anchor_session *session,
    uint8_t schedule_flags,
    const struct anchor_range_window_report *report);
uint8_t *mesh_anchor_click_cir_capture_begin(size_t *capacity);
void mesh_stop_role_scan(void);
void mesh_restart_role_scan(void);
int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
int mesh_send_c5_control(const struct mesh_outbound *out,
                         uint8_t purpose,
                         enum mesh_c5_control_send_mode mode,
                         const char *reason);
int mesh_send_c5_flood(const struct mesh_outbound *out,
                       uint8_t purpose,
                       const char *reason);
void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
int mesh_prepare_channel9_outbound(struct mesh_outbound *out,
                                   const struct mesh_event_plan *plan,
                                   uint32_t now_ms,
                                   uint32_t *required_ms);
int mesh_request_route(uint64_t target_id, const char *reason);
void mesh_clear_route_waiting_tx(const struct proto_packet *packet);
int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason);
void mesh_report_resume_restored_outbox(const char *reason);
void mesh_preempt_for_click_event(void);
void report_tx_schedule(uint32_t delay_ms);
uint32_t report_tx_queue_used(void);
bool mesh_report_tx_backlog_active(void);
bool mesh_report_ch9_ack_wait_active(void);
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
void mesh_submit_queued_rx(void);
bool mesh_process_queued_rx_now(const char *reason);
int mesh_start_uwb_rx(const char *reason);
bool mesh_route_waiting_tx_active(void);
uint32_t mesh_rx_pending_count(void);
bool mesh_rx_response_active(void);
bool mesh_anchor_low_duty_scan_should_defer(uint32_t *retry_ms);
bool mesh_anchor_connected_radio_active(void);
void mesh_gateway_route_adv_start(void);
void mesh_gateway_route_adv_request(uint32_t delay_ms, const char *reason);
/* Returns zero when the forced advertisement is queued or rescheduled. */
int mesh_gateway_route_adv_force_request(uint32_t delay_ms, const char *reason);
int mesh_gateway_command_priority_submit(struct k_work_delayable *work);

#endif
