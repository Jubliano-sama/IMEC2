#include "app_mesh_report.h"

#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "route.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_report, LOG_LEVEL_DBG);

struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload_len;
    uint64_t previous_hop_id;
    uint8_t link_quality;
    uint32_t received_at_ms;
};

struct mesh_frame_parse_context {
    bool found;
    uint64_t previous_hop_id;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

K_MSGQ_DEFINE(mesh_rx_msgq, sizeof(struct mesh_rx_pending), MESH_RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(report_tx_msgq, sizeof(struct mesh_outbound), REPORT_TX_QUEUE_DEPTH, 4);
static struct mesh_outbound mesh_route_waiting_tx;
static bool mesh_route_waiting_tx_valid;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_uwb_rx_work;
static struct k_work_delayable mesh_tx_timeout_work;
static struct k_work_delayable report_tx_work;


static const struct app_mesh_report_callbacks *mesh_report_callbacks;

static void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);

static bool mesh_report_anchor_survey_discovery_is_pending(void)
{
    return mesh_report_callbacks != NULL &&
           mesh_report_callbacks->anchor_survey_discovery_is_pending != NULL &&
           mesh_report_callbacks->anchor_survey_discovery_is_pending();
}

static void mesh_report_note_anchor_uwb_awake_since(int64_t start_ms,
                                                    uint32_t already_counted_us)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_note_uwb_awake_since != NULL) {
        mesh_report_callbacks->anchor_note_uwb_awake_since(start_ms, already_counted_us);
    }
}

static void mesh_report_anchor_handle_local_command(const struct proto_packet *packet,
                                                    const uint8_t *payload,
                                                    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_local_command != NULL) {
        mesh_report_callbacks->anchor_handle_local_command(packet, payload, payload_len);
    }
}

static void mesh_report_anchor_handle_survey_discovery_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_survey_discovery_start != NULL) {
        mesh_report_callbacks->anchor_handle_survey_discovery_start(packet, payload, payload_len);
    }
}

static void mesh_report_anchor_handle_survey_pair_prepare(const struct proto_packet *packet,
                                                          const uint8_t *payload,
                                                          size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_survey_pair_prepare != NULL) {
        mesh_report_callbacks->anchor_handle_survey_pair_prepare(packet, payload, payload_len);
    }
}

static void mesh_report_gateway_handle_survey_discovery_report(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->gateway_handle_survey_discovery_report != NULL) {
        mesh_report_callbacks->gateway_handle_survey_discovery_report(packet, payload, payload_len);
    }
}

static void mesh_rx_pending_refresh_age(struct mesh_rx_pending *pending, uint32_t now_ms)
{
    if (pending == NULL) {
        return;
    }

    if (pending->received_at_ms != 0u) {
        packet_age_add_elapsed(&pending->packet, now_ms - pending->received_at_ms);
    }
    pending->received_at_ms = now_ms;
}

int anchor_append_sequence_time_tlvs(uint8_t *payload,
                                            size_t payload_cap,
                                            size_t *payload_len,
                                            int64_t local_ms)
{
    uint64_t timestamp_ms = 0u;

    anchor_sequence_timestamp_at(local_ms, &timestamp_ms);

    return tlv_append_u64(payload,
                          payload_cap,
                          payload_len,
                          TLV_TIMESTAMP_MS,
                          timestamp_ms);
}

static bool range_result_has_raw_timestamps(const struct dwm3000_range_result *result)
{
    return result != NULL &&
           (result->poll_tx_ts_32 != 0u ||
            result->poll_rx_ts_32 != 0u ||
            result->resp_tx_ts_32 != 0u ||
            result->resp_rx_ts_32 != 0u ||
            result->final_tx_ts_32 != 0u ||
            result->final_rx_ts_32 != 0u);
}

int append_range_result_timing_tlvs(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    const struct dwm3000_range_result *result)
{
    const uint8_t *rx_diag = NULL;
    uint8_t rx_diag_len = 0u;
    int ret;

    if (payload == NULL || payload_len == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    if (result->rsl_sampled) {
        ret = tlv_append_i8(payload, payload_cap, payload_len,
                            TLV_UWB_RSL_DBM, result->rsl_dbm);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (result->clock_offset_sampled) {
        ret = tlv_append_u16(payload, payload_cap, payload_len,
                             TLV_UWB_CLOCK_OFFSET_RAW,
                             (uint16_t)result->clock_offset_raw);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (result->carrier_integrator_sampled) {
        ret = tlv_append_i32(payload, payload_cap, payload_len,
                             TLV_UWB_CARRIER_INTEGRATOR,
                             result->carrier_integrator);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (range_result_has_raw_timestamps(result)) {
        uint8_t timestamps[6u * sizeof(uint32_t)];

        proto_put_u32_le(&timestamps[0], result->poll_tx_ts_32);
        proto_put_u32_le(&timestamps[4], result->poll_rx_ts_32);
        proto_put_u32_le(&timestamps[8], result->resp_tx_ts_32);
        proto_put_u32_le(&timestamps[12], result->resp_rx_ts_32);
        proto_put_u32_le(&timestamps[16], result->final_tx_ts_32);
        proto_put_u32_le(&timestamps[20], result->final_rx_ts_32);
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_UWB_RAW_TIMESTAMPS,
                               timestamps,
                               sizeof(timestamps));
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    if (result->clicker_rx_diag_sampled) {
        rx_diag = result->clicker_rx_diag_raw;
        rx_diag_len = result->clicker_rx_diag_raw_len;
    } else if (result->anchor_rx_diag_sampled) {
        rx_diag = result->anchor_rx_diag_raw;
        rx_diag_len = result->anchor_rx_diag_raw_len;
    }
    if (rx_diag != NULL && rx_diag_len > 0u) {
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_UWB_RX_DIAG_BYTES,
                               rx_diag,
                               rx_diag_len);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    return PROTO_OK;
}

static uint32_t anchor_status_bits(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0u;
    }

    return uwb_session_status_bits_from_diagnostics(&anchor_uwb_session.diagnostics);
}

int append_anchor_status_tlvs(uint8_t *payload, size_t payload_cap, size_t *payload_len)
{
    struct anchor_heartbeat_fields fields = {
        .device_role = (uint8_t)DEVICE_ROLE,
        .battery_mv = ANCHOR_BATTERY_MV_UNKNOWN,
        .status_bits = anchor_status_bits(),
        .uptime_ms = k_uptime_get_32(),
    };
    int ret;

    anchor_sequence_timestamp_at(k_uptime_get(), &fields.timestamp_ms);
    ret = report_append_anchor_heartbeat_tlvs(payload, payload_cap, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_append_status_tlvs(&mesh_runtime, payload, payload_cap, payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CHANNEL_SWITCHES,
                         mesh_event_stats.channel_switches);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_PLL_READY_FAILURES,
                         mesh_event_stats.pll_ready_failures);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_LATE_CHANNEL5_RETURNS,
                         mesh_event_stats.late_channel5_returns);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_DEFERRALS,
                         mesh_event_stats.mesh_deferrals);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CH9_EVENT_MISSES,
                         mesh_event_stats.ch9_event_misses);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CHANNEL5_PREEMPTIONS,
                         mesh_event_stats.channel5_preemptions);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          payload_len,
                          TLV_MESH_CH9_REPORT_LATENCY_MS,
                          mesh_event_stats.ch9_report_latency_ms);
}

