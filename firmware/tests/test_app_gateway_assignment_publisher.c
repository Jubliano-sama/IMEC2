#include "app_gateway_assignment_publisher.h"
#include "app_gateway_ble_stream.h"
#include "discovery_assignment.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

struct mock_ble {
    struct gateway_command_observability_state observability;
    struct gateway_command_event queue[GATEWAY_BLE_STREAM_QUEUE_DEPTH];
    struct gateway_command_event sent[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES + 3u];
    size_t queue_count;
    size_t sent_count;
    size_t max_depth;
    size_t completed_batches;
    size_t completion_failures;
    struct gateway_command_event completion_event;
    bool completion_event_valid;
    uint32_t durable_sequence_reservations;
    bool connected;
    bool credit;
    bool priority_pending;
    bool receipt_during_emit;
    bool immediate_receipt_accepted;
};

static int mock_complete(const struct gateway_command_event *event, void *ctx)
{
    struct mock_ble *ble = ctx;

    assert(event->kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION);
    assert(event->command_id == CMD_ASSIGN_DISCOVERY_SLOTS);
    assert(event->stage == GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    assert((event->flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u);
    assert(event->event_seq != 0u);
    if (ble->completion_event_valid) {
        assert(event->correlation_id ==
               ble->completion_event.correlation_id);
        assert(event->gateway_sequence ==
               ble->completion_event.gateway_sequence);
        assert(event->gateway_epoch ==
               ble->completion_event.gateway_epoch);
        assert(event->host_session_id ==
               ble->completion_event.host_session_id);
        assert(event->host_seq == ble->completion_event.host_seq);
    } else {
        ble->completion_event = *event;
        ble->completion_event_valid = true;
    }
    ble->completed_batches++;
    if (ble->completion_failures > 0u) {
        ble->completion_failures--;
        return -EIO;
    }
    return 0;
}

static int mock_emit(struct gateway_command_event *event,
                     bool terminal,
                     void *ctx)
{
    struct mock_ble *ble = ctx;
    int ret;

    assert(event->event_seq != 0u);
    if (!ble->connected || !ble->credit || ble->priority_pending ||
        ble->queue_count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH) {
        return -EAGAIN;
    }
    ret = gateway_command_observability_prepare_with_sequence(
        &ble->observability,
        event,
        terminal,
        event->event_seq);
    assert(ret == 0);
    ble->queue[ble->queue_count++] = *event;
    if (ble->queue_count > ble->max_depth) {
        ble->max_depth = ble->queue_count;
    }
    if (ble->receipt_during_emit) {
        assert(app_gateway_assignment_publisher_note_host_receipt(event) > 0);
        ble->immediate_receipt_accepted = true;
    }
    return 0;
}

static int mock_reserve_event_seq(struct gateway_command_event *event,
                                  void *ctx)
{
    struct mock_ble *ble = ctx;

    if (event == NULL || event->event_seq != 0u) {
        return -EINVAL;
    }
    ble->durable_sequence_reservations++;
    event->event_seq = ble->durable_sequence_reservations;
    return 0;
}

static void remove_head(struct mock_ble *ble)
{
    for (size_t i = 0u; i + 1u < ble->queue_count; i++) {
        ble->queue[i] = ble->queue[i + 1u];
    }
    ble->queue_count--;
}

static bool send_head(struct mock_ble *ble, bool preempt_after_send)
{
    struct gateway_command_event event;
    struct gateway_command_event drifted_event;
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    size_t completed_batches;
    size_t queued_after_remove;
    bool advanced;

    assert(ble->connected);
    assert(ble->queue_count > 0u);
    event = ble->queue[0];
    remove_head(ble);
    if (event.event_seq == 0u) {
        return false;
    }
    assert(ble->sent_count < sizeof(ble->sent) / sizeof(ble->sent[0]));
    ble->sent[ble->sent_count++] = event;
    gateway_command_observability_mark_sent(&ble->observability,
                                            event.event_seq);
    ble->priority_pending = preempt_after_send;
    completed_batches = ble->completed_batches;
    queued_after_remove = ble->queue_count;
    drifted_event = event;
    drifted_event.event_seq ^= UINT32_C(0x80000000);
    assert(app_gateway_assignment_publisher_note_host_receipt(
               &drifted_event) == -ESTALE);
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    assert(diagnostics.inflight_event_seq == event.event_seq);
    advanced = app_gateway_assignment_publisher_note_host_receipt(&event) > 0;
    /* A retry after publisher advance is safe, but must not advance twice. */
    assert(app_gateway_assignment_publisher_note_host_receipt(&event) > 0);
    assert(ble->completed_batches == completed_batches);
    assert(ble->queue_count == queued_after_remove);
    return advanced;
}

static int route_owner_service(void)
{
    int ret = app_gateway_assignment_publisher_complete_pending();

    if (ret == 0) {
        app_gateway_assignment_publisher_pump();
    }
    return ret;
}

static struct gateway_command_event base_event(uint32_t epoch)
{
    return (struct gateway_command_event) {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .gateway_epoch = 9u,
        .correlation_id = UINT32_C(0x12340000) | epoch,
        .gateway_sequence = epoch,
        .host_session_id = UINT32_C(0x77889900) | epoch,
        .host_seq = (uint16_t)epoch,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
}

static void run_pressure_case(size_t count)
{
    struct discovery_assignment_entry entries[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    uint64_t anchor_ids[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    uint8_t slots[APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES];
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event base = base_event((uint32_t)count);
    struct gateway_command_event table = base;
    struct gateway_command_event terminal = base;
    struct mock_ble ble;
    bool disconnected_once = false;

    memset(&ble, 0, sizeof(ble));
    gateway_command_observability_init(&ble.observability);
    ops = (struct app_gateway_assignment_publisher_ops) {
        .emit_if_available = mock_emit,
        .reserve_event_seq = mock_reserve_event_seq,
        .batch_completed = mock_complete,
        .ctx = &ble,
    };
    assert(app_gateway_assignment_publisher_init(&ops) == 0);
    for (size_t i = 0u; i < count; i++) {
        anchor_ids[i] = UINT64_C(0x1000000000000000) + i;
    }
    assert(discovery_assignment_sort_anchor_ids(anchor_ids, count) == PROTO_OK);
    for (size_t i = 0u; i < count; i++) {
        entries[i].anchor_id = anchor_ids[i];
        entries[i].slot = (uint8_t)i;
        slots[i] = (uint8_t)i;
    }

    ble.queue_count = GATEWAY_BLE_STREAM_QUEUE_DEPTH;
    ble.max_depth = ble.queue_count;
    ble.connected = true;
    ble.credit = false;
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, NULL, count,
               (UINT64_C(1) << count) - 1u, 2u) == 0);
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, NULL, count,
               (UINT64_C(1) << count) - 1u, 2u) == 1);
    assert(app_gateway_assignment_publisher_commit_prepared_batch(&base) == 0);
    table.stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
    table.attempt = 1u;
    app_gateway_assignment_publisher_stage_table_ready(&table);
    app_gateway_assignment_publisher_stage_table_ready(&table);
    terminal.stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
    terminal.status = COMMAND_OK;
    terminal.progress_count = (uint16_t)count;
    terminal.total_count = (uint16_t)count;
    terminal.success_count = (uint16_t)count;
    assert(app_gateway_assignment_publisher_capture_terminal(&terminal));
    assert(ble.observability.next_event_seq == 0u);
    assert(ble.observability.lost_event_count == 0u);
    assert(ble.durable_sequence_reservations == 1u);

    ble.credit = true;
    ble.priority_pending = true;
    app_gateway_assignment_publisher_pump();
    assert(ble.observability.next_event_seq == 0u);
    assert(ble.durable_sequence_reservations == 1u);
    ble.priority_pending = false;
    ble.connected = false;
    app_gateway_assignment_publisher_pump();
    assert(ble.observability.next_event_seq == 0u);
    assert(ble.durable_sequence_reservations == 1u);
    ble.connected = true;

    while (ble.queue_count > 0u && ble.queue[0].event_seq == 0u) {
        assert(!send_head(&ble, false));
        if (app_gateway_assignment_publisher_work_pending()) {
            assert(route_owner_service() == 0);
        }
    }
    while (ble.sent_count < count + 3u) {
        bool click_preempt = ble.sent_count == 1u;
        int service_ret;

        assert(ble.queue_count > 0u);
        if (!disconnected_once && ble.sent_count == 2u) {
            ble.connected = false;
            app_gateway_assignment_publisher_pump();
            assert(ble.queue_count > 0u);
            ble.connected = true;
            disconnected_once = true;
        }
        assert(send_head(&ble, click_preempt));
        if (click_preempt) {
            uint32_t next_seq = ble.observability.next_event_seq;

            assert(route_owner_service() == 0);
            assert(ble.observability.next_event_seq == next_seq);
            ble.priority_pending = false;
            service_ret = route_owner_service();
        } else {
            service_ret = route_owner_service();
        }
        assert(service_ret ==
               (ble.sent_count == count + 3u ? 1 : 0));
    }

    assert(ble.max_depth <= GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(ble.sent_count == count + 3u);
    assert(ble.durable_sequence_reservations == count + 3u);
    assert(ble.observability.lost_event_count == 0u);
    for (size_t i = 0u; i < ble.sent_count; i++) {
        assert(ble.sent[i].event_seq == i + 1u);
        assert(ble.sent[i].lost_event_count == 0u);
    }
    for (size_t i = 0u; i < count; i++) {
        assert(ble.sent[i].stage ==
               GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
        assert(ble.sent[i].anchor_id == entries[i].anchor_id);
        assert(ble.sent[i].slot == entries[i].slot);
        assert(ble.sent[i].duplicate_count == 2u);
    }
    assert(ble.sent[count].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE);
    assert(ble.sent[count + 1u].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY);
    assert(ble.sent[count + 2u].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    assert((ble.sent[count + 2u].flags &
            GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u);
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    assert(!diagnostics.active);
    assert(ble.completed_batches == 1u);
}

static void run_partial_publish_case(void)
{
    uint64_t claimed_ids[3] = {
        UINT64_C(0x2100000000000001),
        UINT64_C(0x2100000000000002),
        UINT64_C(0x2100000000000003),
    };
    const uint8_t claimed_slots[3] = {0u, 1u, 4u};
    const uint8_t claimed_hop_counts[3] = {1u, 2u, 3u};
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event base = base_event(77u);
    struct gateway_command_event table = base;
    struct gateway_command_event terminal = base;
    struct mock_ble ble;

    memset(&ble, 0, sizeof(ble));
    gateway_command_observability_init(&ble.observability);
    ops = (struct app_gateway_assignment_publisher_ops) {
        .emit_if_available = mock_emit,
        .reserve_event_seq = mock_reserve_event_seq,
        .batch_completed = mock_complete,
        .ctx = &ble,
    };
    assert(app_gateway_assignment_publisher_init(&ops) == 0);
    assert(discovery_assignment_sort_anchor_ids(
               claimed_ids, sizeof(claimed_ids) / sizeof(claimed_ids[0])) ==
           PROTO_OK);

    ble.connected = true;
    ble.credit = true;
    ble.completion_failures = 1u;
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, claimed_ids, claimed_slots, claimed_hop_counts, 3u,
               UINT64_C(0x11), 0u) == 0);
    assert(app_gateway_assignment_publisher_commit_prepared_batch(&base) == 0);
    table.stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
    table.progress_count = 2u;
    table.total_count = 3u;
    app_gateway_assignment_publisher_stage_table_ready(&table);
    terminal.stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
    terminal.status = COMMAND_OK;
    terminal.progress_count = 2u;
    terminal.total_count = 3u;
    terminal.success_count = 2u;
    terminal.failure_count = 1u;
    assert(app_gateway_assignment_publisher_capture_terminal(&terminal));

    while (ble.sent_count < 6u) {
        assert(ble.queue_count > 0u);
        assert(send_head(&ble, false));
        if (ble.sent_count < 6u) {
            assert(route_owner_service() == 0);
        }
    }

    assert(ble.sent[0].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
    assert(ble.sent[0].anchor_id == claimed_ids[0]);
    assert(ble.sent[0].slot == 0u);
    assert(ble.sent[0].hop_count == 1u);
    assert(ble.sent[0].status == COMMAND_OK);
    assert(ble.sent[1].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
    assert(ble.sent[1].anchor_id == claimed_ids[1]);
    assert(ble.sent[1].slot == 1u);
    assert(ble.sent[1].hop_count == 2u);
    assert(ble.sent[1].status == COMMAND_TIMEOUT);
    assert(ble.sent[1].failure_count == 1u);
    assert(ble.sent[2].anchor_id == claimed_ids[2]);
    assert(ble.sent[2].slot == 4u);
    assert(ble.sent[2].hop_count == 3u);
    assert(ble.sent[2].status == COMMAND_OK);
    assert(ble.sent[5].stage == GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    assert(ble.sent[5].status == COMMAND_OK);
    assert(ble.sent[5].total_count == 3u);
    assert(ble.sent[5].success_count == 2u);
    assert(ble.sent[5].failure_count == 1u);
    assert(ble.completed_batches == 0u);
    {
        struct app_gateway_assignment_publisher_diagnostics diagnostics;
        struct gateway_command_event other = base_event(78u);

        app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
        assert(diagnostics.active);
        assert(ble.queue_count == 0u);
        /*
         * Any transport-side pump remains storage-free once the terminal host
         * notification has published its immutable completion debt.
         */
        app_gateway_assignment_publisher_pump();
        assert(ble.completed_batches == 0u);
        assert(app_gateway_assignment_publisher_complete_pending() == -EIO);
        assert(ble.completed_batches == 1u);
        app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
        assert(diagnostics.active);
        assert(app_gateway_assignment_publisher_prepare_table(
                   &other,
                   claimed_ids,
                   claimed_slots,
                   NULL,
                   3u,
                   UINT64_C(0x11),
                   0u) == -EBUSY);
        assert(app_gateway_assignment_publisher_complete_pending() == 1);
        app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
        assert(!diagnostics.active);
        assert(ble.sent_count == 6u);
        assert(ble.completed_batches == 2u);
    }
}

static void run_prepare_commit_atomicity_case(void)
{
    uint64_t anchor_ids[2] = {
        UINT64_C(0x3100000000000001),
        UINT64_C(0x3100000000000002),
    };
    const uint8_t slots[2] = {0u, 1u};
    const uint8_t invalid_hop_counts[2] = {1u, 9u};
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event base = base_event(88u);
    struct gateway_command_event other = base_event(89u);
    struct mock_ble ble;

    memset(&ble, 0, sizeof(ble));
    gateway_command_observability_init(&ble.observability);
    ble.connected = true;
    ble.credit = true;
    ops = (struct app_gateway_assignment_publisher_ops) {
        .emit_if_available = mock_emit,
        .reserve_event_seq = mock_reserve_event_seq,
        .batch_completed = mock_complete,
        .ctx = &ble,
    };
    assert(app_gateway_assignment_publisher_init(&ops) == 0);
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, invalid_hop_counts, 2u,
               UINT64_C(0x3), 0u) == -EINVAL);

    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, NULL, 2u, UINT64_C(0x3), 0u) == 0);
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, NULL, 2u, UINT64_C(0x3), 0u) == 1);
    assert(app_gateway_assignment_publisher_prepare_table(
               &other, anchor_ids, slots, NULL, 2u, UINT64_C(0x3), 0u) == -EBUSY);
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    assert(diagnostics.active);
    assert(ble.queue_count == 0u);
    assert(ble.observability.next_event_seq == 0u);
    assert(app_gateway_assignment_publisher_abort_prepared_batch(&base));
    assert(!app_gateway_assignment_publisher_abort_prepared_batch(&base));
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    assert(!diagnostics.active);

    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, NULL, 2u, UINT64_C(0x3), 0u) == 0);
    assert(app_gateway_assignment_publisher_commit_prepared_batch(&other) ==
           -ESTALE);
    assert(ble.queue_count == 0u);
    assert(app_gateway_assignment_publisher_commit_prepared_batch(&base) == 0);
    assert(ble.queue_count == 1u);
    assert(!app_gateway_assignment_publisher_abort_prepared_batch(&base));
}

