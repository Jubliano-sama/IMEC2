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
};

struct app_mesh_report_callbacks {
    bool (*anchor_survey_discovery_is_pending)(void);
    void (*anchor_note_uwb_awake_since)(int64_t start_ms,
                                        uint32_t already_counted_us);
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
void mesh_stop_role_scan(void);
void mesh_restart_role_scan(void);
int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
int mesh_request_route(uint64_t target_id, const char *reason);
void mesh_clear_route_waiting_tx(const struct proto_packet *packet);
int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason);
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
int mesh_start_uwb_rx(const char *reason);
bool mesh_route_waiting_tx_active(void);
void mesh_gateway_route_adv_start(void);

#endif