void mesh_preempt_for_click_event(void)
{
    struct mesh_outbound pending_report = {0};
    bool requeue_report = false;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        if (mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
            mesh_runtime.pending.packet.src_id == DEVICE_ID) {
            pending_report.packet = mesh_runtime.pending.packet;
            pending_report.payload_len = mesh_runtime.pending.payload_len;
            if (pending_report.payload_len > 0u) {
                memcpy(pending_report.payload,
                       mesh_runtime.pending.payload,
                       pending_report.payload_len);
            }
            requeue_report = true;
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        LOG_INF("anchor click discovery preempted active mesh TX");
    }

    k_msgq_purge(&mesh_rx_msgq);
    if (requeue_report) {
        if (k_msgq_put(&report_tx_msgq, &pending_report, K_NO_WAIT) != 0) {
            LOG_WRN("preempted click report could not be requeued");
        }
    }
}

static void mesh_schedule_tx_timeout(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_relay_tx_active(&mesh_runtime)) {
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        return;
    }

    deadline = mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ?
               mesh_runtime.pending.retry_after_ms :
               mesh_runtime.pending.gateway_ack_deadline_ms;
    delay_ms = uptime_ms_until_deadline(now, deadline);
    (void)k_work_reschedule(&mesh_tx_timeout_work, K_MSEC(delay_ms));
}

static bool mesh_role_uses_uwb_rx(void)
{
    if (stage1_anchor_focused_rx_logs_enabled()) {
        return false;
    }
    return DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY;
}

static uint32_t mesh_uwb_rx_window_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_WINDOW_MS :
           UWB_MESH_ANCHOR_RX_WINDOW_MS;
}

static uint32_t mesh_uwb_rx_idle_delay_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_IDLE_MS :
           UWB_MESH_ANCHOR_RX_INTERVAL_MS;
}

static bool mesh_select_channel9_rx_event(uint32_t now_ms,
                                          struct mesh_event_plan *selected_plan,
                                          uint64_t *selected_peer,
                                          uint8_t *selected_index)
{
    struct mesh_channel5_requirements requirements;
    bool selected = false;

    if (selected_plan == NULL || selected_peer == NULL || selected_index == NULL) {
        return false;
    }

    (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
    mesh_fill_channel5_requirements(&requirements);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_plan plan = {0};
        int ret;

        if (!entry->valid) {
            continue;
        }
        ret = mesh_event_plan_channel9(&entry->timing, &requirements, now_ms, &plan);
        if (ret != PROTO_OK) {
            continue;
        }
        mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        if (plan.action != MESH_EVENT_PLAN_START &&
            plan.action != MESH_EVENT_PLAN_CLIP) {
            continue;
        }
        if (!selected || uptime_deadline_reached(selected_plan->start_ms, plan.start_ms)) {
            *selected_plan = plan;
            *selected_peer = entry->next_hop_id;
            *selected_index = i;
            selected = true;
        }
    }
    return selected;
}

static uint32_t mesh_next_channel9_rx_delay_ms(uint32_t now_ms)
{
    uint32_t delay_ms = mesh_uwb_rx_idle_delay_ms();

    (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        uint32_t candidate_delay_ms;

        if (!entry->valid || !mesh_event_timing_usable(&entry->timing, now_ms)) {
            continue;
        }
        candidate_delay_ms = uptime_ms_until_deadline(now_ms, entry->timing.next_event_time_ms);
        if (candidate_delay_ms < delay_ms) {
            delay_ms = candidate_delay_ms;
        }
    }
    return delay_ms;
}

static void mesh_schedule_uwb_rx(uint32_t delay_ms)
{
    if (!mesh_role_uses_uwb_rx()) {
        return;
    }

    mesh_uwb_rx_active = true;
    (void)k_work_reschedule(&mesh_uwb_rx_work, K_MSEC(delay_ms));
}

void mesh_stop_role_scan(void)
{
    if (mesh_uwb_rx_active) {
        (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
        mesh_uwb_rx_active = false;
    }
}

void mesh_restart_role_scan(void)
{
    int ret;

    if (DEVICE_ROLE == ROLE_ANCHOR && !anchor_uwb_busy) {
        ret = mesh_start_uwb_rx("anchor mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart anchor UWB mesh RX: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = mesh_start_uwb_rx("gateway mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart gateway UWB mesh RX: %d", ret);
        }
    }
}

int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    struct mesh_outbound tx;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    int64_t uwb_window_start_ms = -1;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }

    tx = *out;
    mesh_outbound_refresh_age(&tx, k_uptime_get_32());

    ret = uwb_mesh_frame_encode(NETWORK_ID,
                                DEVICE_ID,
                                tx.next_hop_id,
                                &tx.packet,
                                tx.payload,
                                frame,
                                sizeof(frame),
                                &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        return -EINVAL;
    }

    mesh_stop_role_scan();
    ret = radio_guard_uwb_start("mesh UWB TX");
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = tx.radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
          dwm3000_driver_configure_mesh_payload_mode() :
          dwm3000_driver_configure_default();
    if (tx.radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    }
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_MESH_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_standby();
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                tx.packet.msg_type,
                (unsigned long long)tx.next_hop_id,
                (unsigned int)frame_len,
                ret);
        return ret;
    }

    HIGH_DEBUG_COUNTER_INC(mesh_tx);
    if (tx.packet.msg_type == MSG_GATEWAY_ACK) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_TX",
                             "dst=0x%016llx next=0x%016llx seq=%u channel=%u",
                             (unsigned long long)tx.packet.dst_id,
                             (unsigned long long)tx.next_hop_id,
                             tx.packet.seq,
                             tx.radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                             UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT);
    }
    high_debug_log_event("MESH_TX",
                         "reason=%s msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u age_ms=%u channel=%u frame_len=%u",
                         reason,
                         tx.packet.msg_type,
                         (unsigned long long)tx.packet.src_id,
                         (unsigned long long)tx.packet.dst_id,
                         (unsigned long long)tx.next_hop_id,
                         tx.packet.seq,
                         tx.packet.message_age_ms,
                         tx.radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                         UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
                         (unsigned int)frame_len);
    LOG_INF("mesh UWB TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u age_ms=%u channel=%u frame_len=%u",
            reason,
            tx.packet.msg_type,
            (unsigned long long)tx.packet.src_id,
            (unsigned long long)tx.packet.dst_id,
            (unsigned long long)tx.next_hop_id,
            tx.packet.seq,
            tx.packet.ttl,
            tx.packet.message_age_ms,
            tx.radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
            UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
            (unsigned int)frame_len);
    return 0;
}

