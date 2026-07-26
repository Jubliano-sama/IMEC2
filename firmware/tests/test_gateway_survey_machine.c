#include "gateway_survey_machine.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TEST_SURVEY_ID UINT32_C(0x27182818)
#define TEST_DISCOVERY_TOKEN UINT32_C(0x12345678)

static void assert_machine_unchanged(
    const struct gateway_survey_machine *before,
    const struct gateway_survey_machine *after)
{
    assert(memcmp(before, after, sizeof(*before)) == 0);
}

static void context_init(struct survey_gateway_context *context,
                         size_t pair_count,
                         uint16_t sample_count)
{
    assert(pair_count <= SURVEY_GATEWAY_MAX_REPORTS / 2u);
    memset(context, 0, sizeof(*context));
    context->survey_id = TEST_SURVEY_ID;
    context->sample_count = sample_count;
    context->pair_count = pair_count;
    context->pairs_planned = true;

    for (size_t i = 0u; i < pair_count; i++) {
        const uint64_t initiator_id = UINT64_C(0x1000) + 2u * i;
        const uint64_t responder_id = initiator_id + 1u;

        context->pairs[i] = (struct survey_gateway_pair_entry) {
            .initiator_id = initiator_id,
            .responder_id = responder_id,
        };
        context->reports[2u * i] = (struct survey_gateway_report_slot) {
            .anchor_id = initiator_id,
            .reverse_next_hop_id = initiator_id,
            .reverse_hop_count = 1u,
            .reverse_hint_valid = true,
            .valid = true,
        };
        context->reports[2u * i + 1u] =
            (struct survey_gateway_report_slot) {
                .anchor_id = responder_id,
                .reverse_next_hop_id = responder_id,
                .reverse_hop_count = 1u,
                .reverse_hint_valid = true,
                .valid = true,
            };
    }
    context->report_count = 2u * pair_count;
}

static uint32_t begin_discovery(struct gateway_survey_machine *machine,
                                uint64_t now_ms,
                                uint64_t operation_budget_ms,
                                uint64_t emission_delay_ms,
                                uint64_t collection_duration_ms,
                                uint16_t expected_count,
                                bool expected_present)
{
    uint32_t generation;

    gateway_survey_machine_init(machine);
    assert(gateway_survey_machine_validate(machine) == PROTO_OK);
    assert(gateway_survey_machine_begin(machine,
                                        TEST_SURVEY_ID,
                                        now_ms,
                                        operation_budget_ms,
                                        emission_delay_ms,
                                        collection_duration_ms,
                                        expected_count,
                                        expected_present) == PROTO_OK);
    generation = gateway_survey_machine_generation(machine);
    assert(generation != 0u);
    assert(gateway_survey_machine_bind_discovery_delivery(
               machine, generation, TEST_DISCOVERY_TOKEN) == PROTO_OK);
    assert(gateway_survey_machine_validate(machine) == PROTO_OK);
    return generation;
}

static void deliver_discovery(struct gateway_survey_machine *machine,
                              uint32_t generation,
                              uint64_t terminal_ms)
{
    struct gateway_survey_machine snapshot;
    const struct node_comm_terminal_event event = {
        .handle = TEST_DISCOVERY_TOKEN,
        .reason = NODE_COMM_TERMINAL_DELIVERED,
        .attempts_started = 1u,
    };

    assert(gateway_survey_machine_note_discovery_terminal(
               machine,
               generation,
               TEST_DISCOVERY_TOKEN,
               terminal_ms,
               &event) == PROTO_OK);
    assert(gateway_survey_machine_phase(machine) ==
           GATEWAY_SURVEY_MACHINE_COLLECTING);
    assert(gateway_survey_machine_discovery_delivery_token(machine) == 0u);
    assert(gateway_survey_machine_discovery_rf_started(machine));
    assert(gateway_survey_machine_validate(machine) == PROTO_OK);

    snapshot = *machine;
    assert(gateway_survey_machine_note_discovery_terminal(
               machine,
               generation,
               TEST_DISCOVERY_TOKEN,
               terminal_ms,
               &event) == PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, machine);
}

