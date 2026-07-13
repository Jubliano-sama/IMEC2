#include "app_gateway_assignment_publisher.h"

#include <errno.h>
#include <string.h>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#define PUBLISHER_LOCK_KEY k_spinlock_key_t
#define PUBLISHER_LOCK() k_spin_lock(&publisher_lock)
#define PUBLISHER_UNLOCK(key) k_spin_unlock(&publisher_lock, key)
static struct k_spinlock publisher_lock;
#else
#define PUBLISHER_LOCK_KEY unsigned int
#define PUBLISHER_LOCK() 0u
#define PUBLISHER_UNLOCK(key) ((void)(key))
#endif

struct app_gateway_assignment_publisher_state {
    struct gateway_command_event base_event;
    struct app_gateway_assignment_publisher_ops ops;
    uint64_t anchor_ids[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    uint8_t slots[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    uint32_t inflight_event_seq;
    uint16_t duplicate_count;
    uint16_t entry_count;
    uint16_t cursor;
    uint16_t duplicate_batches;
    uint8_t table_attempt;
    enum command_status table_status;
    enum gateway_command_event_reason table_reason;
    enum command_status terminal_status;
    enum gateway_command_event_reason terminal_reason;
    uint16_t terminal_progress_count;
    uint16_t terminal_total_count;
    uint16_t terminal_success_count;
    uint16_t terminal_failure_count;
    uint16_t terminal_duplicate_count;
    uint8_t terminal_attempt;
    bool active;
    bool table_ready;
    bool terminal_pending;
    bool skip_table_event;
    bool emit_attempt_active;
};

_Static_assert(sizeof(struct app_gateway_assignment_publisher_state) <=
               APP_GATEWAY_ASSIGNMENT_PUBLISHER_RAM_BUDGET_BYTES,
               "assignment telemetry publisher exceeds compact RAM budget");
_Static_assert(APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES == 50u,
               "assignment telemetry publisher must cover the 50-anchor cap");

static struct app_gateway_assignment_publisher_state publisher;

static bool same_identity(const struct gateway_command_event *left,
                          const struct gateway_command_event *right)
{
    return left->kind == right->kind &&
           left->command_id == right->command_id &&
           left->correlation_id == right->correlation_id &&
           left->gateway_sequence == right->gateway_sequence &&
           left->host_session_id == right->host_session_id &&
           left->host_seq == right->host_seq;
}

static bool same_batch(const struct discovery_assignment_entry *entries,
                       size_t entry_count)
{
    if (entry_count != publisher.entry_count) {
        return false;
    }
    for (size_t i = 0u; i < entry_count; i++) {
        if (publisher.anchor_ids[i] != entries[i].anchor_id ||
            publisher.slots[i] != entries[i].slot) {
            return false;
        }
    }
    return true;
}

static void reset_event_progress(struct gateway_command_event *event)
{
    event->flags = 0u;
    event->attempt = 0u;
    event->status = COMMAND_OK;
    event->reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    event->event_seq = 0u;
    event->anchor_id = 0u;
    event->pair_initiator_id = 0u;
    event->pair_responder_id = 0u;
    event->previous_hop_id = 0u;
    event->progress_count = 0u;
    event->total_count = publisher.entry_count;
    event->success_count = 0u;
    event->failure_count = 0u;
    event->duplicate_count = publisher.duplicate_count;
    event->hop_count = 0u;
    event->slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
}

static bool build_next_event(struct gateway_command_event *event,
                             bool *terminal)
{
    uint16_t table_cursor = (uint16_t)(publisher.entry_count + 1u);
    uint16_t terminal_cursor = (uint16_t)(publisher.entry_count + 2u);

    if (!publisher.active || publisher.inflight_event_seq != 0u ||
        publisher.emit_attempt_active) {
        return false;
    }
    if (publisher.cursor < publisher.entry_count) {
        *event = publisher.base_event;
        reset_event_progress(event);
        event->stage = GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED;
        event->anchor_id = publisher.anchor_ids[publisher.cursor];
        event->slot = publisher.slots[publisher.cursor];
        event->progress_count = (uint16_t)(publisher.cursor + 1u);
        *terminal = false;
        return true;
    }
    if (publisher.cursor == publisher.entry_count) {
        *event = publisher.base_event;
        reset_event_progress(event);
        event->stage = GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE;
        event->progress_count = publisher.entry_count;
        *terminal = false;
        return true;
    }
    if (publisher.cursor == table_cursor) {
        if (publisher.skip_table_event) {
            publisher.cursor++;
        } else if (publisher.table_ready) {
            *event = publisher.base_event;
            reset_event_progress(event);
            event->stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
            event->attempt = publisher.table_attempt;
            event->status = publisher.table_status;
            event->reason = publisher.table_reason;
            event->progress_count = publisher.entry_count;
            *terminal = false;
            return true;
        } else {
            return false;
        }
    }
    if (publisher.cursor == terminal_cursor && publisher.terminal_pending) {
        *event = publisher.base_event;
        reset_event_progress(event);
        event->stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
        event->attempt = publisher.terminal_attempt;
        event->status = publisher.terminal_status;
        event->reason = publisher.terminal_reason;
        event->progress_count = publisher.terminal_progress_count;
        event->total_count = publisher.terminal_total_count;
        event->success_count = publisher.terminal_success_count;
        event->failure_count = publisher.terminal_failure_count;
        event->duplicate_count = publisher.terminal_duplicate_count;
        *terminal = true;
        return true;
    }
    return false;
}

int app_gateway_assignment_publisher_init(
    const struct app_gateway_assignment_publisher_ops *ops)
{
    if (ops == NULL || ops->emit_if_available == NULL) {
        return -EINVAL;
    }
    memset(&publisher, 0, sizeof(publisher));
    publisher.ops = *ops;
    return 0;
}

int app_gateway_assignment_publisher_stage_batch(
    const struct gateway_command_event *base_event,
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint16_t duplicate_count)
{
    PUBLISHER_LOCK_KEY key;

    if (base_event == NULL || entries == NULL || entry_count == 0u ||
        entry_count > APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES ||
        base_event->kind != GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION) {
        return -EINVAL;
    }
    key = PUBLISHER_LOCK();
    if (publisher.active) {
        if (same_identity(&publisher.base_event, base_event) &&
            same_batch(entries, entry_count)) {
            if (publisher.duplicate_batches < UINT16_MAX) {
                publisher.duplicate_batches++;
            }
            PUBLISHER_UNLOCK(key);
            return 1;
        }
        PUBLISHER_UNLOCK(key);
        return -EBUSY;
    }

    publisher.base_event = *base_event;
    publisher.entry_count = (uint16_t)entry_count;
    publisher.duplicate_count = duplicate_count;
    for (size_t i = 0u; i < entry_count; i++) {
        publisher.anchor_ids[i] = entries[i].anchor_id;
        publisher.slots[i] = entries[i].slot;
    }
    publisher.cursor = 0u;
    publisher.inflight_event_seq = 0u;
    publisher.table_ready = false;
    publisher.terminal_pending = false;
    publisher.skip_table_event = false;
    publisher.emit_attempt_active = false;
    publisher.active = true;
    PUBLISHER_UNLOCK(key);
    app_gateway_assignment_publisher_pump();
    return 0;
}

void app_gateway_assignment_publisher_stage_table_ready(
    const struct gateway_command_event *event)
{
    PUBLISHER_LOCK_KEY key;

    if (event == NULL) {
        return;
    }
    key = PUBLISHER_LOCK();
    if (publisher.active && same_identity(&publisher.base_event, event) &&
        !publisher.table_ready) {
        publisher.table_attempt = event->attempt;
        publisher.table_status = event->status;
        publisher.table_reason = event->reason;
        publisher.table_ready = true;
    }
    PUBLISHER_UNLOCK(key);
    app_gateway_assignment_publisher_pump();
}

bool app_gateway_assignment_publisher_capture_terminal(
    const struct gateway_command_event *event)
{
    PUBLISHER_LOCK_KEY key;
    bool captured = false;

    if (event == NULL) {
        return false;
    }
    key = PUBLISHER_LOCK();
    if (publisher.active && same_identity(&publisher.base_event, event)) {
        if (!publisher.terminal_pending) {
            publisher.terminal_attempt = event->attempt;
            publisher.terminal_status = event->status;
            publisher.terminal_reason = event->reason;
            publisher.terminal_progress_count = event->progress_count;
            publisher.terminal_total_count = event->total_count;
            publisher.terminal_success_count = event->success_count;
            publisher.terminal_failure_count = event->failure_count;
            publisher.terminal_duplicate_count = event->duplicate_count;
            publisher.terminal_pending = true;
            if (event->status != COMMAND_OK && !publisher.table_ready) {
                publisher.skip_table_event = true;
            }
        }
        captured = true;
    }
    PUBLISHER_UNLOCK(key);
    if (captured) {
        app_gateway_assignment_publisher_pump();
    }
    return captured;
}

void app_gateway_assignment_publisher_pump(void)
{
    struct gateway_command_event event;
    app_gateway_assignment_publisher_emit_fn emit;
    void *ctx;
    bool terminal = false;
    PUBLISHER_LOCK_KEY key = PUBLISHER_LOCK();
    int ret;

    if (!build_next_event(&event, &terminal)) {
        PUBLISHER_UNLOCK(key);
        return;
    }
    publisher.emit_attempt_active = true;
    emit = publisher.ops.emit_if_available;
    ctx = publisher.ops.ctx;
    PUBLISHER_UNLOCK(key);

    ret = emit(&event, terminal, ctx);

    key = PUBLISHER_LOCK();
    publisher.emit_attempt_active = false;
    if (ret == 0 && event.event_seq != 0u) {
        publisher.inflight_event_seq = event.event_seq;
    }
    PUBLISHER_UNLOCK(key);
}

void app_gateway_assignment_publisher_note_sent(uint32_t event_seq)
{
    PUBLISHER_LOCK_KEY key;
    bool pump = false;

    if (event_seq == 0u) {
        return;
    }
    key = PUBLISHER_LOCK();
    if (publisher.active && publisher.inflight_event_seq == event_seq) {
        publisher.inflight_event_seq = 0u;
        publisher.cursor++;
        if (publisher.terminal_pending &&
            publisher.cursor > (uint16_t)(publisher.entry_count + 2u)) {
            struct app_gateway_assignment_publisher_ops ops = publisher.ops;

            memset(&publisher, 0, sizeof(publisher));
            publisher.ops = ops;
        } else {
            pump = true;
        }
    }
    PUBLISHER_UNLOCK(key);
    if (pump) {
        app_gateway_assignment_publisher_pump();
    }
}

void app_gateway_assignment_publisher_get_diagnostics(
    struct app_gateway_assignment_publisher_diagnostics *diagnostics)
{
    PUBLISHER_LOCK_KEY key;

    if (diagnostics == NULL) {
        return;
    }
    key = PUBLISHER_LOCK();
    diagnostics->mapping_count = publisher.entry_count;
    diagnostics->sent_mappings = publisher.cursor < publisher.entry_count ?
                                  publisher.cursor : publisher.entry_count;
    diagnostics->duplicate_batches = publisher.duplicate_batches;
    diagnostics->inflight_event_seq = publisher.inflight_event_seq;
    diagnostics->active = publisher.active;
    diagnostics->terminal_pending = publisher.terminal_pending;
    PUBLISHER_UNLOCK(key);
}