int mesh_request_route(uint64_t target_id, const char *reason)
{
    struct mesh_outbound route_req;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    ret = mesh_relay_prepare_route_request(&mesh_runtime,
                                           target_id,
                                           now_ms,
                                           sys_rand32_get(),
                                           &route_req);
    if (ret != PROTO_OK) {
        if (ret == PROTO_ERR_BUSY) {
            LOG_INF("mesh route discovery backoff active: target=0x%016llx attempts=%u next_ms=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    mesh_runtime.route_discovery.next_request_ms,
                    reason);
            return -EAGAIN;
        }
        if (ret == PROTO_ERR_STALE) {
            LOG_WRN("mesh route discovery exhausted: target=0x%016llx attempts=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    reason);
            return -ETIMEDOUT;
        }
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("mesh route discovery request: target=0x%016llx attempt=%u next_ms=%u reason=%s",
            (unsigned long long)target_id,
            mesh_runtime.route_discovery.attempts,
            mesh_runtime.route_discovery.next_request_ms,
            reason);
    return mesh_send_outbound(&route_req, "route-request");
}

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_GATEWAY_ACK:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements)
{
    uint32_t now_ms;

    if (requirements == NULL) {
        return;
    }

    memset(requirements, 0, sizeof(*requirements));
    requirements->retune_guard_ms = UWB_SCHEDULE_GUARD_MS;
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        now_ms = k_uptime_get_32();
        requirements->next_required_scan_start_ms = now_ms + anchor_uwb_scan_interval_ms;
        requirements->click_epoch_active = anchor_uwb_window_active();
        requirements->discovery_active = requirements->click_epoch_active;
        requirements->ranging_active = requirements->click_epoch_active;
    }
}

static int mesh_prepare_event_timing(struct mesh_event_timing *timing, uint32_t now_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = MESH_EVENT_DEFAULT_INTERVAL_MS,
        .event_window_ms = MESH_EVENT_DEFAULT_WINDOW_MS,
        .first_event_time_ms = now_ms + MESH_EVENT_DEFAULT_FIRST_DELAY_MS,
        .guard_ms = MESH_EVENT_DEFAULT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = MESH_EVENT_DEFAULT_MAX_MISSED,
        .supervision_timeout_ms = MESH_EVENT_DEFAULT_SUPERVISION_MS,
    };

    return mesh_event_timing_negotiate(timing, &params, true);
}

static int mesh_send_event_control(uint64_t peer_id,
                                   uint8_t msg_type,
                                   const struct mesh_event_timing *accepted_timing,
                                   bool install_local,
                                   const char *reason)
{
    struct mesh_event_timing timing = {0};
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return -EINVAL;
    }

    if (accepted_timing != NULL) {
        timing = *accepted_timing;
    } else {
        ret = mesh_prepare_event_timing(&timing, now_ms);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }

    ret = mesh_append_event_timing_tlvs_at(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           &timing,
                                           now_ms);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = mesh_init_event_control(&outbound.packet,
                                  msg_type,
                                  DEVICE_ID,
                                  peer_id,
                                  nonzero_uptime_session_id(),
                                  mesh_next_event_control_seq(),
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = peer_id;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    LOG_INF("mesh channel-9 event control TX: msg=0x%02x peer=0x%016llx interval_ms=%u window_ms=%u next_ms=%u reason=%s",
            msg_type,
            (unsigned long long)peer_id,
            timing.event_interval_ms,
            timing.event_window_ms,
            timing.next_event_time_ms,
            reason == NULL ? "event-control" : reason);
    ret = mesh_send_outbound(&outbound, reason == NULL ? "mesh-event-control" : reason);
    if (ret < 0) {
        return ret;
    }
    if (install_local) {
        ret = mesh_relay_set_channel9_timing(&mesh_runtime, peer_id, &timing);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                      timing.next_event_time_ms));
    }
    return 0;
}

static void mesh_propose_event_after_channel5_contact(uint64_t peer_id, const char *reason)
{
    int ret;

    ret = mesh_send_event_control(peer_id,
                                  MSG_MESH_EVENT_PROPOSE,
                                  NULL,
                                  true,
                                  reason);
    if (ret < 0) {
        LOG_WRN("mesh channel-9 event proposal failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-propose" : reason);
    }
}

static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id)
{
    struct mesh_event_timing timing = {0};
    int ret;

    if (packet == NULL) {
        return false;
    }

    if (packet->msg_type != MSG_MESH_EVENT_PROPOSE &&
        packet->msg_type != MSG_MESH_EVENT_ACCEPT &&
        packet->msg_type != MSG_MESH_EVENT_UPDATE &&
        packet->msg_type != MSG_MESH_EVENT_END) {
        return false;
    }
    if (!mesh_id_is_unicast(previous_hop_id)) {
        LOG_WRN("mesh event control ignored without unicast previous hop");
        return true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        mesh_relay_clear_channel9_timing(&mesh_runtime, previous_hop_id);
        LOG_INF("mesh channel-9 event timing cleared for next=0x%016llx",
                (unsigned long long)previous_hop_id);
        return true;
    }

    ret = mesh_event_timing_from_tlvs_at(&timing,
                                         payload,
                                         payload_len,
                                         k_uptime_get_32(),
                                         true);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing parse failed: msg=0x%02x ret=%d",
                packet->msg_type,
                ret);
        return true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        ret = mesh_send_event_control(previous_hop_id,
                                      MSG_MESH_EVENT_ACCEPT,
                                      &timing,
                                      false,
                                      "mesh-event-accept");
        if (ret < 0) {
            LOG_WRN("mesh channel-9 event ACCEPT failed: next=0x%016llx ret=%d",
                    (unsigned long long)previous_hop_id,
                    ret);
            return true;
        }
    }

    ret = mesh_relay_set_channel9_timing(&mesh_runtime, previous_hop_id, &timing);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing install failed: next=0x%016llx ret=%d",
                (unsigned long long)previous_hop_id,
                ret);
        return true;
    }

    LOG_INF("mesh channel-9 event timing installed: next=0x%016llx interval_ms=%u window_ms=%u next_ms=%u",
            (unsigned long long)previous_hop_id,
            timing.event_interval_ms,
            timing.event_window_ms,
            timing.next_event_time_ms);
    mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(), timing.next_event_time_ms));
    return true;
}

