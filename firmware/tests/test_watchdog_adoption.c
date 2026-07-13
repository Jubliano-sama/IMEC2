#include "watchdog_adoption.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct feed_capture {
    uint8_t requests[WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS];
    uint8_t count;
};

static void capture_feed(uint8_t reload_request, void *ctx)
{
    struct feed_capture *capture = ctx;

    assert(capture != NULL);
    assert(capture->count < WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS);
    capture->requests[capture->count++] = reload_request;
}

static void test_fresh_setup_uses_normal_driver_path(void)
{
    struct watchdog_adoption_plan plan;

    assert(watchdog_adoption_plan(false, 0u, 8u, &plan) == 0);
    assert(plan.mode == WATCHDOG_ADOPTION_FRESH);
    assert(plan.reload_request_mask == 0u);
    assert(plan.reload_request_count == 0u);
}

static void test_inherited_single_request_is_fed_immediately(void)
{
    struct watchdog_adoption_plan plan;
    struct feed_capture capture = {0};

    assert(watchdog_adoption_plan(true, UINT32_C(1) << 3u, 8u, &plan) == 0);
    assert(plan.mode == WATCHDOG_ADOPTION_INHERITED);
    assert(plan.reload_request_count == 1u);
    assert(watchdog_adoption_feed_mask(plan.reload_request_mask,
                                       8u,
                                       true,
                                       capture_feed,
                                       &capture) == 1u);
    assert(capture.count == 1u);
    assert(capture.requests[0] == 3u);
}

static void test_inherited_multiple_requests_are_all_fed(void)
{
    const uint32_t mask = (UINT32_C(1) << 0u) |
                          (UINT32_C(1) << 2u) |
                          (UINT32_C(1) << 7u);
    struct watchdog_adoption_plan plan;
    struct feed_capture capture = {0};

    assert(watchdog_adoption_plan(true, mask, 8u, &plan) == 0);
    assert(plan.reload_request_count == 3u);
    assert(watchdog_adoption_feed_mask(plan.reload_request_mask,
                                       8u,
                                       true,
                                       capture_feed,
                                       &capture) == 3u);
    assert(capture.count == 3u);
    assert(capture.requests[0] == 0u);
    assert(capture.requests[1] == 2u);
    assert(capture.requests[2] == 7u);
}

static void test_stale_or_stopped_policy_feeds_nothing(void)
{
    struct feed_capture capture = {0};
    const uint32_t mask = (UINT32_C(1) << 1u) |
                          (UINT32_C(1) << 6u);

    assert(watchdog_adoption_feed_mask(mask,
                                       8u,
                                       false,
                                       capture_feed,
                                       &capture) == 0u);
    assert(capture.count == 0u);
    assert(watchdog_adoption_feed_mask(mask,
                                       8u,
                                       true,
                                       capture_feed,
                                       &capture) == 2u);
    assert(capture.count == 2u);
}

static void test_invalid_inherited_config_fails_closed(void)
{
    struct watchdog_adoption_plan plan;

    assert(watchdog_adoption_plan(true, 0u, 8u, &plan) == -EIO);
    assert(plan.mode == WATCHDOG_ADOPTION_INVALID);
    assert(watchdog_adoption_plan(true, UINT32_C(1) << 8u, 8u, &plan) ==
           -ERANGE);
    assert(plan.mode == WATCHDOG_ADOPTION_INVALID);
    assert(watchdog_adoption_plan(true, 1u, 0u, &plan) == -EINVAL);
    assert(watchdog_adoption_plan(true, 1u, 9u, &plan) == -EINVAL);
    assert(watchdog_adoption_plan(true, 1u, 8u, NULL) == -EINVAL);
}

static void test_repeated_soft_reset_rebuilds_the_adoption_plan(void)
{
    const uint32_t mask = (UINT32_C(1) << 0u) |
                          (UINT32_C(1) << 5u);
    unsigned int boot;

    for (boot = 0u; boot < 64u; boot++) {
        struct watchdog_adoption_plan plan;
        struct feed_capture immediate = {0};

        assert(watchdog_adoption_plan(true, mask, 8u, &plan) == 0);
        assert(plan.mode == WATCHDOG_ADOPTION_INHERITED);
        assert(watchdog_adoption_feed_mask(plan.reload_request_mask,
                                           8u,
                                           true,
                                           capture_feed,
                                           &immediate) == 2u);
        assert(immediate.count == 2u);
    }
}

int main(void)
{
    test_fresh_setup_uses_normal_driver_path();
    test_inherited_single_request_is_fed_immediately();
    test_inherited_multiple_requests_are_all_fed();
    test_stale_or_stopped_policy_feeds_nothing();
    test_invalid_inherited_config_fails_closed();
    test_repeated_soft_reset_rebuilds_the_adoption_plan();
    puts("watchdog adoption tests passed");
    return 0;
}
