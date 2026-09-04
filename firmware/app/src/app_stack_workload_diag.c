#include "app_stack_workload_diag.h"

#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)

#include "app_stack_diag.h"

/*
 * DEVICE_ROLE is the application CMake compile definition; the native unit
 * build compiles this translation unit without it and without app_config.h.
 */
#if defined(DEVICE_ROLE)
#include "app_config.h"
#include <zephyr/init.h>
#endif

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define STACK_WORKLOAD_DIAG_MAX_RUNS APP_STACK_DIAG_MAX_ACTIVE_RUNS

_Static_assert(STACK_WORKLOAD_DIAG_MAX_RUNS >= APP_STACK_DIAG_COMBINED_PEAK_RUNS,
               "workload diagnostics must cover the combined peak");

struct stack_workload_diag_run {
    struct proto_packet packet;
    uint32_t run_id;
    enum app_stack_diag_workload workload;
    bool active;
};

static struct stack_workload_diag_run stack_workload_diag_runs[STACK_WORKLOAD_DIAG_MAX_RUNS];
K_MUTEX_DEFINE(stack_workload_diag_mutex);

#if defined(CONFIG_IMEC_STACK_WORKLOAD_DIAG_TEST_HOOKS)
void app_stack_workload_diag_test_lock_attempt(void);
#endif

static void stack_workload_diag_lock(void)
{
#if defined(CONFIG_IMEC_STACK_WORKLOAD_DIAG_TEST_HOOKS)
    app_stack_workload_diag_test_lock_attempt();
#endif
    k_mutex_lock(&stack_workload_diag_mutex, K_FOREVER);
}

static bool packet_matches(const struct proto_packet *left,
                           const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->msg_type == right->msg_type;
}

static struct app_stack_diag_state stack_workload_diag_state(
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct app_stack_diag_state state = {0};

    if (pressure != NULL) {
        state.queue_depth = pressure->queue_depth;
        state.custody_depth = pressure->custody_depth;
        state.credit_available = pressure->credit_available;
        state.retry_depth = pressure->retry_depth;
        state.drain_depth = pressure->drain_depth;
    }

    if (packet != NULL) {
        state.source_id = packet->src_id;
        state.destination_id = packet->dst_id;
        state.session_id = packet->session_id;
        state.packet_sequence = packet->seq;
        state.message_type = packet->msg_type;
    }
    return state;
}

static struct stack_workload_diag_run *stack_workload_diag_find_locked(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet)
{
    for (size_t index = 0u; index < STACK_WORKLOAD_DIAG_MAX_RUNS; index++) {
        struct stack_workload_diag_run *run = &stack_workload_diag_runs[index];

        if (run->active && run->workload == workload &&
            packet_matches(&run->packet, packet)) {
            return run;
        }
    }
    return NULL;
}

static void stack_workload_diag_admit_locked(
    enum app_stack_diag_workload workload,
    enum app_stack_diag_owner owner,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_run *slot = NULL;
    struct app_stack_diag_state state;

    if (packet == NULL ||
        stack_workload_diag_find_locked(workload, packet) != NULL) {
        return;
    }
    for (size_t index = 0u; index < STACK_WORKLOAD_DIAG_MAX_RUNS; index++) {
        if (!stack_workload_diag_runs[index].active) {
            slot = &stack_workload_diag_runs[index];
            break;
        }
    }
    if (slot == NULL) {
        state = stack_workload_diag_state(packet, pressure);
        (void)app_stack_diag_run_begin(workload, owner, &state);
        return;
    }
    state = stack_workload_diag_state(packet, pressure);
    slot->run_id = app_stack_diag_run_begin(workload, owner, &state);
    if (slot->run_id == 0u) {
        return;
    }
    slot->packet = *packet;
    slot->workload = workload;
    slot->active = true;
}

static void stack_workload_diag_sample_locked(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_run *run =
        stack_workload_diag_find_locked(workload, packet);
    struct app_stack_diag_state state;

    if (run == NULL) {
        return;
    }
    state = stack_workload_diag_state(packet, pressure);
    (void)app_stack_diag_sample(run->run_id, &state);
}

static void stack_workload_diag_release_locked(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_run *run =
        stack_workload_diag_find_locked(workload, packet);
    struct app_stack_diag_state state;

    if (run == NULL) {
        return;
    }
    state = stack_workload_diag_state(packet, pressure);
    if (app_stack_diag_run_end(run->run_id, outcome, &state) == 0) {
        memset(run, 0, sizeof(*run));
    }
}

