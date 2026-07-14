#include "app_wake_train_politeness.h"

#include <assert.h>
#include <errno.h>

static void test_rx_activity_matches_low_duty_preamble_failures(void)
{
    assert(app_wake_train_politeness_rx_activity(0,
                                                 DWM3000_RX_FAILURE_NONE));
    assert(app_wake_train_politeness_rx_activity(-EIO,
                                                 DWM3000_RX_FAILURE_SFD_TIMEOUT));
    assert(app_wake_train_politeness_rx_activity(-EIO,
                                                 DWM3000_RX_FAILURE_FRAME_TIMEOUT));
    assert(app_wake_train_politeness_rx_activity(-EIO,
                                                 DWM3000_RX_FAILURE_CRC_OR_PHY));
    assert(app_wake_train_politeness_rx_activity(-EMSGSIZE,
                                                 DWM3000_RX_FAILURE_BAD_FRAME));

    assert(!app_wake_train_politeness_rx_activity(-ETIMEDOUT,
                                                  DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT));
    assert(!app_wake_train_politeness_rx_activity(-EIO,
                                                  DWM3000_RX_FAILURE_NONE));
}

static void test_backoff_is_random_exponential_between_bounds(void)
{
    assert(app_wake_train_politeness_backoff_ms(0u, 0u) == 200u);
    assert(app_wake_train_politeness_backoff_ms(0u, 199u) == 399u);
    assert(app_wake_train_politeness_backoff_ms(0u, 200u) == 200u);

    assert(app_wake_train_politeness_backoff_ms(1u, 0u) == 400u);
    assert(app_wake_train_politeness_backoff_ms(1u, 399u) == 799u);
    assert(app_wake_train_politeness_backoff_ms(1u, 400u) == 400u);

    assert(app_wake_train_politeness_backoff_ms(2u, 0u) == 800u);
    assert(app_wake_train_politeness_backoff_ms(2u, 799u) == 1599u);
    assert(app_wake_train_politeness_backoff_ms(2u, 800u) == 800u);

    assert(app_wake_train_politeness_backoff_ms(3u, 0u) == 1600u);
    assert(app_wake_train_politeness_backoff_ms(3u, 400u) == 2000u);
    assert(app_wake_train_politeness_backoff_ms(4u, 0u) == 1600u);
    assert(app_wake_train_politeness_backoff_ms(4u, 400u) == 2000u);
    assert(app_wake_train_politeness_backoff_ms(9u, 401u) == 1600u);
}

static void test_production_opportunity_budget(void)
{
    assert(APP_WAKE_TRAIN_POLITE_SNIFF_MS == 20u);
    assert(APP_WAKE_TRAIN_POLITE_MAX_RETRIES == 3u);
    assert(APP_WAKE_TRAIN_POLITE_MAX_RETRIES + 1u ==
           MESH_RADIO_WAKE_OPPORTUNITIES);
}

int main(void)
{
    test_rx_activity_matches_low_duty_preamble_failures();
    test_backoff_is_random_exponential_between_bounds();
    test_production_opportunity_budget();
    return 0;
}
