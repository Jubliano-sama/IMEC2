/* Production gateway worker and publication owner, with fake time and BLE
 * admission. Generated includes contain exact application source spans. */
#include "survey_protocol.h"
#include "survey_response_lane.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define ROLE_GATEWAY 2u
#define DEVICE_ROLE ROLE_GATEWAY
#define NETWORK_ID UINT32_C(0x494d4543)
#define DEVICE_ID UINT64_C(0x9000)
#define K_MUTEX_DEFINE(name) static int name
#define K_FOREVER 0
#define K_MSEC(ms) (ms)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x) (void)(x)
#define LOG_ERR(...) ((void)0)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define APP_SURVEY_RADIO_RETRY_MS 5u
#define APP_SURVEY_CLEANUP_ABORT_RETRY_MS 100u
struct k_work { int unused; };
struct k_work_delayable { uint64_t due; unsigned schedules; };
static int survey_lock;
static unsigned lock_depth, attempts, releases, stop_feeds;
static uint64_t now_ms;
static int admission;
static int ble_admission;
static int origin_error;
static uint64_t origin_time_ms;
static unsigned emitted_kinds;
static struct survey_event observed;
static struct k_work_delayable gateway_work, gateway_publication_work;
struct app_survey_ops {
    /* Consume a pre-reserved control ID in RAM; never write NVS on START. */
    int (*next_generation)(uint32_t *generation);
    int (*send_control)(const struct survey_control *control,
                        uint32_t *delivery_handle);
    int (*control_origin)(uint32_t delivery_handle,
                          uint64_t *origin_ms);
    int (*control_detach)(uint32_t delivery_handle);
    int (*control_abandon)(uint32_t delivery_handle);
    int (*emit_event)(const struct survey_event *event);
    void (*wake_gateway_rx)(void);
    void (*gateway_terminal)(void);
    int (*anchor_upstream)(uint64_t *parent_id, uint8_t *hop_count);
    int (*anchor_consume_enumeration_handoff)(uint32_t assignment_epoch);
    int (*anchor_reschedule)(struct k_work_delayable *work,
                             uint32_t delay_ms);
    bool (*anchor_receive_cancel)(const uint8_t *frame, size_t frame_len);
};

struct app_survey_gateway_roster {
    struct survey_assignment_identity assignment;
    uint32_t host_session_id;
    uint16_t host_sequence;
    uint64_t node_ids[SURVEY_MAX_ANCHORS];
    uint8_t slots[SURVEY_MAX_ANCHORS];
    uint8_t hop_counts[SURVEY_MAX_ANCHORS];
    size_t node_count;
};

static struct app_survey_ops survey_ops;
#include "survey_publication_state.inc"
static struct app_survey_gateway_state gateway_state;
static void k_mutex_lock(int *lock, int timeout)
{ (void)lock; (void)timeout; assert(lock_depth++ == 0u); }
static void k_mutex_unlock(int *lock)
{ (void)lock; assert(lock_depth-- == 1u); }
static uint64_t k_uptime_get(void) { return now_ms; }
static int k_work_reschedule(struct k_work_delayable *work, uint32_t delay)
{ work->due = now_ms + delay; work->schedules++; return 0; }
static void app_watchdog_stop_feeding(void) { stop_feeds++; }
static void status_debug_printf(const char *format, ...) { (void)format; }
static void gateway_work_reschedule_owned(uint64_t due, const char *reason)
{ (void)reason; gateway_work.due = due; gateway_work.schedules++; }
static int mesh_errno_from_proto(int ret)
{ assert(ret != PROTO_OK); return -EINVAL; }
#include "survey_publication_production.inc"
static int gateway_survey_emit_event(const struct survey_event *event);
static struct survey_event event_roundtrip(const struct survey_event *event)
{
    uint8_t payload[SURVEY_EVENT_MAX_WIRE_LEN];
    struct survey_event decoded;
    size_t length = survey_event_encode(event, payload, sizeof(payload));