/*
 * Every workload lane is queued here and replayed from one work item, so no
 * caller thread ever emits a record.
 *
 * The sampler blocks on RTT buffer space so its records stay complete, which
 * measures 100-300 ms per record burst inside whichever thread calls it.  On
 * the anchor those callers are the UWB scan thread (report encode, scan
 * cycle) and the mesh route thread (queue admission, pre-TX); on the gateway
 * they are the mesh route thread (report ingress, relay retry) and the system
 * workqueue (BLE backpressure, priority control).  A synchronous sample costs
 * the click report several delivery slot cycles, and one taken on the system
 * workqueue additionally preempts the mesh route thread for the whole stack
 * walk because that queue runs cooperatively above it.
 *
 * A single consumer keeps admit/sample/release strictly ordered per run,
 * so a sample cannot overtake its own run boundary.
 * Stack high-water marks are monotonic, so replaying a few hundred
 * milliseconds later still captures the same peak.
 */
#define STACK_WORKLOAD_DIAG_DEFER_MS 400u
/*
 * A record refused by the radio busy hook is put back at the head and retried
 * soon enough to stay inside the same capture window.
 */
#define STACK_WORKLOAD_DIAG_DEFER_CRITICAL_RETRY_MS 20u
/*
 * The hook is a transient radio-path predicate.  Bounding the consecutive
 * holds keeps a wedged radio state from silently suppressing every run the
 * hardware workload policy requires.
 */
#define STACK_WORKLOAD_DIAG_DEFER_CRITICAL_HOLD_MAX 100u
/*
 * The anchor scan cycle runs once per low-duty scan iteration, which is a
 * continuous loop while a click burst is in flight.  Hardware qualification
 * needs one completed anchor_scan run per capture, not one per iteration, so
 * the lane is rate limited.  This also keeps the RTT up-buffer drained enough
 * that the ordinary DBG_* markers are not dropped by buffer pressure.
 */
#define STACK_WORKLOAD_DIAG_ANCHOR_SCAN_MIN_INTERVAL_MS 1000u

/*
 * BLE backpressure is the only lane that reports credit, retry, and drain
 * depth, and it exists only on the gateway.  The native unit build has no
 * role definition and exercises that lane, so it keeps the wide record too.
 */
#if !defined(DEVICE_ROLE) || DEVICE_ROLE == ROLE_GATEWAY
#define STACK_WORKLOAD_DIAG_DEFER_WIDE_PRESSURE 1
#else
#define STACK_WORKLOAD_DIAG_DEFER_WIDE_PRESSURE 0
#endif

/*
 * The gateway queue must hold a whole click burst: two to three reports plus
 * the BLE admit/sample/release triples they raise inside the same 100 ms,
 * produced concurrently by the mesh route thread, the system workqueue, and
 * the Bluetooth receive thread.  The anchor and clicker raise one lane at a
 * time and have to fit the queue inside the anchor's static RAM floor, so
 * they keep the reviewed six-record ring.
 */
#if STACK_WORKLOAD_DIAG_DEFER_WIDE_PRESSURE
#define STACK_WORKLOAD_DIAG_DEFER_DEPTH 16u
#else
#define STACK_WORKLOAD_DIAG_DEFER_DEPTH 6u
#endif

/*
 * Only the gateway can afford a second thread stack; see the anchor row of
 * STACK_BUDGET_DEPLOYABLE_PRESET_POLICY.
 */
#if defined(DEVICE_ROLE) && DEVICE_ROLE == ROLE_GATEWAY
#define STACK_WORKLOAD_DIAG_DEDICATED_QUEUE 1
#else
#define STACK_WORKLOAD_DIAG_DEDICATED_QUEUE 0
#endif

#if STACK_WORKLOAD_DIAG_DEDICATED_QUEUE
/*
 * Measured watermark on the gateway is reported by every typed sample as the
 * stack_diag row, allowing its remaining margin to be inspected.
 */
#define STACK_WORKLOAD_DIAG_QUEUE_STACK_SIZE 2048u
#else
/*
 * Without a dedicated thread the replay runs on the cooperative system
 * workqueue, so it sleeps between records to hand the CPU back to the
 * preemptible mesh route thread instead of holding it across the burst.
 */