static bool mesh_tx_can_wait_for_route(const struct mesh_outbound *out)
{
    if (out == NULL ||
        out->packet.dst_id == MESH_BROADCAST_ID ||
        out->packet.dst_id == DEVICE_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_GATEWAY_ACK:
    case MSG_MESH_DATA:
    case MSG_SELF_TEST_REPORT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static void mesh_store_route_waiting_tx(const struct mesh_outbound *out)
{
    struct mesh_outbound waiting;

    if (!mesh_tx_can_wait_for_route(out)) {
        return;
    }

    waiting = *out;
    if (waiting.queued_at_ms == 0u) {
        waiting.queued_at_ms = k_uptime_get_32();
    }
    mesh_route_waiting_tx = waiting;
    mesh_route_waiting_tx_valid = true;
}

void mesh_clear_route_waiting_tx(const struct proto_packet *packet)
{
    if (!mesh_route_waiting_tx_valid || packet == NULL) {
        return;
    }
    if (mesh_route_waiting_tx.packet.dst_id == packet->dst_id &&
        mesh_route_waiting_tx.packet.session_id == packet->session_id &&
        mesh_route_waiting_tx.packet.seq == packet->seq) {
        mesh_route_waiting_tx_valid = false;
    }
}

static void mesh_try_route_waiting_tx(void)
{
    struct mesh_outbound pending;
    uint32_t now_ms;
    int ret;

    if (!mesh_route_waiting_tx_valid ||
        (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) ||
        mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    pending = mesh_route_waiting_tx;
    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(&pending, now_ms)) {
        return;
    }
    ret = mesh_start_tracked_tx(&pending, "route-discovered-packet");
    if (ret == 0) {
        mesh_route_waiting_tx_valid = false;
    } else if (ret == -EHOSTUNREACH) {
        ret = mesh_request_route(pending.packet.dst_id, "route-waiting-packet");
        if (ret == -ETIMEDOUT) {
            mesh_route_waiting_tx_valid = false;
        }
    } else if (ret == -ETIMEDOUT) {
        mesh_route_waiting_tx_valid = false;
    }
}

int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
{
    struct mesh_outbound aged_out;
    struct mesh_outbound tx;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint32_t now_ms;
    uint32_t channel9_event_start_ms = 0u;
    uint64_t channel9_next_hop_id = 0u;
    bool channel9_success_pending = false;
    bool channel9_report_latency_pending = false;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }

    aged_out = *out;
    now_ms = k_uptime_get_32();
    mesh_outbound_refresh_age(&aged_out, now_ms);

    if (mesh_packet_prefers_channel9(&aged_out.packet)) {
        mesh_fill_channel5_requirements(&requirements);
        ret = mesh_relay_start_channel9_tx(&mesh_runtime,
                                           &aged_out.packet,
                                           aged_out.payload,
                                           aged_out.payload_len,
                                           &requirements,
                                           now_ms,
                                           &plan,
                                           &tx);
        if (ret != PROTO_ERR_NOT_FOUND) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        if (ret == PROTO_OK) {
            channel9_success_pending = true;
            channel9_report_latency_pending = aged_out.packet.msg_type == MSG_CLICK_REPORT;
            channel9_event_start_ms = plan.start_ms;
            channel9_next_hop_id = tx.next_hop_id;
            goto send_prepared;
        }
        if (ret == PROTO_ERR_BUSY) {
            return -EBUSY;
        }
        if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
            int route_ret;

            LOG_WRN("mesh channel-9 timing unavailable for %s; refreshing channel-5 contact: ret=%d",
                    reason,
                    ret);
            mesh_store_route_waiting_tx(&aged_out);
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                return route_ret;
            }
            return -EHOSTUNREACH;
        }
        LOG_WRN("mesh channel-9 TX rejected for %s: ret=%d", reason, ret);
        return mesh_errno_from_proto(ret);
    }

    ret = mesh_relay_start_tx(&mesh_runtime,
                              &aged_out.packet,
                              aged_out.payload,
                              aged_out.payload_len,
                              now_ms,
                              &tx);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh could not start tracked TX for %s: %d", reason, ret);
        if (ret == PROTO_ERR_NOT_FOUND) {
            int route_ret;

            mesh_store_route_waiting_tx(&aged_out);
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                return route_ret;
            }
        }
        return mesh_errno_from_proto(ret);
    }
send_prepared:
    ret = mesh_send_outbound(&tx, reason);
    if (ret < 0) {
        mesh_relay_cancel_tx(&mesh_runtime);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            int route_ret;

            mesh_relay_note_delivery_failure(&mesh_runtime, tx.packet.dst_id);
            mesh_store_route_waiting_tx(&tx);
            route_ret = mesh_request_route(tx.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                return route_ret;
            }
        }
        return ret;
    }
    mesh_relay_note_tx_sent(&mesh_runtime, &tx, k_uptime_get_32());
    if (channel9_success_pending) {
        mesh_relay_note_channel9_success(&mesh_runtime,
                                         channel9_next_hop_id,
                                         channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms > now_ms ?
                                           channel9_event_start_ms - now_ms : 0u);
        }
    }
    mesh_schedule_tx_timeout();
    return 0;
}

void report_tx_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        (void)k_work_reschedule(&report_tx_work, K_MSEC(delay_ms));
    }
}

