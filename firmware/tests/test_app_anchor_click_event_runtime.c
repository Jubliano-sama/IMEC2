#include "app_anchor_click_event_runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static struct uwb_wake_claim_frame make_claim(uint32_t event_id,
                                              uint8_t attempt)
{
    struct uwb_wake_claim_frame claim = {
        .network_id = 1u,
        .clicker_id = UINT64_C(0x1111222233334444),
        .click_event_id = event_id,
        .attempt_index = attempt,
        .priority_id = UINT64_C(0x5555666677778888),
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 400u,
        .discovery_starts_in_ms = 20u,
        .claimed_duration_ms = 1000u,
        .min_anchor_count = 1u,
        .max_anchor_count = 3u,
        .nonce = UINT64_C(0x0102030405060708),
        .flags = FLAG_COUNT_AS_CLICK,
    };

    return claim;
}

static void test_normal_click_phase_and_custody(void)
{
    struct uwb_wake_claim_frame claim = make_claim(10u, 1u);
    struct uwb_wake_claim_frame second = make_claim(11u, 1u);
    struct fw_transition transition;

    app_anchor_click_event_runtime_reset();
    assert(app_anchor_click_event_runtime_claim(&claim, 100u,
                                                &transition) == 0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_CLAIMED);
    assert(transition.effect.type == FW_EFFECT_ANCHOR_WAIT_SCHEDULE);

    /* Repeated copies of one accepted claim do not restart the operation. */
    assert(app_anchor_click_event_runtime_claim(&claim, 101u, NULL) == 0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_CLAIMED);

    assert(app_anchor_click_event_runtime_handle(FW_EVENT_DISCOVER_RECEIVED,
                                                 120u,
                                                 &transition) == 0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_DISCOVERY_REPLIED);
    assert(transition.effect.type == FW_EFFECT_ANCHOR_SEND_DISCOVERY_REPLY);

    assert(app_anchor_click_event_runtime_schedule_received(true, 140u,
                                                            &transition) == 0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_SCHEDULED);
    assert(app_anchor_click_event_runtime_handle(FW_EVENT_RANGE_DUE, 160u,
                                                 &transition) == 0);
    assert(app_anchor_click_event_runtime_state() == FW_ANCHOR_CLICK_RANGING);

    assert(app_anchor_click_event_runtime_handle(FW_EVENT_RESULT_RETAINED,
                                                 200u,
                                                 &transition) == 0);
    assert(app_anchor_click_event_runtime_result_owned());
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_RESULT_OWNED);

    /* Abort cannot discard the retained report owner. */
    assert(app_anchor_click_event_runtime_abort(210u, NULL) == 0);
    assert(app_anchor_click_event_runtime_result_owned());
    assert(app_anchor_click_event_runtime_claim(&second, 220u, NULL) ==
           -EBUSY);
    assert(app_anchor_click_event_runtime_custody_released(
               second.clicker_id, second.click_event_id, second.attempt_index,
               221u, NULL) == -ESTALE);
    assert(app_anchor_click_event_runtime_custody_released(
               claim.clicker_id, claim.click_event_id, claim.attempt_index,
               300u, &transition) == 0);
    assert(!app_anchor_click_event_runtime_active());
    assert(app_anchor_click_event_runtime_state() == FW_ANCHOR_CLICK_IDLE);
    assert(app_anchor_click_event_runtime_claim(&second, 301u,
                                                &transition) == 0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_CLAIMED);
    assert(app_anchor_click_event_runtime_abort(305u, NULL) == 0);
    assert(app_anchor_click_event_runtime_handle(FW_EVENT_RANGE_DUE, 310u,
                                                 NULL) == -ESTALE);
}

static void test_direct_range_only_has_explicit_schedule_boundary(void)
{
    struct uwb_wake_claim_frame claim = make_claim(20u, 1u);
    struct fw_transition transition;
    uint32_t first_generation;

    app_anchor_click_event_runtime_reset();
    assert(app_anchor_click_event_runtime_claim(&claim, 400u,
                                                &transition) == 0);
    first_generation = transition.effect.generation;
    assert(app_anchor_click_event_runtime_schedule_received(false, 450u,
                                                            NULL) == 0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_SCHEDULED);
    assert(app_anchor_click_event_runtime_handle(FW_EVENT_RANGE_DUE, 500u,
                                                 NULL) == 0);
    assert(app_anchor_click_event_runtime_state() == FW_ANCHOR_CLICK_RANGING);
    assert(app_anchor_click_event_runtime_abort(510u, NULL) == 0);
    assert(app_anchor_click_event_runtime_state() == FW_ANCHOR_CLICK_ABORTED);
    assert(!app_anchor_click_event_runtime_active());

    /* An identical claim after abort starts a new lifecycle generation. */
    assert(app_anchor_click_event_runtime_claim(&claim, 520u,
                                                &transition) == 0);
    assert(app_anchor_click_event_runtime_active());
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_CLAIMED);
    assert(transition.effect.generation > first_generation);
}

static void test_priority_replacement_restarts_phase_generation(void)
{
    struct uwb_wake_claim_frame first = make_claim(30u, 1u);
    struct uwb_wake_claim_frame replacement = make_claim(30u, 2u);

    app_anchor_click_event_runtime_reset();
    assert(app_anchor_click_event_runtime_claim(&first, 600u, NULL) == 0);
    assert(app_anchor_click_event_runtime_claim(&replacement, 610u, NULL) ==
           0);
    assert(app_anchor_click_event_runtime_state() ==
           FW_ANCHOR_CLICK_CLAIMED);
    assert(app_anchor_click_event_runtime_active());
    assert(app_anchor_click_event_runtime_abort(620u, NULL) == 0);
    assert(!app_anchor_click_event_runtime_active());
}

int main(void)
{
    test_normal_click_phase_and_custody();
    test_direct_range_only_has_explicit_schedule_boundary();
    test_priority_replacement_restarts_phase_generation();
    puts("app_anchor_click_event_runtime: ok");
    return 0;
}