static void ready_round_machine(struct gateway_survey_machine *machine,
                                const struct survey_gateway_context *context,
                                uint8_t max_parallel_pairs,
                                uint8_t max_reruns)
{
    struct gateway_survey_machine_drive drive;
    const uint64_t now_ms = UINT64_C(0x100000020);
    uint32_t generation = begin_discovery(
        machine,
        now_ms,
        UINT64_C(1000000),
        100u,
        10000u,
        (uint16_t)context->report_count,
        true);

    assert(gateway_survey_machine_note_discovery_rf_started(
               machine, generation, TEST_DISCOVERY_TOKEN) == PROTO_OK);
    deliver_discovery(machine, generation, now_ms + 10u);
    assert(gateway_survey_machine_collection_drive(
               machine,
               now_ms + 100u,
               context->report_count,
               &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_START_ROUNDS);
    assert(gateway_survey_machine_phase(machine) ==
           GATEWAY_SURVEY_MACHINE_ROUND_READY);
    assert(gateway_survey_machine_active(machine));
    assert(!gateway_survey_machine_round_active(machine));
    assert(gateway_survey_machine_round_begin(machine,
                                               context,
                                               max_parallel_pairs,
                                               max_reruns) == PROTO_OK);
    assert(gateway_survey_machine_round_active(machine));
    assert(gateway_survey_machine_validate(machine) == PROTO_OK);
}

static void dispatch_current_batch(struct gateway_survey_machine *machine)
{
    const enum survey_gateway_auto_stage stages[] = {
        SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
        SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_INITIATOR,
    };
    size_t stage_index = 0u;
    size_t previous_lane = SIZE_MAX;
    size_t guard = 0u;

    while (!gateway_survey_machine_round_go_needed(machine)) {
        struct gateway_survey_machine_control control;

        assert(++guard <=
               4u * SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES);
        assert(gateway_survey_machine_round_current_control(
                   machine, &control) == PROTO_OK);
        if (control.lane_index != previous_lane) {
            previous_lane = control.lane_index;
            stage_index = 0u;
        }
        assert(stage_index < sizeof(stages) / sizeof(stages[0]));
        assert(control.stage == stages[stage_index]);
        assert(gateway_survey_machine_round_note_control_success(
                   machine,
                   control.command_id,
                   control.target_id,
                   control.pair.survey_id) == PROTO_OK);
        stage_index++;
    }
}

static void finish_current_batch_success(
    struct gateway_survey_machine *machine)
{
    const size_t lane_count =
        gateway_survey_machine_round_lane_count(machine);

    dispatch_current_batch(machine);
    assert(gateway_survey_machine_round_mark_observing_after_go(machine) ==
           PROTO_OK);
    for (size_t i = 0u; i < lane_count; i++) {
        assert(gateway_survey_machine_round_finalize_lane(
                   machine,
                   i,
                   0u,
                   SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    }
    assert(gateway_survey_machine_round_batch_complete(machine));
}

static void test_lifecycle_uses_64_bit_deadlines(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine snapshot;
    const uint64_t now_ms = UINT64_C(0x100000010);
    const uint64_t operation_budget_ms = UINT64_C(0x200000020);
    const uint64_t emission_delay_ms = UINT64_C(0x100000030);
    uint32_t generation;
    uint64_t safety_deadline_ms = 0u;

    gateway_survey_machine_init(&machine);
    generation = gateway_survey_machine_generation(&machine);
    assert(gateway_survey_machine_begin(
               &machine,
               TEST_SURVEY_ID,
               now_ms,
               operation_budget_ms,
               emission_delay_ms,
               UINT64_C(0x100000040),
               7u,
               true) == PROTO_OK);
    assert(gateway_survey_machine_generation(&machine) != generation);
    assert(gateway_survey_machine_operation_deadline_ms(&machine) ==
           now_ms + operation_budget_ms);
    assert(gateway_survey_machine_operation_remaining_ms(
               &machine, now_ms) == UINT32_MAX);
    assert(gateway_survey_machine_operation_remaining_ms(
               &machine, now_ms + operation_budget_ms - 1u) == 1u);
    assert(gateway_survey_machine_operation_remaining_ms(
               &machine, now_ms + operation_budget_ms) == 0u);
    assert(gateway_survey_machine_operation_remaining_ms(NULL, now_ms) == 0u);
    assert(gateway_survey_machine_emission_deadline_ms(&machine) ==
           now_ms + emission_delay_ms);
    assert(gateway_survey_machine_collection_duration_ms(&machine) ==
           UINT64_C(0x100000040));
    assert(gateway_survey_machine_expected_count(&machine) == 7u);
    assert(gateway_survey_machine_expected_count_present(&machine));
    assert(!gateway_survey_machine_safety_deadline_ms(
        &machine, &safety_deadline_ms));

    gateway_survey_machine_reset(&machine);
    snapshot = machine;
    assert(gateway_survey_machine_begin(
               &machine,
               TEST_SURVEY_ID,
               UINT64_MAX - 10u,
               11u,
               0u,
               1u,
               0u,
               false) == PROTO_ERR_NO_SPACE);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_begin(
               &machine,
               TEST_SURVEY_ID,
               UINT64_MAX - 10u,
               10u,
               11u,
               1u,
               0u,
               false) == PROTO_ERR_NO_SPACE);
    assert_machine_unchanged(&snapshot, &machine);
}

static void test_report_admission_requires_current_rf_evidence(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine snapshot;
    const uint64_t now_ms = UINT64_C(0x200000000);
    uint32_t generation;

    gateway_survey_machine_init(&machine);
    assert(gateway_survey_machine_begin(&machine,
                                        TEST_SURVEY_ID,
                                        now_ms,
                                        1000u,
                                        100u,
                                        500u,
                                        2u,
                                        true) == PROTO_OK);
    generation = gateway_survey_machine_generation(&machine);
    assert(gateway_survey_machine_report_admissible(
               &machine, generation, TEST_SURVEY_ID, now_ms) ==
           PROTO_ERR_BUSY);
    assert(gateway_survey_machine_bind_discovery_delivery(
               &machine, generation, TEST_DISCOVERY_TOKEN) == PROTO_OK);
    assert(gateway_survey_machine_report_admissible(
               &machine, generation, TEST_SURVEY_ID, now_ms) ==
           PROTO_ERR_BUSY);

    snapshot = machine;
    assert(gateway_survey_machine_admit_report(
               &machine, generation + 1u, TEST_DISCOVERY_TOKEN,
               TEST_SURVEY_ID, now_ms + 1u, 1u) == PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_admit_report(
               &machine, generation, TEST_DISCOVERY_TOKEN + 1u,
               TEST_SURVEY_ID, now_ms + 1u, 1u) == PROTO_ERR_NOT_FOUND);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_admit_report(
               &machine, generation, TEST_DISCOVERY_TOKEN,
               TEST_SURVEY_ID, now_ms + 1u, 0u) == PROTO_ERR_BUSY);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_admit_report(
               &machine, generation, TEST_DISCOVERY_TOKEN,
               TEST_SURVEY_ID, now_ms + 1u, 1u) == PROTO_OK);

    snapshot = machine;
    assert(gateway_survey_machine_note_discovery_rf_started(
               &machine, generation + 1u, TEST_DISCOVERY_TOKEN) ==
           PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_note_discovery_rf_started(
               &machine, generation, TEST_DISCOVERY_TOKEN + 1u) ==
           PROTO_ERR_NOT_FOUND);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_note_discovery_rf_started(
               &machine, generation, TEST_DISCOVERY_TOKEN) == PROTO_OK);
    assert(gateway_survey_machine_report_admissible(
               &machine, generation, TEST_SURVEY_ID, now_ms + 1u) ==
           PROTO_OK);
    assert(gateway_survey_machine_report_admissible(
               &machine, generation + 1u, TEST_SURVEY_ID, now_ms + 1u) ==
           PROTO_ERR_STALE);
    assert(gateway_survey_machine_report_admissible(
               &machine, generation, TEST_SURVEY_ID + 1u, now_ms + 1u) ==
           PROTO_ERR_STALE);
    assert(gateway_survey_machine_report_admissible(
               &machine, generation, TEST_SURVEY_ID, now_ms + 1000u) ==
           PROTO_ERR_STALE);
}

