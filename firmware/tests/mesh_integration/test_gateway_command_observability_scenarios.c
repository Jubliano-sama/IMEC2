#include "app_gateway_command_observability.h"
#include "app_gateway_survey_observability.h"
#include "survey.h"

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
        .command_id = kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY ?
                      CMD_SURVEY_REACHABILITY : CMD_ASSIGN_DISCOVERY_SLOTS,
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
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
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
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    terminal.status = COMMAND_TIMEOUT;
    terminal.reason = GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED;
    terminal.attempt = 4u;
    emit(&scenario, &terminal, true, 0);
    assert(terminal.event_seq > previous_event_seq);
}

static void test_pair_success_failure_and_terminal_counts(void)
{
    struct scenario scenario = {
        .correlation_id = 40u,
        .gateway_sequence = 400u,
        .enumerated = 3u,
    };
    const uint64_t pairs[][2] = {{1u, 2u}, {2u, 3u}};

    gateway_command_observability_init(&scenario.state);
    for (size_t i = 0u; i < 2u; i++) {
        struct gateway_command_event start = event_for(
            &scenario,
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            GATEWAY_COMMAND_EVENT_STAGE_PAIR_START);
        start.pair_initiator_id = pairs[i][0];
        start.pair_responder_id = pairs[i][1];
        start.total_count = 2u;
        emit(&scenario, &start, false, 0);

        if (i == 0u) {
            scenario.successes++;
        } else {
            scenario.failures++;
        }
        struct gateway_command_event result = event_for(
            &scenario,
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            i == 0u ? GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS :
                      GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE);
        result.pair_initiator_id = pairs[i][0];
        result.pair_responder_id = pairs[i][1];
        result.status = i == 0u ? COMMAND_OK : COMMAND_TIMEOUT;
        result.reason = i == 0u ? GATEWAY_COMMAND_EVENT_REASON_NONE :
                                  GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE;
        emit(&scenario, &result, false, 0);
    }

    struct gateway_command_event terminal = event_for(
        &scenario,
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    gateway_command_survey_terminal_outcome(
        scenario.enumerated,
        2u,
        true,
        true,
        scenario.failures,
        GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE,
        &terminal.status,
        &terminal.reason);
    terminal.total_count = 2u;
    emit(&scenario, &terminal, true, 0);
    assert(terminal.success_count == 1u);
    assert(terminal.failure_count == 1u);
    assert(terminal.reason == GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE);
}

static void test_mixed_survey_failures_and_zero_pair_failure(void)
{
    struct scenario mixed = {
        .correlation_id = 41u,
        .gateway_sequence = 401u,
        .enumerated = 3u,
        .failures = 4u,
    };
    const enum gateway_command_event_reason failure_reasons[] = {
        GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE,
        GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED,
        GATEWAY_COMMAND_EVENT_REASON_RADIO,
        GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE,
        GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED,
    };
    enum gateway_command_event_reason failure_reason =
        GATEWAY_COMMAND_EVENT_REASON_NONE;
    struct gateway_command_event terminal;

    gateway_command_observability_init(&mixed.state);
    for (size_t i = 0u;
         i < sizeof(failure_reasons) / sizeof(failure_reasons[0]);
         i++) {
        failure_reason = gateway_command_survey_failure_reason_merge(
            failure_reason, failure_reasons[i]);
    }
    terminal = event_for(&mixed,
                         GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
                         GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    gateway_command_survey_terminal_outcome(
        mixed.enumerated,
        2u,
        true,
        true,
        mixed.failures,
        failure_reason,
        &terminal.status,
        &terminal.reason);
    emit(&mixed, &terminal, true, 0);
    assert(terminal.status == COMMAND_INTERNAL_ERROR);
    assert(terminal.reason == GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);

    struct scenario one_anchor = {
        .correlation_id = 42u,
        .gateway_sequence = 402u,
        .enumerated = 1u,
    };

    gateway_command_observability_init(&one_anchor.state);
    terminal = event_for(&one_anchor,
                         GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
                         GATEWAY_COMMAND_EVENT_STAGE_COMPLETE);
    gateway_command_survey_terminal_outcome(
        one_anchor.enumerated,
        0u,
        true,
        true,
        0u,
        GATEWAY_COMMAND_EVENT_REASON_NONE,
        &terminal.status,
        &terminal.reason);
    terminal.total_count = 0u;
    emit(&one_anchor, &terminal, true, 0);
    assert(terminal.status == COMMAND_INTERNAL_ERROR);
    assert(terminal.reason == GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE);
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
                          GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
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
        &scenario.state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY, &replay));
    assert(replay.progress_count == 50u);
    assert(replay.anchor_id == UINT64_C(0x1032));
    assert(replay.lost_event_count == 0u);
    gateway_command_observability_note_enqueue(&scenario.state,
                                               replay.event_seq, 1);
    assert(!gateway_command_observability_pending_snapshot(
        &scenario.state, GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY, &replay));
}

