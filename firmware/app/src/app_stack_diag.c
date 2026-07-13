#include "app_stack_diag.h"

#include "app_board.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#define STACK_DIAG_MAX_ACTIVE_RUNS APP_STACK_DIAG_MAX_ACTIVE_RUNS

BUILD_ASSERT(STACK_DIAG_MAX_ACTIVE_RUNS >= APP_STACK_DIAG_COMBINED_PEAK_RUNS,
             "stack diagnostics must retain the mandated combined peak");

#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)

struct stack_diag_run {
    uint32_t id;
    uint32_t sample_count;
    uint32_t sequence;
    uint32_t previous_click_run;
    struct app_stack_diag_state identity;
    enum app_stack_diag_workload workload;
    enum app_stack_diag_owner owner;
    bool active;
};

static bool stack_diag_started;
static k_tid_t stack_diag_main_thread;
static uint32_t stack_diag_sample_id;
static uint32_t stack_diag_run_id;
static uint32_t stack_diag_click_sequence;
static uint32_t stack_diag_last_click_run;
static uint64_t stack_diag_boot_epoch;
static struct stack_diag_run stack_diag_runs[STACK_DIAG_MAX_ACTIVE_RUNS];
static struct k_spinlock stack_diag_lock;

static const char *stack_diag_workload_name(enum app_stack_diag_workload workload)
{
    switch (workload) {
    case APP_STACK_DIAG_WORKLOAD_CLICK_SPAM:
        return "click_spam";
    case APP_STACK_DIAG_WORKLOAD_CIR_HANDLING:
        return "cir_handling";
    case APP_STACK_DIAG_WORKLOAD_RELAY_RETRY:
        return "relay_retry";
    case APP_STACK_DIAG_WORKLOAD_BLE_BACKPRESSURE:
        return "ble_backpressure";
    default:
        return NULL;
    }
}

static const char *stack_diag_owner_name(enum app_stack_diag_owner owner)
{
    switch (owner) {
    case APP_STACK_DIAG_OWNER_CLICKER_ACTION:
        return "clicker_action";
    case APP_STACK_DIAG_OWNER_ANCHOR_UWB_SCAN:
        return "anchor_uwb_scan";
    case APP_STACK_DIAG_OWNER_MESH_ROUTE:
        return "mesh_route";
    case APP_STACK_DIAG_OWNER_BT_RX:
        return "bt_rx";
    case APP_STACK_DIAG_OWNER_SHARED_MIN:
        return "shared_min";
    case APP_STACK_DIAG_OWNER_SYSTEM_WORKQUEUE:
        return "system_workqueue";
    default:
        return NULL;
    }
}

static const char *stack_diag_terminal_outcome_name(
    enum app_stack_diag_terminal_outcome outcome)
{
    switch (outcome) {
    case APP_STACK_DIAG_TERMINAL_ACK:
        return "ack";
    case APP_STACK_DIAG_TERMINAL_CUSTODY_DROP:
        return "custody_drop";
    case APP_STACK_DIAG_TERMINAL_DIRECT_ACK_FAILURE:
        return "direct_ack_failure";
    case APP_STACK_DIAG_TERMINAL_PREEMPTED:
        return "preempted";
    case APP_STACK_DIAG_TERMINAL_TIMEOUT_DROP:
        return "timeout_drop";
    case APP_STACK_DIAG_TERMINAL_DISCONNECT:
        return "disconnect";
    case APP_STACK_DIAG_TERMINAL_ERROR:
        return "error";
    default:
        return NULL;
    }
}

static struct app_stack_diag_state stack_diag_state_or_empty(
    const struct app_stack_diag_state *state)
{
    if (state != NULL) {
        return *state;
    }
    return (struct app_stack_diag_state){0};
}

static const char *stack_diag_thread_name(const struct k_thread *thread)
{
#if defined(CONFIG_THREAD_NAME)
    const char *name = k_thread_name_get((k_tid_t)thread);

    if (name != NULL && name[0] != '\0') {
        return name;
    }
#else
    ARG_UNUSED(thread);
#endif

    return "unknown";
}

static void stack_diag_emit_thread(const struct k_thread *thread,
                                   const char *name,
                                   uint32_t run_id,
                                   uint32_t sample_id)
{
    size_t unused = 0u;
    uint32_t stack_size;
    uint32_t stack_used = 0u;
    int ret;

    ret = k_thread_stack_space_get(thread, &unused);
    stack_size = (uint32_t)thread->stack_info.size;
    if (ret == 0 && stack_size > unused) {
        stack_used = stack_size - (uint32_t)unused;
    }

    status_debug_printf("DBG_STACK name=%s tid=%p used=%u free=%u size=%u ret=%d run=%u sample=%u\n",
                        name,
                        thread,
                        stack_used,
                        ret == 0 ? (uint32_t)unused : 0u,
                        stack_size,
                        ret,
                        run_id,
                        sample_id);
}

