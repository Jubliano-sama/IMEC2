/*
 * Compile the production report owner as the test translation unit. This is
 * intentionally not a model or copied queue policy: the assertions below call
 * the exact static reservation, fragment, and immutable-head helpers that the
 * anchor image links from app_mesh_report.c.
 */
#include "app_mesh_report.c"
#undef LOG_MODULE_DECLARE
#define LOG_MODULE_DECLARE(...)
#include "app_anchor_survey_result_delivery.c"
#undef LOG_MODULE_DECLARE
#define LOG_MODULE_DECLARE(...)
#include "app_anchor_survey_discovery.c"
#undef LOG_MODULE_DECLARE

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

static uint32_t watchdog_stop_calls;
static uint64_t active_survey_generation_for_test;
static bool report_owner_work_initialized;
static bool report_owner_route_work_started;
static bool event_accept_rx_work_initialized;
static bool range_abort_queue_fault_armed;
static enum mesh_range_report_batch_abort_queue_operation
    range_abort_queue_fault_operation;

void app_anchor_note_channel9_window_released(void)
{
}
static uint32_t range_abort_queue_fault_index;
static bool range_fragment_put_fault_armed;
static uint8_t range_fragment_put_fault_index;
static struct mesh_outbound range_successor_fragment;
static uint64_t range_successor_clicker_id;
static uint64_t range_successor_nonce;
static uint64_t range_successor_generation;
static uint32_t range_successor_event_seq;
static uint8_t range_successor_attempt_index;

struct relay_owner_snapshot {
    struct mesh_pending_tx pending;
    struct mesh_outbox_record outbox_record;
    k_ticks_t timeout_expires;
    bool timeout_pending;
};

K_THREAD_STACK_DEFINE(preempt_lock_holder_stack, 1024);
static struct k_thread preempt_lock_holder_thread;
K_SEM_DEFINE(preempt_lock_holder_acquired, 0, 1);
K_SEM_DEFINE(preempt_lock_holder_release, 0, 1);
K_SEM_DEFINE(preempt_lock_holder_done, 0, 1);

K_THREAD_STACK_DEFINE(preempt_request_caller_stack, 1024);
static struct k_thread preempt_request_caller_thread;
static struct k_work route_preempt_blocker_work;
static struct k_work route_preempt_successor_work;
static struct k_work route_owned_preempt_work;
K_SEM_DEFINE(route_preempt_blocker_entered, 0, 1);
K_SEM_DEFINE(route_preempt_blocker_release, 0, 1);
K_SEM_DEFINE(route_preempt_blocker_done, 0, 1);
K_SEM_DEFINE(route_preempt_successor_done, 0, 1);
K_SEM_DEFINE(route_owned_preempt_done, 0, 1);
K_SEM_DEFINE(preempt_request_caller_done, 0, 1);
static int route_preempt_successor_result;
static int route_owned_preempt_result;
static int preempt_request_caller_result;
static uint32_t preempt_request_caller_deadline_ms;
static uint32_t survey_result_schedule_calls;
static uint16_t survey_discovery_sequence_for_test;
static bool tracked_mesh_tx_capture_enabled;
static uint32_t tracked_mesh_tx_capture_calls;
static uint32_t tracked_mesh_tx_capture_at_ms;
static uint8_t tracked_mesh_tx_capture_frame[UWB_MESH_MAX_FRAME_LEN];
static size_t tracked_mesh_tx_capture_frame_len;
static bool report_custody_rx_injection_armed;
static uint8_t report_custody_rx_injection_frame[UWB_MESH_MAX_FRAME_LEN];
static size_t report_custody_rx_injection_frame_len;
static uint32_t report_custody_rx_attempts;
K_SEM_DEFINE(tracked_mesh_tx_capture_sem, 0, 1);

static int survey_result_schedule_for_test(uint32_t delay_ms)
{
    ARG_UNUSED(delay_ms);
    survey_result_schedule_calls++;
    return 0;
}

static int survey_discovery_boot_for_test(uint32_t *incarnation)
{
    zassert_not_null(incarnation);
    *incarnation = UINT32_C(0x41000001);
    return 0;
}

static uint16_t survey_discovery_sequence_next_for_test(void)
{
    survey_discovery_sequence_for_test++;
    if (survey_discovery_sequence_for_test == 0u) {
        survey_discovery_sequence_for_test++;
    }
    return survey_discovery_sequence_for_test;
}

static int survey_result_active_owner_for_test(
    const struct mesh_outbound *outbound)
{
    ARG_UNUSED(outbound);
    return 0;
}

static void survey_result_wake_owner_for_test(const char *reason)
{
    ARG_UNUSED(reason);
}

/* Production dependencies outside this deliberately narrow composed link. */
void app_watchdog_stop_feeding(void)
{
    watchdog_stop_calls++;
}

bool app_anchor_survey_runtime_operation_generation_active(
    uint64_t operation_generation)
{
    return operation_generation != 0u &&
           operation_generation == active_survey_generation_for_test;
}

static const struct app_mesh_report_callbacks survey_callbacks_for_test = {
    .anchor_survey_operation_generation_active =
        app_anchor_survey_runtime_operation_generation_active,
};

void status_debug_note(const char *text)
{
    ARG_UNUSED(text);
}

void status_debug_printf(const char *fmt, ...)
{
    ARG_UNUSED(fmt);
}

void app_mesh_direct_probe_breadcrumb_note(
    enum app_mesh_direct_probe_phase phase,
    uint8_t attempt,
    uint16_t sequence)
{
    ARG_UNUSED(phase);
    ARG_UNUSED(attempt);
    ARG_UNUSED(sequence);
}

/* This seam composes the real report owner and relay/queue dependencies. The
 * DWM3000 and board LEDs remain external effects, so they are inert here; no
 * test reaches an RF effect because every handoff is observed before schedule. */
void app_watchdog_note_radio_progress(void)
{
}

void dwm3000_driver_request_receive_abort(uint32_t owner_mask)
{
    ARG_UNUSED(owner_mask);
}

bool dwm3000_driver_receive_abort_pending(void)
{
    return false;
}

void gateway_command_result_validation_release_reserved(uint32_t token)
{
    ARG_UNUSED(token);
}

/* The composed anchor seam has no gateway host-recovery owner.  Keep this
 * production dependency explicit so a gateway-only backlog exception cannot
 * accidentally affect the click-preemption cases. */
bool gateway_collection_recovery_flood_owns_rx_backlog(void)
{
    return false;
}

void status_debug_uwb_rx_channel_pulse(uint8_t uwb_channel)
{
    ARG_UNUSED(uwb_channel);
}

void status_debug_gateway_uwb_rx_channel_pulse(uint8_t uwb_channel)
{
    ARG_UNUSED(uwb_channel);
}

void status_debug_uwb_tx_channel_pulse(uint8_t uwb_channel)
{
    ARG_UNUSED(uwb_channel);
}

void status_debug_tx_wake_claim_sent_pulse(void)
{
}

void status_debug_tx_mesh_frame_sent_pulse(void)
{
    if (tracked_mesh_tx_capture_enabled) {
        k_sem_give(&tracked_mesh_tx_capture_sem);
    }
}

void status_debug_tx_gateway_ack_rx_pulse(void)
{
}

void status_debug_gateway_ack_tx_pulse(void)
{
}

int dwm3000_driver_configure_mesh_payload_mode(void)
{
    return 0;
}

int dwm3000_driver_configure_wake_mesh_control_mode(void)
{
    return 0;
}

int dwm3000_driver_configure_wake_mode(void)
{
    return 0;
}

int dwm3000_driver_ensure_wake_mode(void)
{
    return 0;
}

int dwm3000_driver_idle(void)
{
    return 0;
}

int dwm3000_driver_standby(void)
{
    return 0;
}

int dwm3000_driver_force_recovery(void)
{
    return 0;
}

int dwm3000_driver_send_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t timeout_ms)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
}

int dwm3000_driver_send_frame_tracked(const uint8_t *frame,
                                      size_t frame_len,
                                      uint32_t timeout_ms,
                                      bool *rf_started)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(timeout_ms);
    if (rf_started != NULL) {
        *rf_started = false;
    }
    return -ENOTSUP;
}

int dwm3000_driver_send_frame_tracked_until(
    const uint8_t *frame,
    size_t frame_len,
    uint32_t timeout_ms,
    uint64_t absolute_deadline_ms,
    struct dwm3000_tx_observation *observation)
{
    ARG_UNUSED(timeout_ms);
    ARG_UNUSED(absolute_deadline_ms);
    if (tracked_mesh_tx_capture_enabled) {
        zassert_true(frame_len <= sizeof(tracked_mesh_tx_capture_frame));
        memcpy(tracked_mesh_tx_capture_frame, frame, frame_len);
        tracked_mesh_tx_capture_frame_len = frame_len;
        tracked_mesh_tx_capture_at_ms = k_uptime_get_32();
        tracked_mesh_tx_capture_calls++;
        if (observation != NULL) {
            observation->rf_started = true;
            observation->rf_started_at_ms = tracked_mesh_tx_capture_at_ms;
            observation->tx_completed = true;
            observation->tx_completed_at_ms = tracked_mesh_tx_capture_at_ms;
        }
        return 0;
    }
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    return -ENOTSUP;
}

static int report_custody_receive_timeout(uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure)
{
    report_custody_rx_attempts++;
    if (report_custody_rx_injection_armed) {
        zassert_not_null(frame);
        zassert_true(report_custody_rx_injection_frame_len <= frame_cap);
        memcpy(frame,
               report_custody_rx_injection_frame,
               report_custody_rx_injection_frame_len);
        if (frame_len != NULL) {
            *frame_len = report_custody_rx_injection_frame_len;
        }
        if (quality != NULL) {
            *quality = 93u;
        }
        if (rsl_dbm != NULL) {
            *rsl_dbm = -71;
        }
        if (failure != NULL) {
            *failure = DWM3000_RX_FAILURE_NONE;
        }
        report_custody_rx_injection_armed = false;
        return 0;
    }
    if (frame_len != NULL) {
        *frame_len = 0u;
    }
    if (quality != NULL) {
        *quality = 0u;
    }
    if (rsl_dbm != NULL) {
        *rsl_dbm = 0;
    }
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_FRAME_TIMEOUT;
    }
    return -ETIMEDOUT;
}

int dwm3000_driver_receive_frame(uint32_t timeout_ms,
                                 uint8_t *frame,
                                 size_t frame_cap,
                                 size_t *frame_len,
                                 uint8_t *quality,
                                 int8_t *rsl_dbm)
{
    ARG_UNUSED(timeout_ms);
    return report_custody_receive_timeout(frame, frame_cap, frame_len,
                                          quality, rsl_dbm, NULL);
}

int dwm3000_driver_receive_frame_detailed(uint32_t timeout_ms,
                                          uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure)
{
    ARG_UNUSED(timeout_ms);
    return report_custody_receive_timeout(frame, frame_cap, frame_len,
                                          quality, rsl_dbm, failure);
}

int dwm3000_driver_receive_frame_detailed_quiet(
    uint32_t timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    return dwm3000_driver_receive_frame_detailed(timeout_ms, frame, frame_cap,
                                                 frame_len, quality, rsl_dbm,
                                                 failure);
}

int dwm3000_driver_sniff_activity(uint32_t timeout_ms,
                                  enum dwm3000_rx_failure *failure)
{
    ARG_UNUSED(timeout_ms);
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    }
    return -ETIMEDOUT;
}

int dwm3000_driver_receive_frame_continuous(
    uint32_t timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    return dwm3000_driver_receive_frame_detailed(timeout_ms, frame, frame_cap,
                                                 frame_len, quality, rsl_dbm,
                                                 failure);
}

int dwm3000_driver_receive_frame_continuous_extend_on_activity(
    uint32_t acquire_timeout_ms,
    uint32_t completion_timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    ARG_UNUSED(acquire_timeout_ms);
    return dwm3000_driver_receive_frame_detailed(completion_timeout_ms,
                                                 frame, frame_cap, frame_len,
                                                 quality, rsl_dbm, failure);
}

int dwm3000_driver_receive_frame_continuous_timed(
    uint32_t timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure,
    struct dwm3000_rx_frame_timing *timing)
{
    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    return dwm3000_driver_receive_frame_detailed(timeout_ms, frame, frame_cap,
                                                 frame_len, quality, rsl_dbm,
                                                 failure);
}

void dwm3000_driver_last_rx_debug_get(struct dwm3000_rx_debug_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

int dwm3000_driver_last_rx_host_uptime(uint32_t *received_at_ms)
{
    ARG_UNUSED(received_at_ms);
    return -ENODATA;
}

static void report_owner_reset(void)
{
    struct mesh_outbound discarded;

    while (k_msgq_get(&report_tx_msgq, &discarded, K_NO_WAIT) == 0) {
    }
    memset(&anchor_range_report_batch_reservation, 0,
           sizeof(anchor_range_report_batch_reservation));
    memset(&anchor_range_report_ack_runtime, 0,
           sizeof(anchor_range_report_ack_runtime));
    memset(&report_tx_queue_head_owner, 0,
           sizeof(report_tx_queue_head_owner));
    memset(&report_tx_queue_overflow_dropped, 0,
           sizeof(report_tx_queue_overflow_dropped));
    memset(&report_tx_queue_rotation_scratch, 0,
           sizeof(report_tx_queue_rotation_scratch));
    memset(&mesh_send_scratch_tx, 0, sizeof(mesh_send_scratch_tx));
    memset(&mesh_ch9_tx_batch_storage, 0, sizeof(mesh_ch9_tx_batch_storage));
    memset(&mesh_ch9_tx_pending, 0, sizeof(mesh_ch9_tx_pending));
    report_tx_queue_recovery_valid = false;
    report_tx_backoff_until_ms = 0u;
    report_tx_backoff_active = false;
    mesh_preempt_test_before_cancel_hook = NULL;
    mesh_range_report_batch_abort_test_queue_fault = NULL;
    mesh_range_report_fragment_test_put_fault = NULL;
    mesh_range_report_test_after_lifecycle_release_hook = NULL;
    range_abort_queue_fault_armed = false;
    range_fragment_put_fault_armed = false;
    app_anchor_click_event_runtime_reset();
    atomic_set(&mesh_transport_paused_state, 0);
    fw_delivery_loss_init(&mesh_delivery_loss);
    app_mesh_queue_head_owner_init(&report_tx_queue_head_owner);
    if (report_owner_work_initialized) {
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        (void)k_work_cancel_delayable(&report_tx_work);
    }
    if (!report_owner_route_work_started) {
        k_work_queue_start(&mesh_route_work_q,
                           mesh_route_work_q_stack,
                           K_THREAD_STACK_SIZEOF(mesh_route_work_q_stack),
                           MESH_ROUTE_WORKQUEUE_PRIORITY,
                           &mesh_route_work_q_config);
        report_owner_route_work_started = true;
    }
    mesh_click_preempt_request_init();
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
    report_owner_work_initialized = true;
    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_ANCHOR,
                    DEVICE_ID,
                    GATEWAY_ID,
                    7u);
    zassert_equal(mesh_relay_note_direct_gateway_route(&mesh_runtime,
                                                        k_uptime_get_32()),
                  PROTO_OK,
                  "direct gateway route setup failed");
    tracked_mesh_tx_capture_enabled = false;
    tracked_mesh_tx_capture_calls = 0u;
    tracked_mesh_tx_capture_at_ms = 0u;
    tracked_mesh_tx_capture_frame_len = 0u;
    report_custody_rx_injection_armed = false;
    report_custody_rx_injection_frame_len = 0u;
    report_custody_rx_attempts = 0u;
    while (k_sem_take(&tracked_mesh_tx_capture_sem, K_NO_WAIT) == 0) {
    }
    watchdog_stop_calls = 0u;
    anchor_uwb_busy = false;
    anchor_click_window_busy = false;
}

static struct mesh_event_timing event_accept_timing_for_test(
    uint32_t first_event_time_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = MESH_EVENT_DEFAULT_INTERVAL_MS,
        .event_window_ms = MESH_EVENT_DEFAULT_WINDOW_MS,
        .first_event_time_ms = first_event_time_ms,
        .guard_ms = MESH_EVENT_DEFAULT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = MESH_EVENT_DEFAULT_MAX_MISSED,
        .supervision_timeout_ms = MESH_EVENT_DEFAULT_SUPERVISION_MS,
    };
    struct mesh_event_timing timing = {0};

    zassert_equal(mesh_event_timing_negotiate(&timing, &params, true),
                  PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&timing, false);
    return timing;
}

static void event_proposal_for_test(
    uint64_t peer_id,
    struct mesh_event_control_record *proposal,
    uint32_t *received_at_ms)
{
    const uint32_t session_id = UINT32_C(0x6f000101);
    uint32_t encode_at_ms;
    size_t payload_len = 0u;

    zassert_not_null(proposal);
    zassert_not_null(received_at_ms);
    memset(proposal, 0, sizeof(*proposal));
    encode_at_ms = k_uptime_get_32();
    proposal->timing = event_accept_timing_for_test(
        encode_at_ms + MESH_EVENT_DEFAULT_FIRST_DELAY_MS);
    zassert_true(mesh_event_timing_bind_proposal_session(
        &proposal->timing, session_id));
    zassert_equal(mesh_append_event_timing_tlvs_at(
                      proposal->payload,
                      sizeof(proposal->payload),
                      &payload_len,
                      &proposal->timing,
                      encode_at_ms),
                  PROTO_OK);
    zassert_equal(tlv_append_u64(proposal->payload,
                                 sizeof(proposal->payload),
                                 &payload_len,
                                 TLV_MESH_EVENT_BOOT_NONCE,
                                 UINT64_C(0x8d00000000000101)),
                  PROTO_OK);
    zassert_true(payload_len <= UINT8_MAX);
    zassert_equal(mesh_init_event_control(&proposal->packet,
                                          MSG_MESH_EVENT_PROPOSE,
                                          peer_id,
                                          DEVICE_ID,
                                          session_id,
                                          UINT16_C(0x612),
                                          (uint8_t)payload_len),
                  PROTO_OK);
    proposal->peer_id = DEVICE_ID;
    proposal->payload_len = (uint8_t)payload_len;
    proposal->valid = true;
    *received_at_ms = encode_at_ms + MESH_EVENT_CONTROL_CH5_AIRTIME_MS;
}