#define MODEL_QUEUE_DEPTH 3u
#define MODEL_FRAME_LEN (GATEWAY_COMMAND_EVENT_WIRE_LEN + 40u)

struct flow_model {
    uint16_t queued_bytes[MODEL_QUEUE_DEPTH];
    uint8_t queue_count;
    uint16_t head_offset;
    uint32_t accepted;
    uint32_t sent;
    uint32_t transient_refusals;
    uint32_t notify_retries;
    bool ccc;
    bool connected;
    bool credit;
};

static bool flow_model_accept(struct flow_model *model)
{
    if (!model->connected || !model->ccc ||
        model->queue_count >= MODEL_QUEUE_DEPTH) {
        model->transient_refusals++;
        return false;
    }
    model->queued_bytes[model->queue_count++] = MODEL_FRAME_LEN;
    model->accepted++;
    return true;
}

static void flow_model_step(struct flow_model *model,
                            uint16_t mtu,
                            bool transient_notify_failure)
{
    uint16_t chunk;

    if (!model->connected || !model->ccc || model->queue_count == 0u ||
        !model->credit) {
        return;
    }
    if (transient_notify_failure) {
        model->notify_retries++;
        return;
    }
    assert(mtu >= 23u);
    chunk = (uint16_t)(mtu - 3u);
    if (chunk > model->queued_bytes[0] - model->head_offset) {
        chunk = (uint16_t)(model->queued_bytes[0] - model->head_offset);
    }
    model->head_offset += chunk;
    if (model->head_offset == model->queued_bytes[0]) {
        for (uint8_t i = 1u; i < model->queue_count; i++) {
            model->queued_bytes[i - 1u] = model->queued_bytes[i];
        }
        model->queue_count--;
        model->head_offset = 0u;
        model->sent++;
    }
}

static void test_50_anchor_1225_pair_flow_control_sweep(void)
{
    const uint32_t report_events = 50u + 2u;
    const uint32_t pair_events = 2u * 1225u;
    const uint32_t total_events = 2u + report_events + pair_events + 1u;

    for (uint32_t seed = 0u; seed < 64u; seed++) {
        struct flow_model model = {
            .ccc = true,
            .connected = true,
            .credit = true,
        };
        uint32_t produced = 0u;

        for (uint32_t tick = 0u; tick < 500000u && model.sent < total_events;
             tick++) {
            uint32_t phase = tick + seed * 17u;

            model.connected = (phase % 211u) >= 9u;
            model.ccc = model.connected && (phase % 173u) >= 7u;
            model.credit = (phase % 13u) != 0u;

            /* A producer may advance only after the next progress record has custody. */
            if (produced < total_events && flow_model_accept(&model)) {
                produced++;
            }
            flow_model_step(&model,
                            (phase % 5u) == 0u ? 23u : 247u,
                            (phase % 29u) == 0u);
        }
        assert(produced == total_events);
        assert(model.sent == total_events);
        assert(model.queue_count == 0u);
        assert(model.transient_refusals > 0u);
        assert(model.notify_retries > 0u);
    }
}

struct survey_emit_fixture {
    struct gateway_command_event events[64];
    size_t count;
    bool blocked;
};

static int survey_emit(struct gateway_command_event *event,
                       bool terminal,
                       void *ctx)
{
    struct survey_emit_fixture *fixture = ctx;

    assert(!terminal);
    if (fixture->blocked) {
        return -EAGAIN;
    }
    assert(fixture->count < sizeof(fixture->events) / sizeof(fixture->events[0]));
    fixture->events[fixture->count++] = *event;
    return 0;
}