    if (length == 0u) {
        fprintf(stderr, "event encode failed: kind=%u generation=%u host=%u/%u status=%u batch=%u\n",
                event->kind, event->identity.generation, event->host_session_id,
                event->host_sequence, event->status, event->batch_index);
    }
    assert(length != 0u);
    int ret = survey_event_decode(payload, length, &decoded);
    if (ret != PROTO_OK) {
        fprintf(stderr, "event decode failed: kind=%u length=%zu ret=%d\n",
                event->kind, length, ret);
    }
    assert(ret == PROTO_OK);
    assert(decoded.kind == event->kind && decoded.status == event->status);
    assert(survey_identity_equal(&decoded.identity, &event->identity));
    if (event->kind == SURVEY_EVENT_STARTED || event->kind == SURVEY_EVENT_PLAN_ACCEPTED) {
        assert(decoded.host_session_id == event->host_session_id);
        assert(decoded.host_sequence == event->host_sequence);
    }
    return decoded;
}
static int emit(const struct survey_event *event)
{
    assert(lock_depth == 0u);
    attempts++;
    observed = event_roundtrip(event);
    ble_admission = admission;
    int ret = gateway_survey_emit_event(event);
    if (ret != admission) {
        fprintf(stderr, "event BLE admission failed: kind=%u ret=%d expected=%d\n",
                event->kind, ret, admission);
    }
    assert(ret == admission);
    emitted_kinds |= 1u << event->kind;
    return ret;
}
static void release(void) { assert(lock_depth == 0u); releases++; }
static int origin(uint32_t handle, uint64_t *out)
{ assert(handle == 7u); *out = origin_time_ms; return origin_error; }
static void reset(void)
{
    memset(&gateway_state, 0, sizeof(gateway_state));
    memset(&gateway_work, 0, sizeof(gateway_work));
    memset(&gateway_publication_work, 0, sizeof(gateway_publication_work));
    memset(&survey_ops, 0, sizeof(survey_ops));
    survey_ops.emit_event = emit;
    survey_ops.gateway_terminal = release;
    survey_ops.control_origin = origin;
    gateway_state.identity.generation = 10u;
    gateway_state.identity.assignment.assignment_epoch = 7u;
    gateway_state.identity.assignment.table_command_seq = 8u;
    memset(gateway_state.identity.assignment.table_commitment.bytes, 0xa5,
           sizeof(gateway_state.identity.assignment.table_commitment.bytes));
    gateway_state.identity.assignment.slot_span = 2u;
    gateway_state.identity.assignment.max_hop_count = 1u;
    gateway_state.active = true;
    gateway_state.hard_deadline_ms = 500000u;
    gateway_state.self_stop_ms = 180000u;
    gateway_state.graph.occupied_slot_mask = 3u;
    gateway_state.graph.received_report_mask = 3u;
    gateway_state.graph.reports[0].own_slot = 0u;
    gateway_state.graph.reports[1].own_slot = 1u;
    now_ms = 1000u;
    admission = -ENOSPC;
    origin_error = 0;
    origin_time_ms = 100u;
    attempts = releases = stop_feeds = 0u;
}
static void tick_publication(enum survey_event_kind expected)
{
    unsigned before = attempts;
    gateway_publication_work_handler(NULL);
    assert(attempts == before + 1u);
    assert(observed.kind == expected);
    assert(observed.identity.generation == 10u);
    if (admission != 0) {
        assert(gateway_publication_work.due == now_ms + 100u);
    }
}
static void graph_signals_terminal(void)
{
    reset();
    gateway_state.stage = APP_SURVEY_GATEWAY_NEIGHBORS;
    gateway_state.response_lane_end_ms = now_ms;
    gateway_state.signal_count = 1u;
    gateway_state.signal_received_mask[0] = 1u;
    gateway_state.records.signals[0].bytes[0] = 50u;
    gateway_work_handler(NULL);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN);
    tick_publication(SURVEY_EVENT_NEIGHBOR_GRAPH);
    struct survey_event first = observed;
    uint64_t radio_due = gateway_work.due;
    for (unsigned i = 0; i < 10u; i++) {
        now_ms += 100u;
        tick_publication(SURVEY_EVENT_NEIGHBOR_GRAPH);
        assert(observed.partial_reasons == first.partial_reasons);
        assert(observed.graph.occupied_slot_mask == first.graph.occupied_slot_mask);
        assert(gateway_work.due == radio_due);
    }
    /* Radio expiry must complete and release even with both BLE events blocked. */
    now_ms = gateway_state.self_stop_ms;
    gateway_work_handler(NULL);
    assert(!gateway_state.active);
    assert(releases == 1u);
    assert(gateway_state.last_event.kind == SURVEY_EVENT_TERMINAL);
    tick_publication(SURVEY_EVENT_NEIGHBOR_GRAPH);
    assert(observed.partial_reasons == first.partial_reasons);
    admission = 0;
    tick_publication(SURVEY_EVENT_NEIGHBOR_GRAPH);
    admission = -ENOMEM;
    tick_publication(SURVEY_EVENT_SIGNALS);
    assert(observed.signal_count == 1u && observed.records.signals[0].bytes[0] == 50u);
    admission = 0;
    tick_publication(SURVEY_EVENT_SIGNALS);
    admission = -ENOSPC;
    tick_publication(SURVEY_EVENT_TERMINAL);
    admission = 0;
    tick_publication(SURVEY_EVENT_TERMINAL);
    assert(gateway_state.publication_pending == 0u);
    assert(releases == 1u && stop_feeds == 0u);
}
static void plan_batch_terminal(void)
{
    reset();
    gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN_RF;
    gateway_state.pending_control_handle = 7u;
    gateway_state.pending_control_deadline_ms = 10000u;
    gateway_state.plan_build.plan.identity = gateway_state.identity;
    gateway_state.plan_build.plan.self_stop_delay_ms = 180000u;
    gateway_state.plan_build.plan.execution_start_delay_ms = 3000u;
    gateway_state.plan_build.plan.wave_count = 1u;
    gateway_state.plan_build.plan.pair_count = 1u;
    gateway_state.plan_build.plan.pairs[0].responder_slot = 1u;
    assert(survey_plan_commitment(&gateway_state.plan_build.plan,
                                 gateway_state.plan_build.plan.commitment));
    gateway_work_handler(NULL);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING);
    tick_publication(SURVEY_EVENT_PLAN_ACCEPTED);
    uint16_t partial = observed.partial_reasons;
    gateway_state.records.results[0] = (struct survey_range_result) {
        .pair_index = 0u, .responder_slot = 1u, .success_count = 3u, .median_mm = 1200u,
    };
    gateway_state.result_received_mask[0] = 1u;
    gateway_state.stride_index = SURVEY_EXTRA_DRAIN_STRIDES;
    now_ms = gateway_state.response_lane_end_ms;
    gateway_work_handler(NULL);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN);
    tick_publication(SURVEY_EVENT_PLAN_ACCEPTED);
    assert(observed.partial_reasons == partial && observed.plan.pair_count == 1u);
    now_ms = gateway_state.self_stop_ms;
    gateway_work_handler(NULL);
    assert(releases == 1u && !gateway_state.active);
    admission = 0;
    tick_publication(SURVEY_EVENT_PLAN_ACCEPTED);
    admission = -EAGAIN;
    tick_publication(SURVEY_EVENT_BATCH_COMPLETE);
    assert(observed.result_count == 1u && observed.records.results[0].median_mm == 1200u);
    admission = 0;
    tick_publication(SURVEY_EVENT_BATCH_COMPLETE);
    tick_publication(SURVEY_EVENT_TERMINAL);
    assert(gateway_state.publication_pending == 0u);
}