static void local_event_proposal_for_test(
    uint64_t peer_id,
    const struct mesh_event_timing *proposed)
{
    const uint32_t session_id = UINT32_C(0x6f000201);
    const uint16_t sequence = UINT16_C(0x613);
    struct app_mesh_event_request_identity request;
    struct app_mesh_rf_retry_key retry_key;
    uint32_t now_ms = k_uptime_get_32();

    zassert_not_null(proposed);
    zassert_ok(mesh_prepare_event_control_record(
        &mesh_event_propose_record,
        peer_id,
        MSG_MESH_EVENT_PROPOSE,
        proposed,
        session_id,
        sequence,
        0u));
    mesh_event_propose_record.transmit_phase_frozen = true;
    request = mesh_event_request_identity(
        &mesh_event_propose_record.packet,
        mesh_event_propose_record.payload,
        mesh_event_propose_record.payload_len);
    retry_key = mesh_rf_retry_packet_key(
        &mesh_event_propose_record.packet,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    zassert_ok(app_mesh_event_retry_begin(
        &mesh_event_propose_retry,
        peer_id,
        &request,
        &retry_key,
        now_ms,
        now_ms + 60000u,
        mesh_event_propose_record.timing.event_interval_ms,
        MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS / 2u));
}

static void event_accept_for_local_proposal(
    uint64_t peer_id,
    uint16_t phase_shift_ms,
    struct mesh_event_control_record *accept,
    uint32_t *received_at_ms)
{
    struct mesh_event_timing accepted;
    uint32_t encode_at_ms;
    size_t payload_len = 0u;

    zassert_not_null(accept);
    zassert_not_null(received_at_ms);
    zassert_true(mesh_event_propose_record.valid);
    memset(accept, 0, sizeof(*accept));
    accepted = mesh_event_propose_record.timing;
    zassert_true(app_mesh_event_timing_apply_phase_shift(
        &accepted, phase_shift_ms));
    mesh_event_timing_set_local_first_slot_tx(&accepted, false);
    encode_at_ms = k_uptime_get_32();
    zassert_equal(mesh_append_event_timing_tlvs_at(
                      accept->payload,
                      sizeof(accept->payload),
                      &payload_len,
                      &accepted,
                      encode_at_ms),
                  PROTO_OK);
    if (phase_shift_ms != 0u) {
        zassert_equal(tlv_append_u16(accept->payload,
                                     sizeof(accept->payload),
                                     &payload_len,
                                     TLV_MESH_EVENT_PHASE_SHIFT_MS,
                                     phase_shift_ms),
                      PROTO_OK);
    }
    zassert_true(payload_len <= UINT8_MAX);
    zassert_equal(mesh_init_event_control(
                      &accept->packet,
                      MSG_MESH_EVENT_ACCEPT,
                      peer_id,
                      DEVICE_ID,
                      mesh_event_propose_record.packet.session_id,
                      mesh_event_propose_record.packet.seq,
                      (uint8_t)payload_len),
                  PROTO_OK);
    accept->timing = accepted;
    accept->peer_id = DEVICE_ID;
    accept->payload_len = (uint8_t)payload_len;
    accept->valid = true;
    *received_at_ms = encode_at_ms + MESH_EVENT_CONTROL_CH5_AIRTIME_MS;
}

static struct mesh_outbound command_result_route_wait_for_test(
    uint64_t source_id)
{
    struct mesh_outbound outbound = {
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = GATEWAY_ID,
    };
    size_t payload_len = 0u;

    zassert_equal(mesh_append_command_result(outbound.payload,
                                              sizeof(outbound.payload),
                                              &payload_len,
                                              CMD_SURVEY_REACHABILITY,
                                              COMMAND_OK,
                                              0u),
                  PROTO_OK);
    zassert_true(payload_len <= UINT8_MAX);
    zassert_equal(mesh_init_command_result(&outbound.packet,
                                            source_id,
                                            GATEWAY_ID,
                                            UINT32_C(0x6f000201),
                                            UINT16_C(0x621),
                                            (uint8_t)payload_len,
                                            false),
                  PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    return outbound;
}

static struct mesh_outbound same_peer_gateway_ack_route_wait_for_test(
    uint64_t peer_id)
{
    const struct mesh_outbound acknowledged =
        command_result_route_wait_for_test(peer_id);
    struct mesh_outbound outbound = {
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = peer_id,
    };
    size_t payload_len = 0u;

    zassert_equal(mesh_append_requested_seq(outbound.payload,
                                             sizeof(outbound.payload),
                                             &payload_len,
                                             acknowledged.packet.seq),
                  PROTO_OK);
    zassert_equal(mesh_append_ack_semantic_identity(
                      outbound.payload,
                      sizeof(outbound.payload),
                      &payload_len,
                      &acknowledged.packet,
                      acknowledged.payload,
                      acknowledged.payload_len),
                  PROTO_OK);
    zassert_true(payload_len <= UINT8_MAX);
    zassert_equal(mesh_init_gateway_ack(&outbound.packet,
                                        GATEWAY_ID,
                                        peer_id,
                                        acknowledged.packet.session_id,
                                        UINT16_C(0x622),
                                        (uint8_t)payload_len),
                  PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    return outbound;
}

static struct mesh_outbound gateway_ack_for_test(
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len,
    uint16_t ack_sequence)
{
    struct mesh_outbound outbound = {
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = DEVICE_ID,
    };
    size_t payload_len = 0u;

    zassert_not_null(acknowledged_packet);
    zassert_equal(mesh_append_requested_seq(outbound.payload,
                                             sizeof(outbound.payload),
                                             &payload_len,
                                             acknowledged_packet->seq),
                  PROTO_OK);
    zassert_equal(mesh_append_ack_semantic_identity(
                      outbound.payload,
                      sizeof(outbound.payload),
                      &payload_len,
                      acknowledged_packet,
                      acknowledged_payload,
                      acknowledged_payload_len),
                  PROTO_OK);
    zassert_true(payload_len <= UINT8_MAX);
    zassert_equal(mesh_init_gateway_ack(&outbound.packet,
                                        GATEWAY_ID,
                                        DEVICE_ID,
                                        acknowledged_packet->session_id,
                                        ack_sequence,
                                        (uint8_t)payload_len),
                  PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    return outbound;
}

static void inject_mesh_rx_frame_for_test(const struct mesh_outbound *outbound,
                                          uint64_t previous_hop_id)
{
    zassert_not_null(outbound);
    zassert_false(report_custody_rx_injection_armed);
    zassert_equal(uwb_mesh_frame_encode(
                      NETWORK_ID,
                      previous_hop_id,
                      DEVICE_ID,
                      &outbound->packet,
                      outbound->payload,
                      report_custody_rx_injection_frame,
                      sizeof(report_custody_rx_injection_frame),
                      &report_custody_rx_injection_frame_len),
                  PROTO_OK);
    report_custody_rx_injection_armed = true;
}

static void event_accept_rx_work_for_test(struct k_work *work)
{
    ARG_UNUSED(work);
}

static void event_accept_finish_reset(void)
{
    mesh_event_accept_clear();
    mesh_event_propose_clear();
    mesh_event_accept_rx_clear();
    mesh_event_owner_registry_reset(&mesh_event_owner_registry);
    memset(mesh_event_accept_completed, 0,
           sizeof(mesh_event_accept_completed));
    mesh_event_accept_completed_cursor = 0u;
    memset(&mesh_event_local_proposal_windows, 0,
           sizeof(mesh_event_local_proposal_windows));
    mesh_c5_contact_clear("event-accept-test-reset");
    atomic_set(&mesh_rx_response_active_state, 0);
    mesh_uwb_rx_active = false;
    if (event_accept_rx_work_initialized) {
        (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    } else {
        k_work_init_delayable(&mesh_uwb_rx_work,
                              event_accept_rx_work_for_test);
        event_accept_rx_work_initialized = true;
    }
}

static void event_accept_finish_prepare(
    uint64_t peer_id,
    const struct mesh_event_timing *pre_send_timing)
{
    uint32_t now_ms = k_uptime_get_32();

    zassert_not_null(pre_send_timing);
    memset(&mesh_event_accept_retry, 0, sizeof(mesh_event_accept_retry));
    mesh_event_accept_retry.response = (struct mesh_event_control_record) {
        .packet = {
            .msg_type = MSG_MESH_EVENT_ACCEPT,
            .src_id = DEVICE_ID,
            .dst_id = peer_id,
            .session_id = UINT32_C(0x6f000001),
            .seq = UINT16_C(0x611),
            .ttl = MESH_DEFAULT_TTL,
        },
        .timing = *pre_send_timing,
        .peer_id = peer_id,
        .valid = true,
    };
    mesh_event_accept_retry.retry.request =
        (struct app_mesh_event_request_identity) {
            .source_id = peer_id,
            .session_id = UINT32_C(0x6f000001),
            .sequence = UINT16_C(0x611),
            .message_type = MSG_MESH_EVENT_PROPOSE,
        };
    memset(mesh_event_accept_retry.retry.request.payload_digest, 0x5a,
           sizeof(mesh_event_accept_retry.retry.request.payload_digest));
    mesh_event_accept_retry.retry.peer_id = peer_id;
    mesh_event_accept_retry.retry.deadline_ms = now_ms + 60000u;
    mesh_event_accept_retry.retry.event_interval_ms =
        pre_send_timing->event_interval_ms;
    mesh_event_accept_retry.retry.active = true;
    mesh_event_accept_retry.remote_boot_nonce =
        UINT64_C(0x8d00000000000001);
}

static void assert_event_timing_equal(
    const struct mesh_event_timing *actual,
    const struct mesh_event_timing *expected)
{
    zassert_not_null(actual);
    zassert_not_null(expected);
    zassert_equal(actual->mesh_channel, expected->mesh_channel);
    zassert_equal(actual->event_interval_ms, expected->event_interval_ms);
    zassert_equal(actual->event_window_ms, expected->event_window_ms);
    zassert_equal(actual->next_event_time_ms, expected->next_event_time_ms);
    zassert_equal(actual->event_counter, expected->event_counter);
    zassert_equal(actual->guard_ms, expected->guard_ms);
    zassert_equal(actual->peer_clock_skew_estimate_ppm,
                  expected->peer_clock_skew_estimate_ppm);
    zassert_equal(actual->max_missed_events, expected->max_missed_events);
    zassert_equal(actual->missed_event_count, expected->missed_event_count);
    zassert_equal(actual->supervision_timeout_ms,
                  expected->supervision_timeout_ms);
    zassert_equal(actual->last_successful_ch9_event_ms,
                  expected->last_successful_ch9_event_ms);
    zassert_equal(actual->local_tx_on_even_events,
                  expected->local_tx_on_even_events);
    zassert_equal(actual->route_fresh, expected->route_fresh);
    zassert_equal(actual->timing_fresh, expected->timing_fresh);
    zassert_equal(actual->fallback_required, expected->fallback_required);
}

static const struct mesh_relay_event_timing_entry *
event_timing_entry_for_test(uint64_t peer_id)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_runtime.event_timings); i++) {
        if (mesh_runtime.event_timings[i].valid &&
            mesh_runtime.event_timings[i].next_hop_id == peer_id) {
            return &mesh_runtime.event_timings[i];
        }
    }
    return NULL;
}

static int preempt_for_test(void)
{
    return mesh_preempt_for_click_event_until(k_uptime_get_32() + 1000u);
}

static struct mesh_outbound queued_report(uint64_t source_id, uint16_t seq)
{
    struct mesh_outbound outbound = {0};

    outbound.packet = (struct proto_packet) {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = source_id,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x6a000000) + seq,
        .seq = seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 3u,
    };
    outbound.payload[0] = TLV_SAMPLE_COUNT;
    outbound.payload[1] = 1u;
    outbound.payload[2] = (uint8_t)seq;
    outbound.payload_len = 3u;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound.next_hop_id = GATEWAY_ID;
    outbound.queued_at_ms = k_uptime_get_32();
    outbound.queued_at_valid = true;
    return outbound;
}

static size_t child_click_report_for_test(struct proto_packet *packet,
                                          uint8_t *payload,
                                          size_t payload_cap,
                                          uint64_t child_id,
                                          uint16_t sequence)
{
    const uint64_t clicker_id = UINT64_C(0x2000000000000f01);
    const uint32_t event_sequence = UINT32_C(0x6a000501);
    const struct range_report_fields fields = {
        .clicker_id = clicker_id,
        .anchor_id = child_id,
        .event_seq = event_sequence,
        .timestamp_ms = UINT64_C(0x12345678),
        .distance_mm = 1840,
        .quality = 91u,
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .omit_cir = true,
    };
    size_t payload_len = 0u;

    zassert_not_null(packet);
    zassert_not_null(payload);
    zassert_equal(report_append_range_tlvs(payload,
                                            payload_cap,
                                            &payload_len,
                                            &fields),
                  PROTO_OK);
    zassert_true(payload_len <= UINT8_MAX);
    zassert_equal(report_init_click_packet(
                      packet,
                      child_id,
                      GATEWAY_ID,
                      proto_click_report_session_id(clicker_id,
                                                    event_sequence),
                      sequence,
                      (uint8_t)payload_len),
                  PROTO_OK);
    return payload_len;
}

static struct mesh_outbound survey_pair_result_for_test(
    uint64_t operation_generation,
    uint16_t round_id,
    uint16_t sample_index)
{
    struct survey_sample sample = {
        .pair = {
            .operation_generation = operation_generation,
            .survey_id = UINT32_C(0x31000001),
            .initiator_id = UINT64_C(0x1111222233334444),
            .responder_id = DEVICE_ID,
            .sample_count = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
        },
        .round_id = round_id,
        .sample_index = sample_index,
        .distance_mm = 1875,
        .quality = 90u,
        .range_status = RANGE_OK,
    };
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t sequence = 0u;

    zassert_equal(survey_append_sample_tlvs(outbound.payload,
                                            sizeof(outbound.payload),
                                            &payload_len,
                                            &sample),
                  PROTO_OK);
    zassert_equal(tlv_append_u64(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 TLV_TIMESTAMP_MS,
                                 UINT64_C(123456789)),
                  PROTO_OK);
    zassert_equal(survey_pair_result_transport_sequence(round_id,
                                                        sample_index,
                                                        &sequence),
                  PROTO_OK);
    zassert_equal(survey_init_result_packet_from_reporter(
                      &outbound.packet,
                      &sample,
                      DEVICE_ID,
                      GATEWAY_ID,
                      sequence,
                      (uint8_t)payload_len),
                  PROTO_OK);
    outbound.payload_len = (uint8_t)payload_len;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound.next_hop_id = GATEWAY_ID;
    return outbound;
}

static struct mesh_outbound survey_discovery_report_from_for_test(
    uint64_t anchor_id,
    uint64_t operation_generation,
    uint16_t sequence)
{
    struct mesh_outbound outbound = {0};
    const uint32_t survey_id = UINT32_C(0x32000001);
    const uint32_t boot_incarnation = UINT32_C(0x41000001);
    size_t payload_len = 0u;

    zassert_equal(survey_append_reach_report_tlvs(outbound.payload,
                                                  sizeof(outbound.payload),
                                                  &payload_len,
                                                  survey_id,
                                                  anchor_id,
                                                  NULL,
                                                  0u),
                  PROTO_OK);
    zassert_equal(survey_operation_generation_append_tlv(
                      outbound.payload,
                      sizeof(outbound.payload),
                      &payload_len,
                      operation_generation),
                  PROTO_OK);
    zassert_equal(tlv_append_u32(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 TLV_NODE_BOOT_COUNTER,
                                 boot_incarnation),
                  PROTO_OK);
    zassert_equal(tlv_append_u16(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 TLV_COMMAND_STATUS,
                                 COMMAND_OK),
                  PROTO_OK);
    zassert_equal(survey_init_discovery_report_packet(
                      &outbound.packet,
                      anchor_id,
                      GATEWAY_ID,
                      survey_id,
                      operation_generation,
                      boot_incarnation,
                      sequence,
                      (uint8_t)payload_len),
                  PROTO_OK);
    outbound.payload_len = (uint8_t)payload_len;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound.next_hop_id = GATEWAY_ID;
    return outbound;
}

static struct mesh_outbound survey_discovery_report_for_test(
    uint64_t operation_generation,
    uint16_t sequence)
{
    return survey_discovery_report_from_for_test(
        DEVICE_ID, operation_generation, sequence);
}

static struct mesh_outbound targeted_pair_prepare_for_test(
    uint64_t target_id,
    uint16_t sequence)
{
    const struct survey_pair pair = {
        .operation_generation = UINT64_C(0x0000005300000001),
        .survey_id = UINT32_C(0x53000001),
        .initiator_id = target_id,
        .responder_id = UINT64_C(0x2000000000000d51),
        .sample_count = 3u,
    };
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {
        0x53u, 0x52u, 0x43u,
    };
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;

    zassert_equal(survey_append_pair_tlvs(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           &pair),
                  PROTO_OK);
    zassert_equal(survey_round_id_append_tlv(outbound.payload,
                                              sizeof(outbound.payload),
                                              &payload_len,
                                              1u),
                  PROTO_OK);
    zassert_equal(survey_round_commitment_append_tlv(
                      outbound.payload,
                      sizeof(outbound.payload),
                      &payload_len,
                      round_commitment),
                  PROTO_OK);
    zassert_equal(survey_init_pair_prepare_packet(&outbound.packet,
                                                   &pair,
                                                   GATEWAY_ID,
                                                   target_id,
                                                   sequence,
                                                   (uint8_t)payload_len),
                  PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    outbound.next_hop_id = DEVICE_ID;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return outbound;
}

static void seed_stale_direct_reverse_route_for_test(uint64_t target_id,
                                                     uint32_t now_ms)
{
    mesh_runtime.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = target_id,
        .next_hop_id = target_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = mesh_runtime.upstream.current_epoch,
        .last_seen_ms = now_ms,
        .hop_count = 1u,
        .quality = 100u,
        .valid = true,
    };
}

static size_t survey_discovery_start_for_test(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    uint64_t operation_generation,
    uint32_t survey_id,
    uint16_t sequence)
{
    const struct survey_discovery_config config = {
        .operation_generation = operation_generation,
        .survey_id = survey_id,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 4u,
        .round_count = SURVEY_DISCOVERY_MAX_ROUND_COUNT,
    };
    size_t payload_len = 0u;

    zassert_equal(survey_append_discovery_start_tlvs(
                      payload, payload_cap, &payload_len, &config),
                  PROTO_OK);
    zassert_equal(survey_init_discovery_start_packet(packet,
                                                      GATEWAY_ID,
                                                      &config,
                                                      sequence,
                                                      (uint8_t)payload_len),
                  PROTO_OK);
    return payload_len;
}

ZTEST(production_seam_report_custody,
      test_new_survey_generation_retires_only_older_pair_result_owner)
{
    const uint64_t old_generation = UINT64_C(0x0000000200000001);
    const uint64_t new_generation = UINT64_C(0x0000000200000002);
    struct app_node_comm_reservation_lease stale_reservation = {
        .token = 91u,
        .owner_generation = old_generation,
        .owner_kind = APP_NODE_COMM_RESERVATION_OWNER_SURVEY_RESULT,
    };
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = survey_result_schedule_for_test,
        .active_owner_matches_outbound = survey_result_active_owner_for_test,
        .wake_active_outbox = survey_result_wake_owner_for_test,
    };
    const struct mesh_outbound old_result =
        survey_pair_result_for_test(old_generation, 1u, 0u);
    const struct mesh_outbound current_result =
        survey_pair_result_for_test(new_generation, 1u, 1u);
    size_t retiring_count = 0u;

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    survey_result_schedule_calls = 0u;
    zassert_ok(app_mesh_local_delivery_stage(
        &result_delivery_slots[0].delivery,
        &old_result,
        survey_operation_session_id(old_generation)));
    result_delivery_slots[0].reservation_owner_generation = old_generation;
    zassert_ok(app_mesh_local_delivery_stage(
        &result_delivery_slots[1].delivery,
        &current_result,
        survey_operation_session_id(new_generation)));
    result_delivery_slots[1].reservation_owner_generation = new_generation;

    zassert_equal(app_anchor_survey_result_delivery_supersede_before(
                      new_generation, &retiring_count),
                  -EINPROGRESS);
    zassert_equal(retiring_count, 1u);
    zassert_true(result_delivery_slots[0].retirement_in_progress);
    zassert_false(result_delivery_slots[1].retirement_in_progress);
    zassert_true(survey_result_schedule_calls > 0u);
    zassert_ok(result_delivery_service_slot(&result_delivery_slots[0]));
    zassert_false(app_mesh_local_delivery_occupied(
        &result_delivery_slots[0].delivery));
    zassert_true(app_mesh_local_delivery_occupied(
        &result_delivery_slots[1].delivery));

    retiring_count = 99u;
    zassert_ok(app_anchor_survey_result_delivery_supersede_before(
        new_generation, &retiring_count));
    zassert_equal(retiring_count, 0u);
    zassert_equal(app_anchor_survey_result_delivery_supersede_before(
                      old_generation, &retiring_count),
                  -ESTALE);
    zassert_equal(app_anchor_survey_result_delivery_stage_reserved(
                      &stale_reservation,
                      &old_result,
                      (const uint8_t[SEMANTIC_DIGEST_SHA256_LEN]) {0}),
                  -ECANCELED);
    zassert_ok(app_mesh_local_delivery_cancel(
        &result_delivery_slots[1].delivery));
    result_delivery_slots[1].reservation_owner_generation = 0u;
}

ZTEST(production_seam_report_custody,
      test_new_survey_generation_retires_old_discovery_without_touching_current)
{
    const uint64_t old_generation = UINT64_C(0x0000000300000001);
    const uint64_t current_generation = UINT64_C(0x0000000300000002);
    const uint64_t next_generation = UINT64_C(0x0000000300000003);
    const struct app_mesh_local_delivery_ops delivery_ops = {
        .save = survey_delivery_save,
        .clear = survey_delivery_clear,
    };
    const struct mesh_outbound old_report =
        survey_discovery_report_for_test(old_generation, 1u);
    const struct mesh_outbound current_report =
        survey_discovery_report_for_test(current_generation, 2u);
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    bool retirement_pending = false;

    app_mesh_local_delivery_init(delivery, &delivery_ops);
    discovery_ops.schedule_work_ms = survey_result_schedule_for_test;
    survey_delivery_retry_round = 0u;
    survey_delivery_handle = 0u;
    survey_delivery_failed_abandon_handle = 0u;
    survey_delivery_comm_owned = false;
    survey_delivery_comm_identity_valid = false;
    survey_delivery_generation_floor = 0u;
    survey_delivery_retirement_in_progress = false;
    survey_report_stage_retry_valid = false;
    survey_result_schedule_calls = 0u;

    zassert_ok(app_mesh_local_delivery_stage(
        delivery,
        &old_report,
        survey_operation_session_id(old_generation)));
    zassert_equal(app_anchor_survey_discovery_supersede_before(
                      current_generation, &retirement_pending),
                  -EINPROGRESS);
    zassert_true(retirement_pending);
    zassert_true(survey_delivery_retirement_in_progress);
    zassert_true(survey_result_schedule_calls > 0u);
    zassert_ok(survey_delivery_service_retirement());
    zassert_false(app_mesh_local_delivery_occupied(delivery));

    zassert_ok(app_mesh_local_delivery_stage(
        delivery,
        &current_report,
        survey_operation_session_id(current_generation)));
    retirement_pending = true;
    zassert_ok(app_anchor_survey_discovery_supersede_before(
        current_generation, &retirement_pending));
    zassert_false(retirement_pending);
    zassert_true(app_mesh_local_delivery_occupied(delivery));
    zassert_ok(app_mesh_local_delivery_cancel(delivery));

    survey_report_stage_retry_outbound = current_report;
    survey_report_stage_retry_generation =
        survey_operation_session_id(current_generation);
    survey_report_stage_retry_valid = true;
    zassert_ok(app_anchor_survey_discovery_supersede_before(
        next_generation, &retirement_pending));
    zassert_false(retirement_pending);
    zassert_false(survey_report_stage_retry_valid);
    zassert_equal(app_anchor_survey_discovery_supersede_before(
                      old_generation, &retirement_pending),
                  -ESTALE);
}

ZTEST(production_seam_report_custody,
      test_staged_empty_discovery_report_survives_paused_schedule_loss)
{
    const uint64_t operation_generation =
        UINT64_C(0x0000000400000001);
    const struct survey_discovery_config config = {
        .operation_generation = operation_generation,
        .survey_id = UINT32_C(0x32000044),
        .start_delay_ms = SURVEY_DISCOVERY_START_DELAY_MS,
        .slot_ms = SURVEY_DISCOVERY_SLOT_MS,
        .slot_count = SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
        .round_count = SURVEY_DISCOVERY_MAX_ROUND_COUNT,
    };
    const struct app_mesh_local_delivery_ops delivery_ops = {
        .save = survey_delivery_save,
        .clear = survey_delivery_clear,
    };
    struct app_mesh_local_delivery *delivery = survey_delivery_instance();
    uint32_t report_delay_ms = 0u;
    uint32_t start_ms;
    int service_ret;

    report_owner_reset();
    app_mesh_local_delivery_init(delivery, &delivery_ops);
    discovery_ops.schedule_work_ms = survey_result_schedule_for_test;
    discovery_ops.boot_incarnation = survey_discovery_boot_for_test;
    discovery_ops.next_sequence = survey_discovery_sequence_next_for_test;
    survey_delivery_retry_round = 0u;
    survey_delivery_handle = 0u;
    survey_delivery_failed_abandon_handle = 0u;
    survey_delivery_comm_owned = false;
    survey_delivery_comm_identity_valid = false;
    survey_delivery_generation_floor = 0u;
    survey_delivery_retirement_in_progress = false;
    survey_report_stage_retry_valid = false;
    survey_result_schedule_calls = 0u;
    survey_discovery_sequence_for_test = 0u;

    zassert_equal(survey_discovery_report_delay_ms(
                      &config,
                      local_survey_discovery_slot(config.slot_count),
                      SURVEY_RESULT_MESH_SLOT_MS,
                      &report_delay_ms),
                  PROTO_OK);
    start_ms = k_uptime_get_32() - report_delay_ms - 1u;

    zassert_ok(mesh_transport_pause_preserving_queued());
    zassert_true(mesh_transport_pause_active());
    zassert_ok(app_anchor_survey_discovery_stage_empty_report(
        &config, start_ms));
    zassert_true(app_anchor_survey_discovery_report_staged(
        survey_operation_session_id(operation_generation)));

    /* Reproduce the escaped edge: the physical queue scheduler rejects work
     * during transport pause, while the empty report exists only in its local
     * delivery owner and is absent from the physical report queue. */
    report_tx_schedule(0u);
    zassert_false(k_work_delayable_is_pending(&report_tx_work));
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_true(app_mesh_local_delivery_active(delivery));

    mesh_transport_resume();
    zassert_false(mesh_transport_pause_active());
    zassert_false(k_work_delayable_is_pending(&report_tx_work),
                  "resume inferred a local-delivery owner from an empty queue");

    /* The exact post-resume service helper must either submit profile-2
     * custody now or retain the immutable owner with a runnable retry. */
    service_ret = app_anchor_survey_discovery_retry_report();
    zassert_true(service_ret == 0 || service_ret == -EAGAIN,
                 "post-resume local-delivery service failed: %d",
                 service_ret);
    zassert_true(app_mesh_local_delivery_active(delivery));
    zassert_true(survey_delivery_handle != 0u ||
                 survey_result_schedule_calls > 0u,
                 "staged empty report has neither communication custody nor retry work");

    if (survey_delivery_handle != 0u) {
        zassert_ok(app_node_comm_abandon_delivery(survey_delivery_handle));
        survey_delivery_handle = 0u;
    }
    survey_delivery_comm_owned = false;
    survey_delivery_comm_identity_valid = false;
    zassert_ok(app_mesh_local_delivery_cancel(delivery));
}

static void start_pending_gateway_report(uint64_t source_id, uint16_t seq)
{
    const struct mesh_outbound outbound = queued_report(source_id, seq);
    struct mesh_outbound started = {0};

    zassert_equal(mesh_relay_start_tx(&mesh_runtime,
                                      &outbound.packet,
                                      outbound.payload,
                                      outbound.payload_len,
                                      k_uptime_get_32(),
                                      &started),
                  PROTO_OK,
                  "could not create pending report");
    /* The production preemption path must preserve the exact physical
     * channel and hop captured by the live relay, not infer either later. */
    mesh_runtime.pending.radio_channel = outbound.radio_channel;
    mesh_runtime.pending.next_hop_id = outbound.next_hop_id;
}

static void start_pending_outbound_for_test(
    const struct mesh_outbound *outbound)
{
    struct mesh_outbound started = {0};

    zassert_not_null(outbound);
    zassert_equal(mesh_relay_start_tx(&mesh_runtime,
                                      &outbound->packet,
                                      outbound->payload,
                                      outbound->payload_len,
                                      k_uptime_get_32(),
                                      &started),
                  PROTO_OK,
                  "could not create pending transit outbound");
    mesh_runtime.pending.radio_channel = outbound->radio_channel;
    mesh_runtime.pending.next_hop_id = outbound->next_hop_id;
}

