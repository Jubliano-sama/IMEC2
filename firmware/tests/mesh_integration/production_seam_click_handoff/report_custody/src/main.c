/*
 * Compile the production report owner as the test translation unit. This is
 * intentionally not a model or copied queue policy: the assertions below call
 * the exact static reservation, fragment, and immutable-head helpers that the
 * anchor image links from app_mesh_report.c.
 */
#include "app_mesh_report.c"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

static uint32_t watchdog_stop_calls;
static bool report_owner_work_initialized;
static bool report_owner_route_work_started;
static bool range_abort_queue_fault_armed;
static enum mesh_range_report_batch_abort_queue_operation
    range_abort_queue_fault_operation;
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

/* Production dependencies outside this deliberately narrow composed link. */
void app_watchdog_stop_feeding(void)
{
    watchdog_stop_calls++;
}

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

void status_debug_uwb_tx_channel_pulse(uint8_t uwb_channel)
{
    ARG_UNUSED(uwb_channel);
}

void status_debug_tx_wake_claim_sent_pulse(void)
{
}

void status_debug_tx_mesh_frame_sent_pulse(void)
{
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
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(timeout_ms);
    ARG_UNUSED(absolute_deadline_ms);
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
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_cap);
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
    watchdog_stop_calls = 0u;
    anchor_uwb_busy = false;
    anchor_click_window_busy = false;
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
      test_range_batch_reserve_proves_empty_queue_and_restores_transit)
{
    const uint64_t clicker_id = UINT64_C(0x1111222233334444);
    const uint32_t event_seq = 0x10203040u;
    const uint8_t attempt_index = 2u;
    const struct mesh_outbound local = queued_report(DEVICE_ID, 901u);
    const struct mesh_outbound transit = queued_report(
        UINT64_C(0x4545000000000001), 902u);
    struct mesh_outbound queued = {0};

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(k_msgq_put(&report_tx_msgq, &local, K_NO_WAIT));
    zassert_equal(mesh_range_report_batch_reserve(clicker_id,
                                                  event_seq,
                                                  attempt_index),
                  -EAGAIN,
                  "local queue bytes were displaced for a batch");
    zassert_false(anchor_range_report_batch_reservation.active);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_mem_equal(&queued, &local, sizeof(queued));

    report_owner_reset();
    anchor_uwb_busy = true;
    zassert_ok(k_msgq_put(&report_tx_msgq, &transit, K_NO_WAIT));
    zassert_ok(mesh_range_report_batch_reserve(clicker_id,
                                               event_seq,
                                               attempt_index));
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 0u,
                  "reservation did not establish an empty real queue");
    zassert_true(report_tx_queue_recovery_valid);
    zassert_mem_equal(&report_tx_queue_overflow_dropped,
                      &transit,
                      sizeof(transit));

    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    zassert_false(anchor_range_report_batch_reservation.active);
    zassert_false(report_tx_queue_recovery_valid);
    zassert_equal(k_msgq_num_used_get(&report_tx_msgq), 1u);
    zassert_ok(k_msgq_peek(&report_tx_msgq, &queued));
    zassert_mem_equal(&queued, &transit, sizeof(queued));
    zassert_equal(watchdog_stop_calls, 0u);
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

ZTEST_SUITE(production_seam_report_custody,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL);