#define STACK_WORKLOAD_DIAG_DEFER_RECORD_GAP_MS 1u
#endif

enum stack_workload_diag_defer_op {
    STACK_WORKLOAD_DIAG_DEFER_ADMIT = 0,
    STACK_WORKLOAD_DIAG_DEFER_SAMPLE,
    STACK_WORKLOAD_DIAG_DEFER_RELEASE,
    STACK_WORKLOAD_DIAG_DEFER_RELEASE_ALL,
    STACK_WORKLOAD_DIAG_DEFER_CYCLE,
};

/*
 * The queue lives in the anchor's static RAM headroom, so it carries the
 * exact identity the correlation table matches on rather than a whole packet
 * model, and only the pressure depths the compiled lanes actually report.
 */
struct stack_workload_diag_defer_event {
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
    uint16_t queue_depth;
    uint16_t custody_depth;
#if STACK_WORKLOAD_DIAG_DEFER_WIDE_PRESSURE
    uint16_t credit_available;
    uint16_t retry_depth;
    uint16_t drain_depth;
#endif
    uint8_t msg_type;
    uint8_t op;
    uint8_t workload;
    uint8_t owner;
    uint8_t outcome;
    uint8_t reserved;
};

static struct stack_workload_diag_defer_event
    stack_workload_diag_defer_ring[STACK_WORKLOAD_DIAG_DEFER_DEPTH];
static struct k_work_delayable stack_workload_diag_defer_work;
static uint8_t stack_workload_diag_defer_head;
static uint8_t stack_workload_diag_defer_count;
static uint8_t stack_workload_diag_defer_critical_holds;
static bool stack_workload_diag_defer_scheduled;
static bool stack_workload_diag_defer_ready;
static bool (*stack_workload_diag_critical_hook)(void);
static uint32_t stack_workload_diag_anchor_scan_last_ms;
static bool stack_workload_diag_anchor_scan_seen;

/*
 * The queue keeps its own lock.  It is never held across a record emit, so a
 * producer on the scan, route, BLE, or system workqueue thread never waits
 * for the RTT transport.
 */
K_MUTEX_DEFINE(stack_workload_diag_defer_mutex);

#if STACK_WORKLOAD_DIAG_DEDICATED_QUEUE
K_THREAD_STACK_DEFINE(stack_workload_diag_work_q_stack,
                      STACK_WORKLOAD_DIAG_QUEUE_STACK_SIZE);
static struct k_work_q stack_workload_diag_work_q;
static const struct k_work_queue_config stack_workload_diag_work_q_config = {
    .name = "stack_diag",
};
static bool stack_workload_diag_work_q_ready;

/*
 * K_LOWEST_APPLICATION_THREAD_PRIO is preemptible and below every mesh owner,
 * so a stack walk here can never take the CPU away from an ACK transmit or an
 * RX re-arm.  The queue starts before main() hands control to the role, so no
 * production record is ever produced before it exists.
 */