static struct app_mesh_c5_tx_authorization_token
forwarded_ack_event_repair_owner_prepare(uint64_t peer_id,
                                         uint64_t origin_id)
{
    const struct mesh_outbound transit =
        survey_discovery_report_from_for_test(
            origin_id, UINT64_C(0x0000005500000001), UINT16_C(0x651));
    struct mesh_outbound ack = gateway_ack_for_test(
        &transit.packet, transit.payload, transit.payload_len,
        UINT16_C(0x652));
    const struct app_mesh_ch9_ack_batch *batch;
    struct app_mesh_c5_tx_authorization_token authorization;
    enum app_mesh_ch9_ack_queue_result queue_result =
        APP_MESH_CH9_ACK_QUEUE_TABLE_FULL;

    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    start_pending_outbound_for_test(&transit);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    mesh_runtime.pending.gateway_ack_forward_pending = true;
    mesh_runtime.pending.gateway_ack_forward_next_hop_id = peer_id;

    ack.packet.dst_id = origin_id;
    ack.next_hop_id = peer_id;
    zassert_equal(app_mesh_ch9_ack_table_queue_forwarded(
                      &mesh_ch9_ack_table, &ack, &queue_result),
                  PROTO_OK);
    zassert_equal(queue_result, APP_MESH_CH9_ACK_QUEUE_ADDED);
    batch = app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    zassert_not_null(batch);
    zassert_true(app_mesh_ch9_c5_repair_authorization_capture(
        &authorization,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
        &mesh_runtime.pending,
        mesh_relay_tx_active(&mesh_runtime),
        batch,
        peer_id));
    return authorization;
}

ZTEST(production_seam_report_custody,
      test_transport_pause_preserves_live_and_queued_custody)
{
    const struct mesh_outbound queued = queued_report(DEVICE_ID, 0x3401u);
    const struct mesh_outbound transit =
        queued_report(UINT64_C(0x55550000000000c1), 0x3402u);
    struct mesh_pending_tx pending_before;
    struct mesh_outbound queued_after = {0};
    struct mesh_outbound discarded = {0};

    report_owner_reset();
    start_pending_outbound_for_test(&transit);
    pending_before = mesh_runtime.pending;
    zassert_ok(k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT));

    zassert_ok(mesh_transport_pause_preserving_queued());
    zassert_true(mesh_transport_paused());
    zassert_true(radio_guard_uwb_admission_paused());
    zassert_true(mesh_transport_quiesced());
    zassert_true(mesh_relay_tx_active(&mesh_runtime));
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "transport pause changed live relay custody");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued_after));
    zassert_mem_equal(&queued_after,
                      &queued,
                      sizeof(queued),
                      "transport pause changed queued report custody");

    /* Remove test-owned records while admission remains paused, then use the
     * production resume boundary without exposing either record to RF. */
    mesh_relay_cancel_tx(&mesh_runtime);
    zassert_ok(k_msgq_get(&report_tx_msgq, &discarded, K_NO_WAIT));
    mesh_transport_resume();
    zassert_false(mesh_transport_paused());
    zassert_false(radio_guard_uwb_admission_paused());
}

ZTEST(production_seam_report_custody,
      test_due_survey_reflood_alone_may_cross_live_ack_custody)
{
    const uint64_t transit_source = UINT64_C(0x55550000000000aa);
    struct mesh_relay_result first_result;
    struct mesh_relay_result redrive_result;
    struct mesh_pending_tx transit_before;
    struct mesh_outbound invalid_forward;
    struct mesh_rx_pending discarded_rx;
    struct proto_packet survey_start;
    uint8_t survey_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t survey_payload_len;
    uint32_t first_received_ms;

    report_owner_reset();
    while (k_msgq_get(&mesh_rx_msgq, &discarded_rx, K_NO_WAIT) == 0) {
    }
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    mesh_route_waiting_tx_valid = false;
    active_survey_generation_for_test = 0u;
    survey_payload_len = survey_discovery_start_for_test(
        &survey_start,
        survey_payload,
        sizeof(survey_payload),
        UINT64_C(0x0000001100000022),
        UINT32_C(0x33445566),
        0x3456u);
    first_received_ms = k_uptime_get_32();

    zassert_equal(mesh_relay_handle_rx_with_random(
                      &mesh_runtime,
                      &survey_start,
                      survey_payload,
                      survey_payload_len,
                      GATEWAY_ID,
                      90u,
                      first_received_ms,
                      0u,
                      &first_result),
                  PROTO_OK);
    zassert_true((first_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    zassert_true((first_result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);

    /* Hold an unrelated child report in the exact Channel-9 ACK-wait state
     * observed on hardware, then drive the gateway's byte-identical survey
     * redrive at the core's reflood boundary. */
    start_pending_gateway_report(transit_source, 0x4567u);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    mesh_runtime.pending.gateway_ack_deadline_ms =
        first_received_ms + 60000u;
    transit_before = mesh_runtime.pending;
    zassert_true(app_mesh_ch9_core_ack_wait_active(
        &mesh_runtime.pending, mesh_relay_tx_active(&mesh_runtime)));

    zassert_equal(mesh_relay_handle_rx_with_random(
                      &mesh_runtime,
                      &survey_start,
                      survey_payload,
                      survey_payload_len,
                      GATEWAY_ID,
                      90u,
                      first_received_ms +
                          SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS,
                      0u,
                      &redrive_result),
                  PROTO_OK);
    zassert_equal(redrive_result.status, PROTO_ERR_STALE);
    zassert_true((redrive_result.actions &
                  MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    zassert_true((redrive_result.actions & MESH_RELAY_ACTION_FORWARD) != 0u,
                 "exact due survey duplicate lost its core reflood action");
    zassert_true(mesh_c5_gateway_survey_control_candidate_valid(
        &redrive_result.forward));
    zassert_equal(mesh_c5_forwarded_control_intent(&redrive_result.forward),
                  FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL);

    zassert_false(mesh_coordinator_c5_tx_allowed_authorized_intent(
        "generic-c5",
        &redrive_result.forward,
        NULL,
        FW_C5_TX_INTENT_BACKGROUND));
    zassert_true(mesh_coordinator_c5_tx_allowed_authorized_intent(
        "validated-survey-reflood",
        &redrive_result.forward,
        NULL,
        FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL));

    invalid_forward = redrive_result.forward;
    invalid_forward.packet.src_id = transit_source;
    zassert_false(mesh_c5_gateway_survey_control_candidate_valid(
        &invalid_forward));
    zassert_equal(mesh_c5_forwarded_control_intent(&invalid_forward),
                  FW_C5_TX_INTENT_BACKGROUND);
    zassert_false(mesh_coordinator_c5_tx_allowed_authorized_intent(
        "unvalidated-survey-reflood",
        &invalid_forward,
        NULL,
        FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL));

    zassert_mem_equal(&mesh_runtime.pending,
                      &transit_before,
                      sizeof(transit_before),
                      "survey-control admission changed retained ACK custody");
    mesh_relay_cancel_tx(&mesh_runtime);
}

static void assert_transit_survey_retirement_case(
    const struct mesh_rx_pending *admitted_start,
    const struct mesh_outbound *transit,
    bool gateway_ack_forward_pending,
    bool expect_retired)
{
    const struct app_mesh_report_callbacks *callbacks_before =
        mesh_report_callbacks;
    struct mesh_pending_tx before;

    report_owner_reset();
    mesh_report_callbacks = &survey_callbacks_for_test;
    start_pending_outbound_for_test(transit);
    mesh_runtime.pending.gateway_ack_forward_pending =
        gateway_ack_forward_pending;
    if (gateway_ack_forward_pending) {
        mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    }
    before = mesh_runtime.pending;

    zassert_ok(mesh_retire_stale_transit_survey_result_after_admission(
        admitted_start));
    if (expect_retired) {
        zassert_false(mesh_relay_tx_active(&mesh_runtime),
                      "older transit survey custody survived durable supersession");
    } else {
        zassert_true(mesh_relay_tx_active(&mesh_runtime),
                     "non-stale transit survey custody was canceled");
        zassert_mem_equal(&mesh_runtime.pending,
                          &before,
                          sizeof(before),
                          "non-stale transit survey custody changed");
        mesh_relay_cancel_tx(&mesh_runtime);
    }
    mesh_report_callbacks = callbacks_before;
}

ZTEST(production_seam_report_custody,
      test_durable_survey_admission_retires_only_older_transit_result)
{
    const uint64_t child_id = UINT64_C(0x55550000000000bb);
    const uint64_t old_generation = UINT64_C(0x0000002100000001);
    const uint64_t admitted_generation = UINT64_C(0x0000002100000002);
    const uint64_t newer_generation = UINT64_C(0x0000002100000003);
    struct mesh_outbound old_transit =
        survey_discovery_report_from_for_test(
            child_id, old_generation, 101u);
    const struct mesh_outbound same_transit =
        survey_discovery_report_from_for_test(
            child_id, admitted_generation, 102u);
    const struct mesh_outbound newer_transit =
        survey_discovery_report_from_for_test(
            child_id, newer_generation, 103u);
    struct mesh_outbound malformed_transit = old_transit;
    struct mesh_rx_pending admitted_start = {0};
    const uint8_t *generation_value = NULL;
    uint8_t generation_len = 0u;

    admitted_start.payload_len = survey_discovery_start_for_test(
        &admitted_start.packet,
        admitted_start.payload,
        sizeof(admitted_start.payload),
        admitted_generation,
        UINT32_C(0x44556677),
        0x4567u);
    admitted_start.previous_hop_id = GATEWAY_ID;
    admitted_start.radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    /* The cleanup boundary is inert until the anchor's durable generation
     * owner confirms the exact incoming generation as active. */
    active_survey_generation_for_test = 0u;
    assert_transit_survey_retirement_case(
        &admitted_start, &old_transit, false, false);

    active_survey_generation_for_test = admitted_generation;
    assert_transit_survey_retirement_case(
        &admitted_start, &old_transit, false, true);
    assert_transit_survey_retirement_case(
        &admitted_start, &same_transit, false, false);
    assert_transit_survey_retirement_case(
        &admitted_start, &newer_transit, false, false);

    zassert_equal(tlv_find_unique(malformed_transit.payload,
                                  malformed_transit.payload_len,
                                  TLV_SURVEY_OPERATION_GENERATION,
                                  &generation_value,
                                  &generation_len),
                  PROTO_OK);
    zassert_equal(generation_len, sizeof(uint64_t));
    memset(&malformed_transit.payload[
               generation_value - malformed_transit.payload],
           0,
           generation_len);
    assert_transit_survey_retirement_case(
        &admitted_start, &malformed_transit, false, false);

    /* Once the exact gateway ACK is being handed back toward the child, that
     * return custody is terminal and cannot be inferred stale here. */
    assert_transit_survey_retirement_case(
        &admitted_start, &old_transit, true, false);
    active_survey_generation_for_test = 0u;
}

static void arm_far_relay_timeout(void)
{
    mesh_runtime.pending.gateway_ack_deadline_ms =
        k_uptime_get_32() + 30000u;
    zassert_true(mesh_schedule_tx_timeout() >= 0,
                 "could not arm relay timeout");
    zassert_true(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                 "relay timeout was not armed");
}

static void arm_far_timeout_work(void)
{
    zassert_true(mesh_route_owner_work_reschedule(&mesh_tx_timeout_work,
                                                   30000u) >= 0,
                 "could not arm terminal timeout");
    zassert_true(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                 "terminal timeout was not armed");
}

static struct relay_owner_snapshot relay_owner_snapshot_take(void)
{
    return (struct relay_owner_snapshot) {
        .pending = mesh_runtime.pending,
        .outbox_record = mesh_runtime.outbox_record,
        .timeout_pending = k_work_delayable_is_pending(&mesh_tx_timeout_work),
        .timeout_expires = k_work_delayable_expires_get(
            &mesh_tx_timeout_work),
    };
}

static void assert_relay_owner_unchanged(
    const struct relay_owner_snapshot *expected)
{
    zassert_not_null(expected);
    zassert_mem_equal(&mesh_runtime.pending, &expected->pending,
                      sizeof(mesh_runtime.pending),
                      "failed handoff changed the live relay owner");
    zassert_mem_equal(&mesh_runtime.outbox_record, &expected->outbox_record,
                      sizeof(mesh_runtime.outbox_record),
                      "failed handoff changed relay outbox custody");
    zassert_equal(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                  expected->timeout_pending,
                  "failed handoff changed timeout ownership");
    zassert_equal(k_work_delayable_expires_get(&mesh_tx_timeout_work),
                  expected->timeout_expires,
                  "failed handoff changed exact timeout expiry");
}

static void route_preempt_blocker_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    k_sem_give(&route_preempt_blocker_entered);
    (void)k_sem_take(&route_preempt_blocker_release, K_FOREVER);
    k_sem_give(&route_preempt_blocker_done);
}

static void route_preempt_successor_handler(struct k_work *work)
{
    const struct mesh_outbound successor = queued_report(
        UINT64_C(0x7777000000000001), 811u);
    struct mesh_outbound started = {0};
    int ret;

    ARG_UNUSED(work);
    mesh_relay_cancel_tx(&mesh_runtime);
    ret = mesh_relay_start_tx(&mesh_runtime,
                              &successor.packet,
                              successor.payload,
                              successor.payload_len,
                              k_uptime_get_32(),
                              &started);
    if (ret == PROTO_OK) {
        mesh_runtime.pending.radio_channel = successor.radio_channel;
        mesh_runtime.pending.next_hop_id = successor.next_hop_id;
        mesh_runtime.pending.gateway_ack_deadline_ms =
            k_uptime_get_32() + 30000u;
        ret = mesh_schedule_tx_timeout();
        if (ret >= 0) {
            ret = PROTO_OK;
        }
    }
    route_preempt_successor_result = ret;
    k_sem_give(&route_preempt_successor_done);
}

static void route_owned_preempt_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    atomic_set(&mesh_transport_paused_state, 1);
    route_owned_preempt_result = mesh_click_preempt_run_route_owned(
        k_uptime_get_32() + 1000u);
    atomic_set(&mesh_transport_paused_state, 0);
    k_sem_give(&route_owned_preempt_done);
}

static void preempt_request_caller(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    preempt_request_caller_result =
        mesh_preempt_for_click_event_until(preempt_request_caller_deadline_ms);
    k_sem_give(&preempt_request_caller_done);
}

static void drain_sem(struct k_sem *sem)
{
    while (k_sem_take(sem, K_NO_WAIT) == 0) {
    }
}

static void route_preempt_test_reset(void)
{
    drain_sem(&route_preempt_blocker_entered);
    drain_sem(&route_preempt_blocker_release);
    drain_sem(&route_preempt_blocker_done);
    drain_sem(&route_preempt_successor_done);
    drain_sem(&route_owned_preempt_done);
    drain_sem(&preempt_request_caller_done);
    k_work_init(&route_preempt_blocker_work, route_preempt_blocker_handler);
    k_work_init(&route_preempt_successor_work,
                route_preempt_successor_handler);
    k_work_init(&route_owned_preempt_work, route_owned_preempt_handler);
    route_preempt_successor_result = -EINPROGRESS;
    route_owned_preempt_result = -EINPROGRESS;
    preempt_request_caller_result = -EINPROGRESS;
    preempt_request_caller_deadline_ms = 0u;
}

static bool click_preempt_request_state_is(uint8_t state)
{
    k_spinlock_key_t key;
    bool matches;

    key = k_spin_lock(&mesh_click_preempt_request_lock);
    matches = mesh_click_preempt_request.state == state;
    k_spin_unlock(&mesh_click_preempt_request_lock, key);
    return matches;
}

static void wait_for_click_preempt_request_state(uint8_t state)
{
    for (uint8_t i = 0u; i < 100u; i++) {
        if (click_preempt_request_state_is(state)) {
            return;
        }
        k_msleep(1);
    }
    zassert_true(false, "click preemption request did not reach state %u", state);
}

static void force_preempt_cancel_mismatch(void)
{
    const struct mesh_outbound successor = queued_report(
        UINT64_C(0x8888000000000001), 710u);
    struct mesh_outbound started = {0};

    mesh_relay_cancel_tx(&mesh_runtime);
    zassert_equal(mesh_relay_start_tx(&mesh_runtime,
                                      &successor.packet,
                                      successor.payload,
                                      successor.payload_len,
                                      k_uptime_get_32(),
                                      &started),
                  PROTO_OK,
                  "could not install cancel-mismatch successor");
    mesh_runtime.pending.radio_channel = successor.radio_channel;
    mesh_runtime.pending.next_hop_id = successor.next_hop_id;
}

static void preempt_lock_holder(void *arg1, void *arg2, void *arg3)
{
    int ret;

    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    if (ret == 0) {
        k_sem_give(&preempt_lock_holder_acquired);
        (void)k_sem_take(&preempt_lock_holder_release, K_FOREVER);
        k_mutex_unlock(&report_tx_queue_overflow_lock);
    }
    k_sem_give(&preempt_lock_holder_done);
}

static void preempt_lock_holder_reset(void)
{
    while (k_sem_take(&preempt_lock_holder_acquired, K_NO_WAIT) == 0) {
    }
    while (k_sem_take(&preempt_lock_holder_release, K_NO_WAIT) == 0) {
    }
    while (k_sem_take(&preempt_lock_holder_done, K_NO_WAIT) == 0) {
    }
}

static struct mesh_outbound range_fragment(uint64_t clicker_id,
                                           uint32_t event_seq,
                                           uint8_t attempt_index,
                                           uint8_t fragment_index)
{
    struct mesh_outbound outbound = {0};
    int32_t distance_sample_mm = 1000 + fragment_index;
    uint64_t sequence_timestamp_ms = UINT64_C(10000) + fragment_index;
    uint8_t range_round_index = fragment_index;
    struct range_report_fields fields = {
        .clicker_id = clicker_id,
        .anchor_id = DEVICE_ID,
        .event_seq = event_seq,
        .timestamp_ms = UINT64_C(9000) + fragment_index,
        .distance_mm = distance_sample_mm,
        .quality = 90u,
        .rsl_dbm = -60,
        .range_status = RANGE_OK,
        .distance_samples_mm = &distance_sample_mm,
        .range_round_indices = &range_round_index,
        .sequence_start_timestamps_ms = &sequence_timestamp_ms,
        .sample_index = fragment_index,
        .sample_count = RANGE_REPORT_MAX_PACKET_FRAGMENTS,
        .distance_sample_count = 1u,
        .burst_id = event_seq,
        .attempt_index = attempt_index,
        .detection_source = DETECTION_SOURCE_UWB_WAKE_CLAIM,
        .burst_id_present = true,
        .detection_attempt_present = true,
    };
    size_t payload_len = 0u;
    uint16_t fragment_seq = 0u;

    zassert_equal(report_append_range_tlvs(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           &fields),
                  PROTO_OK);
    zassert_equal(report_range_transport_seq(attempt_index,
                                             fragment_index,
                                             &fragment_seq),
                  PROTO_OK);
    zassert_equal(report_init_range_packet(
                      &outbound.packet,
                      DEVICE_ID,
                      GATEWAY_ID,
                      proto_click_report_session_id(clicker_id, event_seq),
                      fragment_seq,
                      FLAG_COUNT_AS_CLICK,
                      (uint16_t)payload_len),
                  PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound.next_hop_id = GATEWAY_ID;
    return outbound;
}

static uint16_t range_fragment_seq(uint8_t attempt_index,
                                   uint8_t fragment_index)
{
    uint16_t seq = 0u;

    zassert_equal(report_range_transport_seq(attempt_index,
                                             fragment_index,
                                             &seq),
                  PROTO_OK);
    return seq;
}

static int range_abort_queue_fault(
    enum mesh_range_report_batch_abort_queue_operation operation,
    uint32_t index)
{
    if (!range_abort_queue_fault_armed ||
        operation != range_abort_queue_fault_operation ||
        index != range_abort_queue_fault_index) {
        return 0;
    }
    range_abort_queue_fault_armed = false;
    if (operation == MESH_RANGE_REPORT_BATCH_ABORT_POST_GET) {
        report_tx_queue_rotation_scratch.packet.seq++;
        return 0;
    }
    return -EIO;
}

static void range_abort_queue_fault_arm(
    enum mesh_range_report_batch_abort_queue_operation operation,
    uint32_t index)
{
    range_abort_queue_fault_operation = operation;
    range_abort_queue_fault_index = index;
    range_abort_queue_fault_armed = true;
    mesh_range_report_batch_abort_test_queue_fault = range_abort_queue_fault;
}

static int range_fragment_put_fault(uint8_t fragment_index)
{
    if (!range_fragment_put_fault_armed ||
        fragment_index != range_fragment_put_fault_index) {
        return 0;
    }
    range_fragment_put_fault_armed = false;
    return -EIO;
}

static void range_fragment_put_fault_arm(uint8_t fragment_index)
{
    range_fragment_put_fault_index = fragment_index;
    range_fragment_put_fault_armed = true;
    mesh_range_report_fragment_test_put_fault = range_fragment_put_fault;
}

static void prepare_range_abort_fragments(uint64_t clicker_id,
                                          uint32_t event_seq,
                                          uint8_t attempt_index,
                                          uint8_t fragment_count)
{
    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    for (uint8_t i = 0u; i < fragment_count; i++) {
        const struct mesh_outbound fragment = range_fragment(
            clicker_id, event_seq, attempt_index, i);

        zassert_ok(queue_anchor_range_report_fragment(&fragment,
                                                      clicker_id,
                                                      event_seq,
                                                      attempt_index,
                                                      false));
    }
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), fragment_count);
    zassert_equal(anchor_range_report_batch_reservation.control.fragment_count,
                  fragment_count);
}

static struct uwb_wake_claim_frame range_lifecycle_claim(
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    uint64_t nonce)
{
    return (struct uwb_wake_claim_frame) {
        .network_id = 1u,
        .clicker_id = clicker_id,
        .click_event_id = event_seq,
        .attempt_index = attempt_index,
        .priority_id = UINT64_C(0x5555666677778888),
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 400u,
        .discovery_starts_in_ms = 20u,
        .claimed_duration_ms = 1000u,
        .min_anchor_count = 1u,
        .max_anchor_count = 3u,
        .nonce = nonce,
        .flags = FLAG_COUNT_AS_CLICK,
    };
}

static void range_lifecycle_install_result_owner(uint64_t clicker_id,
                                                 uint32_t event_seq,
                                                 uint8_t attempt_index,
                                                 uint64_t nonce)
{
    const struct uwb_wake_claim_frame claim = range_lifecycle_claim(
        clicker_id, event_seq, attempt_index, nonce);
    const uint32_t now_ms = k_uptime_get_32();

    zassert_ok(app_anchor_click_event_runtime_claim(&claim, now_ms, NULL));
    zassert_ok(app_anchor_click_event_runtime_schedule_received(
        false, now_ms + 1u, NULL));
    zassert_ok(app_anchor_click_event_runtime_handle(
        FW_EVENT_RANGE_DUE, now_ms + 2u, NULL));
    zassert_ok(app_anchor_click_event_runtime_handle(
        FW_EVENT_RESULT_RETAINED, now_ms + 3u, NULL));
    zassert_true(app_anchor_click_event_runtime_result_owned());
}

static struct mesh_outbound prepare_range_ack_runtime(uint64_t clicker_id,
                                                      uint32_t event_seq,
                                                      uint8_t attempt_index,
                                                      uint64_t nonce)
{
    struct mesh_outbound sent = {0};
    const struct mesh_outbound fragment = range_fragment(
        clicker_id, event_seq, attempt_index, 0u);

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    zassert_ok(queue_anchor_range_report_fragment(&fragment,
                                                  clicker_id,
                                                  event_seq,
                                                  attempt_index,
                                                  true));
    zassert_true(anchor_range_report_ack_runtime.active);
    zassert_ok(k_msgq_get(&report_tx_msgq, &sent, K_NO_WAIT));
    range_lifecycle_install_result_owner(clicker_id,
                                         event_seq,
                                         attempt_index,
                                         nonce);
    return fragment;
}

static void install_byte_identical_range_successor(void)
{
    struct mesh_outbound sent = {0};

    mesh_range_report_test_after_lifecycle_release_hook = NULL;
    zassert_ok(mesh_range_report_batch_reserve(range_successor_clicker_id,
                                               range_successor_event_seq,
                                               range_successor_attempt_index));
    zassert_ok(queue_anchor_range_report_fragment(
        &range_successor_fragment,
        range_successor_clicker_id,
        range_successor_event_seq,
        range_successor_attempt_index,
        true));
    range_successor_generation = anchor_range_report_ack_runtime.generation;
    zassert_ok(k_msgq_get(&report_tx_msgq, &sent, K_NO_WAIT));
    range_lifecycle_install_result_owner(range_successor_clicker_id,
                                         range_successor_event_seq,
                                         range_successor_attempt_index,
                                         range_successor_nonce);
}

static void arm_byte_identical_range_successor(
    const struct mesh_outbound *fragment,
    uint64_t clicker_id,
    uint32_t event_seq,
    uint8_t attempt_index,
    uint64_t nonce)
{
    range_successor_fragment = *fragment;
    range_successor_clicker_id = clicker_id;
    range_successor_event_seq = event_seq;
    range_successor_attempt_index = attempt_index;
    range_successor_nonce = nonce;
    range_successor_generation = 0u;
    mesh_range_report_test_after_lifecycle_release_hook =
        install_byte_identical_range_successor;
}

static void assert_range_abort_failed_closed(void)
{
    zassert_true(anchor_range_report_batch_reservation.active,
                 "uncertain rollback released the sole batch owner");
    zassert_true(
        anchor_range_report_batch_reservation.queue_admission_fail_closed,
        "uncertain rollback remained retryable");
    zassert_false(anchor_range_report_ack_runtime.active,
                  "abort armed an ACK ledger for an incomplete batch");
    zassert_true(watchdog_stop_calls > 0u,
                 "uncertain rollback kept watchdog progress alive");
}

ZTEST(production_seam_report_custody,
      test_range_fragment_admission_rejects_unmatchable_identity)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct mesh_outbound valid = range_fragment(clicker_id,
                                                event_seq,
                                                attempt_index,
                                                0u);
    struct mesh_outbound invalid;
    const uint8_t *attempt_value = NULL;
    uint8_t attempt_len = 0u;
    size_t attempt_offset;

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));

    invalid = valid;
    invalid.packet.msg_type = MSG_SELF_TEST_REPORT;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.flags = FLAG_COUNT_AS_CLICK;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.flags = FLAG_GATEWAY_ACK_REQUIRED |
                           FLAG_COUNT_AS_CLICK |
                           FLAG_DIAGNOSTIC;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.src_id++;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.dst_id++;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.session_id++;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.seq = 0u;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.packet.seq++;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG,
                  "noncanonical attempt sequence entered relay dedup");
    invalid = valid;
    invalid.packet.ttl--;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG,
                  "noncanonical source TTL became queue-visible");
    invalid = valid;
    invalid.packet.payload_len++;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG);
    invalid = valid;
    invalid.payload[1] = UINT8_MAX;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG,
                  "gateway-rejected malformed TLVs became queue-visible");
    invalid = valid;
    zassert_equal(tlv_find_unique(invalid.payload,
                                  invalid.payload_len,
                                  TLV_ATTEMPT_INDEX,
                                  &attempt_value,
                                  &attempt_len),
                  PROTO_OK);
    zassert_equal(attempt_len, sizeof(attempt_index));
    attempt_offset = (size_t)(attempt_value - invalid.payload);
    invalid.payload[attempt_offset]++;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EBADMSG,
                  "payload attempt disagreed with lifecycle control");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u,
                  "rejected fragment became queue-visible");
    zassert_equal(anchor_range_report_batch_reservation.queued_fragment_count,
                  0u);

    zassert_ok(queue_anchor_range_report_fragment(&valid,
                                                  clicker_id,
                                                  event_seq,
                                                  attempt_index,
                                                  false));
    invalid = range_fragment(clicker_id,
                             event_seq,
                             attempt_index,
                             1u);
    invalid.packet.seq = valid.packet.seq;
    zassert_equal(queue_anchor_range_report_fragment(&invalid,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EALREADY,
                  "duplicate sequence made ACK cleanup ambiguous");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_equal(anchor_range_report_batch_reservation.queued_fragment_count,
                  1u);

    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    zassert_false(anchor_range_report_batch_reservation.active);
    zassert_false(anchor_range_report_ack_runtime.active);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_range_batch_abort_queue_or_head_mismatch_retains_owner)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct mesh_outbound unrelated = queued_report(
        UINT64_C(0x4545000000000002), 902u);

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    zassert_ok(k_msgq_put(&report_tx_msgq, &unrelated, K_NO_WAIT));
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    report_tx_queue_head_owner.active = true;
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
}