static void earlier_partial_batch_survives_clean_final_batch(void)
{
    static const uint16_t reasons[] = {
        SURVEY_PARTIAL_MISSING_RANGE_RESULT,
        SURVEY_PARTIAL_INSUFFICIENT_RANGE,
    };

    for (size_t failure = 0u; failure < ARRAY_SIZE(reasons); failure++) {
        reset();
        admission = 0;
        gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;
        gateway_state.plan_build.plan.identity = gateway_state.identity;
        gateway_state.plan_build.plan.wave_count = 1u;
        gateway_state.plan_build.plan.pair_count = 1u;
        gateway_state.plan_build.plan.pairs[0].responder_slot = 1u;
        gateway_state.stride_index = SURVEY_EXTRA_DRAIN_STRIDES;
        gateway_state.response_lane_end_ms = now_ms;
        if (reasons[failure] == SURVEY_PARTIAL_INSUFFICIENT_RANGE) {
            gateway_state.result_received_mask[0] = 1u;
            gateway_state.records.results[0] = (struct survey_range_result) {
                .pair_index = 0u, .responder_slot = 1u,
                .success_count = 2u, .median_mm = 1200u,
            };
        }
        gateway_work_handler(NULL);
        assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN);
        tick_publication(SURVEY_EVENT_BATCH_COMPLETE);
        assert(observed.status == SURVEY_TERMINAL_PARTIAL);
        assert(observed.partial_reasons == reasons[failure]);
        assert(gateway_state.publication_pending == 0u);

        /* The next PLAN replaces the previous batch's bounded result bank.
         * Its complete result must not erase the prior batch's failure. */
        gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;
        gateway_state.plan_build.plan.batch_index = 1u;
        gateway_state.plan_build.plan.final_batch = true;
        gateway_state.stride_index = SURVEY_EXTRA_DRAIN_STRIDES;
        gateway_state.result_received_mask[0] = 1u;
        gateway_state.records.results[0] = (struct survey_range_result) {
            .pair_index = 0u, .responder_slot = 1u,
            .success_count = 3u, .median_mm = 1200,
        };
        gateway_work_handler(NULL);
        assert(gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP);
        now_ms = gateway_state.cleanup_deadline_ms;
        gateway_work_handler(NULL);
        assert(!gateway_state.active && releases == 1u);
        tick_publication(SURVEY_EVENT_TERMINAL);
        assert(observed.status == SURVEY_TERMINAL_PARTIAL);
        assert(observed.partial_reasons == reasons[failure]);
        assert(observed.batch_index == 1u && observed.final_batch);
        assert(observed.result_count == 1u);
        assert(observed.records.results[0].success_count == 3u);
        assert(gateway_state.publication_pending == 0u);
    }
}

static uint16_t gateway_survey_event_seq, admitted_seq;
static unsigned ble_attempts;
static uint32_t k_uptime_get_32(void) { return (uint32_t)now_ms; }
#include "survey_publication_envelope.inc"
static int gateway_ble_stream_packet(const struct proto_packet *packet,
                                     const uint8_t *payload, size_t length,
                                     uint32_t timestamp)
{
    (void)timestamp;
    assert(lock_depth == 1u);
    assert(length != 0u && payload != NULL);
    assert(packet->msg_type == MSG_SURVEY_EVENT);
    assert(packet->flags == FLAG_GATEWAY_ACK_REQUIRED);
    uint32_t generation;
    if (!gateway_survey_event_envelope_valid(payload, length, &generation) ||
        generation != packet->session_id) {
        return -EBADMSG;
    }
    struct survey_event received;
    assert(survey_event_decode(payload, length, &received) == PROTO_OK);
    assert(received.identity.generation == packet->session_id);
    admitted_seq = packet->seq;
    ble_attempts++;
    return ble_admission;
}
#include "survey_publication_adapter.inc"
static void adapter_admission_identity(void)
{
    struct survey_event event = {
        .kind = SURVEY_EVENT_TERMINAL,
        .status = SURVEY_TERMINAL_ABORTED,
        .identity = {
            .generation = 10u,
            .assignment = { .assignment_epoch = 7u, .table_command_seq = 8u,
                            .slot_span = 2u, .max_hop_count = 1u },
        },
    };
    memset(event.identity.assignment.table_commitment.bytes, 0xa5,
           sizeof(event.identity.assignment.table_commitment.bytes));
    gateway_survey_event_seq = 40u;
    ble_attempts = 0u;
    ble_admission = -ENOSPC;
    assert(gateway_survey_emit_event(&event) == -ENOSPC);
    assert(admitted_seq == 41u && gateway_survey_event_seq == 40u);
    assert(gateway_survey_emit_event(&event) == -ENOSPC);
    assert(admitted_seq == 41u && gateway_survey_event_seq == 40u);
    ble_admission = 0;
    assert(gateway_survey_emit_event(&event) == 0);
    assert(admitted_seq == 41u && gateway_survey_event_seq == 41u);
    assert(gateway_survey_emit_event(&event) == 0);
    assert(admitted_seq == 42u && gateway_survey_event_seq == 42u);
    gateway_survey_event_seq = UINT16_MAX;
    assert(gateway_survey_emit_event(&event) == 0);
    assert(admitted_seq == 1u && gateway_survey_event_seq == 1u);
    assert(ble_attempts == 5u);
}

static uint32_t reserved_generation = 4000u;
static unsigned allocations, controls;
static int allocator_error;
static int allocate_generation(uint32_t *generation)
{
    assert(lock_depth == 1u);
    allocations++;
    if (allocator_error != 0) return allocator_error;
    *generation = reserved_generation++;
    return 0;
}
static int send_control(const struct survey_control *control, uint32_t *handle)
{
    assert(lock_depth == 0u);
    assert(control->identity.generation == gateway_state.identity.generation);
    controls++;
    *handle = 7u;
    return 0;
}
static int detach(uint32_t handle) { (void)handle; return 0; }

static void assert_cleanup_status_retains_owner(void)
{
    struct survey_event snapshot;
    uint8_t frozen[SURVEY_EVENT_MAX_WIRE_LEN];
    uint8_t projected[sizeof(frozen)];
    const int saved_admission = admission;

    assert(gateway_state.active && releases == 0u);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP);
    assert(gateway_state.last_event.kind == SURVEY_EVENT_TERMINAL);
    assert((gateway_state.publication_pending &
            (1u << SURVEY_EVENT_TERMINAL)) == 0u);
    size_t frozen_len = survey_event_encode(&gateway_state.last_event,
                                            frozen, sizeof(frozen));
    assert(frozen_len != 0u);
    assert(app_survey_gateway_status(&snapshot) == 0);
    assert(snapshot.kind == SURVEY_EVENT_RANGE_PROGRESS);
    assert(snapshot.status == (snapshot.partial_reasons == 0u ?
           SURVEY_TERMINAL_COMPLETE : SURVEY_TERMINAL_PARTIAL));
    /* Compare the full public wire shape: changing the projection must retain
     * every frozen result and identity without changing the private outcome. */
    size_t projected_len = survey_event_encode(&gateway_state.last_event,
                                               projected, sizeof(projected));
    assert(projected_len == frozen_len);
    assert(memcmp(projected, frozen, frozen_len) == 0);
    projected_len = survey_event_encode(&snapshot, projected, sizeof(projected));
    frozen[1] = (uint8_t)snapshot.kind;
    frozen[2] = (uint8_t)snapshot.status;
    assert(projected_len == frozen_len);
    assert(memcmp(projected, frozen, frozen_len) == 0);
    admission = 0;
    assert(emit(&snapshot) == 0); /* Real codec, emitter and BLE admission. */
    admission = saved_admission;
    assert(gateway_state.active && releases == 0u);
}