static void test_stale_discovery_events_are_side_effect_free(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine snapshot;
    struct node_comm_terminal_event event = {
        .handle = TEST_DISCOVERY_TOKEN,
        .reason = NODE_COMM_TERMINAL_DELIVERED,
        .attempts_started = 1u,
    };
    uint32_t old_generation;

    old_generation = begin_discovery(
        &machine, 100u, 10000u, 200u, 500u, 1u, true);
    snapshot = machine;
    assert(gateway_survey_machine_note_discovery_terminal(
               &machine,
               old_generation + 1u,
               TEST_DISCOVERY_TOKEN,
               110u,
               &event) == PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_note_discovery_terminal(
               &machine,
               old_generation,
               TEST_DISCOVERY_TOKEN + 1u,
               110u,
               &event) == PROTO_ERR_NOT_FOUND);
    assert_machine_unchanged(&snapshot, &machine);
    event.handle++;
    assert(gateway_survey_machine_note_discovery_terminal(
               &machine,
               old_generation,
               TEST_DISCOVERY_TOKEN,
               110u,
               &event) == PROTO_ERR_NOT_FOUND);
    assert_machine_unchanged(&snapshot, &machine);

    gateway_survey_machine_reset(&machine);
    snapshot = machine;
    event.handle = TEST_DISCOVERY_TOKEN;
    assert(gateway_survey_machine_note_discovery_terminal(
               &machine,
               old_generation,
               TEST_DISCOVERY_TOKEN,
               110u,
               &event) == PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);

    assert(gateway_survey_machine_begin(
               &machine,
               TEST_SURVEY_ID,
               200u,
               10000u,
               200u,
               500u,
               1u,
               true) == PROTO_OK);
    assert(gateway_survey_machine_bind_discovery_delivery(
               &machine,
               gateway_survey_machine_generation(&machine),
               TEST_DISCOVERY_TOKEN + 10u) == PROTO_OK);
    snapshot = machine;
    assert(gateway_survey_machine_note_discovery_terminal(
               &machine,
               old_generation,
               TEST_DISCOVERY_TOKEN,
               210u,
               &event) == PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
}

static void test_discovery_terminal_failure_mappings(void)
{
    const struct {
        enum node_comm_terminal_reason event_reason;
        enum gateway_survey_machine_terminal_reason machine_reason;
    } cases[] = {
        {
            NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
            GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_TIMEOUT,
        },
        {
            NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
            GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RETRY_EXHAUSTED,
        },
        {
            NODE_COMM_TERMINAL_PERMANENT_FAILURE,
            GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO,
        },
        {
            NODE_COMM_TERMINAL_CANCELLED,
            GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO,
        },
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct gateway_survey_machine machine;
        struct gateway_survey_machine_drive drive;
        struct node_comm_terminal_event event = {
            .handle = TEST_DISCOVERY_TOKEN,
            .reason = cases[i].event_reason,
            .attempts_started = 1u,
        };
        uint32_t generation = begin_discovery(
            &machine, 100u, 1000u, 100u, 500u, 1u, true);

        assert(gateway_survey_machine_note_discovery_terminal(
                   &machine,
                   generation,
                   TEST_DISCOVERY_TOKEN,
                   110u,
                   &event) == PROTO_OK);
        assert(gateway_survey_machine_phase(&machine) ==
               GATEWAY_SURVEY_MACHINE_TERMINAL);
        assert(gateway_survey_machine_terminal_reason(&machine) ==
               cases[i].machine_reason);
        assert(gateway_survey_machine_discovery_delivery_token(&machine) ==
               0u);
        assert(gateway_survey_machine_collection_drive(
                   &machine, 111u, 0u, &drive) == PROTO_OK);
        assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_TERMINAL);
        assert(drive.reason == cases[i].machine_reason);
        assert(gateway_survey_machine_validate(&machine) == PROTO_OK);
    }

    {
        struct gateway_survey_machine machine;
        struct node_comm_terminal_event event = {
            .handle = TEST_DISCOVERY_TOKEN,
            .reason = NODE_COMM_TERMINAL_DELIVERED,
            .attempts_started = 0u,
        };
        uint32_t generation = begin_discovery(
            &machine, 100u, 1000u, 100u, 500u, 1u, true);

        assert(gateway_survey_machine_note_discovery_terminal(
                   &machine,
                   generation,
                   TEST_DISCOVERY_TOKEN,
                   110u,
                   &event) == PROTO_OK);
        assert(gateway_survey_machine_terminal_reason(&machine) ==
               GATEWAY_SURVEY_MACHINE_TERMINAL_DISCOVERY_RADIO);
        assert(gateway_survey_machine_discovery_delivery_token(&machine) ==
               0u);
    }
}

