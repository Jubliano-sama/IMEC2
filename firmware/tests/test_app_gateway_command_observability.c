#include "app_gateway_command_observability.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct gateway_command_event sample_event(
    enum gateway_command_event_kind kind,
    enum gateway_command_event_stage stage,
    uint32_t correlation_id)
{
    return (struct gateway_command_event) {
        .kind = kind,
        .stage = stage,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY ?
                      CMD_SURVEY_REACHABILITY : CMD_ASSIGN_DISCOVERY_SLOTS,
        .gateway_epoch = 7u,
        .correlation_id = correlation_id,
        .gateway_sequence = 0x11223344u,
        .host_session_id = 0x55667788u,
        .host_seq = 0x1234u,
        .anchor_id = UINT64_C(0x1111222233334444),
        .pair_initiator_id = UINT64_C(0x0102030405060708),
        .pair_responder_id = UINT64_C(0x1112131415161718),
        .previous_hop_id = UINT64_C(0x2122232425262728),
        .progress_count = 4u,
        .total_count = 9u,
        .success_count = 3u,
        .failure_count = 1u,
        .duplicate_count = 2u,
        .hop_count = 3u,
        .slot = 5u,
    };
}

static void test_fixed_record_round_trip(void)
{
    struct gateway_command_observability_state state;
    struct gateway_command_event input = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        42u);
    struct gateway_command_event decoded;
    uint8_t wire[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t written = 0u;

    gateway_command_observability_init(&state);
    assert(gateway_command_observability_prepare(&state, &input, false) == 0);
    assert(gateway_command_event_encode(&input, wire, sizeof(wire), &written) == 0);
    assert(written == GATEWAY_COMMAND_EVENT_WIRE_LEN);
    assert(wire[0] == GATEWAY_COMMAND_EVENT_SCHEMA_VERSION);
    assert(wire[1] == GATEWAY_COMMAND_EVENT_WIRE_LEN);
    assert(gateway_command_event_decode(wire, written, &decoded) == 0);
    assert(decoded.event_seq == 1u);
    assert(decoded.correlation_id == 42u);
    assert(decoded.anchor_id == input.anchor_id);
    assert(decoded.previous_hop_id == input.previous_hop_id);
    assert(decoded.hop_count == 3u);
    assert(decoded.slot == 5u);
}

static void test_parser_rejects_unknown_and_malformed_records(void)
{
    struct gateway_command_observability_state state;
    struct gateway_command_event event = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE,
        99u);
    struct gateway_command_event decoded;
    uint8_t wire[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t written = 0u;

    gateway_command_observability_init(&state);
    assert(gateway_command_observability_prepare(&state, &event, true) == 0);
    assert(gateway_command_event_encode(&event, wire, sizeof(wire), &written) == 0);
    wire[0]++;
    assert(gateway_command_event_decode(wire, written, &decoded) == -EPROTONOSUPPORT);
    wire[0] = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION;
    wire[1]--;
    assert(gateway_command_event_decode(wire, written, &decoded) == -EMSGSIZE);
    wire[1] = GATEWAY_COMMAND_EVENT_WIRE_LEN;
    wire[2] = 0xffu;
    assert(gateway_command_event_decode(wire, written, &decoded) == -EINVAL);
    wire[2] = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY;
    wire[3] = 0xffu;
    assert(gateway_command_event_decode(wire, written, &decoded) == -EINVAL);
}

static void test_terminal_is_retained_across_backpressure_until_sent(void)
{
    struct gateway_command_observability_state state;
    struct gateway_command_event terminal = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        123u);
    struct gateway_command_event replay;

    gateway_command_observability_init(&state);
    terminal.status = COMMAND_TIMEOUT;
    terminal.reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS;
    assert(gateway_command_observability_prepare(&state, &terminal, true) == 0);
    gateway_command_observability_note_enqueue(&state, terminal.event_seq, -ENOTCONN);
    assert(gateway_command_observability_pending_terminal(&state, &replay));
    assert(replay.event_seq == terminal.event_seq);
    assert(replay.lost_event_count == 1u);
    assert((replay.flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u);
    assert((replay.flags & GATEWAY_COMMAND_EVENT_FLAG_REPLAY) != 0u);

    gateway_command_observability_note_enqueue(&state, replay.event_seq, 0);
    assert(!gateway_command_observability_pending_terminal(&state, &replay));
    gateway_command_observability_mark_sent(&state, terminal.event_seq);
    assert(!gateway_command_observability_reconnect_snapshot(
        &state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION, &replay));
}

