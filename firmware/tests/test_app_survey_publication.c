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
    bool (*anchor_handle_click_wake_claim)(
        const struct uwb_wake_claim_frame *claim,
        uint8_t link_quality,
        int64_t received_at_ms);
};

struct app_survey_gateway_roster {
    struct survey_assignment_identity assignment;
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
#include "survey_publication_production.inc"
static int emit(const struct survey_event *event)
{ assert(lock_depth == 0u); attempts++; observed = *event; return admission; }
static void release(void) { assert(lock_depth == 0u); releases++; }
static int origin(uint32_t handle, uint64_t *out)
{ assert(handle == 7u); *out = 100u; return 0; }
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
    gateway_state.identity.assignment.slot_span = 2u;
    gateway_state.identity.assignment.max_hop_count = 1u;
    gateway_state.active = true;
    gateway_state.hard_deadline_ms = 500000u;
    gateway_state.self_stop_ms = 180000u;
    gateway_state.graph.occupied_slot_mask = 3u;
    gateway_state.graph.received_report_mask = 3u;
    now_ms = 1000u;
    admission = -ENOSPC;
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
    gateway_state.plan_build.plan.wave_count = 1u;
    gateway_state.plan_build.plan.pair_count = 1u;
    gateway_state.plan_build.plan.pairs[0].responder_slot = 1u;
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
static uint16_t gateway_survey_event_seq, admitted_seq;
static unsigned ble_attempts;
static int ble_admission;
static uint32_t k_uptime_get_32(void) { return (uint32_t)now_ms; }
static int gateway_ble_stream_packet(const struct proto_packet *packet,
                                     const uint8_t *payload, size_t length,
                                     uint32_t timestamp)
{
    (void)timestamp;
    assert(lock_depth == 1u);
    assert(length != 0u && payload != NULL);
    assert(packet->msg_type == MSG_SURVEY_EVENT);
    assert(packet->flags == FLAG_GATEWAY_ACK_REQUIRED);
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
        .node_ids = {0x11u, 0x22u}, .slots = {0u, 1u},
        .hop_counts = {1u, 1u}, .node_count = 2u,
    };
    struct survey_identity first, next;
    memset(roster.assignment.table_commitment.bytes, 0xa5,
           sizeof(roster.assignment.table_commitment.bytes));
    setup_start();
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

int main(void)
{
    graph_signals_terminal();
    plan_batch_terminal();
    adapter_admission_identity();
    start_recovery_identity_and_publication_gate();
    gateway_bundle_admission_is_atomic();
    puts("survey publication retry and radio cleanup tests passed");
    return 0;
}