static void test_delivery_arms_safety_from_actual_terminal(void)
{
    struct gateway_survey_machine machine;
    uint64_t deadline_ms = 0u;
    uint32_t generation = begin_discovery(
        &machine,
        UINT64_C(0x100000000),
        10000u,
        500u,
        UINT64_C(0x100000020),
        1u,
        true);
    const uint64_t terminal_ms = UINT64_C(0x100000123);

    deliver_discovery(&machine, generation, terminal_ms);
    assert(gateway_survey_machine_safety_deadline_ms(
        &machine, &deadline_ms));
    assert(deadline_ms == terminal_ms + UINT64_C(0x100000020));

    gateway_survey_machine_reset(&machine);
    generation = begin_discovery(
        &machine,
        UINT64_MAX - 1000u,
        1000u,
        0u,
        600u,
        1u,
        true);
    {
        const struct node_comm_terminal_event event = {
            .handle = TEST_DISCOVERY_TOKEN,
            .reason = NODE_COMM_TERMINAL_DELIVERED,
            .attempts_started = 1u,
        };

        assert(gateway_survey_machine_note_discovery_terminal(
                   &machine,
                   generation,
                   TEST_DISCOVERY_TOKEN,
                   UINT64_MAX - 500u,
                   &event) == PROTO_ERR_NO_SPACE);
        assert(gateway_survey_machine_terminal_reason(&machine) ==
               GATEWAY_SURVEY_MACHINE_TERMINAL_INTERNAL);
    }
}

static void test_collection_deadline_and_expected_count_boundaries(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine_drive drive;
    const uint64_t now_ms = UINT64_C(0x300000000);
    uint32_t generation = begin_discovery(
        &machine, now_ms, 5000u, 100u, 1000u, 2u, true);

    deliver_discovery(&machine, generation, now_ms + 10u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 99u, 2u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_WAIT);
    assert(drive.wake_at_ms == now_ms + 100u);
    assert(drive.wake_at_ms > now_ms + 99u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 100u, 2u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_START_ROUNDS);

    gateway_survey_machine_reset(&machine);
    generation = begin_discovery(
        &machine, now_ms, 5000u, 100u, 1000u, 3u, true);
    deliver_discovery(&machine, generation, now_ms + 10u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 100u, 2u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_WAIT);
    assert(drive.wake_at_ms == now_ms + 1010u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 1009u, 2u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_WAIT);
    assert(drive.wake_at_ms > now_ms + 1009u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 1010u, 2u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_TERMINAL);
    assert(drive.reason ==
           GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_MISSING);

    gateway_survey_machine_reset(&machine);
    generation = begin_discovery(
        &machine, now_ms, 5000u, 100u, 1000u, 2u, true);
    deliver_discovery(&machine, generation, now_ms + 10u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 100u, 3u, &drive) == PROTO_OK);
    assert(drive.reason ==
           GATEWAY_SURVEY_MACHINE_TERMINAL_EXPECTED_COUNT_EXCEEDED);

    gateway_survey_machine_reset(&machine);
    generation = begin_discovery(
        &machine, now_ms, 5000u, 100u, 1000u, 1u, true);
    deliver_discovery(&machine, generation, now_ms + 10u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 1010u, 0u, &drive) == PROTO_OK);
    assert(drive.reason == GATEWAY_SURVEY_MACHINE_TERMINAL_NO_ANCHORS);

    gateway_survey_machine_reset(&machine);
    generation = begin_discovery(
        &machine, now_ms, 5000u, 100u, 1000u, 0u, false);
    deliver_discovery(&machine, generation, now_ms + 10u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 100u, 1u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_WAIT);
    assert(drive.wake_at_ms == now_ms + 1010u);
    assert(gateway_survey_machine_collection_drive(
               &machine, now_ms + 1010u, 1u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_START_ROUNDS);
}