static void test_survey_progress_reconstructs_50_reports_and_gates_boundaries(void)
{
    struct app_gateway_survey_observability_state state;
    struct survey_emit_fixture fixture = {0};
    const struct app_gateway_survey_observability_ops ops = {
        .emit_if_available = survey_emit,
        .ctx = &fixture,
    };
    struct survey_gateway_context survey;
    struct gateway_command_event base = {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        .command_id = CMD_SURVEY_REACHABILITY,
        .gateway_sequence = 77u,
    };
    struct gateway_command_event pair = base;

    assert(survey_gateway_begin(&survey, 77u, 1u) == PROTO_OK);
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        const uint64_t anchor_id = UINT64_C(0x8000) + i;
        const struct survey_gateway_reverse_hint reverse_hint = {
            .target_id = anchor_id,
            .next_hop_id = anchor_id,
            .quality = 100u,
            .hop_count = 1u,
            .valid = true,
        };

        assert(survey_gateway_note_reach_report_with_reverse_hint(
                   &survey,
                   survey.survey_id,
                   anchor_id,
                   NULL,
                   0u,
                   &reverse_hint) == PROTO_OK);
    }
    survey.pair_count = SURVEY_GATEWAY_MAX_PAIRS;
    app_gateway_survey_observability_reset(&state);
    fixture.blocked = true;
    assert(app_gateway_survey_observability_emit_collection_next(
        &state, &ops, &survey, &base, 3u) == -EAGAIN);
    assert(state.report_cursor == 0u);
    assert(fixture.count == 0u);

    fixture.blocked = false;
    while (app_gateway_survey_observability_emit_collection_next(
               &state, &ops, &survey, &base, 3u) == 0) {
    }
    assert(fixture.count == SURVEY_GATEWAY_MAX_REPORTS + 2u);
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        assert(fixture.events[i].stage ==
               GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED);
        assert(fixture.events[i].anchor_id == UINT64_C(0x8000) + i);
        assert(fixture.events[i].progress_count == i + 1u);
    }
    assert(fixture.events[50].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_ENUMERATION_COMPLETE);
    assert(fixture.events[51].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY);

    pair.stage = GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS;
    pair.pair_initiator_id = 1u;
    pair.pair_responder_id = 2u;
    fixture.blocked = true;
    assert(app_gateway_survey_observability_submit_boundary(
        &state, &ops, &pair) == -EAGAIN);
    assert(state.boundary_pending);
    assert(app_gateway_survey_observability_submit_boundary(
        &state, &ops, &pair) == -EBUSY);
    fixture.blocked = false;
    assert(app_gateway_survey_observability_flush_boundary(&state, &ops) == 1);
    assert(!state.boundary_pending);
    assert(fixture.count == SURVEY_GATEWAY_MAX_REPORTS + 3u);
}

static void test_backpressured_pair_start_precedes_pair_success(void)
{
    struct app_gateway_survey_observability_state state;
    struct survey_emit_fixture fixture = {0};
    const struct app_gateway_survey_observability_ops ops = {
        .emit_if_available = survey_emit,
        .ctx = &fixture,
    };
    struct gateway_command_event pair_start = {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_PAIR_START,
        .command_id = CMD_SURVEY_START_PAIR,
        .gateway_sequence = 78u,
        .pair_initiator_id = 1u,
        .pair_responder_id = 2u,
        .total_count = 3u,
    };
    struct gateway_command_event pair_success = pair_start;

    pair_success.stage = GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS;
    pair_success.progress_count = pair_start.total_count;
    pair_success.success_count = 1u;

    app_gateway_survey_observability_reset(&state);
    fixture.blocked = true;
    assert(app_gateway_survey_observability_submit_boundary(
        &state, &ops, &pair_start) == -EAGAIN);
    assert(state.boundary_pending);
    assert(fixture.count == 0u);

    /*
     * The survey worker must retry this boundary before either its waiting
     * early-exit or pair finalization.  Once BLE custody is available, the
     * exact pair-start record must therefore precede the direct pair result.
     */
    fixture.blocked = false;
    assert(app_gateway_survey_observability_flush_boundary(
        &state, &ops) == 1);
    assert(!state.boundary_pending);
    assert(survey_emit(&pair_success, false, &fixture) == 0);

    assert(fixture.count == 2u);
    assert(fixture.events[0].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_PAIR_START);
    assert(fixture.events[1].stage ==
           GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS);
    assert(fixture.events[0].pair_initiator_id ==
           fixture.events[1].pair_initiator_id);
    assert(fixture.events[0].pair_responder_id ==
           fixture.events[1].pair_responder_id);
}