static int stack_workload_diag_queue_init(void)
{
    k_work_queue_start(&stack_workload_diag_work_q,
                       stack_workload_diag_work_q_stack,
                       K_THREAD_STACK_SIZEOF(stack_workload_diag_work_q_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO,
                       &stack_workload_diag_work_q_config);
    stack_workload_diag_work_q_ready = true;
    return 0;
}

SYS_INIT(stack_workload_diag_queue_init, APPLICATION, 98);
#endif

void app_stack_workload_diag_set_critical_hook(bool (*critical)(void))
{
    stack_workload_diag_critical_hook = critical;
}

static void stack_workload_diag_defer_work_handler(struct k_work *work);

static void stack_workload_diag_defer_arm(uint32_t delay_ms)
{
#if STACK_WORKLOAD_DIAG_DEDICATED_QUEUE
    if (stack_workload_diag_work_q_ready) {
        (void)k_work_reschedule_for_queue(&stack_workload_diag_work_q,
                                          &stack_workload_diag_defer_work,
                                          K_MSEC(delay_ms));
        return;
    }
#endif
    (void)k_work_reschedule(&stack_workload_diag_defer_work, K_MSEC(delay_ms));
}

/*
 * A full queue drops the oldest additional sample first, then the oldest
 * self-contained cycle: both are extra evidence, while admissions and
 * completions are the run boundaries the verifier accounts against.
 */
static bool stack_workload_diag_defer_evict_locked(void)
{
    static const uint8_t shed_order[] = {
        (uint8_t)STACK_WORKLOAD_DIAG_DEFER_SAMPLE,
        (uint8_t)STACK_WORKLOAD_DIAG_DEFER_CYCLE,
    };

    for (size_t choice = 0u;
         choice < sizeof(shed_order) / sizeof(shed_order[0]); choice++) {
        for (uint8_t offset = 0u; offset < stack_workload_diag_defer_count;
             offset++) {
            size_t index = (size_t)((stack_workload_diag_defer_head + offset) %
                                    STACK_WORKLOAD_DIAG_DEFER_DEPTH);

            if (stack_workload_diag_defer_ring[index].op !=
                shed_order[choice]) {
                continue;
            }
            for (uint8_t back = offset; back > 0u; back--) {
                size_t destination =
                    (size_t)((stack_workload_diag_defer_head + back) %
                             STACK_WORKLOAD_DIAG_DEFER_DEPTH);
                size_t source =
                    (size_t)((stack_workload_diag_defer_head + back - 1u) %
                             STACK_WORKLOAD_DIAG_DEFER_DEPTH);

                stack_workload_diag_defer_ring[destination] =
                    stack_workload_diag_defer_ring[source];
            }
            stack_workload_diag_defer_head =
                (uint8_t)((stack_workload_diag_defer_head + 1u) %
                          STACK_WORKLOAD_DIAG_DEFER_DEPTH);
            stack_workload_diag_defer_count--;
            return true;
        }
    }
    return false;
}

static void stack_workload_diag_defer_submit(
    enum stack_workload_diag_defer_op op,
    enum app_stack_diag_workload workload,
    enum app_stack_diag_owner owner,
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    struct stack_workload_diag_defer_event event = {
        .op = (uint8_t)op,
        .workload = (uint8_t)workload,
        .owner = (uint8_t)owner,
        .outcome = (uint8_t)outcome,
    };
    size_t index;
    bool schedule = false;

    /* Only a lane-wide release has no packet identity of its own. */
    if (packet == NULL && op != STACK_WORKLOAD_DIAG_DEFER_RELEASE_ALL) {
        return;
    }
    if (packet != NULL) {
        event.src_id = packet->src_id;
        event.dst_id = packet->dst_id;
        event.session_id = packet->session_id;
        event.seq = packet->seq;
        event.msg_type = packet->msg_type;
    }
    if (pressure != NULL) {
        event.queue_depth = pressure->queue_depth;
        event.custody_depth = pressure->custody_depth;
#if STACK_WORKLOAD_DIAG_DEFER_WIDE_PRESSURE
        event.credit_available = pressure->credit_available;
        event.retry_depth = pressure->retry_depth;
        event.drain_depth = pressure->drain_depth;
#endif
    }

    k_mutex_lock(&stack_workload_diag_defer_mutex, K_FOREVER);
    if (!stack_workload_diag_defer_ready) {
        k_work_init_delayable(&stack_workload_diag_defer_work,
                              stack_workload_diag_defer_work_handler);
        stack_workload_diag_defer_ready = true;
    }
    if (stack_workload_diag_defer_count >= STACK_WORKLOAD_DIAG_DEFER_DEPTH &&
        !stack_workload_diag_defer_evict_locked()) {
        k_mutex_unlock(&stack_workload_diag_defer_mutex);
        return;
    }
    index = (size_t)((stack_workload_diag_defer_head +
                      stack_workload_diag_defer_count) %
                     STACK_WORKLOAD_DIAG_DEFER_DEPTH);
    stack_workload_diag_defer_ring[index] = event;
    stack_workload_diag_defer_count++;
    if (!stack_workload_diag_defer_scheduled) {
        stack_workload_diag_defer_scheduled = true;
        schedule = true;
    }
    k_mutex_unlock(&stack_workload_diag_defer_mutex);

    if (schedule) {
        stack_workload_diag_defer_arm(STACK_WORKLOAD_DIAG_DEFER_MS);
    }
}

/* Called with the correlation lock held; never emits without it. */
static void stack_workload_diag_defer_execute_locked(
    const struct stack_workload_diag_defer_event *event)
{
    enum app_stack_diag_workload workload =
        (enum app_stack_diag_workload)event->workload;
    enum app_stack_diag_terminal_outcome outcome =
        (enum app_stack_diag_terminal_outcome)event->outcome;
    const struct app_stack_workload_diag_pressure pressure = {
        event->queue_depth,
        event->custody_depth,
#if STACK_WORKLOAD_DIAG_DEFER_WIDE_PRESSURE
        event->credit_available,
        event->retry_depth,
        event->drain_depth,
#else
        0u, 0u, 0u,
#endif
    };
    const struct proto_packet packet = {
        .src_id = event->src_id,
        .dst_id = event->dst_id,
        .session_id = event->session_id,
        .seq = event->seq,
        .msg_type = event->msg_type,
    };

    switch ((enum stack_workload_diag_defer_op)event->op) {
    case STACK_WORKLOAD_DIAG_DEFER_ADMIT:
        stack_workload_diag_admit_locked(
            workload, (enum app_stack_diag_owner)event->owner, &packet,
            &pressure);
        break;
    case STACK_WORKLOAD_DIAG_DEFER_SAMPLE:
        stack_workload_diag_sample_locked(workload, &packet, &pressure);
        break;
    case STACK_WORKLOAD_DIAG_DEFER_RELEASE:
        stack_workload_diag_release_locked(workload, &packet, outcome,
                                           &pressure);
        break;
    case STACK_WORKLOAD_DIAG_DEFER_RELEASE_ALL:
        for (size_t index = 0u; index < STACK_WORKLOAD_DIAG_MAX_RUNS;
             index++) {
            struct stack_workload_diag_run *run =
                &stack_workload_diag_runs[index];

            if (run->active && run->workload == workload) {
                stack_workload_diag_release_locked(run->workload, &run->packet,
                                                   outcome, &pressure);
            }
        }
        break;
    case STACK_WORKLOAD_DIAG_DEFER_CYCLE: {
        /* A cycle owns its whole run and never occupies a correlation slot. */
        struct app_stack_diag_state state =
            stack_workload_diag_state(&packet, &pressure);
        uint32_t run_id = app_stack_diag_run_begin(
            workload, (enum app_stack_diag_owner)event->owner, &state);

        if (run_id != 0u) {
            (void)app_stack_diag_sample(run_id, &state);
            (void)app_stack_diag_run_end(run_id, outcome, &state);
        }
        break;
    }
    default:
        break;
    }
}

/*
 * The radio path owns the CPU while a frame is being turned around.  Putting
 * the record back at the head keeps the per-run order intact and retries it
 * once the turnaround has completed.
 */
static bool stack_workload_diag_defer_hold_for_radio(
    const struct stack_workload_diag_defer_event *event)
{
    bool (*critical)(void) = stack_workload_diag_critical_hook;
    bool held = false;

    if (critical == NULL || !critical()) {
        return false;
    }
    k_mutex_lock(&stack_workload_diag_defer_mutex, K_FOREVER);
    if (stack_workload_diag_defer_critical_holds <
            STACK_WORKLOAD_DIAG_DEFER_CRITICAL_HOLD_MAX &&
        stack_workload_diag_defer_count < STACK_WORKLOAD_DIAG_DEFER_DEPTH) {
        stack_workload_diag_defer_critical_holds++;
        stack_workload_diag_defer_head =
            (uint8_t)((stack_workload_diag_defer_head +
                       STACK_WORKLOAD_DIAG_DEFER_DEPTH - 1u) %
                      STACK_WORKLOAD_DIAG_DEFER_DEPTH);
        stack_workload_diag_defer_ring[stack_workload_diag_defer_head] = *event;
        stack_workload_diag_defer_count++;
        stack_workload_diag_defer_scheduled = true;
        held = true;
    } else {
        /* The required evidence still has to land within the capture. */
        stack_workload_diag_defer_critical_holds = 0u;
    }
    k_mutex_unlock(&stack_workload_diag_defer_mutex);
    if (held) {
        stack_workload_diag_defer_arm(
            STACK_WORKLOAD_DIAG_DEFER_CRITICAL_RETRY_MS);
    }
    return held;
}

/*
 * The correlation lock is taken before the record is dequeued, not after.
 * That makes the whole dequeue-and-emit step the single consumer the ordering
 * contract assumes: a sample can never be emitted ahead of the admission it
 * belongs to just because two replays raced for the sampler.
 */
static void stack_workload_diag_defer_work_handler(struct k_work *work)
{
    (void)work;
    for (;;) {
        struct stack_workload_diag_defer_event event;
        bool pending;

        stack_workload_diag_lock();
        k_mutex_lock(&stack_workload_diag_defer_mutex, K_FOREVER);
        /* Cleared before the drain so a producer re-arms the work. */
        stack_workload_diag_defer_scheduled = false;
        pending = stack_workload_diag_defer_count > 0u;
        if (pending) {
            event = stack_workload_diag_defer_ring[
                stack_workload_diag_defer_head];
            stack_workload_diag_defer_head =
                (uint8_t)((stack_workload_diag_defer_head + 1u) %
                          STACK_WORKLOAD_DIAG_DEFER_DEPTH);
            stack_workload_diag_defer_count--;
        }
        k_mutex_unlock(&stack_workload_diag_defer_mutex);
        if (!pending || stack_workload_diag_defer_hold_for_radio(&event)) {
            k_mutex_unlock(&stack_workload_diag_mutex);
            return;
        }
        stack_workload_diag_defer_critical_holds = 0u;
        stack_workload_diag_defer_execute_locked(&event);
        k_mutex_unlock(&stack_workload_diag_mutex);
#if !STACK_WORKLOAD_DIAG_DEDICATED_QUEUE
        /*
         * The system workqueue is cooperative and sits above mesh_route, so
         * only an explicit sleep hands the radio owners the CPU back between
         * records.  k_yield() cannot: it never selects a lower priority.
         */
        (void)k_sleep(K_MSEC(STACK_WORKLOAD_DIAG_DEFER_RECORD_GAP_MS));
#endif
    }
}

static void stack_workload_diag_admit(
    enum app_stack_diag_workload workload,
    enum app_stack_diag_owner owner,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_defer_submit(STACK_WORKLOAD_DIAG_DEFER_ADMIT, workload,
                                     owner, packet,
                                     APP_STACK_DIAG_TERMINAL_ACK, pressure);
}

static void stack_workload_diag_sample(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_defer_submit(STACK_WORKLOAD_DIAG_DEFER_SAMPLE, workload,
                                     APP_STACK_DIAG_OWNER_SHARED_MIN, packet,
                                     APP_STACK_DIAG_TERMINAL_ACK, pressure);
}

static void stack_workload_diag_release(
    enum app_stack_diag_workload workload,
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_defer_submit(STACK_WORKLOAD_DIAG_DEFER_RELEASE,
                                     workload,
                                     APP_STACK_DIAG_OWNER_SHARED_MIN, packet,
                                     outcome, pressure);
}

#define DEFINE_WORKLOAD_DIAG(prefix, kind, owner) \
    void app_stack_workload_diag_##prefix##_admit(const struct proto_packet *packet, \
                                                  uint16_t queue_depth, uint16_t custody_depth) \
    { const struct app_stack_workload_diag_pressure pressure = { queue_depth, custody_depth, 0u, 0u, 0u }; \
      stack_workload_diag_admit(kind, owner, packet, &pressure); } \
    void app_stack_workload_diag_##prefix##_sample(const struct proto_packet *packet, \
                                                   uint16_t queue_depth, uint16_t custody_depth) \
    { const struct app_stack_workload_diag_pressure pressure = { queue_depth, custody_depth, 0u, 0u, 0u }; \
      stack_workload_diag_sample(kind, packet, &pressure); } \
    void app_stack_workload_diag_##prefix##_release(const struct proto_packet *packet, int result, \
                                                    uint16_t queue_depth, uint16_t custody_depth) \
    { const struct app_stack_workload_diag_pressure pressure = { queue_depth, custody_depth, 0u, 0u, 0u }; \
      stack_workload_diag_release(kind, packet, result == 0 ? APP_STACK_DIAG_TERMINAL_ACK : APP_STACK_DIAG_TERMINAL_ERROR, &pressure); }