static void test_operation_deadline_wins_at_equality(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine_drive drive;
    uint32_t generation = begin_discovery(
        &machine, 100u, 100u, 100u, 1000u, 1u, true);

    deliver_discovery(&machine, generation, 110u);
    assert(gateway_survey_machine_collection_drive(
               &machine, 199u, 1u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_WAIT);
    assert(drive.wake_at_ms == 200u);
    assert(gateway_survey_machine_collection_drive(
               &machine, 200u, 1u, &drive) == PROTO_OK);
    assert(drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_TERMINAL);
    assert(drive.reason ==
           GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT);

    gateway_survey_machine_reset(&machine);
    generation = begin_discovery(
        &machine, 100u, 100u, 20u, 1000u, 1u, true);
    {
        const struct node_comm_terminal_event event = {
            .handle = TEST_DISCOVERY_TOKEN,
            .reason = NODE_COMM_TERMINAL_DELIVERED,
            .attempts_started = 1u,
        };

        assert(gateway_survey_machine_note_discovery_terminal(
                   &machine,
                   generation,
                   TEST_DISCOVERY_TOKEN,
                   200u,
                   &event) == PROTO_OK);
        assert(gateway_survey_machine_terminal_reason(&machine) ==
               GATEWAY_SURVEY_MACHINE_TERMINAL_OPERATION_TIMEOUT);
    }
}

static void test_abort_and_reset_invalidate_operation(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine snapshot;
    struct gateway_survey_machine_drive drive;
    uint32_t generation = begin_discovery(
        &machine, 100u, 1000u, 100u, 500u, 1u, true);

    snapshot = machine;
    assert(gateway_survey_machine_abort(
               &machine, generation + 1u, TEST_SURVEY_ID) ==
           PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_abort(
               &machine, generation, TEST_SURVEY_ID + 1u) ==
           PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_abort(
               &machine, generation, TEST_SURVEY_ID) == PROTO_OK);
    assert(gateway_survey_machine_terminal_reason(&machine) ==
           GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED);
    snapshot = machine;
    assert(gateway_survey_machine_abort(
               &machine, generation, TEST_SURVEY_ID) == PROTO_OK);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_collection_drive(
               &machine, 101u, 0u, &drive) == PROTO_OK);
    assert(drive.reason == GATEWAY_SURVEY_MACHINE_TERMINAL_ABORTED);

    gateway_survey_machine_reset(&machine);
    assert(gateway_survey_machine_phase(&machine) ==
           GATEWAY_SURVEY_MACHINE_IDLE);
    assert(gateway_survey_machine_generation(&machine) != generation);
    assert(gateway_survey_machine_validate(&machine) == PROTO_OK);
}

static void test_maximum_25_lane_batch_dispatch_order(void)
{
    struct survey_gateway_context context;
    struct gateway_survey_machine machine;
    struct gateway_survey_machine_control control;
    bool complete = false;

    context_init(&context, 25u, 2u);
    ready_round_machine(&machine,
                        &context,
                        SURVEY_PAIR_ROUND_RUNTIME_MAX_LANES,
                        1u);
    assert(gateway_survey_machine_planned_round_count(&machine) == 1u);
    assert(gateway_survey_machine_round_lane_count(&machine) == 25u);
    assert(gateway_survey_machine_round_id(&machine) != 0u);
    assert(gateway_survey_machine_round_current_control(
               &machine, &control) == PROTO_OK);
    assert(gateway_survey_machine_round_note_control_success(
               &machine,
               control.command_id,
               control.target_id + 1u,
               control.pair.survey_id) == PROTO_ERR_NOT_FOUND);
    assert(gateway_survey_machine_round_current_control(
               &machine, &control) == PROTO_OK);
    assert(control.stage == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR);

    finish_current_batch_success(&machine);
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_OK);
    assert(complete);
    assert(gateway_survey_machine_round_complete(&machine));
    assert(gateway_survey_machine_round_success_count(&machine) == 25u);
    assert(gateway_survey_machine_round_failure_count(&machine) == 0u);
    assert(gateway_survey_machine_validate(&machine) == PROTO_OK);
}

static void test_planner_round_splits_into_multiple_capped_batches(void)
{
    struct survey_gateway_context context;
    struct gateway_survey_machine machine;
    const size_t expected_batch_sizes[] = {7u, 7u, 7u, 4u};
    size_t batch_index = 0u;
    bool complete = false;

    context_init(&context, 25u, 1u);
    ready_round_machine(&machine, &context, 7u, 0u);
    while (!complete) {
        assert(batch_index <
               sizeof(expected_batch_sizes) /
                   sizeof(expected_batch_sizes[0]));
        assert(gateway_survey_machine_round_lane_count(&machine) ==
               expected_batch_sizes[batch_index]);
        finish_current_batch_success(&machine);
        assert(gateway_survey_machine_round_advance_batch(
                   &machine, &complete) == PROTO_OK);
        batch_index++;
    }
    assert(batch_index ==
           sizeof(expected_batch_sizes) / sizeof(expected_batch_sizes[0]));
    assert(gateway_survey_machine_round_success_count(&machine) == 25u);
    assert(gateway_survey_machine_round_complete(&machine));
}