uint32_t report_tx_queue_used(void)
{
    return (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
}

static void report_tx_work_handler(struct k_work *work)
{
    struct mesh_outbound outbound;
    struct mesh_outbound dropped;
    uint32_t now_ms;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (anchor_uwb_window_active() ||
        mesh_report_anchor_survey_discovery_is_pending() ||
        mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    ret = k_msgq_peek(&report_tx_msgq, &outbound);
    if (ret != 0) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(&outbound, now_ms)) {
        report_tx_schedule(uptime_ms_until_deadline(now_ms, outbound.earliest_tx_ms));
        return;
    }

    ret = mesh_start_tracked_tx(&outbound, "queued-click-report");
    if (ret == 0) {
        (void)k_msgq_get(&report_tx_msgq, &dropped, K_NO_WAIT);
        return;
    }

    if (ret == -EHOSTUNREACH || ret == -EBUSY) {
        LOG_WRN("queued gateway-bound report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    (void)k_msgq_get(&report_tx_msgq, &dropped, K_NO_WAIT);
    LOG_WRN("queued gateway-bound report dropped after permanent TX error: ret=%d", ret);
}

int queue_anchor_report(const struct mesh_outbound *outbound)
{
    struct mesh_outbound queued;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }

    queued = *outbound;
    queued.queued_at_ms = k_uptime_get_32();

    ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
    if (ret != 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("anchor report queue full; gateway-bound report dropped");
        return -ENOSPC;
    }

    high_debug_log_event("ANCHOR_REPORT_QUEUE",
                         "msg=0x%02x dst=0x%016llx seq=%u earliest_tx_ms=%u queue_depth=%u",
                         queued.packet.msg_type,
                         (unsigned long long)queued.packet.dst_id,
                         queued.packet.seq,
                         queued.earliest_tx_ms,
                         k_msgq_num_used_get(&report_tx_msgq));
    LOG_INF("anchor queued gateway-bound report: msg=0x%02x earliest_tx_ms=%u queue_depth=%u",
            queued.packet.msg_type,
            queued.earliest_tx_ms,
            k_msgq_num_used_get(&report_tx_msgq));
    if (!anchor_uwb_window_active()) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t delay_ms = mesh_outbound_ready_for_tx(&queued, now_ms) ?
                            0u :
                            uptime_ms_until_deadline(now_ms, queued.earliest_tx_ms);

        report_tx_schedule(delay_ms);
    }
    return 0;
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result)
{
    bool forward_sent = false;

    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        struct mesh_outbound gateway_ack = result->gateway_ack;
        struct mesh_channel5_requirements requirements;
        struct mesh_event_plan plan = {0};
        uint32_t now_ms;
        int ret;

        mesh_fill_channel5_requirements(&requirements);
        now_ms = k_uptime_get_32();
        (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
        ret = mesh_relay_require_channel9_event(&mesh_runtime,
                                                gateway_ack.next_hop_id,
                                                &requirements,
                                                now_ms,
                                                &plan);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        if (ret != PROTO_OK) {
            LOG_WRN("gateway ACK falling back to channel-5 contact: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack.next_hop_id,
                    ret);
            gateway_ack.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
            if (mesh_send_outbound(&gateway_ack, "gateway-ack-channel5") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, &gateway_ack, k_uptime_get_32());
            } else {
                mesh_store_route_waiting_tx(&gateway_ack);
                (void)mesh_request_route(gateway_ack.packet.dst_id,
                                         "gateway-ack-channel9-refresh");
            }
        } else {
            gateway_ack.radio_channel = MESH_EVENT_CHANNEL;
            if (mesh_send_outbound(&gateway_ack, "gateway-ack") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, &gateway_ack, k_uptime_get_32());
                mesh_relay_note_channel9_success(&mesh_runtime,
                                                 gateway_ack.next_hop_id,
                                                 plan.start_ms);
            }
        }
    }
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        int ret;

        if (result->forward.packet.dst_id == MESH_BROADCAST_ID) {
            ret = mesh_send_outbound(&result->forward, "broadcast-forward");
        } else {
            ret = mesh_start_tracked_tx(&result->forward, "forward");
        }
        forward_sent = ret == 0;
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) && forward_sent) {
        if (mesh_send_outbound(&result->hop_ack, "hop-ack") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->hop_ack, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_send_outbound(&result->route_request, "route-request-forward");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) {
        if (mesh_send_outbound(&result->route_reply, "route-reply") == 0 &&
            mesh_id_is_unicast(result->route_reply.next_hop_id)) {
            mesh_propose_event_after_channel5_contact(result->route_reply.next_hop_id,
                                                      "route-reply-event-propose");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        struct mesh_outbound retransmit = result->retransmit;
        struct mesh_event_plan plan = {0};
        bool channel9_replanned = false;
        int ret = PROTO_OK;

        if (mesh_packet_prefers_channel9(&retransmit.packet)) {
            struct mesh_channel5_requirements requirements;
            uint32_t now_ms = k_uptime_get_32();

            mesh_fill_channel5_requirements(&requirements);
            (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
            ret = mesh_relay_require_channel9_event(&mesh_runtime,
                                                    retransmit.next_hop_id,
                                                    &requirements,
                                                    now_ms,
                                                    &plan);
            if (ret != PROTO_ERR_STALE) {
                mesh_event_note_plan_action(&mesh_event_stats, plan.action);
            }
            if (ret == PROTO_OK) {
                retransmit.radio_channel = MESH_EVENT_CHANNEL;
                channel9_replanned = true;
            } else {
                LOG_WRN("mesh retransmit deferred until channel-9 timing is refreshed: msg=0x%02x dst=0x%016llx ret=%d",
                        retransmit.packet.msg_type,
                        (unsigned long long)retransmit.packet.dst_id,
                        ret);
                mesh_store_route_waiting_tx(&retransmit);
                (void)mesh_request_route(retransmit.packet.dst_id,
                                         "retransmit-channel9-refresh");
            }
        }
        if (ret == PROTO_OK && mesh_send_outbound(&retransmit, "retransmit") == 0) {
            HIGH_DEBUG_COUNTER_INC(mesh_retry);
            mesh_relay_note_tx_sent(&mesh_runtime, &retransmit, k_uptime_get_32());
            if (channel9_replanned) {
                mesh_relay_note_channel9_success(&mesh_runtime,
                                                 retransmit.next_hop_id,
                                                 plan.start_ms);
            }
        }
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        LOG_WRN("mesh route discovery needed after delivery failure");
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY) {
        const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);

        LOG_INF("mesh reactive route ready");
        if (selected != NULL) {
            mesh_propose_event_after_channel5_contact(selected->next_hop_id,
                                                      "route-ready-event-propose");
        }
        mesh_try_route_waiting_tx();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        LOG_INF("mesh pending TX gateway acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) {
        LOG_INF("mesh pending TX hop progress acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !mesh_relay_tx_active(&mesh_runtime)) {
        report_tx_schedule(0u);
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        mesh_try_route_waiting_tx();
    }
}

static void mesh_rx_work_handler(struct k_work *work)
{
    struct mesh_rx_pending pending;
    struct mesh_relay_result result;
    int ret;

    ARG_UNUSED(work);

    while (k_msgq_get(&mesh_rx_msgq, &pending, K_NO_WAIT) == 0) {
        uint32_t now_ms = k_uptime_get_32();

        mesh_rx_pending_refresh_age(&pending, now_ms);
        ret = mesh_relay_handle_rx(&mesh_runtime,
                                   &pending.packet,
                                   pending.payload,
                                   pending.payload_len,
                                   pending.previous_hop_id,
                                   pending.link_quality,
                                   now_ms,
                                   &result);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh RX rejected: %d", ret);
            continue;
        }

        LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx actions=0x%08x status=%d",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id,
                (unsigned long long)pending.previous_hop_id,
                result.actions,
                result.status);
        if (result.status == PROTO_ERR_NOT_FOUND &&
            pending.packet.dst_id != DEVICE_ID &&
            pending.packet.dst_id != MESH_BROADCAST_ID) {
            (void)mesh_request_route(pending.packet.dst_id, "rx-forward-miss");
        }
        mesh_handle_result_actions(&result);
        if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_handle_event_control(&pending.packet,
                                      pending.payload,
                                      pending.payload_len,
                                      pending.previous_hop_id)) {
            continue;
        }
        if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            DEVICE_ROLE == ROLE_GATEWAY) {
            if (pending.packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending.packet,
                                            pending.payload,
                                            pending.payload_len);
            }
            mesh_report_gateway_handle_survey_discovery_report(&pending.packet,
                                                   pending.payload,
                                                   pending.payload_len);
            ret = gateway_emit_host_packet(&pending.packet,
                                           pending.payload,
                                           pending.payload_len);
            if (ret < 0) {
                LOG_WRN("gateway BLE COBS frame not emitted: %d", ret);
            }
        } else if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
                   DEVICE_ROLE == ROLE_ANCHOR) {
            mesh_report_anchor_handle_local_command(&pending.packet, pending.payload, pending.payload_len);
            mesh_report_anchor_handle_survey_discovery_start(&pending.packet,
                                                 pending.payload,
                                                 pending.payload_len);
            mesh_report_anchor_handle_survey_pair_prepare(&pending.packet,
                                              pending.payload,
                                              pending.payload_len);
        }
    }
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result result;
    struct mesh_outbound pending_waiting = {0};
    bool pending_route_waiting = false;
    struct mesh_outbound pending_report = {0};
    bool pending_anchor_report = false;

    ARG_UNUSED(work);

    if (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) {
        (void)k_work_reschedule(&mesh_tx_timeout_work, K_MSEC(REPORT_TX_RETRY_DELAY_MS));
        return;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR &&
        mesh_relay_tx_active(&mesh_runtime) &&
        mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
        mesh_runtime.pending.packet.src_id == DEVICE_ID) {
        pending_report.packet = mesh_runtime.pending.packet;
        pending_report.payload_len = mesh_runtime.pending.payload_len;
        if (pending_report.payload_len > 0u) {
            memcpy(pending_report.payload,
                   mesh_runtime.pending.payload,
                   pending_report.payload_len);
        }
        pending_anchor_report = true;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        pending_waiting.packet = mesh_runtime.pending.packet;
        pending_waiting.payload_len = mesh_runtime.pending.payload_len;
        pending_waiting.radio_channel = mesh_runtime.pending.radio_channel;
        pending_waiting.next_hop_id = mesh_runtime.pending.next_hop_id;
        if (pending_waiting.payload_len > 0u) {
            memcpy(pending_waiting.payload,
                   mesh_runtime.pending.payload,
                   pending_waiting.payload_len);
        }
        pending_route_waiting = mesh_tx_can_wait_for_route(&pending_waiting);
    }

    if (mesh_relay_tick_with_random(&mesh_runtime,
                                    k_uptime_get_32(),
                                    sys_rand32_get(),
                                    &result) != PROTO_OK) {
        return;
    }
    mesh_handle_result_actions(&result);
    if (mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }

    if (pending_route_waiting &&
        (result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        int route_ret;

        mesh_store_route_waiting_tx(&pending_waiting);
        route_ret = mesh_request_route(pending_waiting.packet.dst_id, "pending-tx-timeout");
        if (route_ret == -ETIMEDOUT) {
            mesh_route_waiting_tx_valid = false;
        }
    }

    if (pending_anchor_report &&
        (result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        LOG_WRN("requeueing click report after mesh route loss");
        (void)queue_anchor_report(&pending_report);
    }
}

bool mesh_queue_from_frame(const uint8_t *frame,
                           size_t frame_len,
                           uint8_t link_quality,
                           bool *valid_mesh_frame,
                           uint64_t *previous_hop_id)
{
    struct mesh_frame_parse_context context = {0};
    struct mesh_rx_pending pending = {0};
    int ret;

    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = false;
    }
    if (frame == NULL || frame_len == 0u) {
        return false;
    }
    if (uwb_mesh_frame_decode(frame,
                              frame_len,
                              NETWORK_ID,
                              DEVICE_ID,
                              &context.previous_hop_id,
                              &context.packet,
                              context.payload,
                              sizeof(context.payload),
                              &context.payload_len) != PROTO_OK ||
        context.payload_len > UINT8_MAX) {
        return false;
    }
    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = true;
    }
    if (previous_hop_id != NULL) {
        *previous_hop_id = context.previous_hop_id;
    }

    pending.packet = context.packet;
    if (context.payload_len > 0u) {
        memcpy(pending.payload, context.payload, context.payload_len);
    }
    pending.payload_len = (uint8_t)context.payload_len;
    pending.previous_hop_id = context.previous_hop_id;
    pending.link_quality = link_quality;
    pending.received_at_ms = k_uptime_get_32();

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB RX queue full; dropped msg=0x%02x src=0x%016llx dst=0x%016llx",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id);
        return false;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        uwb_anchor_note_mesh_packet(&anchor_uwb_session);
    }
    HIGH_DEBUG_COUNTER_INC(mesh_rx);
    if (pending.packet.msg_type == MSG_GATEWAY_ACK) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_RX",
                             "src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                             (unsigned long long)pending.packet.src_id,
                             (unsigned long long)pending.packet.dst_id,
                             (unsigned long long)pending.previous_hop_id,
                             pending.packet.seq,
                             pending.link_quality);
    }
    high_debug_log_event("MESH_RX",
                         "msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u queue_depth=%u",
                         pending.packet.msg_type,
                         (unsigned long long)pending.packet.src_id,
                         (unsigned long long)pending.packet.dst_id,
                         (unsigned long long)pending.previous_hop_id,
                         pending.packet.seq,
                         pending.link_quality,
                         k_msgq_num_used_get(&mesh_rx_msgq));
    LOG_INF("mesh UWB RX queued: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u queue_depth=%u role=%s",
            pending.packet.msg_type,
            (unsigned long long)pending.packet.src_id,
            (unsigned long long)pending.packet.dst_id,
            (unsigned long long)pending.previous_hop_id,
            pending.link_quality,
            k_msgq_num_used_get(&mesh_rx_msgq),
            role_name());

    (void)k_work_submit(&mesh_rx_work);
    return true;
}