static void run_immediate_host_receipt_case(void)
{
    const uint64_t anchor_ids[] = {UINT64_C(0x4100000000000001)};
    const uint8_t slots[] = {0u};
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event base = base_event(90u);
    struct mock_ble ble;

    memset(&ble, 0, sizeof(ble));
    gateway_command_observability_init(&ble.observability);
    ble.connected = true;
    ble.credit = true;
    ble.receipt_during_emit = true;
    ops = (struct app_gateway_assignment_publisher_ops) {
        .emit_if_available = mock_emit,
        .reserve_event_seq = mock_reserve_event_seq,
        .batch_completed = mock_complete,
        .ctx = &ble,
    };
    assert(app_gateway_assignment_publisher_init(&ops) == 0);
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, anchor_ids, slots, NULL, 1u, UINT64_C(1), 0u) == 0);
    assert(app_gateway_assignment_publisher_commit_prepared_batch(&base) == 0);
    assert(ble.immediate_receipt_accepted);
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    assert(diagnostics.active);
    assert(diagnostics.inflight_event_seq == 0u);
    assert(diagnostics.sent_mappings == 1u);

    /* The receipt arrived while emit_if_available still owned the callback.
     * The next event can nevertheless acquire a fresh exact inflight slot. */
    ble.receipt_during_emit = false;
    app_gateway_assignment_publisher_pump();
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    assert(diagnostics.inflight_event_seq == 2u);
    assert(ble.queue_count == 2u);
}