static void test_one_lane_failure_rerun_does_not_disturb_peer(void)
{
    struct survey_gateway_context context;
    struct gateway_survey_machine machine;
    const struct survey_pair failed_pair = {
        .initiator_id = UINT64_C(0x1002),
        .responder_id = UINT64_C(0x1003),
        .survey_id = TEST_SURVEY_ID,
        .sample_count = 2u,
    };
    struct survey_sample sample;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    bool complete = true;

    context_init(&context, 2u, 2u);
    ready_round_machine(&machine, &context, 2u, 1u);
    dispatch_current_batch(&machine);
    assert(gateway_survey_machine_round_mark_observing_after_go(&machine) ==
           PROTO_OK);

    sample = (struct survey_sample) {
        .pair = failed_pair,
        .round_id = gateway_survey_machine_round_id(&machine),
        .sample_index = 0u,
        .distance_mm = 900,
        .quality = 80u,
        .range_status = RANGE_OK,
    };
    assert(gateway_survey_machine_round_note_sample(
               &machine,
               failed_pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(lane_index == 1u);
    assert(accepted_new);
    assert(gateway_survey_machine_round_lane(&machine, 0u)
               ->usable_result_mask == 0u);

    assert(gateway_survey_machine_round_finalize_lane(
               &machine,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK);
    assert(gateway_survey_machine_round_finalize_lane(
               &machine,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(gateway_survey_machine_round_note_cleanup_complete(
               &machine,
               0u,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK) == PROTO_OK);
    assert(gateway_survey_machine_round_success_count(&machine) == 1u);
    assert(gateway_survey_machine_round_note_cleanup_complete(
               &machine,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(!gateway_survey_machine_round_batch_complete(&machine));
    assert(gateway_survey_machine_round_note_cleanup_complete(
               &machine,
               1u,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(gateway_survey_machine_round_batch_complete(&machine));

    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_OK);
    assert(!complete);
    assert(gateway_survey_machine_round_lane_count(&machine) == 1u);
    assert(gateway_survey_machine_round_lane(&machine, 0u)
               ->pair.initiator_id == failed_pair.initiator_id);
    assert(gateway_survey_machine_round_lane(&machine, 0u)
               ->reruns_started == 1u);
    dispatch_current_batch(&machine);
    assert(gateway_survey_machine_round_mark_observing_after_go(&machine) ==
           PROTO_OK);
    assert(gateway_survey_machine_round_finalize_lane(
               &machine,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_FAIL) == PROTO_OK);
    assert(gateway_survey_machine_round_batch_complete(&machine));
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_OK);
    assert(complete);
    assert(gateway_survey_machine_round_success_count(&machine) == 1u);
    assert(gateway_survey_machine_round_failure_count(&machine) == 1u);
}

static void test_stale_and_duplicate_samples_do_not_cross_rerun(void)
{
    struct survey_gateway_context context;
    struct gateway_survey_machine machine;
    struct gateway_survey_machine snapshot;
    struct survey_sample sample;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;
    bool complete = true;
    uint16_t old_round_id;

    context_init(&context, 1u, 1u);
    ready_round_machine(&machine, &context, 1u, 1u);
    dispatch_current_batch(&machine);
    assert(gateway_survey_machine_round_mark_observing_after_go(&machine) ==
           PROTO_OK);
    old_round_id = gateway_survey_machine_round_id(&machine);
    sample = (struct survey_sample) {
        .pair = gateway_survey_machine_round_lane(&machine, 0u)->pair,
        .round_id = old_round_id,
        .sample_index = 0u,
        .distance_mm = 123,
        .quality = 77u,
        .range_status = RANGE_OK,
    };
    assert(gateway_survey_machine_round_note_sample(
               &machine,
               sample.pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(accepted_new);
    snapshot = machine;
    accepted_new = true;
    assert(gateway_survey_machine_round_note_sample(
               &machine,
               sample.pair.responder_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(!accepted_new);
    assert_machine_unchanged(&snapshot, &machine);

    assert(gateway_survey_machine_round_finalize_lane(
               &machine,
               0u,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY) == PROTO_OK);
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_OK);
    assert(!complete);
    dispatch_current_batch(&machine);
    assert(gateway_survey_machine_round_mark_observing_after_go(&machine) ==
           PROTO_OK);
    assert(gateway_survey_machine_round_id(&machine) != old_round_id);
    snapshot = machine;
    assert(gateway_survey_machine_round_note_sample(
               &machine,
               sample.pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_ERR_STALE);
    assert_machine_unchanged(&snapshot, &machine);
    sample.round_id = gateway_survey_machine_round_id(&machine);
    assert(gateway_survey_machine_round_note_sample(
               &machine,
               sample.pair.initiator_id,
               &sample,
               &lane_index,
               &accepted_new) == PROTO_OK);
    assert(accepted_new);
}

static void test_failed_control_cleanup_ownership_and_release_barrier(void)
{
    struct survey_gateway_context context;
    struct gateway_survey_machine machine;
    struct gateway_survey_machine snapshot;
    struct gateway_survey_machine_control control;
    size_t failed_lane = SIZE_MAX;
    size_t owned_lane = SIZE_MAX;
    bool complete = true;

    context_init(&context, 1u, 1u);
    ready_round_machine(&machine, &context, 1u, 1u);
    assert(gateway_survey_machine_round_current_control(
               &machine, &control) == PROTO_OK);
    assert(gateway_survey_machine_round_note_control_failure(
               &machine,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &failed_lane) == PROTO_OK);
    assert(failed_lane == 0u);
    assert(gateway_survey_machine_failed_control_cleanup_pending(&machine));
    assert(gateway_survey_machine_failed_control_cleanup_lane(
               &machine, &owned_lane) == PROTO_OK);
    assert(owned_lane == failed_lane);

    snapshot = machine;
    assert(gateway_survey_machine_round_note_control_failure(
               &machine,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               NULL) == PROTO_ERR_BUSY);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_round_current_control(
               &machine, &control) == PROTO_ERR_BUSY);
    assert(gateway_survey_machine_release_failed_control_cleanup(
               &machine, failed_lane) == PROTO_ERR_BUSY);
    assert(gateway_survey_machine_round_note_cleanup_complete(
               &machine,
               failed_lane,
               SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK);
    assert(gateway_survey_machine_release_failed_control_cleanup(
               &machine, failed_lane) == PROTO_ERR_BUSY);
    assert(gateway_survey_machine_round_note_cleanup_complete(
               &machine,
               failed_lane,
               SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK);
    assert(gateway_survey_machine_round_batch_complete(&machine));

    snapshot = machine;
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_ERR_BUSY);
    assert_machine_unchanged(&snapshot, &machine);
    assert(gateway_survey_machine_release_failed_control_cleanup(
               &machine, failed_lane + 1u) == PROTO_ERR_NOT_FOUND);
    assert(gateway_survey_machine_release_failed_control_cleanup(
               &machine, failed_lane) == PROTO_OK);
    assert(!gateway_survey_machine_failed_control_cleanup_pending(&machine));
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_OK);
    assert(!complete);

    assert(gateway_survey_machine_round_current_control(
               &machine, &control) == PROTO_OK);
    assert(gateway_survey_machine_round_note_control_failure(
               &machine,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &failed_lane) == PROTO_OK);
    assert(gateway_survey_machine_round_lane(&machine, failed_lane)->state ==
           SURVEY_PAIR_ROUND_LANE_FAILED);
    assert(gateway_survey_machine_round_batch_complete(&machine));
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_ERR_BUSY);
    assert(gateway_survey_machine_release_failed_control_cleanup(
               &machine, failed_lane) == PROTO_OK);
    assert(gateway_survey_machine_round_advance_batch(
               &machine, &complete) == PROTO_OK);
    assert(complete);
    assert(gateway_survey_machine_round_failure_count(&machine) == 1u);
}

static void test_failed_lane_does_not_block_an_armed_peer(void)
{
    struct survey_gateway_context context;
    struct gateway_survey_machine machine;
    struct gateway_survey_machine_control control;
    size_t failed_lane = SIZE_MAX;

    context_init(&context, 2u, 2u);
    ready_round_machine(&machine, &context, 2u, 1u);
    assert(gateway_survey_machine_round_current_control(
               &machine, &control) == PROTO_OK);
    assert(gateway_survey_machine_round_note_control_failure(
               &machine,
               control.command_id,
               control.target_id,
               control.pair.survey_id,
               0u,
               SURVEY_PAIR_ROUND_CLEANUP_RETRY,
               &failed_lane) == PROTO_OK);
    assert(gateway_survey_machine_round_lane(&machine, failed_lane)->state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    assert(!gateway_survey_machine_round_go_needed(&machine));
    assert(gateway_survey_machine_release_failed_control_cleanup(
               &machine, failed_lane) == PROTO_OK);
    dispatch_current_batch(&machine);
    assert(gateway_survey_machine_round_go_needed(&machine));
    assert(gateway_survey_machine_round_mark_observing_after_go(&machine) ==
           PROTO_OK);
    assert(gateway_survey_machine_round_lane(&machine, 0u)->state ==
           SURVEY_PAIR_ROUND_LANE_RERUN_QUEUED);
    assert(gateway_survey_machine_round_lane(&machine, 1u)->state ==
           SURVEY_PAIR_ROUND_LANE_OBSERVING);
}

static void test_go_policy_is_exhaustive(void)
{
    for (int error = -4096; error <= 4096; error++) {
        const bool expected =
            error == -EAGAIN || error == -EBUSY ||
            error == -ENOSPC || error == -ESHUTDOWN;

        assert(gateway_survey_machine_round_go_submit_retryable(error) ==
               expected);
    }
    for (int raw_reason = -4;
         raw_reason <= NODE_COMM_TERMINAL_CANCELLED + 4;
         raw_reason++) {
        const enum node_comm_terminal_reason reason =
            (enum node_comm_terminal_reason)raw_reason;
        const bool retryable_reason =
            reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ||
            reason == NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED ||
            reason == NODE_COMM_TERMINAL_CANCELLED;

        assert(gateway_survey_machine_round_go_terminal_retryable(
                   reason, 0u) == retryable_reason);
        assert(!gateway_survey_machine_round_go_terminal_retryable(
            reason, 1u));
        assert(!gateway_survey_machine_round_go_terminal_retryable(
            reason, UINT8_MAX));
    }
}

static void test_validate_detects_corrupt_invariants(void)
{
    struct gateway_survey_machine machine;
    struct gateway_survey_machine corrupt;
    uint32_t generation = begin_discovery(
        &machine, 100u, 1000u, 100u, 500u, 2u, true);

    corrupt = machine;
    corrupt.generation = 0u;
    assert(gateway_survey_machine_validate(&corrupt) ==
           PROTO_ERR_MALFORMED);
    corrupt = machine;
    corrupt.expected_count = 0u;
    assert(gateway_survey_machine_validate(&corrupt) ==
           PROTO_ERR_MALFORMED);
    corrupt = machine;
    corrupt.safety_deadline_armed = true;
    assert(gateway_survey_machine_validate(&corrupt) ==
           PROTO_ERR_MALFORMED);

    deliver_discovery(&machine, generation, 110u);
    corrupt = machine;
    corrupt.discovery_rf_started = false;
    assert(gateway_survey_machine_validate(&corrupt) ==
           PROTO_ERR_MALFORMED);
    corrupt = machine;
    corrupt.phase = (enum gateway_survey_machine_phase)UINT8_MAX;
    assert(gateway_survey_machine_validate(&corrupt) ==
           PROTO_ERR_MALFORMED);
}

static uint32_t stress_next(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void test_deterministic_lifecycle_stress(void)
{
    struct gateway_survey_machine machine;
    uint32_t rng = UINT32_C(0x5eed5307);
    uint32_t previous_generation = 0u;

    gateway_survey_machine_init(&machine);
    for (size_t cycle = 0u; cycle < 2048u; cycle++) {
        struct gateway_survey_machine_drive drive;
        struct node_comm_terminal_event event;
        uint64_t now_ms =
            UINT64_C(0x100000000) + (uint64_t)cycle * 10000u;
        uint16_t expected_count =
            (uint16_t)(1u + stress_next(&rng) % 8u);
        bool expected_present = (stress_next(&rng) & 1u) != 0u;
        uint32_t generation;

        assert(gateway_survey_machine_begin(
                   &machine,
                   TEST_SURVEY_ID,
                   now_ms,
                   9000u,
                   100u,
                   1000u,
                   expected_count,
                   expected_present) == PROTO_OK);
        generation = gateway_survey_machine_generation(&machine);
        assert(generation != 0u);
        assert(generation != previous_generation);
        previous_generation = generation;
        assert(gateway_survey_machine_bind_discovery_delivery(
                   &machine, generation, TEST_DISCOVERY_TOKEN) == PROTO_OK);

        event = (struct node_comm_terminal_event) {
            .handle = TEST_DISCOVERY_TOKEN,
            .reason = (enum node_comm_terminal_reason)
                (stress_next(&rng) %
                 (NODE_COMM_TERMINAL_CANCELLED + 1u)),
            .attempts_started = (uint8_t)(stress_next(&rng) % 3u),
        };
        assert(gateway_survey_machine_note_discovery_terminal(
                   &machine,
                   generation,
                   TEST_DISCOVERY_TOKEN,
                   now_ms + 10u,
                   &event) == PROTO_OK);
        assert(gateway_survey_machine_validate(&machine) == PROTO_OK);
        if (gateway_survey_machine_phase(&machine) ==
            GATEWAY_SURVEY_MACHINE_COLLECTING) {
            size_t report_count = expected_present ?
                stress_next(&rng) % (expected_count + 2u) :
                stress_next(&rng) % 9u;

            assert(gateway_survey_machine_collection_drive(
                       &machine,
                       now_ms + 100u,
                       report_count,
                       &drive) == PROTO_OK);
            if (drive.kind == GATEWAY_SURVEY_MACHINE_DRIVE_WAIT) {
                assert(drive.wake_at_ms > now_ms + 100u);
                assert(gateway_survey_machine_collection_drive(
                           &machine,
                           drive.wake_at_ms,
                           report_count,
                           &drive) == PROTO_OK);
            }
            assert(drive.kind != GATEWAY_SURVEY_MACHINE_DRIVE_WAIT);
        }
        assert(gateway_survey_machine_validate(&machine) == PROTO_OK);
        gateway_survey_machine_reset(&machine);
        assert(gateway_survey_machine_validate(&machine) == PROTO_OK);
    }
}

int main(void)
{
    test_lifecycle_uses_64_bit_deadlines();
    test_report_admission_requires_current_rf_evidence();
    test_stale_discovery_events_are_side_effect_free();
    test_discovery_terminal_failure_mappings();
    test_delivery_arms_safety_from_actual_terminal();
    test_collection_deadline_and_expected_count_boundaries();
    test_operation_deadline_wins_at_equality();
    test_abort_and_reset_invalidate_operation();
    test_maximum_25_lane_batch_dispatch_order();
    test_planner_round_splits_into_multiple_capped_batches();
    test_one_lane_failure_rerun_does_not_disturb_peer();
    test_stale_and_duplicate_samples_do_not_cross_rerun();
    test_failed_control_cleanup_ownership_and_release_barrier();
    test_failed_lane_does_not_block_an_armed_peer();
    test_go_policy_is_exhaustive();
    test_validate_detects_corrupt_invariants();
    test_deterministic_lifecycle_stress();
    puts("gateway survey machine tests passed");
    return 0;
}