ZTEST(production_seam_report_custody,
      test_click_sized_batch_preserves_existing_queue_and_independent_owners)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    const struct mesh_outbound local = queued_report(DEVICE_ID, 901u);
    const struct mesh_outbound transit = queued_report(
        UINT64_C(0x4545000000000001), 902u);
    const struct mesh_outbound first = range_fragment(
        clicker_id, event_seq, attempt_index, 0u);
    const struct mesh_outbound second = range_fragment(
        clicker_id, event_seq, attempt_index, 1u);
    struct mesh_outbound queued = {0};

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(k_msgq_put(&report_tx_msgq, &local, K_NO_WAIT));
    zassert_ok(k_msgq_put(&report_tx_msgq, &transit, K_NO_WAIT));
    mesh_ch9_tx_pending.active = true;
    mesh_ch9_tx_pending.count = 1u;
    zassert_ok(mesh_range_report_batch_reserve_capacity(clicker_id,
                                                        event_seq,
                                                        attempt_index,
                                                        2u));
    zassert_equal(anchor_range_report_batch_reservation.queue_prefix_count, 2u);
    zassert_equal(anchor_range_report_batch_reservation.fragment_capacity, 2u);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 2u,
                  "reservation displaced the exact pre-existing prefix");
    zassert_ok(queue_anchor_range_report_fragment(&first,
                                                  clicker_id,
                                                  event_seq,
                                                  attempt_index,
                                                  false));
    zassert_ok(queue_anchor_range_report_fragment(&second,
                                                  clicker_id,
                                                  event_seq,
                                                  attempt_index,
                                                  false));

    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    zassert_false(anchor_range_report_batch_reservation.active);
    zassert_false(report_tx_queue_recovery_valid);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 2u);
    zassert_ok(k_msgq_get(&report_tx_msgq, &queued, K_NO_WAIT));
    zassert_mem_equal(&queued, &local, sizeof(queued));
    zassert_ok(k_msgq_get(&report_tx_msgq, &queued, K_NO_WAIT));
    zassert_mem_equal(&queued, &transit, sizeof(queued));
    zassert_true(mesh_ch9_tx_pending.active,
                 "new range reservation released the older ACK-wait owner");
    zassert_equal(watchdog_stop_calls, 0u);

    report_owner_reset();
    for (uint8_t i = 0u; i < REPORT_TX_QUEUE_DEPTH - 1u; i++) {
        queued = queued_report(DEVICE_ID, (uint16_t)(910u + i));
        zassert_ok(k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT));
    }
    zassert_equal(mesh_range_report_batch_reserve_capacity(clicker_id,
                                                           event_seq,
                                                           attempt_index,
                                                           2u),
                  -EAGAIN,
                  "click reservation exceeded the actual free suffix");
}

ZTEST(production_seam_report_custody,
      test_range_batch_prefix_rotation_failure_retains_exact_scratch_owner)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    const struct mesh_outbound prefix = queued_report(DEVICE_ID, 930u);
    const struct mesh_outbound fragment = range_fragment(
        clicker_id, event_seq, attempt_index, 0u);

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(k_msgq_put(&report_tx_msgq, &prefix, K_NO_WAIT));
    zassert_ok(mesh_range_report_batch_reserve_capacity(clicker_id,
                                                        event_seq,
                                                        attempt_index,
                                                        1u));
    zassert_ok(queue_anchor_range_report_fragment(&fragment,
                                                  clicker_id,
                                                  event_seq,
                                                  attempt_index,
                                                  false));
    range_abort_queue_fault_arm(
        MESH_RANGE_REPORT_BATCH_ABORT_ROTATE_PUT, 0u);
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_true(anchor_range_report_batch_reservation.rollback_scratch_owned);
    zassert_mem_equal(&report_tx_queue_rotation_scratch,
                      &prefix,
                      sizeof(prefix),
                      "failed prefix rotation lost the dequeued owner");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u,
                  "failed prefix rotation changed the reserved suffix");
}

ZTEST(production_seam_report_custody,
      test_range_batch_abort_faults_retain_exact_remaining_owner)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct mesh_outbound first = {0};
    struct mesh_outbound second = {0};
    struct mesh_outbound queued = {0};

    prepare_range_abort_fragments(clicker_id,
                                  event_seq,
                                  attempt_index,
                                  2u);
    zassert_ok(k_msgq_peek_at(&report_tx_msgq, &first, 0u));
    zassert_ok(k_msgq_peek_at(&report_tx_msgq, &second, 1u));
    range_abort_queue_fault_arm(
        MESH_RANGE_REPORT_BATCH_ABORT_PREFLIGHT_PEEK, 1u);
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 2u);
    zassert_equal(anchor_range_report_batch_reservation.queued_fragment_count,
                  2u);
    zassert_ok(k_msgq_peek_at(&report_tx_msgq, &queued, 0u));
    zassert_mem_equal(&queued, &first, sizeof(queued));
    zassert_ok(k_msgq_peek_at(&report_tx_msgq, &queued, 1u));
    zassert_mem_equal(&queued, &second, sizeof(queued));

    prepare_range_abort_fragments(clicker_id,
                                  event_seq,
                                  attempt_index,
                                  2u);
    zassert_ok(k_msgq_peek_at(&report_tx_msgq, &second, 1u));
    range_abort_queue_fault_arm(
        MESH_RANGE_REPORT_BATCH_ABORT_REMOVE_GET, 1u);
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_equal(anchor_range_report_batch_reservation.queued_fragment_count,
                  1u);
    zassert_equal(anchor_range_report_batch_reservation.control.fragment_count,
                  1u);
    zassert_equal(anchor_range_report_batch_reservation.control.fragments[0].seq,
                  second.packet.seq,
                  "partial rollback ledger did not describe the queue head");
    zassert_false(anchor_range_report_batch_reservation.rollback_scratch_owned);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_mem_equal(&queued, &second, sizeof(queued));
}

ZTEST(production_seam_report_custody,
      test_range_batch_abort_post_get_mismatch_preserves_scratch_owner)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct anchor_range_report_control control_snapshot;
    struct mesh_outbound scratch_snapshot;
    const struct mesh_outbound append = range_fragment(
        clicker_id, event_seq, attempt_index, 2u);

    prepare_range_abort_fragments(clicker_id,
                                  event_seq,
                                  attempt_index,
                                  2u);
    range_abort_queue_fault_arm(MESH_RANGE_REPORT_BATCH_ABORT_POST_GET, 0u);
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_true(anchor_range_report_batch_reservation.rollback_scratch_owned,
                 "dequeued bytes lost their explicit scratch owner");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    control_snapshot = anchor_range_report_batch_reservation.control;
    scratch_snapshot = report_tx_queue_rotation_scratch;

    zassert_equal(queue_anchor_range_report_fragment(&append,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -ESTALE,
                  "fail-closed append overwrote rollback scratch");
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    zassert_mem_equal(&anchor_range_report_batch_reservation.control,
                      &control_snapshot,
                      sizeof(control_snapshot));
    zassert_mem_equal(&report_tx_queue_rotation_scratch,
                      &scratch_snapshot,
                      sizeof(scratch_snapshot));
    zassert_true(anchor_range_report_batch_reservation.rollback_scratch_owned);
}

ZTEST(production_seam_report_custody,
      test_range_fragment_put_failure_is_definite_non_transfer)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    const struct mesh_outbound second = range_fragment(
        clicker_id, event_seq, attempt_index, 1u);
    const struct mesh_outbound empty = {0};

    prepare_range_abort_fragments(clicker_id,
                                  event_seq,
                                  attempt_index,
                                  1u);
    range_fragment_put_fault_arm(1u);
    zassert_equal(queue_anchor_range_report_fragment(&second,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     false),
                  -EIO);
    zassert_true(anchor_range_report_batch_reservation.active);
    zassert_false(
        anchor_range_report_batch_reservation.queue_admission_fail_closed);
    zassert_false(anchor_range_report_batch_reservation.rollback_scratch_owned);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_equal(anchor_range_report_batch_reservation.control.fragment_count,
                  1u);
    zassert_mem_equal(&report_tx_queue_rotation_scratch,
                      &empty,
                      sizeof(empty));

    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    zassert_false(anchor_range_report_batch_reservation.active);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_range_batch_abort_wrong_fragment_retains_owner)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct mesh_outbound valid = range_fragment(clicker_id,
                                                event_seq,
                                                attempt_index,
                                                0u);
    struct mesh_outbound wrong = queued_report(
        UINT64_C(0x4545000000000003), 903u);
    struct mesh_outbound discarded;

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    zassert_ok(queue_anchor_range_report_fragment(&valid,
                                                  clicker_id,
                                                  event_seq,
                                                  attempt_index,
                                                  false));
    zassert_ok(k_msgq_get(&report_tx_msgq, &discarded, K_NO_WAIT));
    zassert_ok(k_msgq_put(&report_tx_msgq, &wrong, K_NO_WAIT));

    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    assert_range_abort_failed_closed();
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u,
                  "preflight destructively removed foreign queue bytes");
    zassert_ok(k_msgq_peek(&report_tx_msgq, &discarded));
    zassert_mem_equal(&discarded,
                      &wrong,
                      sizeof(discarded),
                      "preflight changed the foreign queue owner");
}

ZTEST(production_seam_report_custody,
      test_exact_nine_slot_batch_and_retrying_head_keep_identity)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct mesh_outbound first = {0};
    struct mesh_outbound retried = {0};
    struct mesh_outbound current = {0};
    struct app_mesh_queue_head_token first_token = {0};
    struct app_mesh_queue_head_token retry_token = {0};

    BUILD_ASSERT(REPORT_TX_QUEUE_DEPTH == 9u,
                 "production report queue capacity changed");
    BUILD_ASSERT(RANGE_REPORT_MAX_PACKET_FRAGMENTS == 9u,
                 "maximum range report no longer owns the full queue");

    report_owner_reset();
    /* Production builds the batch while the click owns the UWB window. Keep
     * the real queue worker parked until all fragments are visible. */
    anchor_uwb_busy = true;
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    zassert_true(anchor_range_report_batch_reservation.active);
    zassert_equal(k_msgq_num_free_get(&report_tx_msgq),
                  RANGE_REPORT_MAX_PACKET_FRAGMENTS);
    zassert_equal(mesh_range_report_batch_reserve(clicker_id + 1u,
                                                  event_seq + 1u,
                                                  attempt_index),
                  -EBUSY,
                  "a second click replaced the exact batch owner");

    for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
        struct mesh_outbound fragment = range_fragment(
            clicker_id, event_seq, attempt_index, i);

        zassert_ok(queue_anchor_range_report_fragment(
            &fragment,
            clicker_id,
            event_seq,
            attempt_index,
            i + 1u == RANGE_REPORT_MAX_PACKET_FRAGMENTS));
        zassert_equal(k_msgq_num_used_get(&report_tx_msgq), i + 1u);
    }

    zassert_false(anchor_range_report_batch_reservation.active);
    zassert_true(anchor_range_report_ack_runtime.active);
    zassert_equal(anchor_range_report_ack_runtime.control.fragment_count,
                  RANGE_REPORT_MAX_PACKET_FRAGMENTS);
    zassert_equal(anchor_range_report_ack_runtime.control.clicker_id,
                  clicker_id);
    zassert_equal(anchor_range_report_ack_runtime.control.event_seq,
                  event_seq);
    zassert_equal(anchor_range_report_ack_runtime.control.attempt_index,
                  attempt_index);
    zassert_equal(report_tx_queue_used(),
                  RANGE_REPORT_MAX_PACKET_FRAGMENTS);
    anchor_uwb_busy = false;

    zassert_ok(report_tx_queue_begin_head(&first, &first_token));
    zassert_equal(first.packet.session_id,
                  proto_click_report_session_id(clicker_id, event_seq));
    zassert_equal(first.packet.seq,
                  range_fragment_seq(attempt_index, 0u));
    zassert_equal(report_tx_queue_begin_head(&current, &retry_token),
                  -EBUSY,
                  "a retry replaced the immutable in-flight head");

    /* The transient-send branch in report_tx_work_handler aborts this exact
     * token. Reacquiring it must return byte-identical custody. */
    zassert_ok(report_tx_queue_abort_head(&first_token));
    zassert_ok(report_tx_queue_begin_head(&retried, &retry_token));
    zassert_mem_equal(&retried, &first, sizeof(first),
                      "head retry changed the report identity or payload");
    zassert_equal(report_tx_queue_used(),
                  RANGE_REPORT_MAX_PACKET_FRAGMENTS,
                  "retry released physical report custody");

    zassert_ok(report_tx_queue_commit_head(&retry_token, &retried));
    zassert_equal(report_tx_queue_used(),
                  RANGE_REPORT_MAX_PACKET_FRAGMENTS - 1u);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &current));
    zassert_equal(current.packet.seq,
                  range_fragment_seq(attempt_index, 1u),
                  "committing the first head reordered the batch");
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_gateway_ack_cleanup_cannot_clear_byte_identical_successor)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct anchor_range_report_control retired_control;
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint64_t retired_generation;
    const struct mesh_outbound fragment = prepare_range_ack_runtime(
        clicker_id,
        event_seq,
        attempt_index,
        UINT64_C(0x0102030405060708));

    zassert_true(mesh_packet_semantic_digest(&fragment.packet,
                                             fragment.payload,
                                             fragment.payload_len,
                                             digest));
    retired_control = anchor_range_report_ack_runtime.control;
    retired_generation = anchor_range_report_ack_runtime.generation;
    arm_byte_identical_range_successor(
        &fragment,
        clicker_id,
        event_seq,
        attempt_index,
        UINT64_C(0x1112131415161718));

    zassert_ok(mesh_anchor_range_report_note_gateway_confirmed(
        &fragment.packet, digest));
    zassert_true(anchor_range_report_ack_runtime.active,
                 "old ACK cleanup erased a successor runtime");
    zassert_equal(anchor_range_report_ack_runtime.generation,
                  range_successor_generation);
    zassert_not_equal(anchor_range_report_ack_runtime.generation,
                      retired_generation,
                      "successor reused the retired RAM incarnation");
    zassert_true(anchor_range_report_control_matches(
        &anchor_range_report_ack_runtime.control, &retired_control));
    zassert_equal(anchor_range_report_ack_runtime.acknowledged_mask, 0u);
    zassert_true(app_anchor_click_event_runtime_result_owned());
}

ZTEST(production_seam_report_custody,
      test_new_click_detaches_delivered_phase_without_dropping_old_transport)
{
    const uint64_t old_clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t old_event_seq = 0x10203040u;
    const uint8_t old_attempt_index = 2u;
    const uint64_t successor_clicker_id = old_clicker_id;
    const uint32_t successor_event_seq = old_event_seq + 1u;
    const uint8_t successor_attempt_index = 1u;
    const uint64_t successor_nonce = UINT64_C(0x4142434445464748);
    const struct mesh_outbound old_fragment = prepare_range_ack_runtime(
        old_clicker_id,
        old_event_seq,
        old_attempt_index,
        UINT64_C(0x0102030405060708));
    const struct uwb_wake_claim_frame successor_claim = range_lifecycle_claim(
        successor_clicker_id,
        successor_event_seq,
        successor_attempt_index,
        successor_nonce);
    const struct mesh_outbound successor_fragment = range_fragment(
        successor_clicker_id,
        successor_event_seq,
        successor_attempt_index,
        0u);
    uint8_t old_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint64_t successor_generation;

    zassert_true(anchor_range_report_ack_runtime.active);
    zassert_equal(anchor_range_report_ack_runtime.acknowledged_mask, 0u);
    zassert_true(app_anchor_click_event_runtime_result_owned());
    zassert_ok(app_anchor_click_event_runtime_claim(
        &successor_claim, k_uptime_get_32(), NULL));
    zassert_equal(app_anchor_click_event_runtime_state(),
                  FW_ANCHOR_CLICK_CLAIMED,
                  "new click remained blocked by an independently owned report");

    /* The old queue entry was already transferred to the relay by the helper.
     * Replacing its lifecycle ledger cannot release those transport bytes. */
    zassert_ok(mesh_range_report_batch_reserve(successor_clicker_id,
                                               successor_event_seq,
                                               successor_attempt_index));
    zassert_ok(queue_anchor_range_report_fragment(&successor_fragment,
                                                  successor_clicker_id,
                                                  successor_event_seq,
                                                  successor_attempt_index,
                                                  true));
    successor_generation = anchor_range_report_ack_runtime.generation;
    range_lifecycle_install_result_owner(successor_clicker_id,
                                         successor_event_seq,
                                         successor_attempt_index,
                                         successor_nonce);
    zassert_true(mesh_packet_semantic_digest(&old_fragment.packet,
                                             old_fragment.payload,
                                             old_fragment.payload_len,
                                             old_digest));
    zassert_ok(mesh_anchor_range_report_note_gateway_confirmed(
        &old_fragment.packet, old_digest));
    zassert_true(anchor_range_report_ack_runtime.active,
                 "late old ACK erased successor report custody");
    zassert_equal(anchor_range_report_ack_runtime.generation,
                  successor_generation);
    zassert_equal(anchor_range_report_ack_runtime.control.event_seq,
                  successor_event_seq);
    zassert_true(app_anchor_click_event_runtime_result_owned());
}

ZTEST(production_seam_report_custody,
      test_terminal_cleanup_cannot_clear_byte_identical_successor)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    struct anchor_range_report_control retired_control;
    uint64_t retired_generation;
    const struct mesh_outbound fragment = prepare_range_ack_runtime(
        clicker_id,
        event_seq,
        attempt_index,
        UINT64_C(0x2122232425262728));

    retired_control = anchor_range_report_ack_runtime.control;
    retired_generation = anchor_range_report_ack_runtime.generation;
    arm_byte_identical_range_successor(
        &fragment,
        clicker_id,
        event_seq,
        attempt_index,
        UINT64_C(0x3132333435363738));

    zassert_ok(mesh_anchor_range_report_note_terminal_release(
        &fragment.packet, fragment.payload, fragment.payload_len));
    zassert_true(anchor_range_report_ack_runtime.active,
                 "old terminal cleanup erased a successor runtime");
    zassert_equal(anchor_range_report_ack_runtime.generation,
                  range_successor_generation);
    zassert_not_equal(anchor_range_report_ack_runtime.generation,
                      retired_generation,
                      "successor reused the retired RAM incarnation");
    zassert_true(anchor_range_report_control_matches(
        &anchor_range_report_ack_runtime.control, &retired_control));
    zassert_equal(anchor_range_report_ack_runtime.acknowledged_mask, 0u);
    zassert_true(app_anchor_click_event_runtime_result_owned());
}