static void cleanup_status_and_publication_wait_for_owner_release(void)
{
    for (unsigned cancel = 0u; cancel < 2u; cancel++) {
        reset();
        survey_ops.next_generation = allocate_generation;
        survey_ops.send_control = send_control;
        survey_ops.control_detach = detach;
        survey_ops.control_abandon = detach;
        struct app_survey_gateway_roster roster = {
            .assignment = gateway_state.identity.assignment,
            .host_session_id = 99u, .host_sequence = 42u,
            .node_ids = {0x11u, 0x22u}, .slots = {0u, 1u},
            .hop_counts = {1u, 1u}, .node_count = 2u,
        };
        if (cancel) {
            gateway_state.stage = APP_SURVEY_GATEWAY_NEIGHBORS;
            gateway_state.start_host_session_id = roster.host_session_id;
            gateway_state.start_host_sequence = roster.host_sequence;
            gateway_state.last_event = (struct survey_event) {
                .kind = SURVEY_EVENT_STARTED,
                .identity = gateway_state.identity,
                .host_session_id = roster.host_session_id,
                .host_sequence = roster.host_sequence,
            };
            gateway_publication_note_locked(&gateway_state.last_event);
            assert(app_survey_gateway_abort(&gateway_state.identity) == 0);
        } else {
            gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;
            gateway_state.plan_build.plan.identity = gateway_state.identity;
            gateway_state.plan_build.plan.wave_count = 1u;
            gateway_state.plan_build.plan.pair_count = 1u;
            gateway_state.plan_build.plan.final_batch = true;
            gateway_state.plan_build.plan.pairs[0].responder_slot = 1u;
            gateway_state.records.results[0] = (struct survey_range_result) {
                .responder_slot = 1u, .success_count = 5u, .median_mm = 1234u,
            };
            gateway_state.result_received_mask[0] = 1u;
            gateway_state.stride_index = SURVEY_EXTRA_DRAIN_STRIDES;
            gateway_state.response_lane_end_ms = now_ms;
            gateway_work_handler(NULL);
        }
        const uint64_t deadline = gateway_state.cleanup_deadline_ms;
        const uint64_t query_times[] = {now_ms, 60000u, deadline - 1u, deadline};
        for (size_t i = 0u; i < ARRAY_SIZE(query_times); i++) {
            now_ms = query_times[i];
            /* Even at the clock edge, status cannot precede the actual owner
             * transition performed by the gateway worker. */
            assert_cleanup_status_retains_owner();
            struct survey_identity next;
            assert(app_survey_gateway_start(&roster, &next) == -EBUSY);
            unsigned before = attempts;
            gateway_publication_work_handler(NULL);
            assert(attempts == before + cancel);
            if (cancel) {
                /* A blocked acceptance must never inherit the frozen ABORTED
                 * event through the publication worker's last_event path. */
                assert(observed.kind == SURVEY_EVENT_STARTED);
                assert(observed.host_sequence == roster.host_sequence);
            }
        }
        if (cancel) {
            admission = 0;
            tick_publication(SURVEY_EVENT_STARTED);
        }
        assert(gateway_state.publication_pending == 0u);
        unsigned before = attempts;
        gateway_publication_work_handler(NULL);
        assert(attempts == before);
        gateway_work_handler(NULL);
        assert(!gateway_state.active && releases == 1u);
        assert(gateway_state.stage == APP_SURVEY_GATEWAY_TERMINAL);
        struct survey_event terminal;
        assert(app_survey_gateway_status(&terminal) == 0);
        assert(terminal.kind == SURVEY_EVENT_TERMINAL);
        assert(terminal.status == (cancel ? SURVEY_TERMINAL_ABORTED :
                                           SURVEY_TERMINAL_COMPLETE));
        assert(terminal.result_count == (cancel ? 0u : 1u));
        admission = -ENOSPC;
        tick_publication(SURVEY_EVENT_TERMINAL);
        admission = 0;
        tick_publication(SURVEY_EVENT_TERMINAL);
        assert(gateway_state.publication_pending == 0u);
        assert(releases == 1u && stop_feeds == 0u);
    }
}

