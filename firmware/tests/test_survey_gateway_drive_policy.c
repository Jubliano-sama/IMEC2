#include "survey_gateway_transaction.h"
#include "survey.h"

#include <assert.h>
#include <stdio.h>

static enum survey_gateway_drive_action decide(
    bool survey_active,
    bool auto_running,
    bool auto_waiting,
    bool pair_observation_active,
    bool cleanup_pending,
    bool boundary_pending,
    bool response_ack_settle_pending)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = survey_active,
        .auto_running = auto_running,
        .auto_waiting = auto_waiting,
        .pair_observation_active = pair_observation_active,
        .cleanup_pending = cleanup_pending,
        .boundary_pending = boundary_pending,
        .response_ack_settle_pending = response_ack_settle_pending,
    };

    return survey_gateway_drive_action(&state);
}

static void test_cleanup_always_keeps_polling(void)
{
    assert(decide(true, true, false, false, true, false, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_CLEANUP);
    assert(decide(false, false, false, false, true, false, true) ==
           SURVEY_GATEWAY_DRIVE_POLL_CLEANUP);
}

static void test_boundary_custody_uses_bounded_retry(void)
{
    assert(decide(true, true, false, false, false, true, true) ==
           SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY);
}

static void test_runnable_orphan_is_driven_now(void)
{
    /* Covers both another pair and the final LOAD_PAIR completion step. */
    assert(decide(true, true, false, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_RUN_NOW);
}

static void test_round_drive_ready_does_not_depend_on_serial_auto_owner(void)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = true,
        .auto_running = false,
        .auto_waiting = false,
        .pair_observation_active = false,
        .round_drive_ready = true,
        .cleanup_pending = false,
        .boundary_pending = false,
        .response_ack_settle_pending = false,
    };

    /*
     * A completed parallel batch can queue a rerun while the compatibility
     * serial owner is idle. The round phase remains the runnable owner.
     */
    assert(survey_gateway_drive_action(&state) ==
           SURVEY_GATEWAY_DRIVE_RUN_NOW);
}

static void test_observing_round_polls_without_busy_spin(void)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = true,
        .auto_running = false,
        .auto_waiting = false,
        .pair_observation_active = true,
        .round_drive_ready = false,
        .cleanup_pending = false,
        .boundary_pending = false,
        .response_ack_settle_pending = false,
    };

    /*
     * OBSERVING is an external wait, not runnable round work: preserve the
     * bounded deadline poll and never resubmit the worker at zero delay.
     */
    assert(survey_gateway_drive_action(&state) ==
           SURVEY_GATEWAY_DRIVE_POLL_WAIT);
}

static void test_response_ack_settle_blocks_next_phase(void)
{
    assert(decide(true, true, false, false, false, false, true) ==
           SURVEY_GATEWAY_DRIVE_NONE);
}

