#include "protocol_rx_lifecycle.h"

#include <assert.h>
#include <stdint.h>

static void test_ddd_activation_rf_work_and_success(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x101), 100u, 10000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(anchor.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);

    /* A repeated gateway/relay copy belongs to the same active protocol. */
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x101), 200u, 20000u) ==
           PROTOCOL_RX_BEGIN_DUPLICATE);
    assert(anchor.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);
    assert(anchor.deadline_ms == 10000u);

    assert(protocol_rx_lifecycle_rf_begin(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x101)));
    assert(anchor.mode == PROTOCOL_RX_MODE_OWNED_RF_WORK);
    assert(protocol_rx_lifecycle_rf_end(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x101)));
    assert(anchor.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);
    assert(protocol_rx_lifecycle_terminate(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x101)));
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
}

static void test_forced_hop_activation_is_local_and_wake_once(void)
{
    struct protocol_rx_lifecycle first_hop;
    struct protocol_rx_lifecycle second_hop;

    protocol_rx_lifecycle_init(&first_hop);
    protocol_rx_lifecycle_init(&second_hop);

    assert(protocol_rx_lifecycle_begin(
               &first_hop, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x202), 100u, 30000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    /* F1's first relay activates idle F2. Later phases are duplicates. */
    assert(protocol_rx_lifecycle_begin(
               &second_hop, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x202), 200u, 30000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(protocol_rx_lifecycle_begin(
               &second_hop, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x202), 500u, 30000u) ==
           PROTOCOL_RX_BEGIN_DUPLICATE);
    assert(second_hop.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);

    assert(protocol_rx_lifecycle_terminate(
        &first_hop, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x202)));
    assert(protocol_rx_lifecycle_terminate(
        &second_hop, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x202)));
}

static void test_abort_failure_timeout_and_stale_terminal_return_low_duty(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x303), UINT32_MAX - 50u, 25u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(!protocol_rx_lifecycle_terminate(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x302)));
    assert(anchor.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);
    assert(!protocol_rx_lifecycle_expire(&anchor, UINT32_MAX));
    assert(protocol_rx_lifecycle_expire(&anchor, 25u));
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);

    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_HERE_I_AM,
               UINT64_C(0x404), 30u, 100u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(protocol_rx_lifecycle_terminate(
        &anchor, PROTOCOL_RX_OPERATION_HERE_I_AM, UINT64_C(0x404)));
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
}

static void test_one_gateway_operation_owns_channel_five(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x505), 0u, 1000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_HERE_I_AM,
               UINT64_C(0x606), 1u, 1000u) ==
           PROTOCOL_RX_BEGIN_BUSY);
    assert(anchor.operation == PROTOCOL_RX_OPERATION_ENUMERATION);
    assert(anchor.generation == UINT64_C(0x505));
}

static void test_terminal_failure_during_owned_rf_returns_low_duty(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x606), 10u, 1000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(protocol_rx_lifecycle_rf_begin(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x606)));
    assert(protocol_rx_lifecycle_terminate(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x606)));
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
}

static void test_unexpected_receive_error_recovery_is_fail_closed(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(protocol_rx_lifecycle_note_rx_recovery(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x707), true) == PROTOCOL_RX_RECOVERY_INVALID);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x707), 10u, 1000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);

    /* Adapter recovered and reconfigured after an unexpected -EIO. */
    assert(protocol_rx_lifecycle_note_rx_recovery(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x707), true) == PROTOCOL_RX_RECOVERY_REARMED);
    assert(anchor.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5);

    /* A bad DWM state that survives bounded recovery terminates fail-closed. */
    assert(protocol_rx_lifecycle_note_rx_recovery(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x707), false) == PROTOCOL_RX_RECOVERY_TERMINATED);
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
}

static void test_ambiguous_deadline_is_rejected(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
               UINT64_C(0x707), 0u, UINT32_C(0x80000000)) ==
           PROTOCOL_RX_BEGIN_INVALID);
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
}

static void test_survey_plan_replaces_only_its_live_operation_deadline(void)
{
    struct protocol_rx_lifecycle anchor;

    protocol_rx_lifecycle_init(&anchor);
    assert(protocol_rx_lifecycle_begin(
               &anchor, PROTOCOL_RX_OPERATION_SURVEY,
               UINT64_C(0x808), 100u, 1000u) ==
           PROTOCOL_RX_BEGIN_ACCEPTED);
    assert(!protocol_rx_lifecycle_set_deadline(
        &anchor, PROTOCOL_RX_OPERATION_ENUMERATION,
        UINT64_C(0x808), 200u, 2000u));
    assert(!protocol_rx_lifecycle_set_deadline(
        &anchor, PROTOCOL_RX_OPERATION_SURVEY,
        UINT64_C(0x809), 200u, 2000u));
    assert(!protocol_rx_lifecycle_set_deadline(
        &anchor, PROTOCOL_RX_OPERATION_SURVEY,
        UINT64_C(0x808), 200u, 200u));
    assert(protocol_rx_lifecycle_set_deadline(
        &anchor, PROTOCOL_RX_OPERATION_SURVEY,
        UINT64_C(0x808), 200u, 2000u));
    assert(anchor.deadline_ms == 2000u);
    assert(!protocol_rx_lifecycle_expire(&anchor, 1999u));
    assert(protocol_rx_lifecycle_expire(&anchor, 2000u));
    assert(anchor.mode == PROTOCOL_RX_MODE_LOW_DUTY);
}

static void test_forced_hop_downstream_wakes_once_per_generation(void)
{
    struct protocol_rx_downstream_activation activation;

    protocol_rx_downstream_activation_init(&activation);
    assert(!protocol_rx_downstream_activation_needs_wake(
        &activation, PROTOCOL_RX_OPERATION_NONE, UINT64_C(0x808), 100u));
    assert(!protocol_rx_downstream_activation_needs_wake(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, 0u, 100u));
    assert(protocol_rx_downstream_activation_needs_wake(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x808), 100u));
    assert(protocol_rx_downstream_activation_mark(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x808),
        100u, 1000u));
    assert(!protocol_rx_downstream_activation_needs_wake(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x808), 200u));
    assert(protocol_rx_downstream_activation_needs_wake(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x809), 200u));
    assert(!protocol_rx_downstream_activation_mark(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x809),
        200u, 1000u));
    assert(!protocol_rx_downstream_activation_clear(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x807)));
    assert(protocol_rx_downstream_activation_clear(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x808)));
    assert(protocol_rx_downstream_activation_needs_wake(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x809), 200u));
    assert(protocol_rx_downstream_activation_mark(
        &activation, PROTOCOL_RX_OPERATION_ENUMERATION, UINT64_C(0x809),
        UINT32_MAX - 10u, 20u));
    assert(!protocol_rx_downstream_activation_expire(&activation,
                                                      UINT32_MAX));
    assert(protocol_rx_downstream_activation_expire(&activation, 20u));
}

int main(void)
{
    test_ddd_activation_rf_work_and_success();
    test_forced_hop_activation_is_local_and_wake_once();
    test_abort_failure_timeout_and_stale_terminal_return_low_duty();
    test_one_gateway_operation_owns_channel_five();
    test_terminal_failure_during_owned_rf_returns_low_duty();
    test_unexpected_receive_error_recovery_is_fail_closed();
    test_ambiguous_deadline_is_rejected();
    test_survey_plan_replaces_only_its_live_operation_deadline();
    test_forced_hop_downstream_wakes_once_per_generation();
    return 0;
}