static void test_topology_completeness_is_independent_terminal_gate(void)
{
    const uint64_t anchor_a = UINT64_C(0xa700000000000001);
    const uint64_t anchor_b = UINT64_C(0xa700000000000002);
    const uint64_t anchor_c = UINT64_C(0xa700000000000003);
    const uint64_t anchor_d = UINT64_C(0xa700000000000004);
    const uint64_t isolated = UINT64_C(0xa700000000000005);
    const struct survey_reachability_entry a_entries[] = {
        {.peer_id = anchor_b, .rssi_dbm = -60, .quality = 90u},
    };
    const struct survey_reachability_entry c_entries[] = {
        {.peer_id = anchor_d, .rssi_dbm = -65, .quality = 85u},
    };
    const struct {
        uint64_t anchor_id;
        const struct survey_reachability_entry *entries;
        size_t entry_count;
    } reports[] = {
        {anchor_a, a_entries, 1u},
        {anchor_c, c_entries, 1u},
        {isolated, NULL, 0u},
    };
    struct survey_gateway_context context;
    enum command_status status;
    enum gateway_command_event_reason reason;

    assert(survey_gateway_begin(&context, 0xAABBCCDDu, 2u) == PROTO_OK);
    for (size_t i = 0u; i < sizeof(reports) / sizeof(reports[0]); i++) {
        assert(survey_gateway_note_reach_report(
                   &context,
                   context.survey_id,
                   reports[i].anchor_id,
                   reports[i].entries,
                   reports[i].entry_count) == PROTO_OK);
    }
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == 2u);
    assert(!context.topology_complete);

    gateway_command_survey_terminal_outcome(
        context.report_count,
        context.pair_count,
        context.pairs_planned,
        context.topology_complete,
        0u,
        GATEWAY_COMMAND_EVENT_REASON_NONE,
        &status,
        &reason);
    assert(status == COMMAND_INTERNAL_ERROR);
    assert(reason == GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE);

    assert(survey_gateway_begin(&context, 0xAABBCCDEu, 2u) == PROTO_OK);
    assert(survey_gateway_note_reach_report(
               &context,
               context.survey_id,
               anchor_a,
               a_entries,
               1u) == PROTO_OK);
    assert(survey_gateway_plan_pairs(&context) == PROTO_OK);
    assert(context.pair_count == 1u);
    assert(context.topology_complete);

    gateway_command_survey_terminal_outcome(
        context.report_count,
        context.pair_count,
        context.pairs_planned,
        context.topology_complete,
        0u,
        GATEWAY_COMMAND_EVENT_REASON_NONE,
        &status,
        &reason);
    assert(status == COMMAND_OK);
    assert(reason == GATEWAY_COMMAND_EVENT_REASON_NONE);
}

int main(void)
{
    test_enumeration_success_and_duplicate_deduplication();
    test_no_anchor_and_partial_enumeration_terminals();
    test_retry_backoff_and_timeout_are_correlated();
    test_pair_success_failure_and_terminal_counts();
    test_mixed_survey_failures_and_zero_pair_failure();
    test_ble_disconnect_backpressure_and_reconnect_snapshot();
    test_progress_backpressure_coalesces_without_irreversible_loss();
    test_50_anchor_1225_pair_flow_control_sweep();
    test_survey_progress_reconstructs_50_reports_and_gates_boundaries();
    test_backpressured_pair_start_precedes_pair_success();
    test_topology_completeness_is_independent_terminal_gate();
    puts("gateway command observability integration scenarios passed");
    return 0;
}