struct stack_diag_sample_context {
    uint32_t run_id;
    uint32_t sample_id;
};

static void stack_diag_thread_cb(const struct k_thread *thread, void *user_data)
{
    const struct stack_diag_sample_context *context = user_data;

    if ((k_tid_t)thread == stack_diag_main_thread) {
        stack_diag_emit_thread(thread, "main", context->run_id,
                               context->sample_id);
        return;
    }
    stack_diag_emit_thread(thread, stack_diag_thread_name(thread),
                           context->run_id, context->sample_id);
}

static struct stack_diag_run *stack_diag_find_run(uint32_t run_id)
{
    for (size_t index = 0u; index < ARRAY_SIZE(stack_diag_runs); index++) {
        if (stack_diag_runs[index].active && stack_diag_runs[index].id == run_id) {
            return &stack_diag_runs[index];
        }
    }
    return NULL;
}

#endif

void app_stack_diag_start(void)
{
#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
    if (stack_diag_started) {
        return;
    }

    stack_diag_started = true;
    stack_diag_main_thread = k_current_get();
    stack_diag_boot_epoch = ((uint64_t)sys_rand32_get() << 32) | sys_rand32_get();
    if (stack_diag_boot_epoch == 0u) {
        stack_diag_boot_epoch = 1u;
    }
    (void)k_thread_name_set(stack_diag_main_thread, "main");
    status_debug_printf("DBG_STACK_BOOT preset=%s build=%s epoch=%llu uptime=%u\n",
                        IMEC_BUILD_PRESET_NAME,
                        IMEC_STACK_DIAG_BUILD_ID,
                        (unsigned long long)stack_diag_boot_epoch,
                        k_uptime_get_32());
#endif
}

uint32_t app_stack_diag_run_begin(enum app_stack_diag_workload workload,
                                  enum app_stack_diag_owner owner,
                                  const struct app_stack_diag_state *state)
{
#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
    const char *workload_name = stack_diag_workload_name(workload);
    const char *owner_name = stack_diag_owner_name(owner);
    const struct app_stack_diag_state captured = stack_diag_state_or_empty(state);
    struct stack_diag_run *run = NULL;
    k_spinlock_key_t key;

    if (workload_name == NULL || owner_name == NULL ||
        stack_diag_run_id >= UINT32_MAX - 1u ||
        (workload == APP_STACK_DIAG_WORKLOAD_CLICK_SPAM &&
         stack_diag_click_sequence >= UINT32_MAX - 1u)) {
        status_debug_printf("DBG_STACK_RUN_DROP kind=%s owner=%s reason=identity_exhausted epoch=%llu uptime=%u\n",
                            workload_name == NULL ? "invalid" : workload_name,
                            owner_name == NULL ? "invalid" : owner_name,
                            (unsigned long long)stack_diag_boot_epoch,
                            k_uptime_get_32());
        return 0u;
    }

    key = k_spin_lock(&stack_diag_lock);
    for (size_t index = 0u; index < ARRAY_SIZE(stack_diag_runs); index++) {
        if (!stack_diag_runs[index].active) {
            run = &stack_diag_runs[index];
            break;
        }
    }
    if (run != NULL) {
        stack_diag_run_id++;
        run->id = stack_diag_run_id;
        run->sample_count = 0u;
        run->workload = workload;
        run->owner = owner;
        run->sequence = 0u;
        run->previous_click_run = 0u;
        run->identity = captured;
        if (workload == APP_STACK_DIAG_WORKLOAD_CLICK_SPAM) {
            stack_diag_click_sequence++;
            run->sequence = stack_diag_click_sequence;
            if (stack_diag_find_run(stack_diag_last_click_run) != NULL) {
                run->previous_click_run = stack_diag_last_click_run;
            }
            stack_diag_last_click_run = run->id;
        }
        run->active = true;
    }
    k_spin_unlock(&stack_diag_lock, key);

    if (run == NULL) {
        status_debug_printf("DBG_STACK_RUN_DROP kind=%s owner=%s uptime=%u\n",
                            workload_name, owner_name, k_uptime_get_32());
        return 0u;
    }

    status_debug_printf("DBG_STACK_RUN_BEGIN epoch=%llu run=%u kind=%s owner=%s queue=%u custody=%u credit=%u retry=%u drain=%u src=%llu dst=%llu session=%u seq=%u type=%u sequence=%u previous=%u uptime=%u\n",
                        (unsigned long long)stack_diag_boot_epoch,
                        run->id, workload_name, owner_name,
                        captured.queue_depth, captured.custody_depth,
                        captured.credit_available, captured.retry_depth,
                        captured.drain_depth,
                        (unsigned long long)captured.source_id,
                        (unsigned long long)captured.destination_id,
                        captured.session_id, captured.packet_sequence,
                        captured.message_type,
                        run->sequence, run->previous_click_run,
                        k_uptime_get_32());
    return run->id;
#else
    ARG_UNUSED(workload);
    ARG_UNUSED(owner);
    ARG_UNUSED(state);
    return 0u;
#endif
}