static void mesh_uwb_rx_work_handler(struct k_work *work)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint64_t channel9_peer_id = 0u;
    uint64_t rx_previous_hop_id = 0u;
    struct mesh_event_plan channel9_plan = {0};
    uint32_t window_ms;
    uint32_t observed_packet_ms = 0u;
    uint8_t channel9_timing_index = 0u;
    int64_t uwb_window_start_ms = -1;
    bool channel9_event;
    int ret;

    ARG_UNUSED(work);

    if (!mesh_role_uses_uwb_rx()) {
        mesh_uwb_rx_active = false;
        return;
    }
    if ((DEVICE_ROLE == ROLE_ANCHOR &&
         (anchor_uwb_window_active() || mesh_report_anchor_survey_discovery_is_pending())) ||
        mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_uwb_rx(mesh_uwb_rx_idle_delay_ms());
        return;
    }

    channel9_event = mesh_select_channel9_rx_event(k_uptime_get_32(),
                                                   &channel9_plan,
                                                   &channel9_peer_id,
                                                   &channel9_timing_index);
    if (DEVICE_ROLE == ROLE_ANCHOR && !channel9_event) {
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }

    ret = radio_guard_uwb_start(channel9_event ? "mesh channel9 UWB RX" : "mesh UWB RX");
    if (ret < 0) {
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }

    window_ms = channel9_event ? channel9_plan.window_ms : mesh_uwb_rx_window_ms();
    uwb_window_start_ms = k_uptime_get();
    ret = channel9_event ?
          dwm3000_driver_configure_mesh_payload_mode() :
          dwm3000_driver_configure_default();
    if (channel9_event) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    }
    if (ret == 0) {
        ret = dwm3000_driver_receive_frame(window_ms,
                                           frame,
                                           sizeof(frame),
                                           &frame_len,
                                           &quality,
                                           NULL);
        if (ret == 0) {
            observed_packet_ms = k_uptime_get_32();
        }
    }
    (void)dwm3000_driver_standby();
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();

    if (ret == 0) {
        bool valid_mesh_frame = false;

        if (mesh_queue_from_frame(frame,
                                  frame_len,
                                  quality,
                                  &valid_mesh_frame,
                                  &rx_previous_hop_id)) {
            if (channel9_event && rx_previous_hop_id == channel9_peer_id) {
                mesh_relay_note_channel9_rx(&mesh_runtime,
                                            rx_previous_hop_id,
                                            channel9_plan.start_ms,
                                            observed_packet_ms);
            }
            LOG_DBG("mesh UWB RX frame accepted: len=%u", (unsigned int)frame_len);
        } else if (!valid_mesh_frame) {
            LOG_DBG("mesh UWB RX ignored non-mesh frame: len=%u quality=%u",
                    (unsigned int)frame_len,
                    quality);
        }
    } else if (ret != -ETIMEDOUT) {
        LOG_WRN("mesh UWB RX failed: ret=%d role=%s", ret, role_name());
    } else if (channel9_event && channel9_timing_index < MESH_RELAY_EVENT_TIMINGS &&
               mesh_runtime.event_timings[channel9_timing_index].valid) {
        mesh_relay_note_channel9_missed(&mesh_runtime,
                                        channel9_peer_id,
                                        &mesh_event_stats);
    }

    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
}