ZTEST(production_seam_report_custody,
      test_click_preemption_atomically_transfers_live_local_relay)
{
    struct mesh_outbound queued = {0};
    uint32_t lost_before;

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    /* Keep the real report worker from sending while this test observes the
     * completed handoff; production starts this path in the click UWB window. */
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 101u);
    arm_far_relay_timeout();

    zassert_ok(preempt_for_test());
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_IDLE,
                  "relay still owns a click after queue admission");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_false(report_tx_queue_recovery_valid);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_equal(queued.packet.src_id, DEVICE_ID);
    zassert_equal(queued.packet.seq, 101u);
    zassert_equal(queued.radio_channel, UWB_CHANNEL_MESH_PAYLOAD);
    zassert_equal(queued.next_hop_id, GATEWAY_ID);
    zassert_false(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                  "success retained only the obsolete relay timeout");
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_full_queue_keeps_transit_recovery_owner)
{
    struct mesh_outbound head = {0};
    struct mesh_outbound transit = {0};
    uint32_t lost_before;

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 102u);
    for (uint16_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        transit = queued_report(UINT64_C(0x3333000000000000) + i,
                                 (uint16_t)(300u + i));
        zassert_ok(k_msgq_put(&report_tx_msgq, &transit, K_NO_WAIT));
    }

    zassert_ok(preempt_for_test());
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_IDLE);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), REPORT_TX_QUEUE_DEPTH);
    zassert_true(report_tx_queue_recovery_valid,
                 "displaced transit bytes lost their explicit owner");
    zassert_equal(report_tx_queue_overflow_dropped.packet.src_id,
                  UINT64_C(0x3333000000000000));
    zassert_equal(report_tx_queue_used(), REPORT_TX_QUEUE_DEPTH + 1u);
    zassert_ok(report_tx_queue_peek(&head));
    zassert_equal(head.packet.src_id, DEVICE_ID,
                  "new local click was not selected ahead of transit");
    zassert_equal(head.packet.seq, 102u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_full_local_queue_keeps_live_relay)
{
    struct relay_owner_snapshot owner_before;
    struct mesh_outbound queued = {0};
    uint32_t lost_before;
    uint16_t seen = 0u;

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 106u);
    arm_far_relay_timeout();
    owner_before = relay_owner_snapshot_take();
    for (uint16_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        queued = queued_report(DEVICE_ID, (uint16_t)(350u + i));
        zassert_ok(k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT));
    }

    zassert_equal(preempt_for_test(), -ENOSPC);
    assert_relay_owner_unchanged(&owner_before);
    zassert_false(report_tx_queue_recovery_valid);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), REPORT_TX_QUEUE_DEPTH);
    for (uint16_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        zassert_ok(k_msgq_get(&report_tx_msgq, &queued, K_NO_WAIT));
        zassert_equal(queued.packet.src_id, DEVICE_ID);
        zassert_true(queued.packet.seq >= 350u &&
                     queued.packet.seq < 350u + REPORT_TX_QUEUE_DEPTH);
        seen |= (uint16_t)(1u << (queued.packet.seq - 350u));
    }
    zassert_equal(seen, (uint16_t)((1u << REPORT_TX_QUEUE_DEPTH) - 1u));
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_rejects_active_batch_without_touching_relay)
{
    struct relay_owner_snapshot owner_before;
    uint32_t lost_before;

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 103u);
    arm_far_relay_timeout();
    owner_before = relay_owner_snapshot_take();
    zassert_ok(mesh_range_report_batch_reserve(UINT64_C(0x1111222233334444),
                                               0x10203040u,
                                               1u));

    zassert_equal(preempt_for_test(), -EAGAIN);
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);
    mesh_range_report_batch_abort(UINT64_C(0x1111222233334444),
                                  0x10203040u,
                                  1u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_rejects_immutable_head_interleaving)
{
    struct app_mesh_queue_head_token head_token = {0};
    struct mesh_outbound transit = queued_report(
        UINT64_C(0x4444000000000001), 401u);
    struct mesh_outbound held_head = {0};
    struct relay_owner_snapshot owner_before;
    uint32_t lost_before;

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    anchor_uwb_busy = true;
    zassert_ok(k_msgq_put(&report_tx_msgq, &transit, K_NO_WAIT));
    zassert_ok(report_tx_queue_begin_head(&held_head, &head_token));
    start_pending_gateway_report(DEVICE_ID, 104u);
    arm_far_relay_timeout();
    owner_before = relay_owner_snapshot_take();

    zassert_equal(preempt_for_test(), -EBUSY);
    assert_relay_owner_unchanged(&owner_before);
    zassert_true(app_mesh_queue_head_owned(&report_tx_queue_head_owner));
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_ok(report_tx_queue_abort_head(&head_token));
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_lock_contention_leaves_live_owner_untouched)
{
    struct relay_owner_snapshot owner_before;
    uint32_t lost_before;

    report_owner_reset();
    preempt_lock_holder_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 105u);
    arm_far_relay_timeout();
    owner_before = relay_owner_snapshot_take();
    (void)k_thread_create(&preempt_lock_holder_thread,
                          preempt_lock_holder_stack,
                          K_THREAD_STACK_SIZEOF(preempt_lock_holder_stack),
                          preempt_lock_holder,
                          NULL, NULL, NULL,
                          K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&preempt_lock_holder_acquired, K_SECONDS(1)));

    zassert_equal(preempt_for_test(), -EBUSY);
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    k_sem_give(&preempt_lock_holder_release);
    zassert_ok(k_sem_take(&preempt_lock_holder_done, K_SECONDS(1)));
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_nonclick_preemption_keeps_relay_bytes_and_fails_closed_without_wake)
{
    struct mesh_pending_tx pending_before;
    struct relay_owner_snapshot owner_before;
    uint32_t lost_before;

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(UINT64_C(0x5555000000000001), 501u);
    arm_far_relay_timeout();
    pending_before = mesh_runtime.pending;

    zassert_ok(preempt_for_test());
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_mem_equal(&mesh_runtime.pending.packet, &pending_before.packet,
                      sizeof(mesh_runtime.pending.packet));
    zassert_mem_equal(mesh_runtime.pending.payload, pending_before.payload,
                      mesh_runtime.pending.payload_len);
    zassert_true(k_work_delayable_is_pending(&mesh_tx_timeout_work));
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);

    report_owner_reset();
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(UINT64_C(0x5555000000000002), 502u);
    arm_far_relay_timeout();
    owner_before = relay_owner_snapshot_take();
    atomic_set(&mesh_transport_paused_state, 1);

    zassert_equal(preempt_for_test(), -ESHUTDOWN);
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(watchdog_stop_calls, 0u,
                  "rejected bridge submission should retain existing custody");
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    atomic_set(&mesh_transport_paused_state, 0);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_recomputes_shared_timeout_for_ch9_owner)
{
    uint32_t ch9_deadline_ms;
    uint32_t expected_remaining_ms;
    int64_t actual_remaining_ms;
    k_ticks_t old_timeout_expires;
    k_ticks_t new_timeout_expires;

    report_owner_reset();
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 107u);
    arm_far_relay_timeout();
    old_timeout_expires = k_work_delayable_expires_get(&mesh_tx_timeout_work);
    ch9_deadline_ms = k_uptime_get_32() + 20000u;
    mesh_ch9_tx_pending.active = true;
    mesh_ch9_tx_pending.count = 1u;
    mesh_ch9_tx_pending.next_hop_id = GATEWAY_ID;
    mesh_ch9_tx_pending.deadline_ms = ch9_deadline_ms;

    zassert_ok(preempt_for_test());
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_IDLE);
    zassert_true(mesh_ch9_tx_pending_is_active(),
                 "click transfer cleared the unrelated CH9 timeout owner");
    zassert_equal(mesh_ch9_tx_pending.deadline_ms, ch9_deadline_ms);
    zassert_true(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                 "shared timeout was canceled after relay release");
    new_timeout_expires = k_work_delayable_expires_get(&mesh_tx_timeout_work);
    expected_remaining_ms = uptime_ms_until_deadline(k_uptime_get_32(),
                                                      ch9_deadline_ms);
    actual_remaining_ms = k_ticks_to_ms_floor64(
        k_work_delayable_remaining_get(&mesh_tx_timeout_work));
    zassert_true(new_timeout_expires < old_timeout_expires,
                 "shared timeout retained the old relay deadline");
    zassert_true(actual_remaining_ms <=
                     (int64_t)expected_remaining_ms + 100 &&
                 actual_remaining_ms >=
                     (int64_t)expected_remaining_ms - 100,
                 "shared timeout did not select CH9 deadline: actual=%lld expected=%u",
                 (long long)actual_remaining_ms,
                 expected_remaining_ms);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_preserves_retry_backoff_deadline)
{
    struct mesh_outbound queued = {0};
    struct relay_owner_snapshot owner_before;
    uint32_t retry_after_ms;

    report_owner_reset();
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 108u);
    retry_after_ms = k_uptime_get_32() + 20000u;
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    mesh_runtime.pending.retry_after_ms = retry_after_ms;
    zassert_true(mesh_schedule_tx_timeout() >= 0);

    zassert_ok(preempt_for_test());
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_true(queued.earliest_tx_valid);
    zassert_equal(queued.earliest_tx_ms, retry_after_ms,
                  "local click transfer bypassed retry collision backoff");
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_IDLE);

    report_owner_reset();
    start_pending_gateway_report(UINT64_C(0x5555000000000003), 503u);
    retry_after_ms = k_uptime_get_32() + 20000u;
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    mesh_runtime.pending.retry_after_ms = retry_after_ms;
    zassert_true(mesh_schedule_tx_timeout() >= 0);
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(mesh_runtime.pending.retry_after_ms, retry_after_ms,
                  "non-click preemption rewrote a live retry deadline");
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_leaves_terminal_and_confirm_custody_untouched)
{
    struct relay_owner_snapshot owner_before;
    uint32_t lost_before;

    report_owner_reset();
    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(DEVICE_ID, 109u);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_TERMINAL_COMMIT;
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);

    report_owner_reset();
    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(UINT64_C(0x5555000000000004), 504u);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_TERMINAL_COMMIT;
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);

    report_owner_reset();
    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(DEVICE_ID, 110u);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    mesh_runtime.pending.retry_after_ms = k_uptime_get_32() + 20000u;
    mesh_runtime.pending.gateway_ack_confirm_pending = true;
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);

    report_owner_reset();
    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(DEVICE_ID, 112u);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    mesh_runtime.pending.retry_after_ms = k_uptime_get_32() + 20000u;
    mesh_runtime.pending.gateway_ack_recovery_flags = 1u;
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);

    report_owner_reset();
    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(UINT64_C(0x5555000000000007), 507u);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    mesh_runtime.pending.gateway_ack_forward_pending = true;
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_leaves_expired_relay_on_terminal_path)
{
    struct relay_owner_snapshot owner_before;
    uint32_t expiry_ms;

    report_owner_reset();
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 111u);
    expiry_ms = mesh_relay_outbox_expiry_s_for_packet(
        &mesh_runtime.pending.packet,
        mesh_runtime.pending.payload,
        mesh_runtime.pending.payload_len) * 1000u;
    mesh_runtime.pending.packet.message_age_ms = expiry_ms;
    mesh_runtime.pending.queued_at_ms = k_uptime_get_32();
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);

    report_owner_reset();
    anchor_uwb_busy = true;
    start_pending_gateway_report(UINT64_C(0x5555000000000005), 505u);
    expiry_ms = mesh_relay_outbox_expiry_s_for_packet(
        &mesh_runtime.pending.packet,
        mesh_runtime.pending.payload,
        mesh_runtime.pending.payload_len) * 1000u;
    mesh_runtime.pending.packet.message_age_ms = expiry_ms;
    mesh_runtime.pending.queued_at_ms = k_uptime_get_32();
    arm_far_timeout_work();
    owner_before = relay_owner_snapshot_take();

    zassert_ok(preempt_for_test());
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_cancel_mismatch_restores_exact_full_fifo)
{
    const uint16_t expected_sequence[REPORT_TX_QUEUE_DEPTH] = {
        700u, 701u, 702u, 703u, 704u, 705u, 706u, 707u, 708u,
    };
    struct mesh_outbound queued = {0};
    uint32_t lost_before;

    report_owner_reset();
    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    queued = queued_report(DEVICE_ID, 700u);
    zassert_ok(k_msgq_put(&report_tx_msgq,
                          &queued, K_NO_WAIT));
    queued = queued_report(UINT64_C(0x6666000000000001), 701u);
    zassert_ok(k_msgq_put(&report_tx_msgq,
                          &queued, K_NO_WAIT));
    for (uint16_t seq = 702u; seq <= 708u; seq++) {
        queued = queued_report(UINT64_C(0x6666000000000000) + seq, seq);
        zassert_ok(k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT));
    }
    start_pending_gateway_report(DEVICE_ID, 709u);
    mesh_preempt_test_before_cancel_hook = force_preempt_cancel_mismatch;

    zassert_equal(preempt_for_test(), -ESTALE);
    mesh_preempt_test_before_cancel_hook = NULL;
    zassert_false(report_tx_queue_recovery_valid);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), REPORT_TX_QUEUE_DEPTH);
    for (uint8_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        zassert_ok(k_msgq_get(&report_tx_msgq, &queued, K_NO_WAIT));
        zassert_equal(queued.packet.seq, expected_sequence[i],
                      "rollback changed FIFO order at index %u", i);
    }
    zassert_equal(mesh_runtime.pending.packet.seq, 710u,
                  "rollback touched the injected successor relay owner");
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_serializes_route_successor_and_timeout)
{
    report_owner_reset();
    route_preempt_test_reset();
    anchor_uwb_busy = true;
    start_pending_gateway_report(DEVICE_ID, 812u);
    arm_far_relay_timeout();

    zassert_true(k_work_submit_to_queue(&mesh_route_work_q,
                                         &route_preempt_blocker_work) >= 0);
    zassert_ok(k_sem_take(&route_preempt_blocker_entered, K_SECONDS(1)));
    zassert_true(k_work_submit_to_queue(&mesh_route_work_q,
                                         &route_preempt_successor_work) >= 0);
    preempt_request_caller_deadline_ms = k_uptime_get_32() + 1000u;
    (void)k_thread_create(&preempt_request_caller_thread,
                          preempt_request_caller_stack,
                          K_THREAD_STACK_SIZEOF(preempt_request_caller_stack),
                          preempt_request_caller,
                          NULL, NULL, NULL,
                          K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
    wait_for_click_preempt_request_state(MESH_CLICK_PREEMPT_REQUEST_QUEUED);
    k_sem_give(&route_preempt_blocker_release);

    zassert_ok(k_sem_take(&route_preempt_blocker_done, K_SECONDS(1)));
    zassert_ok(k_sem_take(&route_preempt_successor_done, K_SECONDS(1)));
    zassert_ok(k_sem_take(&preempt_request_caller_done, K_SECONDS(1)));
    zassert_equal(route_preempt_successor_result, PROTO_OK);
    zassert_ok(preempt_request_caller_result);
    zassert_equal(mesh_runtime.pending.packet.seq, 811u);
    zassert_equal(mesh_runtime.pending.packet.src_id,
                  UINT64_C(0x7777000000000001));
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_true(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                 "route-owned successor lost its shared timeout");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u,
                  "stale local plan reached the report queue");
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_click_preemption_deadline_quarantines_queued_request)
{
    struct relay_owner_snapshot owner_before;
    uint32_t now_ms;
    uint32_t lost_before;

    report_owner_reset();
    route_preempt_test_reset();
    now_ms = k_uptime_get_32();
    zassert_equal(mesh_preempt_for_click_event_until(
                      now_ms + UWB_DISCOVERY_RX_GUARD_MS),
                  -ETIMEDOUT,
                  "bridge accepted a deadline with no routing guard");

    anchor_uwb_busy = true;
    lost_before = fw_delivery_loss_count(&mesh_delivery_loss);
    start_pending_gateway_report(DEVICE_ID, 813u);
    arm_far_relay_timeout();
    owner_before = relay_owner_snapshot_take();
    zassert_true(k_work_submit_to_queue(&mesh_route_work_q,
                                         &route_preempt_blocker_work) >= 0);
    zassert_ok(k_sem_take(&route_preempt_blocker_entered, K_SECONDS(1)));
    preempt_request_caller_deadline_ms = k_uptime_get_32() +
        UWB_DISCOVERY_RX_GUARD_MS + 20u;
    (void)k_thread_create(&preempt_request_caller_thread,
                          preempt_request_caller_stack,
                          K_THREAD_STACK_SIZEOF(preempt_request_caller_stack),
                          preempt_request_caller,
                          NULL, NULL, NULL,
                          K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
    wait_for_click_preempt_request_state(MESH_CLICK_PREEMPT_REQUEST_QUEUED);
    k_msleep(30);
    zassert_ok(k_sem_take(&preempt_request_caller_done, K_SECONDS(1)));
    zassert_equal(preempt_request_caller_result, -ETIMEDOUT);
    zassert_true(click_preempt_request_state_is(
                     MESH_CLICK_PREEMPT_REQUEST_CANCELED));
    zassert_equal(preempt_for_test(), -EBUSY,
                  "timed-out request slot was reused before stale work drained");
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);

    k_sem_give(&route_preempt_blocker_release);
    zassert_ok(k_sem_take(&route_preempt_blocker_done, K_SECONDS(1)));
    wait_for_click_preempt_request_state(MESH_CLICK_PREEMPT_REQUEST_IDLE);
    assert_relay_owner_unchanged(&owner_before);
    zassert_equal(fw_delivery_loss_count(&mesh_delivery_loss), lost_before);
    zassert_ok(preempt_for_test(),
               "drained generation did not permit the next click preemption");
}

