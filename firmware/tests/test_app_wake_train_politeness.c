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

static void test_deadline_requires_complete_opportunity_tail(void)
{
    const int64_t deadline_ms = 1000;
    const uint32_t full_opportunity_ms =
        (2u * APP_WAKE_TRAIN_POLITE_SNIFF_MS) + 400u + 20u;
    uint32_t delay_ms = UINT32_MAX;

    /* The old wake-only admission accepted deadline-401 here. */
    assert(!app_wake_train_deadline_fits(599,
                                         deadline_ms,
                                         full_opportunity_ms));

    /*
     * One opportunity fits from deadline-500. Forced post-sniff activity
     * leaves only 40 ms, so even a clipped minimum backoff cannot admit a
     * second complete RF opportunity and no schedule can follow it.
     */
    assert(app_wake_train_deadline_fits(500,
                                        deadline_ms,
                                        full_opportunity_ms));
    assert(!app_wake_train_deadline_clip_delay(
        960,
        deadline_ms,
        APP_WAKE_TRAIN_POLITE_BACKOFF_MIN_MS,
        full_opportunity_ms,
        &delay_ms));
    assert(delay_ms == 0u);
}

static void test_deadline_clips_backoff_but_preserves_required_tail(void)
{
    uint32_t delay_ms = 0u;

    assert(app_wake_train_deadline_clip_delay(100,
                                               1000,
                                               400u,
                                               700u,
                                               &delay_ms));
    assert(delay_ms == 200u);
    assert(app_wake_train_deadline_clip_delay(100,
                                               INT64_MAX,
                                               400u,
                                               700u,
                                               &delay_ms));
    assert(delay_ms == 400u);
}

int main(void)
{
    test_rx_activity_matches_low_duty_preamble_failures();
    test_backoff_is_random_exponential_between_bounds();
    test_production_opportunity_budget();
    test_deadline_requires_complete_opportunity_tail();
    test_deadline_clips_backoff_but_preserves_required_tail();
    return 0;
}