static void run_durable_replay_batch_case(void)
{
    const uint64_t claimed_ids[] = {
        UINT64_C(0x5100000000000001),
        UINT64_C(0x5100000000000002),
    };
    const uint8_t claimed_slots[] = {0u, 4u};
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event base = base_event(91u);
    struct gateway_command_event table = base;
    struct gateway_command_event terminal = base;
    struct mock_ble ble;

    memset(&ble, 0, sizeof(ble));
    gateway_command_observability_init(&ble.observability);
    ble.connected = true;
    ble.credit = true;
    ops = (struct app_gateway_assignment_publisher_ops) {
        .emit_if_available = mock_emit,
        .reserve_event_seq = mock_reserve_event_seq,
        .batch_completed = mock_complete,
        .ctx = &ble,
    };
    assert(app_gateway_assignment_publisher_init(&ops) == 0);
    assert(app_gateway_assignment_publisher_prepare_table(
               &base, claimed_ids, claimed_slots, NULL, 2u,
               UINT64_C(0x11), 3u) == 0);
    assert(app_gateway_assignment_publisher_mark_prepared_replay(&base) == 0);
    assert(app_gateway_assignment_publisher_commit_prepared_batch(&base) == 0);

    table.stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
    table.attempt = 7u;
    table.progress_count = 2u;
    table.total_count = 2u;
    table.success_count = 2u;
    table.duplicate_count = 3u;
    app_gateway_assignment_publisher_stage_table_ready(&table);
    terminal = table;
    terminal.stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
    terminal.flags = GATEWAY_COMMAND_EVENT_FLAG_TERMINAL;
    assert(app_gateway_assignment_publisher_capture_terminal(&terminal));

    while (ble.sent_count < 5u) {
        assert(ble.queue_count > 0u);
        assert(send_head(&ble, false));
        if (ble.sent_count < 5u) {
            assert(route_owner_service() == 0);
        }
    }

    for (size_t index = 0u; index < ble.sent_count; index++) {
        assert((ble.sent[index].flags & GATEWAY_COMMAND_EVENT_FLAG_REPLAY) !=
               0u);
    }
    assert(ble.sent[0].stage == GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
    assert(ble.sent[1].stage == GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
    assert(ble.sent[2].stage == GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE);
    assert(ble.sent[3].stage == GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY);
    assert(ble.sent[3].attempt == 7u);
    assert(ble.sent[4].stage == GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    assert((ble.sent[4].flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u);
    assert(ble.sent[4].attempt == 7u);
}

int main(void)
{
    run_pressure_case(3u);
    run_pressure_case(20u);
    run_pressure_case(50u);
    run_partial_publish_case();
    run_prepare_commit_atomicity_case();
    run_immediate_host_receipt_case();
    run_durable_replay_batch_case();
    return 0;
}