DEFINE_WORKLOAD_DIAG(click, APP_STACK_DIAG_WORKLOAD_CLICK_SPAM,
                     APP_STACK_DIAG_OWNER_CLICKER_ACTION)
DEFINE_WORKLOAD_DIAG(cir, APP_STACK_DIAG_WORKLOAD_CIR_HANDLING,
                     APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN)
DEFINE_WORKLOAD_DIAG(relay, APP_STACK_DIAG_WORKLOAD_RELAY_RETRY,
                     APP_STACK_DIAG_OWNER_MESH_ROUTE)
DEFINE_WORKLOAD_DIAG(ble, APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE,
                     APP_STACK_DIAG_OWNER_BT_RX)
DEFINE_WORKLOAD_DIAG(click_activity, APP_STACK_DIAG_WORKLOAD_CLICK_ACTIVITY,
                     APP_STACK_DIAG_OWNER_CLICKER_ACTION)
DEFINE_WORKLOAD_DIAG(gateway_control,
                     APP_STACK_DIAG_WORKLOAD_GATEWAY_PRIORITY_CONTROL,
                     APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE)

void app_stack_workload_diag_anchor_scan_cycle(
    const struct proto_packet *packet,
    uint16_t queue_depth,
    uint16_t custody_depth)
{
    const struct app_stack_workload_diag_pressure pressure = {
        queue_depth, custody_depth, 0u, 0u, 0u,
    };
    uint32_t now_ms = k_uptime_get_32();
    bool submit = false;

    if (packet == NULL) {
        return;
    }
    k_mutex_lock(&stack_workload_diag_defer_mutex, K_FOREVER);
    if (!stack_workload_diag_anchor_scan_seen ||
        (int32_t)(now_ms - stack_workload_diag_anchor_scan_last_ms) >=
            (int32_t)STACK_WORKLOAD_DIAG_ANCHOR_SCAN_MIN_INTERVAL_MS) {
        stack_workload_diag_anchor_scan_seen = true;
        stack_workload_diag_anchor_scan_last_ms = now_ms;
        submit = true;
    }
    k_mutex_unlock(&stack_workload_diag_defer_mutex);
    if (!submit) {
        return;
    }
    stack_workload_diag_defer_submit(STACK_WORKLOAD_DIAG_DEFER_CYCLE,
                                     APP_STACK_DIAG_WORKLOAD_ANCHOR_SCAN,
                                     APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN,
                                     packet, APP_STACK_DIAG_TERMINAL_ACK,
                                     &pressure);
}