int mesh_start_uwb_rx(const char *reason)
{
    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }

    mesh_schedule_uwb_rx(0u);
    LOG_INF("mesh UWB RX scheduled: role=%s window_ms=%u idle_ms=%u reason=%s",
            role_name(),
            mesh_uwb_rx_window_ms(),
            mesh_uwb_rx_idle_delay_ms(),
            reason == NULL ? "start" : reason);
    return 0;
}

static int build_range_report_samples(uint64_t clicker_id,
                                       uint32_t event_seq,
                                       uint32_t burst_id,
                                       const struct dwm3000_range_result *range_result,
                                      const int32_t *distance_samples_mm,
                                      const uint8_t *range_round_indices,
                                      const int64_t *sample_sequence_start_ms,
                                      uint16_t sample_count)
{
    struct range_report_fields fields;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t encoded[PACKET_MAX_LEN];
    uint16_t sample_index = 0u;
    uint16_t packet_index = 0u;
    bool fragmented;
    int ret;

    if (range_result == NULL ||
        clicker_id == 0u ||
        event_seq == 0u ||
        range_result->responder_id == 0u ||
        (sample_count > 0u &&
         (distance_samples_mm == NULL ||
          range_round_indices == NULL ||
          sample_sequence_start_ms == NULL)) ||
        sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return -EINVAL;
    }
    fragmented = sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;

    do {
        struct range_report_diagnostics diagnostics;
        size_t payload_len = 0u;
        size_t encoded_len = 0u;
        uint16_t chunk_count = 0u;
        uint16_t chunk_cap = 0u;
        uint16_t packet_seq;
        uint64_t sequence_start_timestamps_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET] = {0};
        int64_t range_local_ms;

        if (sample_count > 0u) {
            chunk_cap = fragmented ?
                        RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT :
                        RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;
            chunk_count = MIN(chunk_cap, sample_count - sample_index);
        }

build_payload:
        payload_len = 0u;
        encoded_len = 0u;
        memset(&fields, 0, sizeof(fields));
        if (chunk_count > 0u) {
            range_local_ms = sample_sequence_start_ms[sample_index];
            if (range_local_ms < 0) {
                range_local_ms = k_uptime_get();
            }
            for (uint16_t i = 0u; i < chunk_count; i++) {
                int64_t sample_local_ms = sample_sequence_start_ms[sample_index + i];

                if (sample_local_ms < 0) {
                    sample_local_ms = k_uptime_get();
                }
                anchor_sequence_timestamp_at(sample_local_ms,
                                             &sequence_start_timestamps_ms[i]);
            }
            fields.timestamp_ms = sequence_start_timestamps_ms[0];
        } else {
            range_local_ms = range_result->exchange_started ?
                             range_result->exchange_start_ms :
                             k_uptime_get();
            anchor_sequence_timestamp_at(range_local_ms,
                                         &fields.timestamp_ms);
        }

        fields.clicker_id = clicker_id;
        fields.anchor_id = range_result->responder_id;
        fields.event_seq = event_seq;
        fields.distance_mm = range_result->distance_mm;
        fields.quality = range_result->quality;
        fields.rsl_dbm = range_result->rsl_dbm;
        fields.cir_sample = range_result->cir_sampled ? range_result->cir_sample : NULL;
        fields.range_status = range_result->status;
        fields.distance_samples_mm = chunk_count > 0u ? &distance_samples_mm[sample_index] : NULL;
        fields.range_round_indices = chunk_count > 0u ?
                                     &range_round_indices[sample_index] :
                                     NULL;
        fields.sequence_start_timestamps_ms = chunk_count > 0u ?
                                              sequence_start_timestamps_ms :
                                              NULL;
        fields.sample_index = sample_index;
        fields.sample_count = sample_count;
        fields.distance_sample_count = chunk_count;
        fields.omit_rsl = packet_index != 0u;
        fields.omit_cir = packet_index != 0u;
        fragmented = sample_count > chunk_count || packet_index != 0u;
        if (packet_index == 0u) {
            uint32_t anchor_diag_len = range_result->cir_sampled ?
                                       UWB_CIR_SAMPLE_LEN : 0u;
            uint32_t clicker_diag_len = range_result->clicker_diag_received ?
                                        range_result->clicker_diag_len : 0u;
            uint32_t clicker_diag_copy_len = clicker_diag_len > 15u ?
                                             clicker_diag_len - 15u : 0u;
            uint32_t clicker_diag_raw_len =
                (range_result->clicker_diag_received && clicker_diag_len >= 15u) ?
                range_result->clicker_diag[14] : clicker_diag_copy_len;

            memset(&diagnostics, 0, sizeof(diagnostics));
            diagnostics.status_flags = range_result->cir_sampled ?
                                       RANGE_DIAG_ANCHOR_PRESENT :
                                       RANGE_DIAG_ANCHOR_MISSING;
            diagnostics.status_flags |= range_result->clicker_diag_received ?
                                        RANGE_DIAG_CLICKER_PRESENT :
                                        RANGE_DIAG_CLICKER_MISSING;
            if (range_result->clicker_diag_truncated) {
                diagnostics.status_flags |= RANGE_DIAG_TRUNCATED;
            }
            if (range_result->clicker_diag_dropped) {
                diagnostics.status_flags |= RANGE_DIAG_CAPTURE_FAILED;
            }
            diagnostics.burst_id = burst_id;
            diagnostics.exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US;
            diagnostics.burst_duration_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS;
            diagnostics.uwb_awake_time_us = anchor_uwb_session.diagnostics.awake_time_us;
            diagnostics.diag_bytes_captured = anchor_diag_len + clicker_diag_raw_len;
            diagnostics.diag_bytes_transmitted = anchor_diag_len + clicker_diag_copy_len;
            diagnostics.diag_bytes_truncated = clicker_diag_raw_len > clicker_diag_copy_len ?
                                               clicker_diag_raw_len - clicker_diag_copy_len :
                                               0u;
            diagnostics.diag_frames_dropped = range_result->clicker_diag_dropped ?
                                              1u : 0u;
            diagnostics.report_fragment_count = fragmented ?
                                                (uint16_t)(1u +
                                                           ((sample_count - chunk_count +
                                                             RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT -
                                                             1u) /
                                                            RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT)) :
                                                1u;
            diagnostics.phy_config_id = UWB_CHANNEL_WAKE_CONTACT;
            diagnostics.clock_offset_raw = range_result->clock_offset_raw;
            diagnostics.clock_offset_present = range_result->clock_offset_sampled;
            diagnostics.carrier_integrator = range_result->carrier_integrator;
            diagnostics.carrier_integrator_present =
                range_result->carrier_integrator_sampled;
            diagnostics.clicker_diag = range_result->clicker_diag_received ?
                                       range_result->clicker_diag : NULL;
            diagnostics.clicker_diag_len = range_result->clicker_diag_received ?
                                           range_result->clicker_diag_len : 0u;
            diagnostics.anchor_diag = range_result->cir_sampled ?
                                      range_result->cir_sample : NULL;
            diagnostics.anchor_diag_len = range_result->cir_sampled ?
                                          UWB_CIR_SAMPLE_LEN : 0u;
            fields.diagnostics = &diagnostics;
        }

        ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
        if (ret == PROTO_ERR_NO_SPACE && chunk_count > 1u) {
            chunk_count--;
            goto build_payload;
        }
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        packet_seq = (uint16_t)((range_result->seq == 0u ?
                                 (uint16_t)event_seq :
                                 range_result->seq) + packet_index);
        ret = report_init_range_packet(&packet,
                                       range_result->responder_id,
                                       GATEWAY_ID,
                                       event_seq,
                                       packet_seq,
                                       range_result->flags,
                                       (uint8_t)payload_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        ret = proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        LOG_INF("range report ready: clicker=0x%016llx event_seq=%u anchor=0x%016llx distance_mm=%d samples=%u chunk_index=%u chunk_samples=%u first_round=%u quality=%u diagnostic=%u rsl_included=%u packet_len=%u",
                (unsigned long long)clicker_id,
                event_seq,
                (unsigned long long)range_result->responder_id,
                range_result->distance_mm,
                sample_count,
                sample_index,
                chunk_count,
                chunk_count > 0u ? range_round_indices[sample_index] : 0u,
                range_result->quality,
                (range_result->flags & FLAG_DIAGNOSTIC) != 0u ? 1u : 0u,
                packet_index == 0u ? 1u : 0u,
                (unsigned int)encoded_len);

        if (DEVICE_ROLE == ROLE_ANCHOR) {
            struct mesh_outbound outbound = {
                .packet = packet,
                .payload_len = (uint8_t)payload_len,
            };

            memcpy(outbound.payload, payload, payload_len);
            ret = queue_anchor_report(&outbound);
            if (ret < 0) {
                LOG_WRN("click report could not be queued for mesh TX: %d", ret);
                return ret;
            }
        }

        sample_index += chunk_count;
        packet_index++;
    } while (sample_index < sample_count);

    return 0;
}

void build_uwb_schedule_report_if_relevant(
    const struct uwb_anchor_session *session,
    uint8_t schedule_flags,
    const struct anchor_range_window_report *report)
{
    int ret;

    if ((schedule_flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) == 0u) {
        return;
    }
    if (session == NULL || report == NULL || !report->have_result) {
        return;
    }

    ret = build_range_report_samples(session->epoch.clicker_id,
                                     session->epoch.click_event_id,
                                     uwb_schedule_burst_id(session->epoch.click_event_id,
                                                           session->epoch.attempt_index),
                                     &report->result,
                                     report->distance_samples_mm,
                                     report->range_round_indices,
                                     report->sample_sequence_start_ms,
                                     report->sample_count);
    if (ret < 0) {
        LOG_WRN("failed to build UWB scheduled anchor range report: %d", ret);
    }
}

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks)
{
    mesh_report_callbacks = callbacks;
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    k_work_init_delayable(&mesh_uwb_rx_work, mesh_uwb_rx_work_handler);
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
    return 0;
}
