#include "app_gateway_assignment_publisher.h"
#include "app_gateway_ble_stream.h"

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
    bool connected;
    bool credit;
    bool priority_pending;
};

static int mock_emit(struct gateway_command_event *event,
                     bool terminal,
                     void *ctx)
{
    struct mock_ble *ble = ctx;
    int ret;

    if (!ble->connected || !ble->credit || ble->priority_pending ||
        ble->queue_count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH) {
        return -EAGAIN;
    }
    ret = gateway_command_observability_prepare(&ble->observability,
                                                event,
                                                terminal);
    assert(ret == 0);
    ble->queue[ble->queue_count++] = *event;
    if (ble->queue_count > ble->max_depth) {
        ble->max_depth = ble->queue_count;
    }
    return 0;
}

static void remove_head(struct mock_ble *ble)
{
    for (size_t i = 0u; i + 1u < ble->queue_count; i++) {
        ble->queue[i] = ble->queue[i + 1u];
    }
    ble->queue_count--;
}

static void send_head(struct mock_ble *ble, bool preempt_after_send)
{
    struct gateway_command_event event;

    assert(ble->connected);
    assert(ble->queue_count > 0u);
    event = ble->queue[0];
    remove_head(ble);
    if (event.event_seq == 0u) {
        app_gateway_assignment_publisher_pump();
        return;
    }
    assert(ble->sent_count < sizeof(ble->sent) / sizeof(ble->sent[0]));
    ble->sent[ble->sent_count++] = event;
    gateway_command_observability_mark_sent(&ble->observability,
                                            event.event_seq);
    ble->priority_pending = preempt_after_send;
    app_gateway_assignment_publisher_note_sent(event.event_seq);
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
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    struct app_gateway_assignment_publisher_ops ops;
    struct gateway_command_event base = base_event((uint32_t)count);
    struct gateway_command_event table = base;
    struct gateway_command_event terminal = base;
    struct mock_ble ble;
    bool disconnected_once = false;

    memset(&ble, 0, sizeof(ble));
    gateway_command_observability_init(&ble.observability);
    ops.emit_if_available = mock_emit;
    ops.ctx = &ble;
    assert(app_gateway_assignment_publisher_init(&ops) == 0);
    for (size_t i = 0u; i < count; i++) {
        anchor_ids[i] = UINT64_C(0x1000000000000000) + i;
    }
    assert(discovery_assignment_sort_anchor_ids(anchor_ids, count) == PROTO_OK);
    for (size_t i = 0u; i < count; i++) {
        entries[i].anchor_id = anchor_ids[i];
        entries[i].slot = (uint8_t)i;
    }

    ble.queue_count = GATEWAY_BLE_STREAM_QUEUE_DEPTH;
    ble.max_depth = ble.queue_count;
    ble.connected = true;
    ble.credit = false;
    assert(app_gateway_assignment_publisher_stage_sorted_ids(
               &base, anchor_ids, count, 2u) == 0);
    assert(app_gateway_assignment_publisher_stage_sorted_ids(
               &base, anchor_ids, count, 2u) == 1);
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

    ble.credit = true;
    ble.priority_pending = true;
    app_gateway_assignment_publisher_pump();
    assert(ble.observability.next_event_seq == 0u);
    ble.priority_pending = false;
    ble.connected = false;
    app_gateway_assignment_publisher_pump();
    assert(ble.observability.next_event_seq == 0u);
    ble.connected = true;

    while (ble.queue_count > 0u && ble.queue[0].event_seq == 0u) {
        send_head(&ble, false);
    }
    while (ble.sent_count < count + 3u) {
        bool click_preempt = ble.sent_count == 1u;

        assert(ble.queue_count > 0u);
        if (!disconnected_once && ble.sent_count == 2u) {
            ble.connected = false;
            app_gateway_assignment_publisher_pump();
            assert(ble.queue_count > 0u);
            ble.connected = true;
            disconnected_once = true;
        }
        send_head(&ble, click_preempt);
        if (click_preempt) {
            uint32_t next_seq = ble.observability.next_event_seq;

            app_gateway_assignment_publisher_pump();
            assert(ble.observability.next_event_seq == next_seq);
            ble.priority_pending = false;
            app_gateway_assignment_publisher_pump();
        }
    }

    assert(ble.max_depth <= GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(ble.sent_count == count + 3u);
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
}

int main(void)
{
    run_pressure_case(3u);
    run_pressure_case(20u);
    run_pressure_case(50u);
    return 0;
}
