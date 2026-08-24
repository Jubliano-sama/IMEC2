#include "app_gateway_command_observability.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

struct scenario {
    struct gateway_command_observability_state state;
    uint32_t correlation_id;
    uint32_t gateway_sequence;
    uint16_t enumerated;
    uint16_t duplicates;
    uint16_t successes;
    uint16_t failures;
};

static struct gateway_command_event event_for(
    const struct scenario *scenario,
    enum gateway_command_event_kind kind,
    enum gateway_command_event_stage stage)
{
    return (struct gateway_command_event) {
        .kind = kind,
        .stage = stage,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .correlation_id = scenario->correlation_id,
        .gateway_sequence = scenario->gateway_sequence,
        .host_session_id = scenario->correlation_id,
        .progress_count = scenario->enumerated,
        .success_count = scenario->successes,
        .failure_count = scenario->failures,
        .duplicate_count = scenario->duplicates,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
}

static void emit(struct scenario *scenario,
                 struct gateway_command_event *event,
                 bool terminal,
                 int transport_result)
{
    assert(gateway_command_observability_prepare(&scenario->state,
                                                 event,
                                                 terminal) == 0);
    gateway_command_observability_note_enqueue(&scenario->state,
                                               event->event_seq,
                                               transport_result);
}

static void test_enumeration_success_and_duplicate_deduplication(void)
{
    struct scenario scenario = {.correlation_id = 10u, .gateway_sequence = 100u};
    const uint64_t replies[] = {11u, 22u, 11u, 33u};
    uint64_t seen[3] = {0};
    size_t seen_count = 0u;

    gateway_command_observability_init(&scenario.state);
    for (size_t i = 0u; i < 4u; i++) {
        bool duplicate = false;

        for (size_t j = 0u; j < seen_count; j++) {
            duplicate |= seen[j] == replies[i];
        }
        if (duplicate) {
            scenario.duplicates++;
            continue;
        }
        seen[seen_count++] = replies[i];
        scenario.enumerated++;
        struct gateway_command_event event = event_for(
            &scenario,
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
        event.anchor_id = replies[i];
        event.hop_count = (uint8_t)(i + 1u);
        emit(&scenario, &event, false, 0);
    }

    struct gateway_command_event terminal = event_for(
        &scenario,
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    terminal.total_count = scenario.enumerated;
    terminal.success_count = scenario.enumerated;
    emit(&scenario, &terminal, true, 0);
    assert(terminal.progress_count == 3u);
    assert(terminal.duplicate_count == 1u);
}

static void test_no_anchor_and_partial_enumeration_terminals(void)
{
    struct scenario empty = {.correlation_id = 20u, .gateway_sequence = 200u};
    struct scenario partial = {
        .correlation_id = 21u,
        .gateway_sequence = 201u,
        .enumerated = 2u,
        .failures = 1u,
    };
    struct gateway_command_event event;

    gateway_command_observability_init(&empty.state);
    event = event_for(&empty,
                      GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
                      GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    event.status = COMMAND_TIMEOUT;
    event.reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS;
    emit(&empty, &event, true, 0);
    assert(event.progress_count == 0u);

    gateway_command_observability_init(&partial.state);
    event = event_for(&partial,
                      GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
                      GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    event.status = COMMAND_TIMEOUT;
    event.reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    event.total_count = 3u;
    emit(&partial, &event, true, 0);
    assert(event.progress_count == 2u);
    assert(event.failure_count == 1u);
}

static void test_retry_backoff_and_timeout_are_correlated(void)
{
    struct scenario scenario = {.correlation_id = 30u, .gateway_sequence = 300u};
    uint32_t previous_event_seq = 0u;

    gateway_command_observability_init(&scenario.state);
    for (uint8_t attempt = 1u; attempt <= 4u; attempt++) {
        struct gateway_command_event event = event_for(
            &scenario,
            GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH,
            GATEWAY_COMMAND_EVENT_STAGE_BACKOFF);
        event.attempt = attempt;
        event.status = COMMAND_BUSY;
        event.reason = GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE;
        emit(&scenario, &event, false, 0);
        assert(event.correlation_id == 30u);
        assert(event.event_seq > previous_event_seq);
        previous_event_seq = event.event_seq;
    }
    struct gateway_command_event terminal = event_for(
        &scenario,
        GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    terminal.status = COMMAND_TIMEOUT;
    terminal.reason = GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED;
    terminal.attempt = 4u;
    emit(&scenario, &terminal, true, 0);
    assert(terminal.event_seq > previous_event_seq);
}

static void test_ble_disconnect_backpressure_and_reconnect_snapshot(void)
{
    struct scenario scenario = {.correlation_id = 50u, .gateway_sequence = 500u};
    struct gateway_command_event event = event_for(
        &scenario,
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    struct gateway_command_event replay;

    gateway_command_observability_init(&scenario.state);
    event.status = COMMAND_TIMEOUT;
    event.reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
    emit(&scenario, &event, true, -ENOTCONN);
    assert(gateway_command_observability_pending_terminal(&scenario.state, &replay));
    assert(replay.lost_event_count == 0u);
    gateway_command_observability_note_enqueue(&scenario.state,
                                               replay.event_seq,
                                               -ENOSPC);
    assert(gateway_command_observability_pending_terminal(&scenario.state, &replay));
    assert(replay.lost_event_count == 0u);
    gateway_command_observability_note_enqueue(&scenario.state,
                                               replay.event_seq,
                                               0);
    gateway_command_observability_mark_sent(&scenario.state, replay.event_seq);
    assert(!gateway_command_observability_pending_terminal(&scenario.state, &replay));
}

static void test_progress_backpressure_coalesces_without_irreversible_loss(void)
{
    struct scenario scenario = {.correlation_id = 60u, .gateway_sequence = 600u};
    struct gateway_command_event event;
    struct gateway_command_event replay;

    gateway_command_observability_init(&scenario.state);
    for (uint16_t progress = 1u; progress <= 50u; progress++) {
        event = event_for(&scenario,
                          GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH,
                          GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
        event.anchor_id = UINT64_C(0x1000) + progress;
        event.progress_count = progress;
        assert(gateway_command_observability_prepare(&scenario.state,
                                                     &event, false) == 0);
        gateway_command_observability_note_enqueue(&scenario.state,
                                                   event.event_seq,
                                                   progress < 50u ? -EAGAIN :
                                                                    -ENOTCONN);
    }
    assert(gateway_command_observability_pending_snapshot(
        &scenario.state, GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH, &replay));
    assert(replay.progress_count == 50u);
    assert(replay.anchor_id == UINT64_C(0x1032));
    assert(replay.lost_event_count == 0u);
    gateway_command_observability_note_enqueue(&scenario.state,
                                               replay.event_seq, 1);
    assert(!gateway_command_observability_pending_snapshot(
        &scenario.state, GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH, &replay));
}

int main(void)
{
    test_enumeration_success_and_duplicate_deduplication();
    test_no_anchor_and_partial_enumeration_terminals();
    test_retry_backoff_and_timeout_are_correlated();
    test_ble_disconnect_backpressure_and_reconnect_snapshot();
    test_progress_backpressure_coalesces_without_irreversible_loss();
    puts("gateway command observability integration scenarios passed");
    return 0;
}