/*
 * Gateway report ingress owns its whole run, so it queues one self-contained
 * cycle instead of holding a correlation slot across the ACK critical path.
 */
void app_stack_workload_diag_gateway_report_cycle(
    const struct proto_packet *packet,
    uint16_t queue_depth,
    uint16_t custody_depth)
{
    const struct app_stack_workload_diag_pressure pressure = {
        queue_depth, custody_depth, 0u, 0u, 0u,
    };

    stack_workload_diag_defer_submit(
        STACK_WORKLOAD_DIAG_DEFER_CYCLE,
        APP_STACK_DIAG_WORKLOAD_GATEWAY_REPORT_INGRESS,
        APP_STACK_DIAG_OWNER_MESH_ROUTE, packet,
        APP_STACK_DIAG_TERMINAL_ACK, &pressure);
}

void app_stack_workload_diag_ble_release_all(int result,
                                             uint16_t queue_depth,
                                             uint16_t custody_depth)
{
    const struct app_stack_workload_diag_pressure pressure = {
        queue_depth, custody_depth, 0u, 0u, 0u,
    };

    app_stack_workload_diag_ble_release_all_with_pressure(
        result == 0 ? APP_STACK_DIAG_TERMINAL_ACK : APP_STACK_DIAG_TERMINAL_DISCONNECT,
        &pressure);
}

void app_stack_workload_diag_ble_release_all_with_pressure(
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_defer_submit(
        STACK_WORKLOAD_DIAG_DEFER_RELEASE_ALL,
        APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE,
        APP_STACK_DIAG_OWNER_SHARED_MIN, NULL, outcome, pressure);
}

void app_stack_workload_diag_ble_admit_with_pressure(
    const struct proto_packet *packet,
    enum app_stack_diag_owner owner,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_admit(APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE, owner,
                              packet, pressure);
}

void app_stack_workload_diag_ble_sample_with_pressure(
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_sample(APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE, packet,
                               pressure);
}

void app_stack_workload_diag_ble_terminal_with_pressure(
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{
    stack_workload_diag_release(APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE, packet,
                                outcome, pressure);
}

#endif