static void test_external_waits_keep_a_bounded_deadline_poll(void)
{
    assert(decide(true, true, true, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_WAIT);
    assert(decide(true, true, false, true, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_WAIT);
    assert(decide(true, false, false, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
    assert(decide(false, true, false, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
}

static void test_response_ack_settle_deadline_boundary(void)
{
    struct survey_gateway_response_ack_settle settle;
    uint64_t accepted_at_ms = 1000u;
    uint64_t deadline_ms =
        accepted_at_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS;

    survey_gateway_response_ack_settle_init(&settle);
    assert(!survey_gateway_response_ack_settle_pending(&settle,
                                                       accepted_at_ms));

    survey_gateway_response_ack_settle_note_result(&settle,
                                                   accepted_at_ms);
    assert(survey_gateway_response_ack_settle_pending(&settle,
                                                       accepted_at_ms));
    assert(survey_gateway_response_ack_settle_pending(&settle,
                                                       deadline_ms - 1u));
    assert(!survey_gateway_response_ack_settle_pending(&settle,
                                                        deadline_ms));
}

static void test_exact_duplicate_restarts_quiet_interval(void)
{
    struct survey_gateway_response_ack_settle settle;
    uint64_t accepted_at_ms = 2000u;
    uint64_t duplicate_at_ms =
        accepted_at_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS - 1u;
    uint64_t original_deadline_ms =
        accepted_at_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS;
    uint64_t extended_deadline_ms =
        duplicate_at_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS;

    survey_gateway_response_ack_settle_init(&settle);
    survey_gateway_response_ack_settle_note_result(&settle,
                                                   accepted_at_ms);
    survey_gateway_response_ack_settle_note_result(&settle,
                                                   duplicate_at_ms);

    assert(survey_gateway_response_ack_settle_pending(&settle,
                                                       original_deadline_ms));
    assert(survey_gateway_response_ack_settle_pending(&settle,
                                                       extended_deadline_ms - 1u));
    assert(!survey_gateway_response_ack_settle_pending(&settle,
                                                        extended_deadline_ms));
}

static void test_each_phase_starts_a_fresh_quiet_interval(void)
{
    struct survey_gateway_response_ack_settle settle;
    uint64_t first_result_ms = 3000u;
    uint64_t second_result_ms =
        first_result_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS;
    uint64_t second_deadline_ms =
        second_result_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS;

    survey_gateway_response_ack_settle_init(&settle);
    survey_gateway_response_ack_settle_note_result(&settle,
                                                   first_result_ms);
    assert(!survey_gateway_response_ack_settle_pending(&settle,
                                                        second_result_ms));

    survey_gateway_response_ack_settle_note_result(&settle,
                                                   second_result_ms);
    assert(survey_gateway_response_ack_settle_pending(&settle,
                                                       second_result_ms));
    assert(!survey_gateway_response_ack_settle_pending(&settle,
                                                        second_deadline_ms));
}

static void test_every_nonterminal_round_control_result_blocks_dispatch(void)
{
    static const enum survey_gateway_auto_stage completed_controls[] = {
        SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
        SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_INITIATOR,
    };

    for (size_t i = 0u;
         i < sizeof(completed_controls) / sizeof(completed_controls[0]);
         i++) {
        struct survey_gateway_response_ack_settle settle;
        uint64_t accepted_at_ms = 4000u + (uint64_t)i * 10000u;
        uint64_t deadline_ms =
            accepted_at_ms + SURVEY_GATEWAY_RESPONSE_ACK_SETTLE_MS;
        bool settle_pending;

        assert(completed_controls[i] >=
                   SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR &&
               completed_controls[i] <=
                   SURVEY_GATEWAY_AUTO_START_INITIATOR);
        survey_gateway_response_ack_settle_init(&settle);

        /* This is the required glue ordering for every successful round
         * control: advance its owner, arm ACK quiet time, then ask policy
         * whether the following control or common GO can run. */
        survey_gateway_response_ack_settle_note_result(&settle,
                                                       accepted_at_ms);
        settle_pending = survey_gateway_response_ack_settle_pending(
            &settle, accepted_at_ms);
        assert(settle_pending);
        assert(decide(true, true, false, false, false, false,
                      settle_pending) == SURVEY_GATEWAY_DRIVE_NONE);

        settle_pending = survey_gateway_response_ack_settle_pending(
            &settle, deadline_ms - 1u);
        assert(settle_pending);
        assert(decide(true, true, false, false, false, false,
                      settle_pending) == SURVEY_GATEWAY_DRIVE_NONE);

        settle_pending = survey_gateway_response_ack_settle_pending(
            &settle, deadline_ms);
        assert(!settle_pending);
        assert(decide(true, true, false, false, false, false,
                      settle_pending) == SURVEY_GATEWAY_DRIVE_RUN_NOW);
    }
}

int main(void)
{
    test_cleanup_always_keeps_polling();
    test_boundary_custody_uses_bounded_retry();
    test_runnable_orphan_is_driven_now();
    test_round_drive_ready_does_not_depend_on_serial_auto_owner();
    test_observing_round_polls_without_busy_spin();
    test_response_ack_settle_blocks_next_phase();
    test_external_waits_keep_a_bounded_deadline_poll();
    test_response_ack_settle_deadline_boundary();
    test_exact_duplicate_restarts_quiet_interval();
    test_each_phase_starts_a_fresh_quiet_interval();
    test_every_nonterminal_round_control_result_blocks_dispatch();
    assert(survey_gateway_drive_action(NULL) == SURVEY_GATEWAY_DRIVE_NONE);
    puts("survey gateway drive policy tests passed");
    return 0;
}