static void later_terminal_has_current_batch_and_no_previous_results(void)
{
    for (unsigned trigger = 0u; trigger < 4u; trigger++) {
        reset();
        admission = 0;
        survey_ops.send_control = send_control;
        survey_ops.control_detach = detach;
        survey_ops.control_abandon = detach;
        gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;
        gateway_state.plan_build.plan.identity = gateway_state.identity;
        gateway_state.plan_build.plan.wave_count = 1u;
        gateway_state.plan_build.plan.pair_count = 1u;
        gateway_state.plan_build.plan.pairs[0].responder_slot = 1u;
        gateway_state.records.results[0] = (struct survey_range_result) {
            .pair_index = 0u, .responder_slot = 1u,
            .success_count = 3u, .median_mm = 1234,
        };
        gateway_state.result_received_mask[0] = 1u;
        gateway_state.stride_index = SURVEY_EXTRA_DRAIN_STRIDES;
        gateway_state.response_lane_end_ms = now_ms;
        gateway_work_handler(NULL);
        tick_publication(SURVEY_EVENT_BATCH_COMPLETE);
        assert(observed.batch_index == 0u && observed.result_count == 1u);
        assert(gateway_state.next_batch_index == 1u);

        if (trigger == 0u) {
            assert(app_survey_gateway_abort(&gateway_state.identity) == 0);
        } else if (trigger == 1u) {
            /* PLAN metadata advances before origin proof; the old results
             * remain in the shared bank until actual execution starts. */
            gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN_RF;
            gateway_state.plan_build.plan.batch_index = 1u;
            gateway_state.plan_build.plan.self_stop_delay_ms = 5000u;
            gateway_state.pending_control_handle = 7u;
            gateway_state.pending_control_deadline_ms = now_ms + 1000u;
            origin_error = -EIO;
            gateway_work_handler(NULL);
        } else {
            now_ms = trigger == 2u ? gateway_state.plan_deadline_ms :
                                     gateway_state.self_stop_ms;
            gateway_work_handler(NULL);
        }
        if (gateway_state.active) {
            assert(gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP);
            now_ms = gateway_state.cleanup_deadline_ms;
            gateway_work_handler(NULL);
        }
        tick_publication(SURVEY_EVENT_TERMINAL);
        assert(observed.batch_index == 1u);
        assert(observed.result_count == 0u && !observed.final_batch);
        assert(observed.status == (trigger < 2u ? SURVEY_TERMINAL_ABORTED :
                                                  SURVEY_TERMINAL_PARTIAL));
        assert(!gateway_state.active && releases == 1u);
    }
}

static void pending_origin_cannot_shorten_remote_cleanup(void)
{
    for (unsigned plan = 0u; plan < 2u; plan++) {
        for (unsigned failed_origin = 0u; failed_origin < 2u; failed_origin++) {
            reset();
            admission = 0;
            survey_ops.send_control = send_control;
            survey_ops.control_detach = detach;
            survey_ops.control_abandon = detach;
            gateway_state.stage = plan ? APP_SURVEY_GATEWAY_WAIT_PLAN_RF :
                                         APP_SURVEY_GATEWAY_WAIT_START_RF;
            gateway_state.plan_build.plan.self_stop_delay_ms = 5000u;
            gateway_state.pending_control_handle = 7u;
            gateway_state.pending_control_deadline_ms =
                now_ms + SURVEY_CONTROL_ORIGIN_BUDGET_MS;
            gateway_state.self_stop_ms = now_ms + 100u;
            uint64_t remote_delay = plan ?
                gateway_state.plan_build.plan.self_stop_delay_ms :
                SURVEY_INITIAL_SELF_EXPIRY_MS;
            uint64_t old_bound = now_ms + remote_delay;
            uint64_t required_bound =
                gateway_state.pending_control_deadline_ms + remote_delay;
            if (failed_origin) {
                origin_error = -EIO;
                gateway_work_handler(NULL);
            } else {
                assert(app_survey_gateway_abort(&gateway_state.identity) == 0);
            }
            assert(gateway_state.pending_control_handle == 0u);
            assert(gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP);
            assert(gateway_state.cleanup_deadline_ms == required_bound);
            /* Abandoning a handle cannot retract a physical copy already
             * committed to start later within the original origin budget. */
            now_ms = old_bound;
            gateway_work_handler(NULL);
            assert(gateway_state.active && releases == 0u);
            assert(gateway_state.publication_pending == 0u);
            now_ms = required_bound;
            gateway_work_handler(NULL);
            assert(!gateway_state.active && releases == 1u);
            tick_publication(SURVEY_EVENT_TERMINAL);
            assert(observed.status == SURVEY_TERMINAL_ABORTED);
            assert(observed.result_count == 0u);
        }
    }
}

static struct survey_host_plan_request plan_request(uint8_t count, bool final)
{
    reset();
    survey_ops.send_control = send_control;
    survey_ops.control_detach = detach;
    survey_ops.control_abandon = detach;
    controls = 0u;
    gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN;
    gateway_state.identity.assignment.assignment_epoch = 7u;
    gateway_state.identity.assignment.table_command_seq = 8u;
    gateway_state.identity.assignment.slot_span = SURVEY_MAX_ANCHORS;
    memset(gateway_state.identity.assignment.table_commitment.bytes, 0xa5,
           sizeof(gateway_state.identity.assignment.table_commitment.bytes));
    gateway_state.plan_deadline_ms = now_ms + SURVEY_HOST_PLAN_TIMEOUT_MS;
    gateway_state.hard_deadline_ms = now_ms + SURVEY_HARD_CAP_MS;
    gateway_state.control_delivery_ms = survey_control_delivery_delay_ms(1u);
    gateway_state.graph.occupied_slot_mask =
        (UINT64_C(1) << SURVEY_MAX_ANCHORS) - 1u;
    gateway_state.graph.received_report_mask = gateway_state.graph.occupied_slot_mask;
    for (uint8_t slot = 0u; slot < SURVEY_MAX_ANCHORS; slot++) {
        gateway_state.graph.reports[slot].own_slot = slot;
        gateway_state.hop_counts[slot] = 1u;
        for (uint8_t peer = 0u; peer < SURVEY_MAX_ANCHORS; peer++) {
            if (peer != slot) {
                assert(survey_neighbor_bitmap_set(
                    gateway_state.graph.reports[slot].heard_bitmap, peer));
            }
        }
    }
    struct survey_host_plan_request request = {
        .identity = gateway_state.identity, .pair_count = count,
        .host_session_id = 99u, .host_sequence = 43u,
        .final_batch = final,
    };
    for (uint8_t pair = 0u; pair < count; pair++) {
        request.pairs[pair].first_slot = pair % SURVEY_MAX_ANCHORS;
        request.pairs[pair].second_slot = (uint8_t)(
            (pair % SURVEY_MAX_ANCHORS + 1u + pair / SURVEY_MAX_ANCHORS) %
            SURVEY_MAX_ANCHORS);
    }
    return request;
}