ZTEST(production_seam_report_custody,
      test_route_owned_deferral_fails_closed_when_timeout_cannot_schedule)
{
    struct mesh_pending_tx pending_before;

    report_owner_reset();
    route_preempt_test_reset();
    start_pending_gateway_report(UINT64_C(0x5555000000000006), 506u);
    pending_before = mesh_runtime.pending;
    zassert_true(k_work_submit_to_queue(&mesh_route_work_q,
                                         &route_owned_preempt_work) >= 0);
    zassert_ok(k_sem_take(&route_owned_preempt_done, K_SECONDS(1)));
    zassert_equal(route_owned_preempt_result, -ESHUTDOWN);
    zassert_equal(mesh_runtime.pending.state, MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_mem_equal(&mesh_runtime.pending.packet, &pending_before.packet,
                      sizeof(mesh_runtime.pending.packet));
    zassert_mem_equal(mesh_runtime.pending.payload, pending_before.payload,
                      mesh_runtime.pending.payload_len);
    zassert_false(k_work_delayable_is_pending(&mesh_tx_timeout_work));
    zassert_true(watchdog_stop_calls > 0u,
                 "post-deferral timeout failure did not fail closed");
}

ZTEST(production_seam_report_custody,
      test_fresh_forwarded_ack_token_promotes_retained_accept_and_transmits)
{
    const uint64_t peer_id = UINT64_C(0x20000000000000a9);
    const uint64_t origin_id = UINT64_C(0x20000000000000c9);
    const struct mesh_event_timing timing = event_accept_timing_for_test(
        k_uptime_get_32() + 60000u);
    struct app_mesh_c5_tx_authorization_token fresh;
    struct app_mesh_c5_tx_authorization_token stale;
    struct app_mesh_c5_tx_authorization_token wrong_peer;
    struct mesh_event_accept_retry_context accept_before;
    struct mesh_pending_tx pending_before;
    struct app_mesh_ch9_ack_table ack_table_before;
    struct proto_packet transmitted_packet = {0};
    uint8_t transmitted_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t transmitted_payload_len = 0u;
    uint64_t transmitted_previous_hop = 0u;
    uint32_t accept_session;
    uint16_t accept_sequence;
    int cancel_ret;

    report_owner_reset();
    event_accept_finish_reset();
    route_preempt_test_reset();
    fresh = forwarded_ack_event_repair_owner_prepare(peer_id, origin_id);
    event_accept_finish_prepare(peer_id, &timing);
    accept_session = mesh_event_accept_retry.response.packet.session_id;
    accept_sequence = mesh_event_accept_retry.response.packet.seq;
    zassert_ok(mesh_prepare_event_control_record(
        &mesh_event_accept_retry.response,
        peer_id,
        MSG_MESH_EVENT_ACCEPT,
        &timing,
        accept_session,
        accept_sequence,
        0u));

    /* Model the HIL escape: ACCEPT retained the token for an older ACK-batch
     * identity, while the same exact ACK custody advanced to a fresh token. */
    stale = fresh;
    stale.retained_ack_seq ^= UINT16_C(1);
    zassert_false(app_mesh_ch9_c5_repair_owner_matches(
        &stale,
        &mesh_runtime.pending,
        mesh_relay_tx_active(&mesh_runtime),
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id)));
    mesh_event_accept_retry.c5_repair_authorization = stale;
    memcpy(&accept_before, &mesh_event_accept_retry, sizeof(accept_before));
    memcpy(&pending_before, &mesh_runtime.pending, sizeof(pending_before));
    memcpy(&ack_table_before, &mesh_ch9_ack_table, sizeof(ack_table_before));

    zassert_equal(mesh_propose_event_after_channel5_contact_authorized(
                      peer_id, "stale-forwarded-ack-token", &stale),
                  -ESTALE);
    zassert_mem_equal(&mesh_event_accept_retry,
                      &accept_before,
                      sizeof(accept_before),
                      "stale supplied token changed retained ACCEPT");
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "stale supplied token changed transit ACK custody");
    zassert_mem_equal(&mesh_ch9_ack_table,
                      &ack_table_before,
                      sizeof(ack_table_before),
                      "stale supplied token changed retained ACK bytes");

    wrong_peer = fresh;
    wrong_peer.peer_id ^= UINT64_C(1);
    zassert_equal(mesh_propose_event_after_channel5_contact_authorized(
                      peer_id, "wrong-forwarded-ack-peer", &wrong_peer),
                  -EBUSY);
    zassert_mem_equal(&mesh_event_accept_retry,
                      &accept_before,
                      sizeof(accept_before),
                      "wrong-peer token changed retained ACCEPT");
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before));
    zassert_mem_equal(&mesh_ch9_ack_table,
                      &ack_table_before,
                      sizeof(ack_table_before));

    /* Hold the real route owner queue so promotion can be inspected before
     * its immediate retry worker executes. */
    k_work_init_delayable(&mesh_event_negotiation_retry_work,
                          mesh_event_negotiation_retry_work_handler);
    zassert_true(k_work_submit_to_queue(&mesh_route_work_q,
                                        &route_preempt_blocker_work) >= 0);
    zassert_ok(k_sem_take(&route_preempt_blocker_entered, K_SECONDS(1)));
    zassert_ok(mesh_propose_event_after_channel5_contact_authorized(
        peer_id, "fresh-forwarded-ack-token", &fresh));
    zassert_true(app_mesh_c5_tx_authorization_token_equal(
        &mesh_event_accept_retry.c5_repair_authorization, &fresh));
    zassert_true(mesh_event_accept_retry.retry.retry_due_armed);
    zassert_mem_equal(&mesh_event_accept_retry.response,
                      &accept_before.response,
                      sizeof(accept_before.response),
                      "promotion rebuilt immutable ACCEPT bytes");
    zassert_mem_equal(&mesh_event_accept_retry.retry.request,
                      &accept_before.retry.request,
                      sizeof(accept_before.retry.request),
                      "promotion changed the accepted proposal identity");
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "promotion changed transit ACK custody");
    zassert_mem_equal(&mesh_ch9_ack_table,
                      &ack_table_before,
                      sizeof(ack_table_before),
                      "promotion changed retained ACK bytes");
    cancel_ret = k_work_cancel_delayable(&mesh_event_negotiation_retry_work);
    zassert_true(cancel_ret >= 0, "promoted ACCEPT retry could not be canceled");
    k_sem_give(&route_preempt_blocker_release);
    zassert_ok(k_sem_take(&route_preempt_blocker_done, K_SECONDS(1)));

    mesh_c5_contact_exchange(
        peer_id,
        C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
        mesh_c5_exchange_expires_at(
            C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION),
        "promoted-accept-test");
    tracked_mesh_tx_capture_enabled = true;
    zassert_ok(mesh_event_accept_attempt("promoted-accept-test"));
    zassert_equal(tracked_mesh_tx_capture_calls, 1u,
                  "promoted ACCEPT did not reach real RF send seam");
    zassert_equal(uwb_mesh_frame_decode(
                      tracked_mesh_tx_capture_frame,
                      tracked_mesh_tx_capture_frame_len,
                      NETWORK_ID,
                      peer_id,
                      &transmitted_previous_hop,
                      &transmitted_packet,
                      transmitted_payload,
                      sizeof(transmitted_payload),
                      &transmitted_payload_len),
                  PROTO_OK);
    zassert_equal(transmitted_previous_hop, DEVICE_ID);
    zassert_equal(transmitted_packet.msg_type, MSG_MESH_EVENT_ACCEPT);
    zassert_equal(transmitted_packet.dst_id, peer_id);
    zassert_equal(transmitted_packet.session_id, accept_session);
    zassert_equal(transmitted_packet.seq, accept_sequence);
    zassert_true(transmitted_payload_len > 0u);
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "ACCEPT RF success changed transit ACK custody");
    zassert_mem_equal(&mesh_ch9_ack_table,
                      &ack_table_before,
                      sizeof(ack_table_before),
                      "ACCEPT RF success changed retained ACK bytes");
    zassert_false(mesh_event_accept_retry.retry.active,
                  "successful promoted ACCEPT retained duplicate retry custody");

    tracked_mesh_tx_capture_enabled = false;
    mesh_relay_cancel_tx(&mesh_runtime);
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_downstream_proposal_waits_for_live_upstream_survey_route_owner)
{
    const uint64_t child_id = UINT64_C(0x20000000000000b1);
    const struct mesh_outbound report = survey_discovery_report_for_test(
        UINT64_C(0x0000005100000001), UINT16_C(0x620));
    struct app_mesh_async_route_transfer_identity route_transfer;
    struct mesh_event_control_record proposal;
    struct mesh_pending_tx pending_before;
    uint32_t received_at_ms = 0u;

    report_owner_reset();
    event_accept_finish_reset();
    mesh_route_waiting_tx_valid = false;
    app_mesh_async_route_request_init(&mesh_route_discovery_request);
    start_pending_outbound_for_test(&report);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    mesh_runtime.pending.retry_after_ms = k_uptime_get_32();
    mesh_relay_invalidate_upstream_route(&mesh_runtime);
    route_transfer = (struct app_mesh_async_route_transfer_identity) {
        .target_id = GATEWAY_ID,
        .owner_generation = report.packet.session_id,
        .packet_seq = report.packet.seq,
        .msg_type = report.packet.msg_type,
        .owner_kind = APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING,
    };
    zassert_true(app_mesh_async_route_request_submit(
        &mesh_route_discovery_request,
        GATEWAY_ID,
        "survey-report-route-repair",
        k_uptime_get_32(),
        &route_transfer,
        NULL));
    pending_before = mesh_runtime.pending;
    event_proposal_for_test(child_id, &proposal, &received_at_ms);

    /* B already owns the local survey report whose immediate gateway repair
     * can occupy C's first receive slot. It must leave C's proposal
     * unacknowledged so the child retries after the upstream owner settles. */
    zassert_false(mesh_handle_event_control(&proposal.packet,
                                             proposal.payload,
                                             proposal.payload_len,
                                             child_id,
                                             received_at_ms));
    zassert_false(mesh_event_accept_retry.retry.active,
                  "rejected proposal retained EVENT_ACCEPT custody");
    zassert_is_null(event_timing_entry_for_test(child_id),
                    "rejected proposal installed a downstream cadence");
    zassert_is_null(mesh_event_owner_for_peer(child_id),
                    "rejected proposal committed a child event owner");

    zassert_true(mesh_relay_tx_active(&mesh_runtime));
    zassert_equal(mesh_runtime.pending.state, pending_before.state);
    zassert_equal(mesh_runtime.pending.packet.msg_type,
                  pending_before.packet.msg_type);
    zassert_equal(mesh_runtime.pending.packet.src_id,
                  pending_before.packet.src_id);
    zassert_equal(mesh_runtime.pending.packet.dst_id,
                  pending_before.packet.dst_id);
    zassert_equal(mesh_runtime.pending.packet.session_id,
                  pending_before.packet.session_id);
    zassert_equal(mesh_runtime.pending.packet.seq,
                  pending_before.packet.seq);
    zassert_equal(mesh_runtime.pending.payload_len,
                  pending_before.payload_len);
    zassert_mem_equal(mesh_runtime.pending.payload,
                      pending_before.payload,
                      pending_before.payload_len,
                      "proposal admission changed survey report custody");
    zassert_true(mesh_route_discovery_request.pending);
    zassert_equal(mesh_route_discovery_request.target_id, GATEWAY_ID);
    zassert_equal(mesh_route_discovery_request.transfer.owner_kind,
                  APP_MESH_ASYNC_ROUTE_TRANSFER_CORE_PENDING);
    zassert_equal(mesh_route_discovery_request.transfer.owner_generation,
                  report.packet.session_id);
    zassert_equal(mesh_route_discovery_request.transfer.packet_seq,
                  report.packet.seq);

    mesh_relay_cancel_tx(&mesh_runtime);
    app_mesh_async_route_request_init(&mesh_route_discovery_request);
    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_downstream_proposal_waits_for_local_command_result_route_owner)
{
    const uint64_t child_id = UINT64_C(0x20000000000000b2);
    struct mesh_event_control_record proposal;
    struct mesh_outbound waiting_before;
    uint32_t received_at_ms = 0u;

    report_owner_reset();
    event_accept_finish_reset();
    app_mesh_async_route_request_init(&mesh_route_discovery_request);
    mesh_route_waiting_tx = command_result_route_wait_for_test(DEVICE_ID);
    mesh_route_waiting_tx_valid = true;
    mesh_route_waiting_tx_owner =
        APP_MESH_ROUTE_WAIT_TX_OWNER_RETAINED_LOCAL;
    waiting_before = mesh_route_waiting_tx;
    event_proposal_for_test(child_id, &proposal, &received_at_ms);

    zassert_false(mesh_handle_event_control(&proposal.packet,
                                             proposal.payload,
                                             proposal.payload_len,
                                             child_id,
                                             received_at_ms));
    zassert_false(mesh_event_accept_retry.retry.active,
                  "local route owner retained EVENT_ACCEPT custody");
    zassert_is_null(event_timing_entry_for_test(child_id),
                    "local route owner allowed downstream cadence install");
    zassert_is_null(mesh_event_owner_for_peer(child_id),
                    "local route owner allowed downstream owner commit");
    zassert_true(mesh_route_waiting_tx_valid);
    zassert_equal(mesh_route_waiting_tx_owner,
                  APP_MESH_ROUTE_WAIT_TX_OWNER_RETAINED_LOCAL);
    zassert_mem_equal(&mesh_route_waiting_tx,
                      &waiting_before,
                      sizeof(waiting_before),
                      "proposal rejection changed local command-result custody");

    mesh_route_waiting_tx_valid = false;
    mesh_route_waiting_tx_owner = APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
    memset(&mesh_route_waiting_tx, 0, sizeof(mesh_route_waiting_tx));
    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_downstream_proposal_coexists_with_stable_upstream_survey_owner)
{
    enum { COUNTERPHASE_SHIFT_MS = MESH_EVENT_DEFAULT_INTERVAL_MS / 2u };
    const uint64_t parent_id = UINT64_C(0x20000000000000a4);
    const uint64_t child_id = UINT64_C(0x20000000000000b4);
    struct mesh_outbound report = survey_discovery_report_for_test(
        UINT64_C(0x0000005100000004), UINT16_C(0x624));
    const struct route_candidate *selected;
    struct route_candidate parent_route = {0};
    struct mesh_event_control_record proposal;
    struct mesh_event_control_record accepted_response;
    struct mesh_event_timing proposed;
    struct mesh_event_timing usable_upstream;
    struct mesh_event_timing downstream_reservation;
    struct mesh_relay_channel9_guard_status guard = {0};
    struct mesh_relay_event_timing_entry upstream_before;
    struct mesh_pending_tx pending_before;
    const struct mesh_relay_event_timing_entry *entry;
    uint32_t received_at_ms = 0u;
    uint32_t wire_reference_ms;
    uint16_t phase_shift_ms = 0u;

    report_owner_reset();
    event_accept_finish_reset();
    mesh_route_waiting_tx_valid = false;
    app_mesh_async_route_request_init(&mesh_route_discovery_request);
    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    parent_route = (struct route_candidate) {
        .next_hop_id = parent_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = mesh_runtime.upstream.current_epoch,
        .last_seen_ms = k_uptime_get_32(),
        .hop_count = 1u,
        .link_quality = 92u,
        .valid = true,
    };
    zassert_equal(route_upsert_candidate(&mesh_runtime.upstream,
                                         &parent_route),
                  PROTO_OK);
    report.next_hop_id = parent_id;
    event_proposal_for_test(child_id, &proposal, &received_at_ms);
    wire_reference_ms = mesh_event_control_rx_reference_ms(received_at_ms);
    zassert_equal(mesh_event_timing_from_tlvs_at(
                      &proposed,
                      proposal.payload,
                      proposal.payload_len,
                      wire_reference_ms,
                      true),
                  PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed, false);
    zassert_equal(mesh_install_channel9_timing_direction(
                      parent_id,
                      &proposed,
                      MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                      "stable-survey-upstream-fixture"),
                  PROTO_OK);
    selected = route_selected(&mesh_runtime.upstream);
    zassert_not_null(selected);
    zassert_equal(selected->next_hop_id, parent_id);
    zassert_true(selected->channel9_timing_valid);
    zassert_true(mesh_find_active_channel9_timing(
        selected->next_hop_id, k_uptime_get_32(), &usable_upstream));

    start_pending_outbound_for_test(&report);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    mesh_runtime.pending.retry_after_ms = k_uptime_get_32() + 1000u;
    zassert_equal(mesh_runtime.pending.next_hop_id, selected->next_hop_id);
    pending_before = mesh_runtime.pending;
    entry = event_timing_entry_for_test(selected->next_hop_id);
    zassert_not_null(entry);
    zassert_equal(entry->direction,
                  MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);
    upstream_before = *entry;

    zassert_true(mesh_handle_event_control(&proposal.packet,
                                            proposal.payload,
                                            proposal.payload_len,
                                            child_id,
                                            received_at_ms),
                 "stable upstream cadence did not admit child proposal");
    zassert_true(mesh_event_accept_retry.retry.active,
                 "admitted child proposal lost retained ACCEPT custody");
    zassert_true(mesh_event_accept_retry.response.valid);
    zassert_true(mesh_event_accept_phase_shift_from_payload(
        mesh_event_accept_retry.response.payload,
        mesh_event_accept_retry.response.payload_len,
        proposed.event_interval_ms,
        &phase_shift_ms));
    zassert_equal(phase_shift_ms, COUNTERPHASE_SHIFT_MS);
    zassert_true(app_mesh_c5_event_accept_reservation(
        &mesh_event_accept_retry.response.timing,
        MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS,
        &downstream_reservation));
    zassert_equal(mesh_relay_check_channel9_timing_guarded_direction(
                      &mesh_runtime,
                      child_id,
                      &downstream_reservation,
                      MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
                      MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                      &guard),
                  PROTO_OK,
                  "accepted child counterphase did not pass exact guard");
    zassert_is_null(event_timing_entry_for_test(child_id),
                    "failed ACCEPT RF installed downstream timing early");
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "proposal admission changed survey-report custody");
    entry = event_timing_entry_for_test(selected->next_hop_id);
    zassert_not_null(entry);
    zassert_mem_equal(entry,
                      &upstream_before,
                      sizeof(upstream_before),
                      "proposal admission changed upstream timing bytes");

    accepted_response = mesh_event_accept_retry.response;
    zassert_ok(mesh_event_accept_finish_send(
        &accepted_response.timing, "stable-survey-accept-test"));
    entry = event_timing_entry_for_test(child_id);
    zassert_not_null(entry);
    zassert_equal(entry->direction,
                  MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert_event_timing_equal(&entry->timing,
                              &accepted_response.timing);
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "ACCEPT commit changed survey-report custody");
    entry = event_timing_entry_for_test(selected->next_hop_id);
    zassert_not_null(entry);
    zassert_mem_equal(entry,
                      &upstream_before,
                      sizeof(upstream_before),
                      "ACCEPT commit changed upstream timing bytes");

    mesh_relay_cancel_tx(&mesh_runtime);
    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_downstream_proposal_allows_same_peer_transit_ack_route_owner)
{
    const uint64_t child_id = UINT64_C(0x20000000000000b3);
    struct mesh_event_control_record proposal;
    struct mesh_outbound waiting_before;
    uint32_t received_at_ms = 0u;

    report_owner_reset();
    event_accept_finish_reset();
    app_mesh_async_route_request_init(&mesh_route_discovery_request);
    mesh_route_waiting_tx =
        same_peer_gateway_ack_route_wait_for_test(child_id);
    mesh_route_waiting_tx_valid = true;
    mesh_route_waiting_tx_owner =
        APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK;
    waiting_before = mesh_route_waiting_tx;
    event_proposal_for_test(child_id, &proposal, &received_at_ms);

    zassert_true(mesh_event_accept_downstream_admission_allowed(
        child_id, &proposal.timing));
    zassert_true(mesh_handle_event_control(&proposal.packet,
                                            proposal.payload,
                                            proposal.payload_len,
                                            child_id,
                                            received_at_ms),
                 "same-peer transit ACK was blanket-rejected");
    zassert_true(mesh_event_accept_retry.retry.active ||
                     event_timing_entry_for_test(child_id) != NULL ||
                     mesh_event_owner_for_peer(child_id) != NULL,
                 "allowed proposal created no ACCEPT/install custody");
    zassert_true(mesh_route_waiting_tx_valid);
    zassert_equal(mesh_route_waiting_tx_owner,
                  APP_MESH_ROUTE_WAIT_TX_OWNER_TRANSIT_GATEWAY_ACK);
    zassert_mem_equal(&mesh_route_waiting_tx,
                      &waiting_before,
                      sizeof(waiting_before),
                      "proposal admission changed transit ACK custody");

    mesh_route_waiting_tx_valid = false;
    mesh_route_waiting_tx_owner = APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
    memset(&mesh_route_waiting_tx, 0, sizeof(mesh_route_waiting_tx));
    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_conflicting_child_proposal_negotiates_immutable_counterphase)
{
    enum { COUNTERPHASE_SHIFT_MS = MESH_EVENT_DEFAULT_INTERVAL_MS / 2u };
    const uint64_t parent_id = UINT64_C(0x20000000000000c1);
    const uint64_t child_id = UINT64_C(0x20000000000000c2);
    struct mesh_event_control_record proposal;
    struct mesh_event_control_record accepted_response;
    struct app_mesh_event_request_identity expected_request;
    struct mesh_event_timing proposed;
    struct mesh_event_timing expected_downstream;
    struct mesh_relay_event_timing_entry upstream_before;
    struct mesh_relay_event_timing_entry downstream_before_replay;
    const struct mesh_relay_event_timing_entry *entry;
    uint8_t upstream_wire_before[96];
    uint8_t upstream_wire_after[96];
    size_t upstream_wire_before_len = 0u;
    size_t upstream_wire_after_len = 0u;
    uint32_t received_at_ms = 0u;
    uint32_t wire_reference_ms;
    uint16_t phase_shift_ms = 0u;

    report_owner_reset();
    event_accept_finish_reset();
    event_proposal_for_test(child_id, &proposal, &received_at_ms);
    wire_reference_ms = mesh_event_control_rx_reference_ms(received_at_ms);
    zassert_equal(mesh_event_timing_from_tlvs_at(
                      &proposed,
                      proposal.payload,
                      proposal.payload_len,
                      wire_reference_ms,
                      true),
                  PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed, false);
    zassert_equal(mesh_install_channel9_timing_direction(
                      parent_id,
                      &proposed,
                      MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                      "counterphase-upstream-fixture"),
                  PROTO_OK);
    entry = event_timing_entry_for_test(parent_id);
    zassert_not_null(entry);
    upstream_before = *entry;
    zassert_equal(mesh_append_event_timing_tlvs_at(
                      upstream_wire_before,
                      sizeof(upstream_wire_before),
                      &upstream_wire_before_len,
                      &entry->timing,
                      wire_reference_ms),
                  PROTO_OK);

    expected_downstream = proposed;
    zassert_true(app_mesh_event_timing_apply_phase_shift(
        &expected_downstream, COUNTERPHASE_SHIFT_MS));
    expected_request = mesh_event_request_identity(
        &proposal.packet, proposal.payload, proposal.payload_len);

    zassert_true(mesh_handle_event_control(&proposal.packet,
                                            proposal.payload,
                                            proposal.payload_len,
                                            child_id,
                                            received_at_ms));
    zassert_true(mesh_event_accept_retry.retry.active,
                 "failed RF stub did not retain ACCEPT custody");
    zassert_true(mesh_event_accept_retry.response.valid);
    zassert_equal(mesh_event_accept_retry.response.packet.msg_type,
                  MSG_MESH_EVENT_ACCEPT);
    zassert_equal(mesh_event_accept_retry.response.packet.src_id, DEVICE_ID);
    zassert_equal(mesh_event_accept_retry.response.packet.dst_id, child_id);
    zassert_equal(mesh_event_accept_retry.response.packet.session_id,
                  proposal.packet.session_id);
    zassert_equal(mesh_event_accept_retry.response.packet.seq,
                  proposal.packet.seq);
    zassert_true(mesh_event_accept_phase_shift_from_payload(
        mesh_event_accept_retry.response.payload,
        mesh_event_accept_retry.response.payload_len,
        proposed.event_interval_ms,
        &phase_shift_ms));
    zassert_equal(phase_shift_ms, COUNTERPHASE_SHIFT_MS);
    assert_event_timing_equal(&mesh_event_accept_retry.response.timing,
                              &expected_downstream);
    zassert_equal(mesh_event_accept_retry.retry.request.source_id,
                  expected_request.source_id);
    zassert_equal(mesh_event_accept_retry.retry.request.session_id,
                  expected_request.session_id);
    zassert_equal(mesh_event_accept_retry.retry.request.sequence,
                  expected_request.sequence);
    zassert_equal(mesh_event_accept_retry.retry.request.message_type,
                  expected_request.message_type);
    zassert_mem_equal(mesh_event_accept_retry.retry.request.payload_digest,
                      expected_request.payload_digest,
                      sizeof(expected_request.payload_digest),
                      "counterproposal lost the exact PROPOSE digest");
    zassert_is_null(event_timing_entry_for_test(child_id),
                    "failed ACCEPT RF installed downstream timing early");

    entry = event_timing_entry_for_test(parent_id);
    zassert_not_null(entry);
    assert_event_timing_equal(&entry->timing, &upstream_before.timing);
    zassert_equal(entry->direction, upstream_before.direction);
    zassert_equal(mesh_append_event_timing_tlvs_at(
                      upstream_wire_after,
                      sizeof(upstream_wire_after),
                      &upstream_wire_after_len,
                      &entry->timing,
                      wire_reference_ms),
                  PROTO_OK);
    zassert_equal(upstream_wire_after_len, upstream_wire_before_len);
    zassert_mem_equal(upstream_wire_after,
                      upstream_wire_before,
                      upstream_wire_before_len,
                      "counterproposal changed upstream timing bytes");

    accepted_response = mesh_event_accept_retry.response;
    zassert_ok(mesh_event_accept_finish_send(
        &accepted_response.timing, "counterphase-accept-test"));
    entry = event_timing_entry_for_test(child_id);
    zassert_not_null(entry);
    zassert_equal(entry->direction,
                  MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert_event_timing_equal(&entry->timing, &expected_downstream);
    downstream_before_replay = *entry;

    zassert_true(mesh_handle_event_control(&proposal.packet,
                                            proposal.payload,
                                            proposal.payload_len,
                                            child_id,
                                            received_at_ms + 37u));
    zassert_true(mesh_event_accept_retry.retry.active);
    zassert_true(mesh_event_accept_retry.replay_existing_response);
    zassert_equal(mesh_event_accept_retry.response.payload_len,
                  accepted_response.payload_len);
    zassert_mem_equal(mesh_event_accept_retry.response.payload,
                      accepted_response.payload,
                      accepted_response.payload_len,
                      "duplicate proposal moved immutable ACCEPT bytes");
    entry = event_timing_entry_for_test(child_id);
    zassert_not_null(entry);
    zassert_equal(entry->direction, downstream_before_replay.direction);
    assert_event_timing_equal(&entry->timing,
                              &downstream_before_replay.timing);

    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_proposer_applies_counterphase_once_and_replay_is_inert)
{
    enum { COUNTERPHASE_SHIFT_MS = MESH_EVENT_DEFAULT_INTERVAL_MS / 2u };
    const uint64_t peer_id = UINT64_C(0x20000000000000c3);
    struct mesh_event_control_record accept;
    struct mesh_event_control_record malformed;
    struct mesh_event_timing proposed = event_accept_timing_for_test(
        k_uptime_get_32() + 2000u);
    struct mesh_event_timing frozen;
    struct mesh_event_timing expected;
    struct mesh_relay_event_timing_entry installed_before_replay;
    const struct mesh_relay_event_timing_entry *installed;
    const uint8_t *phase_raw = NULL;
    uint8_t phase_raw_len = 0u;
    uint32_t received_at_ms = 0u;
    size_t malformed_len;
    size_t phase_value_offset;

    report_owner_reset();
    event_accept_finish_reset();
    local_event_proposal_for_test(peer_id, &proposed);
    frozen = mesh_event_propose_record.timing;
    expected = frozen;
    zassert_true(app_mesh_event_timing_apply_phase_shift(
        &expected, COUNTERPHASE_SHIFT_MS));
    mesh_event_timing_set_local_first_slot_tx(&expected, true);
    event_accept_for_local_proposal(peer_id,
                                    COUNTERPHASE_SHIFT_MS,
                                    &accept,
                                    &received_at_ms);

    malformed = accept;
    malformed_len = malformed.payload_len;
    zassert_equal(tlv_append_u16(
                      malformed.payload,
                      sizeof(malformed.payload),
                      &malformed_len,
                      TLV_MESH_EVENT_PHASE_SHIFT_MS,
                      COUNTERPHASE_SHIFT_MS),
                  PROTO_OK);
    zassert_true(malformed_len <= UINT8_MAX);
    malformed.payload_len = (uint8_t)malformed_len;
    malformed.packet.payload_len = (uint16_t)malformed_len;
    zassert_equal(mesh_packet_rx_envelope_validate(
                      &malformed.packet,
                      malformed.payload,
                      malformed.payload_len,
                      peer_id,
                      DEVICE_ID,
                      GATEWAY_ID,
                      UWB_CHANNEL_WAKE_CONTACT,
                      false),
                  PROTO_ERR_MALFORMED);
    zassert_false(mesh_handle_event_control(&malformed.packet,
                                             malformed.payload,
                                             malformed.payload_len,
                                             peer_id,
                                             received_at_ms));
    zassert_true(mesh_event_propose_record.valid);
    zassert_true(mesh_event_propose_retry.active);

    malformed = accept;
    zassert_equal(tlv_find_unique(malformed.payload,
                                  malformed.payload_len,
                                  TLV_MESH_EVENT_PHASE_SHIFT_MS,
                                  &phase_raw,
                                  &phase_raw_len),
                  PROTO_OK);
    zassert_equal(phase_raw_len, sizeof(uint16_t));
    phase_value_offset = (size_t)(phase_raw - malformed.payload);
    proto_put_u16_le(&malformed.payload[phase_value_offset],
                     MESH_EVENT_DEFAULT_INTERVAL_MS);
    zassert_equal(mesh_packet_rx_envelope_validate(
                      &malformed.packet,
                      malformed.payload,
                      malformed.payload_len,
                      peer_id,
                      DEVICE_ID,
                      GATEWAY_ID,
                      UWB_CHANNEL_WAKE_CONTACT,
                      false),
                  PROTO_ERR_MALFORMED);
    zassert_false(mesh_handle_event_control(&malformed.packet,
                                             malformed.payload,
                                             malformed.payload_len,
                                             peer_id,
                                             received_at_ms));
    zassert_true(mesh_event_propose_record.valid);
    zassert_true(mesh_event_propose_retry.active);

    malformed = accept;
    zassert_equal(tlv_find_unique(malformed.payload,
                                  malformed.payload_len,
                                  TLV_MESH_EVENT_PHASE_SHIFT_MS,
                                  &phase_raw,
                                  &phase_raw_len),
                  PROTO_OK);
    phase_value_offset = (size_t)(phase_raw - malformed.payload);
    zassert_true(phase_value_offset >= PROTO_TLV_HEADER_LEN);
    malformed.payload[phase_value_offset - 1u] = 1u;
    zassert_equal(mesh_packet_rx_envelope_validate(
                      &malformed.packet,
                      malformed.payload,
                      malformed.payload_len,
                      peer_id,
                      DEVICE_ID,
                      GATEWAY_ID,
                      UWB_CHANNEL_WAKE_CONTACT,
                      false),
                  PROTO_ERR_MALFORMED);
    zassert_false(mesh_handle_event_control(&malformed.packet,
                                             malformed.payload,
                                             malformed.payload_len,
                                             peer_id,
                                             received_at_ms));
    zassert_true(mesh_event_propose_record.valid);
    zassert_true(mesh_event_propose_retry.active);

    zassert_equal(mesh_packet_rx_envelope_validate(
                      &accept.packet,
                      accept.payload,
                      accept.payload_len,
                      peer_id,
                      DEVICE_ID,
                      GATEWAY_ID,
                      UWB_CHANNEL_WAKE_CONTACT,
                      false),
                  PROTO_OK);

    zassert_true(mesh_handle_event_control(&accept.packet,
                                            accept.payload,
                                            accept.payload_len,
                                            peer_id,
                                            received_at_ms));
    installed = event_timing_entry_for_test(peer_id);
    zassert_not_null(installed);
    zassert_equal(installed->direction,
                  MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);
    assert_event_timing_equal(&installed->timing, &expected);
    zassert_false(mesh_event_propose_record.valid);
    zassert_false(mesh_event_propose_retry.active);
    installed_before_replay = *installed;

    zassert_true(mesh_handle_event_control(&accept.packet,
                                            accept.payload,
                                            accept.payload_len,
                                            peer_id,
                                            received_at_ms + 91u));
    installed = event_timing_entry_for_test(peer_id);
    zassert_not_null(installed);
    zassert_equal(installed->direction, installed_before_replay.direction);
    assert_event_timing_equal(&installed->timing,
                              &installed_before_replay.timing);

    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_absent_accept_phase_shift_preserves_frozen_proposal)
{
    const uint64_t peer_id = UINT64_C(0x20000000000000c4);
    struct mesh_event_control_record accept;
    struct mesh_event_timing proposed = event_accept_timing_for_test(
        k_uptime_get_32() + 2000u);
    struct mesh_event_timing expected;
    const struct mesh_relay_event_timing_entry *installed;
    const uint8_t *shift_raw = NULL;
    uint8_t shift_raw_len = 0u;
    uint16_t parsed_shift_ms = UINT16_MAX;
    uint32_t received_at_ms = 0u;

    report_owner_reset();
    event_accept_finish_reset();
    local_event_proposal_for_test(peer_id, &proposed);
    expected = mesh_event_propose_record.timing;
    mesh_event_timing_set_local_first_slot_tx(&expected, true);
    event_accept_for_local_proposal(peer_id, 0u, &accept, &received_at_ms);

    zassert_equal(tlv_find_unique(accept.payload,
                                  accept.payload_len,
                                  TLV_MESH_EVENT_PHASE_SHIFT_MS,
                                  &shift_raw,
                                  &shift_raw_len),
                  PROTO_ERR_NOT_FOUND);
    zassert_true(mesh_event_accept_phase_shift_from_payload(
        accept.payload,
        accept.payload_len,
        expected.event_interval_ms,
        &parsed_shift_ms));
    zassert_equal(parsed_shift_ms, 0u);
    zassert_true(mesh_handle_event_control(&accept.packet,
                                            accept.payload,
                                            accept.payload_len,
                                            peer_id,
                                            received_at_ms));
    installed = event_timing_entry_for_test(peer_id);
    zassert_not_null(installed);
    zassert_equal(installed->direction,
                  MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);
    assert_event_timing_equal(&installed->timing, &expected);

    event_accept_finish_reset();
}

ZTEST(production_seam_report_custody,
      test_successful_accept_preserves_proposed_phase_after_delayed_transmit)
{
    const uint64_t peer_id = UINT64_C(0x20000000000000a1);
    uint32_t base_ms = k_uptime_get_32() + 60000u;
    struct mesh_event_timing pre_send =
        event_accept_timing_for_test(base_ms);
    struct mesh_event_timing transmitted = pre_send;
    const struct mesh_relay_event_timing_entry *installed;
    struct mesh_event_owner *owner;

    transmitted.next_event_time_ms +=
        MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS + 37u;
    transmitted.last_successful_ch9_event_ms =
        transmitted.next_event_time_ms;

    report_owner_reset();
    event_accept_finish_reset();
    event_accept_finish_prepare(peer_id, &pre_send);

    zassert_true(transmitted.next_event_time_ms -
                     pre_send.next_event_time_ms >
                     MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS,
                 "fixture did not escape the obsolete reservation slop");
    zassert_ok(mesh_event_accept_finish_send(&transmitted,
                                             "reanchored-accept-test"));

    installed = event_timing_entry_for_test(peer_id);
    zassert_not_null(installed,
                     "successful ACCEPT did not install responder timing");
    assert_event_timing_equal(&installed->timing, &pre_send);
    zassert_equal(installed->direction,
                  MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    owner = mesh_event_owner_for_peer(peer_id);
    zassert_not_null(owner, "successful timing install did not commit owner");
    zassert_true(owner->active);
    zassert_true(owner->proposal_from_peer);
    zassert_false(mesh_event_accept_retry.retry.active);

    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    mesh_uwb_rx_active = false;
}

ZTEST(production_seam_report_custody,
      test_successful_accept_preserves_safe_proposed_phase_despite_transmit_conflict)
{
    const uint64_t peer_id = UINT64_C(0x20000000000000a2);
    uint32_t base_ms = k_uptime_get_32() + 60000u;
    struct mesh_event_timing upstream =
        event_accept_timing_for_test(base_ms);
    struct mesh_event_timing pre_send =
        event_accept_timing_for_test(base_ms);
    struct mesh_event_timing transmitted;
    struct mesh_relay_channel9_guard_status guard = {0};
    const struct mesh_relay_event_timing_entry *installed_upstream;
    bool installable_pre_send_found = false;
    int ret;

    report_owner_reset();
    event_accept_finish_reset();
    zassert_equal(mesh_install_channel9_timing_direction(
                      GATEWAY_ID,
                      &upstream,
                      MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                      "event-accept-conflict-fixture"),
                  PROTO_OK);
    for (uint32_t phase_offset_ms = 1u;
         phase_offset_ms < pre_send.event_interval_ms;
         phase_offset_ms++) {
        pre_send.next_event_time_ms = base_ms + phase_offset_ms;
        pre_send.last_successful_ch9_event_ms =
            pre_send.next_event_time_ms;
        ret = mesh_relay_check_channel9_timing_guarded_direction(
            &mesh_runtime,
            peer_id,
            &pre_send,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
            MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
            &guard);
        if (ret == PROTO_OK) {
            installable_pre_send_found = true;
            break;
        }
    }
    zassert_true(installable_pre_send_found,
                 "fixture could not find a valid second cadence phase");
    transmitted = pre_send;
    transmitted.next_event_time_ms = upstream.next_event_time_ms;
    transmitted.last_successful_ch9_event_ms =
        transmitted.next_event_time_ms;
    zassert_equal(mesh_relay_check_channel9_timing_guarded_direction(
                      &mesh_runtime,
                      peer_id,
                      &transmitted,
                      MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
                      MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                      &guard),
                  PROTO_ERR_BUSY,
                  "reanchored fixture did not conflict with live cadence");
    zassert_equal(guard.reason,
                  MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT);
    event_accept_finish_prepare(peer_id, &pre_send);

    ret = mesh_event_accept_finish_send(&transmitted,
                                        "reanchored-conflict-test");
    zassert_ok(ret,
               "transmit-time reanchor incorrectly displaced the accepted proposal phase");
    zassert_not_null(event_timing_entry_for_test(peer_id));
    assert_event_timing_equal(&event_timing_entry_for_test(peer_id)->timing,
                              &pre_send);
    zassert_not_null(mesh_event_owner_for_peer(peer_id),
                     "proposal-phase timing did not commit its owner");
    installed_upstream = event_timing_entry_for_test(GATEWAY_ID);
    zassert_not_null(installed_upstream,
                     "conflict rejection disturbed existing cadence");
    assert_event_timing_equal(&installed_upstream->timing, &upstream);
}

ZTEST(production_seam_report_custody,
      test_successful_accept_defers_overdue_first_occurrence_by_whole_cadence)
{
    const uint64_t peer_id = UINT64_C(0x20000000000000a3);
    struct mesh_event_timing proposed =
        event_accept_timing_for_test(k_uptime_get_32() + 1u);
    struct mesh_event_timing transmitted = proposed;
    struct mesh_event_timing expected = proposed;
    const struct mesh_relay_event_timing_entry *installed;
    bool proposed_local_tx = mesh_event_timing_local_tx_slot(&proposed);

    transmitted.next_event_time_ms += 500u;
    transmitted.last_successful_ch9_event_ms =
        transmitted.next_event_time_ms;
    expected.next_event_time_ms += expected.event_interval_ms;
    expected.last_successful_ch9_event_ms += expected.event_interval_ms;
    expected.event_counter++;

    report_owner_reset();
    event_accept_finish_reset();
    event_accept_finish_prepare(peer_id, &proposed);

    zassert_ok(mesh_event_accept_finish_send(&transmitted,
                                             "overdue-proposal-phase-test"));
    installed = event_timing_entry_for_test(peer_id);
    zassert_not_null(installed);
    assert_event_timing_equal(&installed->timing, &expected);
    zassert_equal(installed->timing.event_counter,
                  proposed.event_counter + 1u,
                  "skipped physical occurrence did not advance parity");
    zassert_not_equal(mesh_event_timing_local_tx_slot(&installed->timing),
                      proposed_local_tx,
                      "one skipped occurrence did not reverse the local turn");

    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    mesh_uwb_rx_active = false;
}

ZTEST(production_seam_report_custody,
      test_asymmetric_accept_boundary_converges_on_same_phase_and_counter)
{
    struct mesh_event_timing proposer =
        event_accept_timing_for_test(k_uptime_get_32() + 60000u);
    struct mesh_event_timing responder = proposer;
    uint32_t original_start_ms = proposer.next_event_time_ms;
    uint32_t original_prepare_ms = mesh_channel9_prepare_start_ms(&proposer);

    mesh_event_timing_set_local_first_slot_tx(&proposer, true);
    mesh_event_timing_set_local_first_slot_tx(&responder, false);

    /* The responder crosses the preparation boundary while the proposer is
     * still on its original occurrence. This is the exact hardware escape:
     * only one side initially advances by a whole cadence. */
    mesh_event_timing_defer_first_start_if_needed(
        &proposer, original_prepare_ms - 1u);
    mesh_event_timing_defer_first_start_if_needed(
        &responder, original_prepare_ms);

    zassert_equal(proposer.next_event_time_ms, original_start_ms);
    zassert_equal(responder.next_event_time_ms,
                  original_start_ms + responder.event_interval_ms);
    zassert_equal(responder.event_counter,
                  proposer.event_counter + 1u);

    /* Once the earlier peer services or skips its unmatched occurrence, both
     * peers name the same physical event and retain complementary turns. */
    mesh_event_note_unobserved_turn(&proposer, original_start_ms);
    zassert_equal(proposer.next_event_time_ms,
                  responder.next_event_time_ms);
    zassert_equal(proposer.event_counter, responder.event_counter);
    zassert_not_equal(mesh_event_timing_local_tx_slot(&proposer),
                      mesh_event_timing_local_tx_slot(&responder));
}

ZTEST(production_seam_report_custody,
      test_event_control_uses_physical_rx_time_after_queue_age_refresh)
{
    struct mesh_rx_pending pending = {
        .first_received_at_ms = UINT64_C(0x0000000112345678),
        .received_at_ms = 100u,
        .received_at_valid = true,
    };

    mesh_rx_pending_refresh_age(&pending, 250u);

    zassert_equal(pending.received_at_ms, 250u);
    zassert_equal(mesh_rx_pending_physical_received_at_ms(&pending),
                  UINT32_C(0x12345678));
}

ZTEST(production_seam_report_custody,
      test_gateway_batch_queue_commit_repairs_reverse_route_before_control)
{
    const uint64_t ingress_parent_id = UINT64_C(0x2000000000000b41);
    const uint64_t forced2_id = UINT64_C(0x2000000000000c41);
    struct mesh_outbound child_report =
        survey_discovery_report_from_for_test(
            forced2_id,
            UINT64_C(0x0000005300000041),
            UINT16_C(0x5341));
    struct mesh_outbound targeted_control =
        targeted_pair_prepare_for_test(forced2_id, UINT16_C(0x5342));
    struct mesh_outbound queued_before_control = {0};
    struct mesh_outbound queued_after_control = {0};
    struct mesh_relay_result report_result = {0};
    struct mesh_relay_result control_result = {0};
    struct mesh_rx_pending rx = {0};
    const struct mesh_downlink_entry *selected;
    const struct app_mesh_ch9_ack_batch *ack_batch;
    uint32_t now_ms;
    bool gateway_ack_handed_off = false;

    report_owner_reset();
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    anchor_uwb_busy = true;
    now_ms = k_uptime_get_32();
    seed_stale_direct_reverse_route_for_test(forced2_id, now_ms);
    selected = mesh_relay_find_downlink(&mesh_runtime, forced2_id);
    zassert_not_null(selected);
    zassert_equal(selected->next_hop_id, forced2_id);

    /* C originated at the next downstream edge, so B's forwarded copy has
     * consumed one TTL before the direct anchor A sees it. */
    child_report.packet.ttl = MESH_DEFAULT_TTL - 1u;
    zassert_equal(mesh_relay_handle_rx_with_random(
                      &mesh_runtime,
                      &child_report.packet,
                      child_report.payload,
                      child_report.payload_len,
                      ingress_parent_id,
                      94u,
                      now_ms,
                      UINT32_C(0x53410001),
                      &report_result),
                  PROTO_OK);
    zassert_equal(report_result.status, PROTO_OK);
    zassert_true((report_result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    zassert_true((report_result.actions & MESH_RELAY_ACTION_SEND_HOP_ACK) !=
                 0u);
    zassert_equal(report_result.forward.next_hop_id, GATEWAY_ID);
    zassert_equal(report_result.forward.ingress_previous_hop_id,
                  ingress_parent_id);

    rx.packet = child_report.packet;
    memcpy(rx.payload, child_report.payload, child_report.payload_len);
    rx.payload_len = child_report.payload_len;
    rx.previous_hop_id = ingress_parent_id;
    rx.first_received_at_ms = now_ms;
    rx.received_at_ms = now_ms;
    rx.link_quality = 94u;
    rx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    rx.received_at_valid = true;
    mesh_handle_result_actions(&report_result,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               &rx,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               &gateway_ack_handed_off);

    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u,
                  "successful batch admission did not retain report custody");
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued_before_control));
    zassert_equal(queued_before_control.packet.msg_type,
                  report_result.forward.packet.msg_type);
    zassert_equal(queued_before_control.packet.flags,
                  report_result.forward.packet.flags);
    zassert_equal(queued_before_control.packet.src_id, forced2_id);
    zassert_equal(queued_before_control.packet.dst_id, GATEWAY_ID);
    zassert_equal(queued_before_control.packet.session_id,
                  report_result.forward.packet.session_id);
    zassert_equal(queued_before_control.packet.seq,
                  report_result.forward.packet.seq);
    zassert_equal(queued_before_control.packet.ttl,
                  report_result.forward.packet.ttl);
    zassert_equal(queued_before_control.payload_len,
                  report_result.forward.payload_len);
    zassert_mem_equal(queued_before_control.payload,
                      report_result.forward.payload,
                      report_result.forward.payload_len,
                      "batch queue mutated retained survey report bytes");
    zassert_equal(queued_before_control.ingress_previous_hop_id,
                  ingress_parent_id);

    selected = mesh_relay_find_downlink(&mesh_runtime, forced2_id);
    zassert_not_null(selected);
    zassert_equal(selected->next_hop_id, ingress_parent_id,
                  "queued transit custody did not replace stale direct route");
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(&mesh_runtime); i++) {
        const struct mesh_downlink_entry *candidate =
            mesh_relay_downlink_at(&mesh_runtime, i);

        zassert_not_null(candidate);
        zassert_true(!candidate->valid ||
                     candidate->target_id != forced2_id ||
                     candidate->next_hop_id != forced2_id,
                     "stale C-to-C reverse hint survived queue commit");
    }

    ack_batch = app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table,
                                                 ingress_parent_id);
    zassert_not_null(ack_batch,
                     "successful report custody did not retain child ACK");
    zassert_true(ack_batch->valid);
    zassert_equal(ack_batch->peer_id, ingress_parent_id);
    zassert_equal(ack_batch->count, 1u);
    zassert_equal(ack_batch->template_ack.packet.msg_type, MSG_MESH_HOP_ACK);
    zassert_equal(ack_batch->template_ack.next_hop_id, ingress_parent_id);
    zassert_equal(ack_batch->entries[0].session_id,
                  child_report.packet.session_id);
    zassert_equal(ack_batch->entries[0].seq, child_report.packet.seq);

    zassert_equal(mesh_relay_handle_rx(&mesh_runtime,
                                       &targeted_control.packet,
                                       targeted_control.payload,
                                       targeted_control.payload_len,
                                       GATEWAY_ID,
                                       96u,
                                       now_ms + 10u,
                                       &control_result),
                  PROTO_OK);
    zassert_equal(control_result.status, PROTO_OK);
    zassert_true((control_result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    zassert_equal(control_result.forward.next_hop_id, ingress_parent_id,
                  "later targeted control still bypassed forced1");
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued_after_control));
    zassert_mem_equal(&queued_after_control,
                      &queued_before_control,
                      sizeof(queued_before_control),
                      "targeted control changed queued report custody");
    ack_batch = app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table,
                                                 ingress_parent_id);
    zassert_not_null(ack_batch);
    zassert_equal(ack_batch->count, 1u);
    zassert_equal(ack_batch->entries[0].session_id,
                  child_report.packet.session_id);
    zassert_equal(ack_batch->entries[0].seq, child_report.packet.seq);
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_gateway_batch_queue_failure_preserves_stale_route_and_child_custody)
{
    const uint64_t ingress_parent_id = UINT64_C(0x2000000000000b42);
    const uint64_t forced2_id = UINT64_C(0x2000000000000c42);
    struct mesh_outbound child_report =
        survey_discovery_report_from_for_test(
            forced2_id,
            UINT64_C(0x0000005300000042),
            UINT16_C(0x5343));
    struct mesh_outbound queued_before[REPORT_TX_QUEUE_DEPTH];
    struct mesh_outbound queued_after = {0};
    struct mesh_relay_result report_result = {0};
    struct mesh_rx_pending rx = {0};
    const struct mesh_downlink_entry *selected;
    uint32_t now_ms;
    bool gateway_ack_handed_off = false;

    report_owner_reset();
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    anchor_uwb_busy = true;
    now_ms = k_uptime_get_32();
    seed_stale_direct_reverse_route_for_test(forced2_id, now_ms);
    for (uint16_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        queued_before[i] = queued_report(DEVICE_ID,
                                         (uint16_t)(0x5350u + i));
        zassert_ok(k_msgq_put(&report_tx_msgq,
                              &queued_before[i],
                              K_NO_WAIT));
    }

    child_report.packet.ttl = MESH_DEFAULT_TTL - 1u;
    zassert_equal(mesh_relay_handle_rx_with_random(
                      &mesh_runtime,
                      &child_report.packet,
                      child_report.payload,
                      child_report.payload_len,
                      ingress_parent_id,
                      94u,
                      now_ms,
                      UINT32_C(0x53420001),
                      &report_result),
                  PROTO_OK);
    zassert_equal(report_result.status, PROTO_OK);
    zassert_true((report_result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    zassert_true((report_result.actions & MESH_RELAY_ACTION_SEND_HOP_ACK) !=
                 0u);
    rx.packet = child_report.packet;
    memcpy(rx.payload, child_report.payload, child_report.payload_len);
    rx.payload_len = child_report.payload_len;
    rx.previous_hop_id = ingress_parent_id;
    rx.first_received_at_ms = now_ms;
    rx.received_at_ms = now_ms;
    rx.link_quality = 94u;
    rx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    rx.received_at_valid = true;
    mesh_handle_result_actions(&report_result,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               &rx,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               &gateway_ack_handed_off);

    selected = mesh_relay_find_downlink(&mesh_runtime, forced2_id);
    zassert_not_null(selected);
    zassert_equal(selected->next_hop_id, forced2_id,
                  "failed queue admission changed stale reverse route");
    zassert_false(app_mesh_ch9_ack_table_pending_for_peer(
                      &mesh_ch9_ack_table, ingress_parent_id),
                  "failed parent custody falsely ACKed the child");
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq),
                  REPORT_TX_QUEUE_DEPTH);
    zassert_false(report_tx_queue_recovery_valid);
    for (uint16_t i = 0u; i < REPORT_TX_QUEUE_DEPTH; i++) {
        zassert_ok(k_msgq_get(&report_tx_msgq,
                              &queued_after,
                              K_NO_WAIT));
        zassert_mem_equal(&queued_after,
                          &queued_before[i],
                          sizeof(queued_after),
                          "failed batch admission changed existing queue bytes");
    }
    zassert_false(mesh_relay_tx_active(&mesh_runtime));
    zassert_equal(watchdog_stop_calls, 0u);
}

