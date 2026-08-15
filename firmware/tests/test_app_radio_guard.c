#include "app_radio_guard.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>

static int64_t test_uptime_ms;
static uint32_t watchdog_stop_calls;

int64_t k_uptime_get(void)
{
    return test_uptime_ms;
}

void app_watchdog_stop_feeding(void)
{
    watchdog_stop_calls++;
}

static void test_exact_lease_release_rejects_stale_owner(void)
{
    struct radio_guard_uwb_lease owner = {0};
    struct radio_guard_uwb_lease stale;
    struct radio_guard_uwb_lease next = {0};

    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,
                                 "adapter-rx",
                                 &owner) == 0);
    assert(owner.generation != 0u);
    assert(radio_guard_uwb_phase() == RADIO_GUARD_UWB_ACTIVE);
    stale = owner;

    stale.client = RADIO_GUARD_UWB_CLIENT_MESH_TX;
    assert(radio_guard_uwb_release_begin(&stale) == -ESTALE);
    stale = owner;
    assert(radio_guard_uwb_release_begin(&owner) == 0);
    assert(radio_guard_uwb_phase() == RADIO_GUARD_UWB_RELEASING);
    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                 "blocked-while-parking",
                                 &next) == -EBUSY);
    assert(radio_guard_uwb_release_finish(&owner, 0) == 0);
    assert(!radio_guard_uwb_busy());
    assert(owner.generation == 0u);

    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                 "adapter-tx",
                                 &next) == 0);
    assert(next.generation != stale.generation);
    assert(radio_guard_uwb_release_begin(&stale) == -ESTALE);
    assert(radio_guard_uwb_release_begin(&next) == 0);
    assert(radio_guard_uwb_release_finish(&next, 0) == 0);
}

static void test_compatibility_stop_cannot_clear_scoped_owner(void)
{
    struct radio_guard_uwb_lease owner = {0};

    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN,
                                 "scoped-scan",
                                 &owner) == 0);
    radio_guard_uwb_stop();
    assert(radio_guard_uwb_busy());
    assert(radio_guard_uwb_release_begin(&owner) == 0);
    assert(radio_guard_uwb_release_finish(&owner, 0) == 0);

    assert(radio_guard_uwb_start("compatibility-owner") == 0);
    assert(radio_guard_uwb_busy());
    radio_guard_uwb_stop();
    assert(!radio_guard_uwb_busy());
}

static void test_survey_admission_reopen_preserves_click_owner(void)
{
    struct radio_guard_uwb_lease click = {0};
    struct radio_guard_uwb_lease survey = {0};

    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK,
                                 "priority-click",
                                 &click) == 0);
    assert(radio_guard_uwb_owner_client() ==
           RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK);
    radio_guard_uwb_admission_pause();
    assert(radio_guard_uwb_admission_paused());
    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,
                                 "blocked-survey-prep",
                                 &survey) == -ESHUTDOWN);

    /* Physical survey preparation may reopen admission only after mesh
     * quiescence. An already-owned click remains the radio owner and the
     * survey must retry instead of stealing or aborting that lease. */
    radio_guard_uwb_admission_resume();
    assert(!radio_guard_uwb_admission_paused());
    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,
                                 "click-still-priority",
                                 &survey) == -EBUSY);
    assert(radio_guard_uwb_release_begin(&click) == 0);
    assert(radio_guard_uwb_release_finish(&click, 0) == 0);
    assert(radio_guard_uwb_owner_client() == RADIO_GUARD_UWB_CLIENT_NONE);

    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY,
                                 "survey-after-click",
                                 &survey) == 0);
    assert(radio_guard_uwb_owner_client() ==
           RADIO_GUARD_UWB_CLIENT_ANCHOR_SURVEY);
    assert(radio_guard_uwb_release_begin(&survey) == 0);
    assert(radio_guard_uwb_release_finish(&survey, 0) == 0);
}

static void test_failed_parking_poison_retains_owner_and_blocks_rearm(void)
{
    struct radio_guard_uwb_lease owner = {0};
    struct radio_guard_uwb_lease contender = {0};
    uint32_t owner_generation;

    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK,
                                 "adapter-click",
                                 &owner) == 0);
    owner_generation = owner.generation;
    assert(radio_guard_uwb_release_begin(&owner) == 0);
    assert(radio_guard_uwb_release_finish(&owner, -EIO) == -EIO);
    assert(radio_guard_uwb_phase() == RADIO_GUARD_UWB_POISONED);
    assert(radio_guard_uwb_busy());
    assert(radio_guard_uwb_poisoned());
    assert(!radio_guard_uwb_rearm_allowed());
    assert(radio_guard_uwb_poison_error() == -EIO);
    assert(owner.generation == owner_generation);
    assert(watchdog_stop_calls == 1u);
    assert(radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_MESH_RX,
                                 "must-not-rearm",
                                 &contender) == -EIO);
    assert(radio_guard_uwb_release_begin(&owner) == -EIO);
    assert(watchdog_stop_calls == 1u);
}

int main(void)
{
    test_uptime_ms = 1;
    test_exact_lease_release_rejects_stale_owner();
    test_compatibility_stop_cannot_clear_scoped_owner();
    test_survey_admission_reopen_preserves_click_owner();
    test_failed_parking_poison_retains_owner_and_blocks_rearm();
    return 0;
}