static void plan_admission_preserves_all_deadlines(void)
{
    for (unsigned half = 0u; half < 2u; half++) {
        struct survey_host_plan_request request = plan_request(1u, true);
        if (half == 0u) request.host_session_id = 0u;
        else request.host_sequence = 0u;
        struct app_survey_gateway_state before = gateway_state;
        assert(app_survey_gateway_submit_plan(&request, NULL) == -EINVAL);
        assert(controls == 0u && memcmp(&before, &gateway_state, sizeof(before)) == 0);
    }
    for (unsigned expired = 0u; expired < 3u; expired++) {
        struct survey_host_plan_request request = plan_request(1u, true);
        uint64_t *deadline = expired == 0u ? &gateway_state.plan_deadline_ms :
            expired == 1u ? &gateway_state.self_stop_ms :
                            &gateway_state.hard_deadline_ms;
        *deadline = now_ms;
        struct app_survey_gateway_state before = gateway_state;
        assert(app_survey_gateway_submit_plan(&request, NULL) == -ESTALE);
        assert(controls == 0u && memcmp(&before, &gateway_state, sizeof(before)) == 0);
    }
    for (unsigned final = 0u; final < 2u; final++) {
        struct survey_host_plan_request request =
            plan_request(SURVEY_MAX_PAIRS, final != 0u);
        struct survey_plan_build_result built, accepted;
        assert(survey_build_plan(&request.identity, &gateway_state.graph,
            gateway_state.hop_counts, request.pairs, request.pair_count,
            gateway_state.control_delivery_ms, 0u, request.final_batch,
            &built) == PROTO_OK);
        assert(built.plan.pair_count == SURVEY_MAX_PAIRS);
        assert(built.plan.wave_count == SURVEY_MAX_PAIRS);
        uint64_t required_end = now_ms + SURVEY_CONTROL_ORIGIN_BUDGET_MS +
                                built.plan.self_stop_delay_ms;
        gateway_state.hard_deadline_ms = required_end - 1u;
        struct app_survey_gateway_state before = gateway_state;
        assert(app_survey_gateway_submit_plan(&request, NULL) == -E2BIG);
        assert(controls == 0u && memcmp(&before, &gateway_state, sizeof(before)) == 0);

        gateway_state.hard_deadline_ms = required_end;
        uint64_t handoff = now_ms + SURVEY_CONTROL_ORIGIN_BUDGET_MS +
                           gateway_state.control_delivery_ms;
        gateway_state.self_stop_ms = handoff - 1u;
        before = gateway_state;
        assert(app_survey_gateway_submit_plan(&request, NULL) == -E2BIG);
        assert(controls == 0u && memcmp(&before, &gateway_state, sizeof(before)) == 0);
        gateway_state.self_stop_ms = handoff;
        assert(app_survey_gateway_submit_plan(&request, &accepted) == 0);
        assert(controls == 1u && accepted.plan.pair_count == SURVEY_MAX_PAIRS);
        assert(accepted.plan.self_stop_delay_ms == built.plan.self_stop_delay_ms);
        assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN_RF);
        assert(gateway_state.pending_control_deadline_ms ==
               now_ms + SURVEY_CONTROL_ORIGIN_BUDGET_MS);
    }
}

static void gateway_bundle_admission_is_atomic(void)
{
    struct survey_response_bundle bundle = {
        .network_id = NETWORK_ID, .generation = 10u,
        .parent_id = DEVICE_ID, .sender_id = 0x11u,
        .kind = SURVEY_RESPONSE_RANGES, .batch_index = 1u,
        .record_count = 2u,
    };
    struct survey_response_hop_ack ack, unchanged;
    struct survey_range_result result = {
        .pair_index = 0u, .responder_slot = 1u,
        .success_count = 3u, .median_mm = 1200u,
    };
    reset();
    gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;
    gateway_state.response_kind = SURVEY_RESPONSE_RANGES;
    gateway_state.response_lane_start_ms = now_ms;
    gateway_state.plan_build.plan.batch_index = 1u;
    gateway_state.plan_build.plan.pair_count = 2u;
    gateway_state.plan_build.plan.pairs[0].responder_slot = 1u;
    gateway_state.plan_build.plan.pairs[1].responder_slot = 1u;
    assert(survey_range_result_encode(&result, bundle.records[0].bytes) == 8u);
    result.pair_index = 1u;
    result.responder_slot = 2u; /* Valid wire record, wrong plan endpoint. */
    assert(survey_range_result_encode(&result, bundle.records[1].bytes) == 8u);
    struct app_survey_gateway_state before;
    memcpy(&before, &gateway_state, sizeof(before));
    memset(&ack, 0xa5, sizeof(ack));
    unchanged = ack;
    assert(app_survey_gateway_handle_bundle(&bundle, now_ms, &ack) == -ESTALE);
    assert(memcmp(&before, &gateway_state, sizeof(before)) == 0);
    assert(memcmp(&unchanged, &ack, sizeof(ack)) == 0);
    result.responder_slot = 1u;
    assert(survey_range_result_encode(&result, bundle.records[1].bytes) == 8u);
    assert(app_survey_gateway_handle_bundle(&bundle, now_ms, &ack) == 0);
    assert(gateway_state.result_received_mask[0] == 3u);
    assert(ack.batch_index == 1u);
    before = gateway_state;
    assert(app_survey_gateway_handle_bundle(&bundle, now_ms, &ack) == 0);
    assert(memcmp(&before, &gateway_state, sizeof(before)) == 0);
    bundle.batch_index = 0u;
    assert(app_survey_gateway_handle_bundle(&bundle, now_ms, &ack) == -ESTALE);
    assert(memcmp(&before, &gateway_state, sizeof(before)) == 0);
}
static void setup_start(void)
{
    reset();
    gateway_state.active = false;
    survey_ops.next_generation = allocate_generation;
    survey_ops.send_control = send_control;
    survey_ops.control_detach = detach;
    survey_ops.control_abandon = detach;
    allocations = controls = 0u;
    allocator_error = 0;
}
static void start_recovery_identity_and_publication_gate(void)
{
    struct app_survey_gateway_roster roster = {
        .assignment = {.assignment_epoch = 7u, .table_command_seq = 8u,
                       .slot_span = 2u, .max_hop_count = 1u},
        .host_session_id = 0x12345678u, .host_sequence = 42u,
        .node_ids = {0x11u, 0x22u}, .slots = {0u, 1u},
        .hop_counts = {1u, 1u}, .node_count = 2u,
    };
    struct survey_identity first, next;
    memset(roster.assignment.table_commitment.bytes, 0xa5,
           sizeof(roster.assignment.table_commitment.bytes));
    setup_start();
    for (unsigned missing = 0u; missing < 3u; missing++) {
        struct app_survey_gateway_roster malformed = roster;
        if (missing != 1u) malformed.host_session_id = 0u;
        if (missing != 0u) malformed.host_sequence = 0u;
        struct app_survey_gateway_state unchanged = gateway_state;
        assert(app_survey_gateway_start(&malformed, &first) == -EINVAL);
        assert(allocations == 0u && controls == 0u);
        assert(memcmp(&unchanged, &gateway_state, sizeof(unchanged)) == 0);
    }
    gateway_state.publication_pending = 1u;
    assert(app_survey_gateway_start(&roster, &first) == -EBUSY);
    assert(allocations == 0u && controls == 0u);
    gateway_state.publication_pending = 0u;
    allocator_error = -EIO;
    struct app_survey_gateway_state before = gateway_state;
    assert(app_survey_gateway_start(&roster, &first) == -EIO);
    assert(memcmp(&before, &gateway_state, sizeof(before)) == 0);
    assert(controls == 0u);
    allocator_error = 0;
    reserved_generation = 0u;
    assert(app_survey_gateway_start(&roster, &first) == -EIO);
    assert(memcmp(&before, &gateway_state, sizeof(before)) == 0);
    assert(controls == 0u);
    reserved_generation = 4000u;
    assert(app_survey_gateway_start(&roster, &first) == 0);
    assert(first.generation == 4000u && controls == 1u);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_START_RF);
    assert(gateway_state.graph.received_report_mask == 0u);
    assert(gateway_state.publication_pending == (1u << SURVEY_EVENT_STARTED));
    gateway_publication_work_handler(NULL);
    assert(attempts == 1u && observed.kind == SURVEY_EVENT_STARTED);
    assert(observed.identity.generation == first.generation);
    assert(observed.status == SURVEY_TERMINAL_COMPLETE);
    assert(observed.host_session_id == roster.host_session_id);
    assert(observed.host_sequence == roster.host_sequence);
    assert(gateway_state.publication_pending == (1u << SURVEY_EVENT_STARTED));
    struct survey_event accepted = observed;
    admission = 0;
    gateway_publication_work_handler(NULL);
    assert(attempts == 2u && observed.kind == SURVEY_EVENT_STARTED);
    assert(survey_identity_equal(&observed.identity, &accepted.identity));
    assert(observed.host_session_id == accepted.host_session_id);
    assert(observed.host_sequence == accepted.host_sequence);
    assert(gateway_state.publication_pending == 0u);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_START_RF);
    const unsigned allocations_before_busy = allocations;
    assert(app_survey_gateway_start(&roster, &next) == -EBUSY);
    assert(controls == 1u && allocations == allocations_before_busy);
    /* Reinitializing the survey RAM must not rewind its independent reserved
     * identity source. Durable allocator block behavior has its own tests. */
    setup_start();
    assert(app_survey_gateway_start(&roster, &next) == 0);
    assert(next.generation == first.generation + 1u);
    assert(allocations == 1u && controls == 1u);
}

