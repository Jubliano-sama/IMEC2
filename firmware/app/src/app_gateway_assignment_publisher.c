#include "app_gateway_assignment_publisher.h"
#include "discovery_assignment.h"

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

struct app_gateway_assignment_publisher_identity {
    uint32_t correlation_id;
    uint32_t gateway_sequence;
    uint32_t host_session_id;
    uint16_t gateway_epoch;
    uint16_t host_seq;
};

struct app_gateway_assignment_publisher_state {
    struct app_gateway_assignment_publisher_identity identity;
    struct app_gateway_assignment_publisher_ops ops;
    uint64_t anchor_ids[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    uint8_t hop_counts[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    uint64_t acknowledged_mask;
    uint32_t inflight_event_seq;
    uint32_t pending_event_seq;
    uint32_t terminal_event_seq;
    uint32_t last_host_receipt_event_seq;
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
    bool prepared;
    bool table_ready;
    bool terminal_pending;
    bool completion_pending;
    bool skip_table_event;
    bool emit_attempt_active;
    bool replay;
};

_Static_assert(sizeof(struct app_gateway_assignment_publisher_state) <=
               APP_GATEWAY_ASSIGNMENT_PUBLISHER_RAM_BUDGET_BYTES,
               "assignment telemetry publisher exceeds compact RAM budget");
_Static_assert(APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES == 50u,
               "assignment telemetry publisher must cover the 50-anchor cap");

static struct app_gateway_assignment_publisher_state publisher;

static bool valid_base_event(const struct gateway_command_event *event)
{
    return event != NULL &&
           event->kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION &&
           event->command_id == CMD_ASSIGN_DISCOVERY_SLOTS;
}

bool app_gateway_assignment_publisher_event_is_reliable(
    const struct gateway_command_event *event)
{
    const uint8_t allowed_flags = GATEWAY_COMMAND_EVENT_FLAG_TERMINAL |
                                  GATEWAY_COMMAND_EVENT_FLAG_REPLAY;
    bool terminal;

    if (!valid_base_event(event) || event->event_seq == 0u ||
        event->gateway_epoch == 0u || event->correlation_id == 0u ||
        event->gateway_sequence == 0u || event->host_session_id == 0u ||
        event->host_seq == 0u ||
        event->correlation_id != event->host_session_id ||
        (event->flags & ~allowed_flags) != 0u) {
        return false;
    }

    terminal = (event->flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u;
    switch (event->stage) {
    case GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED:
        return !terminal && event->anchor_id != 0u &&
               event->slot < APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES &&
               event->progress_count != 0u && event->total_count != 0u &&
               (uint32_t)event->success_count + event->failure_count == 1u;
    case GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE:
    case GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY:
        return !terminal && event->anchor_id == 0u &&
               event->pair_initiator_id == 0u &&
               event->pair_responder_id == 0u &&
               event->slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
    case GATEWAY_COMMAND_EVENT_STAGE_COMPLETE:
        return terminal && event->anchor_id == 0u &&
               event->pair_initiator_id == 0u &&
               event->pair_responder_id == 0u &&
               event->slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
    default:
        return false;
    }
}

static bool same_identity(const struct gateway_command_event *event)
{
    return valid_base_event(event) &&
           publisher.identity.correlation_id == event->correlation_id &&
           publisher.identity.gateway_sequence == event->gateway_sequence &&
           publisher.identity.host_session_id == event->host_session_id &&
           publisher.identity.gateway_epoch == event->gateway_epoch &&
           publisher.identity.host_seq == event->host_seq;
}

static void store_identity(const struct gateway_command_event *event)
{
    publisher.identity =
        (struct app_gateway_assignment_publisher_identity) {
            .correlation_id = event->correlation_id,
            .gateway_sequence = event->gateway_sequence,
            .host_session_id = event->host_session_id,
            .gateway_epoch = event->gateway_epoch,
            .host_seq = event->host_seq,
        };
}

static void build_base_event(struct gateway_command_event *event)
{
    memset(event, 0, sizeof(*event));
    event->schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION;
    event->record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN;
    event->kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION;
    event->command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    event->gateway_epoch = publisher.identity.gateway_epoch;
    event->correlation_id = publisher.identity.correlation_id;
    event->gateway_sequence = publisher.identity.gateway_sequence;
    event->host_session_id = publisher.identity.host_session_id;
    event->host_seq = publisher.identity.host_seq;
    event->slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
}

static bool same_table(const uint64_t *anchor_ids,
                       const uint8_t *slots,
                       size_t anchor_count,
                       uint64_t acknowledged_mask)
{
    /* Live hop depth is diagnostic evidence, not durable batch identity.
     * Same-boot ambiguous-save adoption deliberately retries with no durable
     * hop sidecar and must preserve the already prepared live projection. */
    if (anchor_count != publisher.entry_count ||
        acknowledged_mask != publisher.acknowledged_mask) {
        return false;
    }
    for (size_t i = 0u; i < anchor_count; i++) {
        if (publisher.anchor_ids[slots[i]] != anchor_ids[i]) {
            return false;
        }
    }
    return true;
}

static void reset_event_progress(struct gateway_command_event *event)
{
    event->flags = publisher.replay ? GATEWAY_COMMAND_EVENT_FLAG_REPLAY : 0u;
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

static void build_terminal_event(struct gateway_command_event *event,
                                 uint32_t event_seq)
{
    build_base_event(event);
    reset_event_progress(event);
    event->stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
    event->flags |= GATEWAY_COMMAND_EVENT_FLAG_TERMINAL;
    event->attempt = publisher.terminal_attempt;
    event->status = publisher.terminal_status;
    event->reason = publisher.terminal_reason;
    event->progress_count = publisher.terminal_progress_count;
    event->total_count = publisher.terminal_total_count;
    event->success_count = publisher.terminal_success_count;
    event->failure_count = publisher.terminal_failure_count;
    event->duplicate_count = publisher.terminal_duplicate_count;
    event->event_seq = event_seq;
}

static uint8_t mapping_slot_for_ordinal(uint16_t ordinal)
{
    uint16_t seen = 0u;

    for (uint8_t slot = 0u;
         slot < APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES;
         slot++) {
        if (publisher.anchor_ids[slot] == 0u) {
            continue;
        }
        if (seen == ordinal) {
            return slot;
        }
        seen++;
    }
    return GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE;
}

static bool build_next_event(struct gateway_command_event *event,
                             bool *terminal)
{
    uint16_t table_cursor = (uint16_t)(publisher.entry_count + 1u);
    uint16_t terminal_cursor = (uint16_t)(publisher.entry_count + 2u);

    if (!publisher.active || publisher.prepared ||
        publisher.inflight_event_seq != 0u ||
        publisher.emit_attempt_active) {
        return false;
    }
    if (publisher.cursor < publisher.entry_count) {
        uint8_t slot = mapping_slot_for_ordinal(publisher.cursor);
        bool acknowledged;

        if (slot == GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE) {
            return false;
        }
        acknowledged =
            (publisher.acknowledged_mask &
             (UINT64_C(1) << slot)) != 0u;
        build_base_event(event);
        reset_event_progress(event);
        event->stage = GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED;
        event->anchor_id = publisher.anchor_ids[slot];
        event->hop_count = publisher.hop_counts[slot];
        event->slot = slot;
        event->progress_count = (uint16_t)(publisher.cursor + 1u);
        event->success_count =
            acknowledged ? (uint16_t)1u : (uint16_t)0u;
        event->failure_count =
            acknowledged ? (uint16_t)0u : (uint16_t)1u;
        event->status = acknowledged ? COMMAND_OK : COMMAND_TIMEOUT;
        event->reason = acknowledged ?
                            GATEWAY_COMMAND_EVENT_REASON_NONE :
                            GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
        event->event_seq = publisher.pending_event_seq;
        *terminal = false;
        return true;
    }
    if (publisher.cursor == publisher.entry_count) {
        build_base_event(event);
        reset_event_progress(event);
        event->stage = GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE;
        event->progress_count = publisher.entry_count;
        event->success_count =
            (uint16_t)__builtin_popcountll(publisher.acknowledged_mask);
        event->failure_count =
            (uint16_t)(publisher.entry_count - event->success_count);
        event->event_seq = publisher.pending_event_seq;
        *terminal = false;
        return true;
    }
    if (publisher.cursor == table_cursor) {
        if (publisher.skip_table_event) {
            publisher.cursor++;
        } else if (publisher.table_ready) {
            build_base_event(event);
            reset_event_progress(event);
            event->stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
            event->attempt = publisher.table_attempt;
            event->status = publisher.table_status;
            event->reason = publisher.table_reason;
            event->progress_count = publisher.entry_count;
            event->event_seq = publisher.pending_event_seq;
            *terminal = false;
            return true;
        } else {
            return false;
        }
    }
    if (publisher.cursor == terminal_cursor && publisher.terminal_pending) {
        build_terminal_event(event, publisher.pending_event_seq);
        *terminal = true;
        return true;
    }
    return false;
}

int app_gateway_assignment_publisher_init(
    const struct app_gateway_assignment_publisher_ops *ops)
{
    if (ops == NULL || ops->emit_if_available == NULL ||
        ops->reserve_event_seq == NULL) {
        return -EINVAL;
    }
    memset(&publisher, 0, sizeof(publisher));
    publisher.ops = *ops;
    return 0;
}

int app_gateway_assignment_publisher_prepare_table(
    const struct gateway_command_event *base_event,
    const uint64_t *anchor_ids,
    const uint8_t *slots,
    const uint8_t *hop_counts,
    size_t anchor_count,
    uint64_t acknowledged_mask,
    uint16_t duplicate_count)
{
    PUBLISHER_LOCK_KEY key;

    if (!valid_base_event(base_event) || anchor_ids == NULL || slots == NULL ||
        anchor_count == 0u ||
        anchor_count > APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES) {
        return -EINVAL;
    }
    if (acknowledged_mask == 0u ||
        (acknowledged_mask &
         ~((UINT64_C(1) << APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES) -
           1u)) != 0u) {
        return -EINVAL;
    }
    for (size_t i = 0u; i < anchor_count; i++) {
        if (anchor_ids[i] == 0u ||
            slots[i] >= APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES ||
            (hop_counts != NULL &&
             hop_counts[i] > DISCOVERY_ASSIGNMENT_MAX_HOPS)) {
            return -EINVAL;
        }
        for (size_t prior = 0u; prior < i; prior++) {
            if (anchor_ids[prior] == anchor_ids[i] ||
                slots[prior] == slots[i]) {
                return -EINVAL;
            }
        }
    }
    for (uint8_t bit = 0u;
         bit < APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES;
         bit++) {
        if ((acknowledged_mask & (UINT64_C(1) << bit)) != 0u) {
            bool occupied = false;

            for (size_t i = 0u; i < anchor_count; i++) {
                if (slots[i] == bit) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) {
                return -EINVAL;
            }
        }
    }
    key = PUBLISHER_LOCK();
    if (publisher.active) {
        if (same_identity(base_event) &&
            same_table(anchor_ids, slots, anchor_count, acknowledged_mask)) {
            if (publisher.duplicate_batches < UINT16_MAX) {
                publisher.duplicate_batches++;
            }
            PUBLISHER_UNLOCK(key);
            return 1;
        }
        PUBLISHER_UNLOCK(key);
        return -EBUSY;
    }

    store_identity(base_event);
    publisher.entry_count = (uint16_t)anchor_count;
    publisher.duplicate_count = duplicate_count;
    publisher.acknowledged_mask = acknowledged_mask;
    memset(publisher.anchor_ids, 0, sizeof(publisher.anchor_ids));
    memset(publisher.hop_counts, 0, sizeof(publisher.hop_counts));
    for (size_t i = 0u; i < anchor_count; i++) {
        publisher.anchor_ids[slots[i]] = anchor_ids[i];
        publisher.hop_counts[slots[i]] =
            hop_counts == NULL ? 0u : hop_counts[i];
    }
    publisher.cursor = 0u;
    publisher.inflight_event_seq = 0u;
    publisher.pending_event_seq = 0u;
    publisher.table_ready = false;
    publisher.terminal_pending = false;
    publisher.skip_table_event = false;
    publisher.emit_attempt_active = false;
    publisher.active = true;
    publisher.prepared = true;
    PUBLISHER_UNLOCK(key);
    return 0;
}

int app_gateway_assignment_publisher_commit_prepared_batch(
    const struct gateway_command_event *base_event)
{
    PUBLISHER_LOCK_KEY key;

    if (base_event == NULL) {
        return -EINVAL;
    }
    key = PUBLISHER_LOCK();
    if (!publisher.active || !publisher.prepared ||
        !same_identity(base_event)) {
        PUBLISHER_UNLOCK(key);
        return -ESTALE;
    }
    publisher.prepared = false;
    PUBLISHER_UNLOCK(key);
    app_gateway_assignment_publisher_pump();
    return 0;
}

int app_gateway_assignment_publisher_mark_prepared_replay(
    const struct gateway_command_event *base_event)
{
    PUBLISHER_LOCK_KEY key;

    if (base_event == NULL) {
        return -EINVAL;
    }
    key = PUBLISHER_LOCK();
    if (!publisher.active || !publisher.prepared ||
        !same_identity(base_event)) {
        PUBLISHER_UNLOCK(key);
        return -ESTALE;
    }
    publisher.replay = true;
    PUBLISHER_UNLOCK(key);
    return 0;
}

bool app_gateway_assignment_publisher_abort_prepared_batch(
    const struct gateway_command_event *base_event)
{
    struct app_gateway_assignment_publisher_ops ops;
    PUBLISHER_LOCK_KEY key;

    if (base_event == NULL) {
        return false;
    }
    key = PUBLISHER_LOCK();
    if (!publisher.active || !publisher.prepared ||
        !same_identity(base_event)) {
        PUBLISHER_UNLOCK(key);
        return false;
    }
    ops = publisher.ops;
    memset(&publisher, 0, sizeof(publisher));
    publisher.ops = ops;
    PUBLISHER_UNLOCK(key);
    return true;
}

void app_gateway_assignment_publisher_stage_table_ready(
    const struct gateway_command_event *event)
{
    PUBLISHER_LOCK_KEY key;

    if (event == NULL) {
        return;
    }
    key = PUBLISHER_LOCK();
    if (publisher.active && same_identity(event) &&
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
    if (publisher.active && same_identity(event)) {
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
    app_gateway_assignment_publisher_reserve_fn reserve;
    void *ctx;
    bool terminal = false;
    PUBLISHER_LOCK_KEY key = PUBLISHER_LOCK();
    int ret;

    /*
     * The terminal host notification publishes an immutable durable-completion
     * debt. Transport callbacks may still call pump while reconnecting, but
     * only the owning persistence workqueue may execute that debt.
     */
    if (publisher.active && publisher.completion_pending) {
        PUBLISHER_UNLOCK(key);
        return;
    }
    if (!build_next_event(&event, &terminal)) {
        PUBLISHER_UNLOCK(key);
        return;
    }
    publisher.emit_attempt_active = true;
    emit = publisher.ops.emit_if_available;
    reserve = publisher.ops.reserve_event_seq;
    ctx = publisher.ops.ctx;
    PUBLISHER_UNLOCK(key);

    if (event.event_seq == 0u) {
        ret = reserve(&event, ctx);
        if (ret != 0 || event.event_seq == 0u) {
            key = PUBLISHER_LOCK();
            if (publisher.active && same_identity(&event)) {
                publisher.emit_attempt_active = false;
            }
            PUBLISHER_UNLOCK(key);
            return;
        }
    }

    key = PUBLISHER_LOCK();
    if (!publisher.active || !same_identity(&event) ||
        !publisher.emit_attempt_active ||
        publisher.inflight_event_seq != 0u ||
        (publisher.pending_event_seq != 0u &&
         publisher.pending_event_seq != event.event_seq)) {
        if (publisher.emit_attempt_active) {
            publisher.emit_attempt_active = false;
        }
        PUBLISHER_UNLOCK(key);
        return;
    }
    /* Install exact publisher custody before the callback can queue bytes.
     * A GUI receipt racing the callback now sees its event as inflight rather
     * than observing a transient no-owner gap. */
    publisher.inflight_event_seq = event.event_seq;
    publisher.pending_event_seq = 0u;
    if (terminal) {
        publisher.terminal_event_seq = event.event_seq;
    }
    PUBLISHER_UNLOCK(key);

    ret = emit(&event, terminal, ctx);

    key = PUBLISHER_LOCK();
    if (publisher.active && same_identity(&event)) {
        publisher.emit_attempt_active = false;
    }
    if (ret != 0 && publisher.active && same_identity(&event) &&
        publisher.last_host_receipt_event_seq != event.event_seq &&
        publisher.inflight_event_seq == event.event_seq) {
        publisher.inflight_event_seq = 0u;
        /* The callback reserved but did not expose this exact identity. Keep
         * it for a later queue-capacity/reconnect retry without consuming a
         * new host-stream sequence. */
        if (terminal) {
            publisher.terminal_event_seq = 0u;
        }
        publisher.pending_event_seq = event.event_seq;
    }
    PUBLISHER_UNLOCK(key);
}

int app_gateway_assignment_publisher_note_host_receipt(
    const struct gateway_command_event *event)
{
    PUBLISHER_LOCK_KEY key;
    int ret = 0;

    if (event == NULL || event->event_seq == 0u) {
        return -EINVAL;
    }
    key = PUBLISHER_LOCK();
    if (!publisher.active || !same_identity(event)) {
        PUBLISHER_UNLOCK(key);
        return 0;
    }
    if (publisher.inflight_event_seq == event->event_seq) {
        publisher.inflight_event_seq = 0u;
        publisher.cursor++;
        if (publisher.terminal_pending &&
            publisher.cursor > (uint16_t)(publisher.entry_count + 2u)) {
            publisher.completion_pending = true;
        }
        publisher.last_host_receipt_event_seq = event->event_seq;
        ret = 1;
    } else if (publisher.last_host_receipt_event_seq == event->event_seq) {
        /* The publisher advanced before a transient stream-retirement error.
         * Let the exact same BLE head retire on retry without moving cursor
         * twice. */
        ret = 1;
    } else {
        ret = -ESTALE;
    }
    PUBLISHER_UNLOCK(key);
    return ret;
}

bool app_gateway_assignment_publisher_work_pending(void)
{
    PUBLISHER_LOCK_KEY key = PUBLISHER_LOCK();
    bool pending = publisher.active &&
                   !publisher.prepared &&
                   publisher.inflight_event_seq == 0u &&
                   !publisher.emit_attempt_active;

    PUBLISHER_UNLOCK(key);
    return pending;
}

int app_gateway_assignment_publisher_complete_pending(void)
{
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event event;
    app_gateway_assignment_publisher_complete_fn complete;
    void *ctx;
    bool matching_debt;
    PUBLISHER_LOCK_KEY key = PUBLISHER_LOCK();
    int ret;

    if (!publisher.active || !publisher.completion_pending) {
        PUBLISHER_UNLOCK(key);
        return 0;
    }
    if (publisher.emit_attempt_active) {
        PUBLISHER_UNLOCK(key);
        return -EBUSY;
    }

    if (publisher.terminal_event_seq == 0u) {
        PUBLISHER_UNLOCK(key);
        return -ESTALE;
    }
    build_terminal_event(&event, publisher.terminal_event_seq);
    publisher.emit_attempt_active = true;
    complete = publisher.ops.batch_completed;
    ctx = publisher.ops.ctx;
    PUBLISHER_UNLOCK(key);

    ret = complete == NULL ? 0 : complete(&event, ctx);

    key = PUBLISHER_LOCK();
    matching_debt = publisher.active &&
                    publisher.completion_pending &&
                    same_identity(&event);
    if (matching_debt) {
        publisher.emit_attempt_active = false;
    }
    if (ret == 0 && matching_debt) {
        ops = publisher.ops;
        memset(&publisher, 0, sizeof(publisher));
        publisher.ops = ops;
    }
    PUBLISHER_UNLOCK(key);

    if (ret < 0) {
        return ret;
    }
    return matching_debt ? 1 : -ESTALE;
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