static void test_active_snapshot_survives_disconnect_and_reports_loss(void)
{
    struct gateway_command_observability_state state;
    struct gateway_command_event progress = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_BACKOFF,
        321u);
    struct gateway_command_event replay;

    gateway_command_observability_init(&state);
    progress.attempt = 2u;
    assert(gateway_command_observability_prepare(&state, &progress, false) == 0);
    gateway_command_observability_note_enqueue(&state, progress.event_seq, -ENOSPC);
    assert(gateway_command_observability_reconnect_snapshot(
        &state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY, &replay));
    assert(replay.correlation_id == 321u);
    assert(replay.attempt == 2u);
    assert(replay.lost_event_count == 1u);
    assert((replay.flags & GATEWAY_COMMAND_EVENT_FLAG_SNAPSHOT) != 0u);
}

static void test_sequential_and_concurrent_correlations_stay_distinct(void)
{
    struct gateway_command_observability_state state;
    struct gateway_command_event enumeration = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_QUEUED,
        1001u);
    struct gateway_command_event survey = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_QUEUED,
        2002u);
    struct gateway_command_event next_enumeration = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_QUEUED,
        3003u);
    struct gateway_command_event replay;

    gateway_command_observability_init(&state);
    assert(gateway_command_observability_prepare(&state, &enumeration, false) == 0);
    assert(gateway_command_observability_prepare(&state, &survey, false) == 0);
    assert(enumeration.event_seq != survey.event_seq);
    assert(gateway_command_observability_reconnect_snapshot(
        &state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION, &replay));
    assert(replay.correlation_id == 1001u);
    assert(gateway_command_observability_reconnect_snapshot(
        &state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY, &replay));
    assert(replay.correlation_id == 2002u);

    assert(gateway_command_observability_prepare(&state, &next_enumeration, false) == 0);
    assert(next_enumeration.event_seq > survey.event_seq);
    assert(gateway_command_observability_reconnect_snapshot(
        &state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION, &replay));
    assert(replay.correlation_id == 3003u);
}

static void test_two_unsent_sequential_terminals_do_not_overwrite(void)
{
    struct gateway_command_observability_state state;
    struct gateway_command_event first = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        4004u);
    struct gateway_command_event second = sample_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        5005u);
    struct gateway_command_event replay;

    gateway_command_observability_init(&state);
    assert(gateway_command_observability_prepare(&state, &first, true) == 0);
    gateway_command_observability_note_enqueue(&state, first.event_seq, -ENOTCONN);
    assert(gateway_command_observability_prepare(&state, &second, true) == 0);
    gateway_command_observability_note_enqueue(&state, second.event_seq, -ENOTCONN);
    assert(gateway_command_observability_pending_terminal(&state, &replay));
    assert(replay.correlation_id == 4004u);
    gateway_command_observability_note_enqueue(&state, replay.event_seq, 0);
    assert(gateway_command_observability_pending_terminal(&state, &replay));
    assert(replay.correlation_id == 5005u);
}

int main(void)
{
    test_fixed_record_round_trip();
    test_parser_rejects_unknown_and_malformed_records();
    test_terminal_is_retained_across_backpressure_until_sent();
    test_active_snapshot_survives_disconnect_and_reports_loss();
    test_sequential_and_concurrent_correlations_stay_distinct();
    test_two_unsent_sequential_terminals_do_not_overwrite();
    puts("gateway command observability tests passed");
    return 0;
}