static void malformed_acceptance_envelopes(const struct survey_event *event)
{
    uint8_t payload[SURVEY_EVENT_MAX_WIRE_LEN + 1u], mutated[sizeof(payload)];
    size_t length = survey_event_encode(event, payload, sizeof(payload));
    uint32_t generation;
    struct survey_event decoded;
    assert(length != 0u);
    assert(gateway_survey_event_envelope_valid(payload, length, &generation));
    assert(generation == event->identity.generation);
    assert(!gateway_survey_event_envelope_valid(payload, length - 1u, &generation));
    payload[length] = 0u;
    assert(!gateway_survey_event_envelope_valid(payload, length + 1u, &generation));
    const uint8_t reserved[] = {62u, 63u, 65u, 66u, 67u, 68u, 69u, 70u, 71u};
    for (size_t i = 0u; i < ARRAY_SIZE(reserved); i++) {
        memcpy(mutated, payload, length);
        mutated[reserved[i]] = 1u;
        assert(!gateway_survey_event_envelope_valid(mutated, length, &generation));
        assert(survey_event_decode(mutated, length, &decoded) != PROTO_OK);
    }
    for (unsigned malformed = 0u; malformed < 5u; malformed++) {
        memcpy(mutated, payload, length);
        if (malformed == 0u) memset(&mutated[56], 0, 4u);
        if (malformed == 1u) memset(&mutated[60], 0, 2u);
        if (malformed == 2u) mutated[2] = SURVEY_TERMINAL_ABORTED;
        if (malformed == 3u) mutated[10] = 1u;
        if (malformed == 4u) mutated[1] = SURVEY_EVENT_STARTED + 1u;
        assert(!gateway_survey_event_envelope_valid(mutated, length, &generation));
        assert(survey_event_decode(mutated, length, &decoded) != PROTO_OK);
    }
    /* Legacy PLAN projections have neither host field. STARTED always needs
     * both fields to identify the original host command. */
    memcpy(mutated, payload, length);
    memset(&mutated[56], 0, 6u);
    bool legacy_plan = event->kind == SURVEY_EVENT_PLAN_ACCEPTED;
    assert(gateway_survey_event_envelope_valid(mutated, length, &generation) == legacy_plan);
    assert((survey_event_decode(mutated, length, &decoded) == PROTO_OK) == legacy_plan);
}