ZTEST(production_seam_report_custody,
      test_busy_parent_queues_child_survey_without_route_repair)
{
    const uint64_t parent_id = UINT64_C(0x2000000000000ce1);
    const uint64_t child_id = UINT64_C(0x2000000000000ce2);
    struct mesh_outbound local_report =
        survey_discovery_report_for_test(
            UINT64_C(0x00000052000000e1), UINT16_C(0x6ce1));
    struct mesh_outbound child_report =
        survey_discovery_report_from_for_test(
            child_id,
            UINT64_C(0x00000052000000e2),
            UINT16_C(0x6ce2));
    struct route_candidate parent_route = {0};
    struct mesh_event_timing parent_timing;
    struct mesh_relay_result result = {0};
    struct mesh_rx_pending rx = {0};
    struct mesh_pending_tx pending_before;
    struct mesh_outbound queued = {0};
    const struct app_mesh_ch9_ack_batch *ack_batch;
    uint32_t now_ms;
    bool gateway_ack_handed_off = false;

    report_owner_reset();
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    anchor_uwb_busy = true;
    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    now_ms = k_uptime_get_32();
    parent_route = (struct route_candidate) {
        .next_hop_id = parent_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = mesh_runtime.upstream.current_epoch,
        .last_seen_ms = now_ms,
        .hop_count = 1u,
        .link_quality = 94u,
        .valid = true,
    };
    zassert_equal(route_upsert_candidate(&mesh_runtime.upstream,
                                         &parent_route),
                  PROTO_OK);
    parent_timing = event_accept_timing_for_test(
        now_ms + MESH_EVENT_DEFAULT_GUARD_MS);
    zassert_equal(mesh_install_channel9_timing_direction(
                      parent_id,
                      &parent_timing,
                      MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                      "busy-parent-queue-fixture"),
                  PROTO_OK);
    local_report.next_hop_id = parent_id;
    start_pending_outbound_for_test(&local_report);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    mesh_runtime.pending.gateway_ack_deadline_ms = now_ms + 60000u;
    pending_before = mesh_runtime.pending;

    zassert_equal(mesh_packet_rx_semantics_validate(
                      &child_report.packet,
                      child_report.payload,
                      child_report.payload_len,
                      child_id,
                      mesh_runtime.local_id,
                      mesh_runtime.gateway_id),
                  PROTO_OK,
                  "child discovery report envelope is not canonical");
    zassert_equal(mesh_relay_handle_rx_with_random(
                      &mesh_runtime,
                      &child_report.packet,
                      child_report.payload,
                      child_report.payload_len,
                      child_id,
                      95u,
                      now_ms + 1u,
                      UINT32_C(0x6ce20001),
                      &result),
                  PROTO_OK);
    zassert_equal(result.status, PROTO_OK,
                  "busy parent child admission status=%d actions=0x%x",
                  result.status,
                  result.actions);
    zassert_true((result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    zassert_true((result.actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u);
    zassert_false((result.actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY) != 0u);
    zassert_false((result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) !=
                  0u);
    zassert_equal(result.forward.next_hop_id, parent_id);
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "child admission replaced the parent's local report");

    rx.packet = child_report.packet;
    memcpy(rx.payload, child_report.payload, child_report.payload_len);
    rx.payload_len = child_report.payload_len;
    rx.previous_hop_id = child_id;
    rx.first_received_at_ms = now_ms + 1u;
    rx.received_at_ms = now_ms + 1u;
    rx.link_quality = 95u;
    rx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    rx.received_at_valid = true;
    mesh_handle_result_actions(&result,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               &rx,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               &gateway_ack_handed_off);

    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_equal(queued.packet.src_id, child_id);
    zassert_equal(queued.next_hop_id, parent_id);
    zassert_equal(queued.ingress_previous_hop_id, child_id);
    zassert_mem_equal(queued.payload,
                      child_report.payload,
                      child_report.payload_len,
                      "queue admission changed the child report");
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "queue admission changed the local report owner");
    ack_batch = app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, child_id);
    zassert_not_null(ack_batch);
    zassert_equal(ack_batch->count, 1u);

    /* A lost hop ACK repairs only that ACK. The already queued report must
     * remain single-owned and route discovery must remain inactive. */
    memset(&result, 0, sizeof(result));
    zassert_equal(mesh_relay_handle_rx(&mesh_runtime,
                                       &child_report.packet,
                                       child_report.payload,
                                       child_report.payload_len,
                                       child_id,
                                       95u,
                                       now_ms + 2u,
                                       &result),
                  PROTO_OK);
    zassert_equal(result.status, PROTO_ERR_STALE);
    zassert_false((result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    zassert_true((result.actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u);
    zassert_false((result.actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY) != 0u);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_false(mesh_runtime.route_discovery.active);
    zassert_mem_equal(&mesh_runtime.pending,
                      &pending_before,
                      sizeof(pending_before),
                      "duplicate child report changed the local owner");

    /* Core ownership can end before the app queue drains. Re-offer the exact
     * retry, let the queue deduplicate it while its byte owner exists, then
     * prove the same retry is admitted again once that owner is gone so the
     * gateway can replace a missed end-to-end ACK. */
    mesh_relay_cancel_tx(&mesh_runtime);
    memset(&result, 0, sizeof(result));
    zassert_equal(mesh_relay_handle_rx(&mesh_runtime,
                                       &child_report.packet,
                                       child_report.payload,
                                       child_report.payload_len,
                                       child_id,
                                       95u,
                                       now_ms + 3u,
                                       &result),
                  PROTO_OK);
    zassert_true((result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    mesh_handle_result_actions(&result,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               &rx,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               &gateway_ack_handed_off);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u,
                  "exact retry duplicated an already queued report");
    zassert_ok(k_msgq_get(&report_tx_msgq, &queued, K_NO_WAIT));
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u);

    memset(&result, 0, sizeof(result));
    zassert_equal(mesh_relay_handle_rx(&mesh_runtime,
                                       &child_report.packet,
                                       child_report.payload,
                                       child_report.payload_len,
                                       child_id,
                                       95u,
                                       now_ms + 4u,
                                       &result),
                  PROTO_OK);
    zassert_true((result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    mesh_handle_result_actions(&result,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               &rx,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               &gateway_ack_handed_off);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u,
                  "unowned exact retry did not recreate gateway delivery");
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_equal(queued.packet.src_id, child_id);
    zassert_mem_equal(queued.payload,
                      child_report.payload,
                      child_report.payload_len,
                      "replacement admission changed report bytes");
    zassert_false(mesh_runtime.route_discovery.active);
}

ZTEST(production_seam_report_custody,
      test_live_gateway_ack_rx_precedes_retained_child_route_wait)
{
    const uint64_t child_id = UINT64_C(0x2000000000000cf1);
    const struct mesh_outbound local_report =
        survey_discovery_report_for_test(
            UINT64_C(0x0000005200000001), UINT16_C(0x6cf1));
    struct mesh_outbound child_report =
        survey_discovery_report_from_for_test(
            child_id,
            UINT64_C(0x0000005200000002),
            UINT16_C(0x6cf2));
    struct mesh_outbound route_wait_before;
    struct mesh_outbound first_ack;
    struct mesh_outbound terminal_ack;
    struct mesh_event_timing upstream_timing;
    struct proto_packet confirm_packet = {0};
    struct mesh_rx_pending terminal_rx = {0};
    struct fw_radio_activity_decision decision;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN] = {0};
    size_t confirm_payload_len = 0u;
    uint32_t now_ms;

    report_owner_reset();
    child_report.ingress_previous_hop_id = child_id;
    event_accept_finish_reset();
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    while (k_msgq_get(&mesh_rx_msgq, &terminal_rx, K_NO_WAIT) == 0) {
    }
    memset(&terminal_rx, 0, sizeof(terminal_rx));
    mesh_route_waiting_tx_valid = false;
    mesh_route_waiting_tx_owner = APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;

    now_ms = k_uptime_get_32();
    upstream_timing = event_accept_timing_for_test(
        now_ms + MESH_EVENT_DEFAULT_GUARD_MS);
    zassert_true(mesh_event_timing_local_rx_slot(&upstream_timing));
    zassert_equal(mesh_install_channel9_timing_direction(
                      GATEWAY_ID,
                      &upstream_timing,
                      MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                      "gateway-ack-route-wait-fixture"),
                  PROTO_OK);

    start_pending_outbound_for_test(&local_report);
    mesh_runtime.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    mesh_runtime.pending.gateway_ack_deadline_ms = now_ms + 60000u;
    zassert_true(app_mesh_ch9_core_ack_wait_active(
        &mesh_runtime.pending, mesh_relay_tx_active(&mesh_runtime)));

    mesh_route_waiting_tx = child_report;
    mesh_route_waiting_tx_valid = true;
    mesh_route_waiting_tx_owner = APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
    route_wait_before = mesh_route_waiting_tx;

    mesh_coordinator_decide_now("ack-with-route-wait", &decision);
    zassert_equal(decision.state, FW_RADIO_ACTIVITY_MESH_RX);
    zassert_true(decision.uwb_rx_allowed,
                 "live gateway-ACK wait did not keep RX admitted");
    zassert_false(decision.route_wait_allowed,
                  "route-wait outranked the live gateway-ACK receiver");
    zassert_mem_equal(&mesh_route_waiting_tx,
                      &route_wait_before,
                      sizeof(route_wait_before),
                      "coordinator admission changed child route-wait bytes");

    first_ack = gateway_ack_for_test(
        &mesh_runtime.pending.packet,
        mesh_runtime.pending.payload,
        mesh_runtime.pending.payload_len,
        UINT16_C(0x6cf3));
    inject_mesh_rx_frame_for_test(&first_ack, GATEWAY_ID);
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    k_work_init_delayable(&mesh_uwb_rx_work, mesh_uwb_rx_work_handler);
    k_work_init_delayable(&mesh_route_waiting_work,
                          mesh_route_waiting_work_handler);
    event_accept_rx_work_initialized = true;
    mesh_uwb_rx_active = true;

    mesh_uwb_rx_work_handler(NULL);
    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    (void)mesh_process_queued_rx_now("ack-with-route-wait-test");
    for (uint16_t attempt = 0u;
         attempt < 100u &&
         !mesh_runtime.pending.gateway_ack_confirm_pending;
         attempt++) {
        k_msleep(1u);
    }

    zassert_true(report_custody_rx_attempts > 0u,
                 "RX worker never reached the physical receive boundary");
    zassert_false(report_custody_rx_injection_armed,
                  "RX worker left the exact gateway ACK unread");
    zassert_true(mesh_runtime.pending.gateway_ack_confirm_pending,
                 "exact gateway ACK did not retire the raw ACK-wait owner");
    zassert_false(app_mesh_ch9_core_ack_wait_active(
                      &mesh_runtime.pending,
                      mesh_relay_tx_active(&mesh_runtime)),
                  "raw gateway-ACK wait remained live after exact receipt");
    zassert_mem_equal(&mesh_route_waiting_tx,
                      &route_wait_before,
                      sizeof(route_wait_before),
                      "gateway ACK handling changed child route-wait bytes");
    zassert_true(mesh_route_waiting_tx_valid);

    zassert_equal(mesh_relay_pending_gateway_ack_confirm_wire(
                      &mesh_runtime,
                      k_uptime_get_32(),
                      &confirm_packet,
                      confirm_payload,
                      sizeof(confirm_payload),
                      &confirm_payload_len),
                  PROTO_OK);
    terminal_ack = gateway_ack_for_test(
        &confirm_packet,
        confirm_payload,
        confirm_payload_len,
        UINT16_C(0x6cf4));
    terminal_rx.packet = terminal_ack.packet;
    memcpy(terminal_rx.payload,
           terminal_ack.payload,
           terminal_ack.payload_len);
    terminal_rx.payload_len = terminal_ack.payload_len;
    terminal_rx.previous_hop_id = GATEWAY_ID;
    terminal_rx.link_quality = 93u;
    terminal_rx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    terminal_rx.first_received_at_ms = k_uptime_get();
    terminal_rx.received_at_ms = k_uptime_get_32();
    terminal_rx.received_at_valid = true;
    zassert_ok(k_msgq_put(&mesh_rx_msgq, &terminal_rx, K_NO_WAIT));
    zassert_true(mesh_process_queued_rx_now(
                     "ack-confirm-with-route-wait-test") > 0u,
                 "terminal gateway ACK was not drained");
    zassert_false(mesh_relay_tx_active(&mesh_runtime),
                  "terminal gateway ACK did not retire local core custody");
    zassert_mem_equal(&mesh_route_waiting_tx,
                      &route_wait_before,
                      sizeof(route_wait_before),
                      "terminal gateway ACK changed child route-wait bytes");

    mesh_coordinator_decide_now("route-wait-after-ack", &decision);
    zassert_true(decision.route_wait_allowed,
                 "route-wait did not resume after ACK custody retired");
    zassert_false(decision.uwb_rx_allowed,
                  "idle ACK receiver still outranked retained route-wait");

    /* Hold the system queue long enough to observe the production RX
     * coordinator turn arming route-wait work, then drain that exact work.
     * A direct call alone would miss a regression in the scheduling branch. */
    route_preempt_test_reset();
    zassert_true(k_work_submit(&route_preempt_blocker_work) >= 0);
    zassert_ok(k_sem_take(&route_preempt_blocker_entered, K_SECONDS(1)));
    mesh_uwb_rx_active = true;
    mesh_uwb_rx_work_handler(NULL);
    zassert_true(k_work_delayable_is_pending(&mesh_route_waiting_work),
                 "RX coordinator did not arm retained route-wait work");
    (void)k_work_cancel_delayable(&mesh_route_waiting_work);
    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    k_sem_give(&route_preempt_blocker_release);
    zassert_ok(k_sem_take(&route_preempt_blocker_done, K_SECONDS(1)));
    mesh_route_waiting_work_handler(NULL);
    zassert_true(mesh_relay_tx_active(&mesh_runtime),
                 "resumed route-wait did not reclaim core TX custody");
    zassert_equal(mesh_runtime.pending.packet.msg_type,
                  route_wait_before.packet.msg_type);
    zassert_equal(mesh_runtime.pending.packet.src_id,
                  route_wait_before.packet.src_id);
    zassert_equal(mesh_runtime.pending.packet.session_id,
                  route_wait_before.packet.session_id);
    zassert_equal(mesh_runtime.pending.packet.seq,
                  route_wait_before.packet.seq);
    zassert_equal(mesh_runtime.pending.payload_len,
                  route_wait_before.payload_len);
    zassert_mem_equal(mesh_runtime.pending.payload,
                      route_wait_before.payload,
                      route_wait_before.payload_len,
                      "resumed route-wait mutated child survey custody");

    (void)k_work_cancel(&mesh_rx_work);
    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    (void)k_work_cancel_delayable(&mesh_route_waiting_work);
    (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
    mesh_relay_cancel_tx(&mesh_runtime);
    mesh_route_waiting_tx_valid = false;
    mesh_route_waiting_tx_owner = APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
    memset(&mesh_route_waiting_tx, 0, sizeof(mesh_route_waiting_tx));
}

ZTEST(production_seam_report_custody,
      test_child_forward_wait_retains_custody_until_upstream_tx_slot)
{
    enum {
        COUNTERPHASE_MS = MESH_EVENT_DEFAULT_INTERVAL_MS / 2u,
    };
    const uint64_t parent_id = UINT64_C(0x2000000000000d01);
    const uint64_t child_id = UINT64_C(0x2000000000000d02);
    struct route_candidate parent_route = {0};
    struct mesh_event_timing child_downstream;
    struct mesh_event_timing parent_upstream;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan child_rx_plan = {0};
    struct mesh_event_plan parent_tx_plan = {0};
    struct mesh_relay_result result = {0};
    struct mesh_rx_pending rx = {0};
    struct proto_packet child_packet = {0};
    struct proto_packet retained_packet;
    struct proto_packet transmitted_packet = {0};
    uint8_t child_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t retained_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t transmitted_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t child_payload_len;
    size_t transmitted_payload_len = 0u;
    uint64_t transmitted_previous_hop = 0u;
    uint32_t fixture_now_ms;
    uint32_t child_slot_start_ms;
    uint32_t parent_slot_start_ms;
    uint32_t parent_prepare_ms;
    uint32_t parent_send_ms;
    bool gateway_ack_handed_off = false;

    report_owner_reset();
    event_accept_finish_reset();
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);

    fixture_now_ms = k_uptime_get_32();
    child_slot_start_ms = fixture_now_ms - 20u;
    parent_slot_start_ms = child_slot_start_ms + COUNTERPHASE_MS;
    parent_prepare_ms =
        parent_slot_start_ms - MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS;
    parent_route = (struct route_candidate) {
        .next_hop_id = parent_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = mesh_runtime.upstream.current_epoch,
        .last_seen_ms = fixture_now_ms,
        .hop_count = 1u,
        .link_quality = 92u,
        .valid = true,
    };
    zassert_equal(route_upsert_candidate(&mesh_runtime.upstream,
                                         &parent_route),
                  PROTO_OK);

    parent_upstream = event_accept_timing_for_test(parent_slot_start_ms);
    mesh_event_timing_set_local_first_slot_tx(&parent_upstream, true);
    child_downstream = event_accept_timing_for_test(child_slot_start_ms);
    mesh_event_timing_set_local_first_slot_tx(&child_downstream, false);
    zassert_equal(mesh_install_channel9_timing_direction(
                      parent_id,
                      &parent_upstream,
                      MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
                      "initial-forward-parent-fixture"),
                  PROTO_OK);
    zassert_equal(mesh_install_channel9_timing_direction(
                      child_id,
                      &child_downstream,
                      MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
                      "initial-forward-child-fixture"),
                  PROTO_OK);

    mesh_fill_channel5_requirements(&requirements);
    zassert_equal(mesh_relay_require_channel9_event(
                      &mesh_runtime,
                      child_id,
                      &requirements,
                      fixture_now_ms,
                      &child_rx_plan),
                  PROTO_OK);
    zassert_equal(child_rx_plan.action, MESH_EVENT_PLAN_START);
    zassert_equal(child_rx_plan.start_ms, child_slot_start_ms);
    zassert_true(mesh_event_timing_local_rx_slot(&child_downstream));
    zassert_equal(mesh_relay_require_channel9_tx_event(
                      &mesh_runtime,
                      parent_id,
                      &requirements,
                      fixture_now_ms,
                      &parent_tx_plan),
                  PROTO_ERR_BUSY);
    zassert_equal(parent_tx_plan.action, MESH_EVENT_PLAN_WAIT);
    zassert_equal(parent_tx_plan.start_ms, parent_slot_start_ms);

    child_payload_len = child_click_report_for_test(
        &child_packet,
        child_payload,
        sizeof(child_payload),
        child_id,
        UINT16_C(0x6d01));
    zassert_equal(mesh_relay_handle_rx_with_random(
                      &mesh_runtime,
                      &child_packet,
                      child_payload,
                      child_payload_len,
                      child_id,
                      93u,
                      fixture_now_ms,
                      UINT32_C(0x6d010001),
                      &result),
                  PROTO_OK);
    zassert_true((result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
    zassert_true((result.actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u);
    zassert_equal(result.forward.next_hop_id, parent_id);
    zassert_equal(result.forward.ingress_previous_hop_id, child_id);
    parent_send_ms = parent_slot_start_ms + MESH_ROUTE_TEST_CH9_TX_OFFSET_MS;
    zassert_true(parent_send_ms > parent_prepare_ms);

    rx.packet = child_packet;
    memcpy(rx.payload, child_payload, child_payload_len);
    rx.payload_len = (uint16_t)child_payload_len;
    rx.previous_hop_id = child_id;
    rx.first_received_at_ms = fixture_now_ms;
    rx.received_at_ms = fixture_now_ms;
    rx.link_quality = 93u;
    rx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    rx.received_at_valid = true;
    rx.current_channel9_plan = child_rx_plan;
    rx.current_channel9_plan_valid = true;
    mesh_relay_note_channel9_rx(&mesh_runtime,
                                child_id,
                                child_rx_plan.start_ms,
                                fixture_now_ms);

    tracked_mesh_tx_capture_enabled = true;
    mesh_handle_result_actions(&result,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               &rx,
                               NULL,
                               NULL,
                               0u,
                               NULL,
                               &gateway_ack_handed_off);

    zassert_true(mesh_relay_tx_active(&mesh_runtime),
                 "PLAN_WAIT dropped the fresh child custody owner");
    zassert_equal(mesh_runtime.pending.state,
                  MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    zassert_equal(mesh_runtime.pending.radio_channel,
                  UWB_CHANNEL_MESH_PAYLOAD);
    zassert_equal(mesh_runtime.pending.next_hop_id, parent_id);
    zassert_equal(mesh_runtime.pending.transit_previous_hop_id, child_id);
    zassert_true(mesh_runtime.pending.retry_after_ms >= parent_prepare_ms);
    zassert_true(mesh_runtime.pending.retry_after_ms <= parent_slot_start_ms);
    zassert_true(mesh_runtime.outbox_record.valid);
    zassert_true(k_work_delayable_is_pending(&mesh_tx_timeout_work),
                 "retained PLAN_WAIT custody has no runnable timeout owner");
    zassert_true(app_mesh_ch9_ack_table_pending_for_peer(
                     &mesh_ch9_ack_table, child_id),
                 "exact retained parent custody did not hand off a child hop ACK");

    retained_packet = mesh_runtime.pending.packet;
    memcpy(retained_payload,
           mesh_runtime.pending.payload,
           mesh_runtime.pending.payload_len);
    zassert_equal(mesh_runtime.pending.payload_len, child_payload_len);
    zassert_mem_equal(retained_payload,
                      result.forward.payload,
                      child_payload_len,
                      "retained custody changed child payload bytes");

    zassert_ok(k_sem_take(&tracked_mesh_tx_capture_sem, K_MSEC(2000)),
               "retained child custody never reached its upstream TX slot");
    k_msleep(20u);
    zassert_equal(tracked_mesh_tx_capture_calls, 1u,
                  "one retained child packet produced multiple RF forwards");
    zassert_true(tracked_mesh_tx_capture_at_ms >= parent_send_ms,
                 "retained retry transmitted at preparation time before the peer RX slot");
    zassert_true(tracked_mesh_tx_capture_at_ms <= parent_tx_plan.end_ms);
    zassert_equal(uwb_mesh_frame_decode(
                      tracked_mesh_tx_capture_frame,
                      tracked_mesh_tx_capture_frame_len,
                      NETWORK_ID,
                      parent_id,
                      &transmitted_previous_hop,
                      &transmitted_packet,
                      transmitted_payload,
                      sizeof(transmitted_payload),
                      &transmitted_payload_len),
                  PROTO_OK);
    zassert_equal(transmitted_previous_hop, DEVICE_ID);
    zassert_equal(transmitted_packet.msg_type, retained_packet.msg_type);
    zassert_equal(transmitted_packet.flags, retained_packet.flags);
    zassert_equal(transmitted_packet.src_id, retained_packet.src_id);
    zassert_equal(transmitted_packet.dst_id, retained_packet.dst_id);
    zassert_equal(transmitted_packet.session_id, retained_packet.session_id);
    zassert_equal(transmitted_packet.seq, retained_packet.seq);
    zassert_equal(transmitted_packet.ttl, retained_packet.ttl);
    zassert_equal(transmitted_payload_len, child_payload_len);
    zassert_mem_equal(transmitted_payload,
                      retained_payload,
                      child_payload_len,
                      "upstream retry did not send immutable custody bytes");

    zassert_true(mesh_relay_tx_active(&mesh_runtime));
    zassert_equal(mesh_runtime.pending.state,
                  MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    zassert_equal(mesh_runtime.pending.transit_previous_hop_id, child_id);
    zassert_equal(mesh_runtime.pending.next_hop_id, parent_id);
    zassert_equal(mesh_runtime.pending.packet.src_id, retained_packet.src_id);
    zassert_equal(mesh_runtime.pending.packet.session_id,
                  retained_packet.session_id);
    zassert_equal(mesh_runtime.pending.packet.seq, retained_packet.seq);
    zassert_equal(mesh_runtime.pending.payload_len, child_payload_len);
    zassert_mem_equal(mesh_runtime.pending.payload,
                      retained_payload,
                      child_payload_len,
                      "successful upstream TX mutated or dropped custody");

    tracked_mesh_tx_capture_enabled = false;
    (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
    (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
    mesh_relay_cancel_tx(&mesh_runtime);
}

ZTEST(production_seam_report_custody,
      test_downstream_timing_cannot_suppress_missing_parent_repair)
{
    const uint64_t parent_id = UINT64_C(0x20000000000000a5);
    const uint64_t child_id = UINT64_C(0x20000000000000b5);
    struct route_candidate parent_route = {0};
    struct mesh_event_timing downstream =
        event_accept_timing_for_test(k_uptime_get_32() + 1000u);
    struct mesh_relay_event_timing_entry child_before;
    const struct mesh_relay_event_timing_entry *child_entry;

    report_owner_reset();
    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    parent_route = (struct route_candidate) {
        .next_hop_id = parent_id,
        .gateway_id = GATEWAY_ID,
        .route_epoch = mesh_runtime.upstream.current_epoch,
        .last_seen_ms = k_uptime_get_32(),
        .hop_count = 1u,
        .link_quality = 92u,
        .valid = true,
    };
    zassert_equal(route_upsert_candidate(&mesh_runtime.upstream,
                                         &parent_route),
                  PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&downstream, false);
    zassert_equal(mesh_install_channel9_timing_direction(
                      child_id,
                      &downstream,
                      MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
                      "missing-parent-downstream-fixture"),
                  PROTO_OK);
    child_entry = event_timing_entry_for_test(child_id);
    zassert_not_null(child_entry);
    child_before = *child_entry;
    zassert_equal(mesh_channel9_connection_count(), 1u);
    zassert_true(mesh_selected_relay_parent_needs_channel9_repair(
                     PROTO_OK, parent_id),
                 "a downstream child hid the missing selected-parent timing");
    zassert_false(mesh_selected_relay_parent_needs_channel9_repair(
                      PROTO_ERR_NOT_FOUND, parent_id));
    zassert_false(mesh_selected_relay_parent_needs_channel9_repair(
                      PROTO_OK, child_id));
    zassert_false(mesh_selected_relay_parent_needs_channel9_repair(
                      PROTO_OK, GATEWAY_ID));
    zassert_mem_equal(event_timing_entry_for_test(child_id),
                      &child_before,
                      sizeof(child_before),
                      "repair classification changed downstream custody");
}

ZTEST_SUITE(production_seam_report_custody,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL);
