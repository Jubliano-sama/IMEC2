#include "watchdog_adoption.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MODEL_TIMEOUT_MS 180000u

struct watchdog_model {
    bool running;
    uint32_t enabled_mask;
    uint32_t pending_mask;
    uint64_t now_ms;
    uint64_t deadline_ms;
    uint32_t reloads;
    uint32_t resets;
    uint32_t request_writes[WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS];
};

static void model_feed_request(uint8_t reload_request, void *ctx)
{
    struct watchdog_model *model = ctx;
    uint32_t bit = UINT32_C(1) << reload_request;

    assert(model != NULL);
    assert(model->running);
    assert((model->enabled_mask & bit) != 0u);
    model->request_writes[reload_request]++;
    model->pending_mask &= ~bit;
    if (model->pending_mask == 0u) {
        model->reloads++;
        model->deadline_ms = model->now_ms + MODEL_TIMEOUT_MS;
        model->pending_mask = model->enabled_mask;
    }
}

static void model_fresh_setup(struct watchdog_model *model, uint32_t mask)
{
    model->running = true;
    model->enabled_mask = mask;
    model->pending_mask = mask;
    model->deadline_ms = model->now_ms + MODEL_TIMEOUT_MS;
}

static void model_advance(struct watchdog_model *model, uint32_t elapsed_ms)
{
    model->now_ms += elapsed_ms;
    if (model->running && model->now_ms >= model->deadline_ms) {
        model->resets++;
        model->running = false;
    }
}

static int model_boot(struct watchdog_model *model, bool healthy)
{
    struct watchdog_adoption_plan plan;
    int ret = watchdog_adoption_plan(model->running,
                                     model->enabled_mask,
                                     WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS,
                                     &plan);

    if (ret < 0) {
        return ret;
    }
    if (plan.mode == WATCHDOG_ADOPTION_FRESH) {
        return 1;
    }
    return (int)watchdog_adoption_feed_mask(
        plan.reload_request_mask,
        WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS,
        healthy,
        model_feed_request,
        model);
}

static void test_soft_reset_near_expiry_is_adopted_immediately(void)
{
    struct watchdog_model model = {0};

    assert(model_boot(&model, true) == 1);
    model_fresh_setup(&model, UINT32_C(1));
    model_advance(&model, MODEL_TIMEOUT_MS - 1u);
    assert(model.resets == 0u);

    /* A CPU soft reset does not reset the nRF watchdog peripheral. */
    assert(model_boot(&model, true) == 1);
    assert(model.reloads == 1u);
    assert(model.deadline_ms == model.now_ms + MODEL_TIMEOUT_MS);
    model_advance(&model, MODEL_TIMEOUT_MS - 1u);
    assert(model.resets == 0u);
}

static void test_every_inherited_request_is_required_each_cycle(void)
{
    const uint32_t mask = (UINT32_C(1) << 0u) |
                          (UINT32_C(1) << 3u) |
                          (UINT32_C(1) << 7u);
    struct watchdog_model model = {0};

    model_fresh_setup(&model, mask);
    model_advance(&model, MODEL_TIMEOUT_MS - 10u);
    assert(model_boot(&model, true) == 3);
    assert(model.reloads == 1u);
    assert(model.request_writes[0] == 1u);
    assert(model.request_writes[3] == 1u);
    assert(model.request_writes[7] == 1u);
}

static void test_stale_health_stops_all_inherited_feeds(void)
{
    struct watchdog_model model = {0};

    model_fresh_setup(&model, UINT32_C(1) | (UINT32_C(1) << 6u));
    assert(model_boot(&model, false) == 0);
    assert(model.reloads == 0u);
    assert(model.request_writes[0] == 0u);
    assert(model.request_writes[6] == 0u);
    model_advance(&model, MODEL_TIMEOUT_MS);
    assert(model.resets == 1u);
}

static void test_repeated_candidate_boots_do_not_dog_reset(void)
{
    struct watchdog_model model = {0};
    unsigned int boot;

    model_fresh_setup(&model, UINT32_C(1) | (UINT32_C(1) << 4u));
    for (boot = 0u; boot < 50u; boot++) {
        model_advance(&model, MODEL_TIMEOUT_MS - 100u);
        assert(model.resets == 0u);
        assert(model_boot(&model, true) == 2);
    }
    assert(model.reloads == 50u);
    assert(model.resets == 0u);
}

static void test_running_without_reload_requests_fails_closed(void)
{
    struct watchdog_model model = {
        .running = true,
        .deadline_ms = MODEL_TIMEOUT_MS,
    };

    assert(model_boot(&model, true) == -EIO);
    assert(model.reloads == 0u);
}

int main(void)
{
    test_soft_reset_near_expiry_is_adopted_immediately();
    test_every_inherited_request_is_required_each_cycle();
    test_stale_health_stops_all_inherited_feeds();
    test_repeated_candidate_boots_do_not_dog_reset();
    test_running_without_reload_requests_fails_closed();
    puts("watchdog adoption hardware-model scenarios passed");
    return 0;
}