static void admitted_survey_lifecycle_and_status_replay(void)
{
    setup_start();
    admission = 0;
    struct app_survey_gateway_roster roster = {
        .assignment = gateway_state.identity.assignment,
        .host_session_id = 0x76543210u, .host_sequence = 51u,
        .node_ids = {0x11u, 0x22u}, .slots = {0u, 1u},
        .hop_counts = {1u, 1u}, .node_count = 2u,
    };
    struct survey_identity identity;
    struct survey_event replay;
    assert(app_survey_gateway_start(&roster, &identity) == 0);
    gateway_publication_work_handler(NULL);
    assert(observed.kind == SURVEY_EVENT_STARTED);
    assert(app_survey_gateway_status(&replay) == 0);
    assert(emit(&replay) == 0);
    assert(app_survey_gateway_acceptance(SURVEY_EVENT_STARTED, &replay) == 0);
    malformed_acceptance_envelopes(&replay);
    assert(emit(&replay) == 0);
    assert(app_survey_gateway_acceptance(SURVEY_EVENT_PLAN_ACCEPTED, &replay) == -ENOENT);

    origin_time_ms = now_ms;
    gateway_work_handler(NULL);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_NEIGHBORS);
    now_ms = gateway_state.response_lane_start_ms;
    struct survey_response_bundle bundle = {
        .network_id = NETWORK_ID, .generation = identity.generation,
        .sender_id = 0x11u, .parent_id = DEVICE_ID,
        .kind = SURVEY_RESPONSE_NEIGHBORS, .record_count = 3u,
    };
    for (uint8_t slot = 0u; slot < 2u; slot++) {
        struct survey_neighbor_report report = {.own_slot = slot};
        assert(survey_neighbor_bitmap_set(report.heard_bitmap, 1u - slot));
        assert(survey_neighbor_report_encode(&report, bundle.records[slot].bytes) == 8u);
    }
    const uint8_t levels[SURVEY_MAX_ANCHORS] = {8u};
    struct survey_signal_record signal;
    assert(survey_signal_record_encode(1u, 0u, levels, &signal) == 8u);
    memcpy(bundle.records[2].bytes, signal.bytes, sizeof(signal.bytes));
    struct survey_response_hop_ack ack;
    assert(app_survey_gateway_handle_bundle(&bundle, now_ms, &ack) == 0);
    now_ms = gateway_state.response_lane_end_ms;
    gateway_work_handler(NULL);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN);
    gateway_publication_work_handler(NULL);
    assert(observed.kind == SURVEY_EVENT_NEIGHBOR_GRAPH);
    assert(observed.graph.received_report_mask == 3u);
    assert(survey_graph_mutual(&observed.graph, 0u, 1u));
    gateway_publication_work_handler(NULL);
    assert(observed.kind == SURVEY_EVENT_SIGNALS && observed.signal_count == 1u);

    struct survey_host_plan_request request = {
        .identity = identity, .host_session_id = roster.host_session_id,
        .host_sequence = 52u, .pair_count = 2u, .final_batch = true,
        .pairs = {{0u, 1u}, {1u, 0u}},
    };
    struct survey_plan_build_result accepted;
    assert(app_survey_gateway_submit_plan(&request, &accepted) == 0);
    assert(accepted.plan.pair_count == 1u && accepted.skipped_count == 1u);
    origin_time_ms = now_ms;
    gateway_work_handler(NULL);
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING);
    gateway_publication_work_handler(NULL);
    assert(observed.kind == SURVEY_EVENT_PLAN_ACCEPTED);
    assert(observed.status == SURVEY_TERMINAL_PARTIAL);
    assert(observed.host_session_id == request.host_session_id);
    assert(observed.host_sequence == request.host_sequence);
    assert(app_survey_gateway_acceptance(SURVEY_EVENT_PLAN_ACCEPTED, &replay) == 0);
    malformed_acceptance_envelopes(&replay);
    assert(emit(&replay) == 0);

    struct survey_range_result result = {
        .responder_slot = accepted.plan.pairs[0].responder_slot,
        .success_count = 5u, .median_mm = 1200u,
    };
    bundle.kind = SURVEY_RESPONSE_RANGES;
    bundle.record_count = 1u;
    assert(survey_range_result_encode(&result, bundle.records[0].bytes) == 8u);
    now_ms = gateway_state.response_lane_start_ms;
    assert(app_survey_gateway_handle_bundle(&bundle, now_ms, &ack) == 0);
    for (unsigned stride = 0u; stride < 1u + SURVEY_EXTRA_DRAIN_STRIDES; stride++) {
        now_ms = gateway_state.response_lane_end_ms;
        gateway_work_handler(NULL);
        if (gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING) {
            assert(observed.kind == SURVEY_EVENT_RANGE_PROGRESS);
            assert(app_survey_gateway_status(&replay) == 0);
            assert(emit(&replay) == 0);
            assert(observed.result_count == 1u && observed.records.results[0].median_mm == 1200u);
        }
    }
    assert(gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP);
    assert_cleanup_status_retains_owner();
    now_ms = gateway_state.cleanup_deadline_ms;
    assert_cleanup_status_retains_owner();
    gateway_work_handler(NULL);
    gateway_publication_work_handler(NULL);
    assert(observed.kind == SURVEY_EVENT_TERMINAL);
    assert(observed.status == SURVEY_TERMINAL_PARTIAL && observed.final_batch);
    assert(observed.partial_reasons == SURVEY_PARTIAL_SKIPPED_PLAN_ENTRY);
    assert(!gateway_state.active && releases == 1u);
    assert(app_survey_gateway_status(&replay) == 0);
    assert(emit(&replay) == 0);
    assert(app_survey_gateway_acceptance(SURVEY_EVENT_STARTED, &replay) == 0);
    assert(emit(&replay) == 0 && observed.host_sequence == roster.host_sequence);
    assert(app_survey_gateway_acceptance(SURVEY_EVENT_PLAN_ACCEPTED, &replay) == 0);
    assert(emit(&replay) == 0 && observed.host_sequence == request.host_sequence);
}

int main(void)
{
    start_recovery_identity_and_publication_gate();
    admitted_survey_lifecycle_and_status_replay();
    cleanup_status_and_publication_wait_for_owner_release();
    graph_signals_terminal();
    plan_batch_terminal();
    earlier_partial_batch_survives_clean_final_batch();
    later_terminal_has_current_batch_and_no_previous_results();
    pending_origin_cannot_shorten_remote_cleanup();
    plan_admission_preserves_all_deadlines();
    adapter_admission_identity();
    gateway_bundle_admission_is_atomic();
    assert(emitted_kinds == ((1u << (SURVEY_EVENT_STARTED + 1u)) - 2u));
    puts("survey publication retry and radio cleanup tests passed");
    return 0;
}