void app_stack_diag_sample(uint32_t run_id,
                           const struct app_stack_diag_state *state)
{
#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
    const struct app_stack_diag_state captured = stack_diag_state_or_empty(state);
    struct stack_diag_sample_context context;
    struct stack_diag_run *run;
    const char *workload_name;
    const char *owner_name;
    k_spinlock_key_t key;

    if (run_id == 0u) {
        return;
    }
    key = k_spin_lock(&stack_diag_lock);
    run = stack_diag_find_run(run_id);
    if (run != NULL) {
        stack_diag_sample_id++;
        if (stack_diag_sample_id == 0u) {
            stack_diag_sample_id++;
        }
        run->sample_count++;
        context.run_id = run->id;
        context.sample_id = stack_diag_sample_id;
        workload_name = stack_diag_workload_name(run->workload);
        owner_name = stack_diag_owner_name(run->owner);
    }
    k_spin_unlock(&stack_diag_lock, key);

    if (run == NULL || workload_name == NULL || owner_name == NULL) {
        status_debug_printf("DBG_STACK_SAMPLE_REJECT run=%u uptime=%u\n",
                            run_id, k_uptime_get_32());
        return;
    }

    status_debug_printf("DBG_STACK_SAMPLE_BEGIN epoch=%llu run=%u sample=%u kind=%s owner=%s queue=%u custody=%u credit=%u retry=%u drain=%u src=%llu dst=%llu session=%u seq=%u type=%u uptime=%u\n",
                        (unsigned long long)stack_diag_boot_epoch,
                        context.run_id, context.sample_id, workload_name,
                        owner_name, captured.queue_depth,
                        captured.custody_depth,
                        captured.credit_available, captured.retry_depth,
                        captured.drain_depth,
                        (unsigned long long)captured.source_id,
                        (unsigned long long)captured.destination_id,
                        captured.session_id, captured.packet_sequence,
                        captured.message_type, k_uptime_get_32());
    /* ISR size is configuration evidence. Zephyr exposes no supported ISR watermark. */
    status_debug_printf("DBG_STACK_ISR_CONFIG size=%u run=%u sample=%u\n",
                        CONFIG_ISR_STACK_SIZE, context.run_id,
                        context.sample_id);
    k_thread_foreach_unlocked(stack_diag_thread_cb, &context);
    status_debug_printf("DBG_STACK_SAMPLE_END run=%u sample=%u\n",
                        context.run_id, context.sample_id);
#else
    ARG_UNUSED(run_id);
    ARG_UNUSED(state);
#endif
}

void app_stack_diag_run_end(uint32_t run_id,
                            enum app_stack_diag_terminal_outcome outcome,
                            const struct app_stack_diag_state *state)
{
#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
    const struct app_stack_diag_state captured = stack_diag_state_or_empty(state);
    struct stack_diag_run completed;
    struct stack_diag_run *run;
    const char *workload_name;
    const char *owner_name;
    const char *outcome_name;
    k_spinlock_key_t key;

    outcome_name = stack_diag_terminal_outcome_name(outcome);
    if (run_id == 0u || outcome_name == NULL) {
        return;
    }
    key = k_spin_lock(&stack_diag_lock);
    run = stack_diag_find_run(run_id);
    if (run != NULL) {
        completed = *run;
        run->active = false;
        workload_name = stack_diag_workload_name(completed.workload);
        owner_name = stack_diag_owner_name(completed.owner);
    }
    k_spin_unlock(&stack_diag_lock, key);

    if (run == NULL || workload_name == NULL || owner_name == NULL) {
        status_debug_printf("DBG_STACK_RUN_REJECT run=%u uptime=%u\n",
                            run_id, k_uptime_get_32());
        return;
    }
    status_debug_printf("DBG_STACK_RUN_END epoch=%llu run=%u kind=%s owner=%s outcome=%s queue=%u custody=%u credit=%u retry=%u drain=%u src=%llu dst=%llu session=%u seq=%u type=%u samples=%u sequence=%u previous=%u uptime=%u\n",
                        (unsigned long long)stack_diag_boot_epoch,
                        completed.id, workload_name, owner_name, outcome_name,
                        captured.queue_depth, captured.custody_depth,
                        captured.credit_available, captured.retry_depth,
                        captured.drain_depth,
                        (unsigned long long)captured.source_id,
                        (unsigned long long)captured.destination_id,
                        captured.session_id, captured.packet_sequence,
                        captured.message_type,
                        completed.sample_count, completed.sequence,
                        completed.previous_click_run, k_uptime_get_32());
#else
    ARG_UNUSED(run_id);
    ARG_UNUSED(outcome);
    ARG_UNUSED(state);
#endif
}

#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
static int stack_diag_boot_init(void)
{
    app_stack_diag_start();
    return 0;
}

SYS_INIT(stack_diag_boot_init, APPLICATION, 99);
#endif
