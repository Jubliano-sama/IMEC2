#include "dwm3000_timing.h"
#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "mesh_sim.h"
#include "uwb.h"
#include "uwb_session.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

#define NETWORK_ID UINT32_C(0x494d4543)
#define ROUTE_EPOCH UINT32_C(7)
#define CLICKER_ID UINT64_C(0x1111111111111111)
#define INTERFERER_ID UINT64_C(0x2222222222222222)
#define ANCHOR_ID UINT64_C(0xa300000000000001)
#define GATEWAY_ID UINT64_C(0x9000000000000001)

#define PRODUCTION_WAKE_TRAIN_US UINT64_C(400000)
#define ENUMERATION_ACTIVATION_WAKE_TRAIN_US \
    (UINT64_C(1000) * MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS)
#define CLAIMED_DURATION_MS 1200u
/* These are the production route-listener bounds. HIL captures measure the
 * actual reset/configuration time; the simulator consumes the configured
 * worst-case budget so a passing phase sweep cannot hide a retune gap. */
#define ROUTE_LISTENER_PHY_RETUNE_GUARD_US UINT64_C(30000)
#define ROUTE_LISTENER_CLICK_ACQUIRE_US UINT64_C(61000)
#define ROUTE_LISTENER_CLICK_PROBE_COMPLETION_US UINT64_C(15000)
#define ROUTE_LISTENER_CLICK_PROBE_US \
    (ROUTE_LISTENER_CLICK_ACQUIRE_US + \
     ROUTE_LISTENER_CLICK_PROBE_COMPLETION_US)

_Static_assert(MESH_RADIO_ANCHOR_SCAN_RX_US == 3500u,
               "wake scenarios require the production 3.5 ms anchor scan");
_Static_assert(MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS == 380u,
               "wake scenarios require the production 380 ms reschedule");
_Static_assert(PRODUCTION_WAKE_TRAIN_US / 1000u <=
                   UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS,
               "the production wake train must fit the UWB claim field");
_Static_assert(MESH_RADIO_ENUMERATION_WAKE_GAP_JITTER_MAX_US == 1000u,
               "enumeration activation requires the bounded 0..1000 us gap");
_Static_assert(ENUMERATION_ACTIVATION_WAKE_TRAIN_US / 1000u <=
                   UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS,
               "the enumeration activation train must fit the claim field");
_Static_assert(CLAIMED_DURATION_MS <= UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS,
               "the test claim must fit the UWB claim field");
_Static_assert(ROUTE_LISTENER_CLICK_PROBE_US == UINT64_C(76000),
               "route-listener standard probe budget drifted from production");
_Static_assert((2u * ROUTE_LISTENER_PHY_RETUNE_GUARD_US) +
                   ROUTE_LISTENER_CLICK_PROBE_US == UINT64_C(136000),
               "route-listener retune/probe sequence budget drifted from production");

struct fixture {
    struct mesh_sim_world *world;
    uint8_t clicker;
    uint8_t anchor;
    uint8_t interferer;
};

struct low_duty_timing {
    uint64_t first_start_us;
    uint64_t first_end_us;
    uint64_t second_start_us;
    uint64_t second_end_us;
};

struct train_schedule {
    uint64_t first_start_us;
    uint64_t last_end_us;
    uint32_t airtime_us;
    uint32_t max_jitter_us;
    uint16_t first_tx_index;
    size_t frame_count;
};

struct phase_case {
    const char *name;
    uint64_t offset_us;
};

static unsigned int failure_count;

static bool check_result(bool condition,
                         const char *scenario,
                         uint32_t seed,
                         int64_t phase_us,
                         const char *message)
{
    if (condition) {
        return true;
    }

    fprintf(stderr,
            "FAIL scenario=%s seed=0x%08" PRIx32 " phase_us=%" PRId64 ": %s\n",
            scenario,
            seed,
            phase_us,
            message);
    failure_count++;
    return false;
}

static struct uwb_clicker_config clicker_config(uint32_t seed, int64_t phase_us)
{
    uint32_t event_id = seed ^ (uint32_t)phase_us ^ UINT32_C(0x5a5a0000);

    if (event_id == 0u) {
        event_id = 1u;
    }
    return (struct uwb_clicker_config) {
        .network_id = NETWORK_ID,
        .clicker_id = CLICKER_ID,
        .click_event_id = event_id,
        .nonce = UINT64_C(0x1020304050607080) ^ seed ^ (uint64_t)phase_us,
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = FLAG_DIAGNOSTIC,
    };
}

static bool setup_fixture(struct fixture *fixture,
                          struct mesh_sim_world *world,
                          uint32_t seed,
                          int64_t phase_us,
                          bool add_interferer)
{
    struct uwb_clicker_config click_config = clicker_config(seed, phase_us);
    const struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = ANCHOR_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    const char *scenario = add_interferer ? "collision_setup" : "wake_setup";

    mesh_sim_init(world, seed);
    fixture->world = world;
    fixture->interferer = UINT8_MAX;
    if (!check_result(mesh_sim_add_role(world,
                                        MESH_SIM_ROLE_CLICKER,
                                        CLICKER_ID,
                                        GATEWAY_ID,
                                        ROUTE_EPOCH,
                                        &fixture->clicker) == MESH_SIM_OK,
                      scenario, seed, phase_us, "could not add clicker")) {
        return false;
    }
    if (!check_result(mesh_sim_add_role(world,
                                        MESH_SIM_ROLE_ANCHOR,
                                        ANCHOR_ID,
                                        GATEWAY_ID,
                                        ROUTE_EPOCH,
                                        &fixture->anchor) == MESH_SIM_OK,
                      scenario, seed, phase_us, "could not add anchor")) {
        return false;
    }
    if (!check_result(mesh_sim_set_link(world,
                                        fixture->clicker,
                                        fixture->anchor,
                                        100u,
                                        0u) == MESH_SIM_OK,
                      scenario, seed, phase_us, "could not link clicker to anchor")) {
        return false;
    }
    if (add_interferer) {
        if (!check_result(mesh_sim_add_role(world,
                                            MESH_SIM_ROLE_CLICKER,
                                            INTERFERER_ID,
                                            GATEWAY_ID,
                                            ROUTE_EPOCH,
                                            &fixture->interferer) == MESH_SIM_OK,
                          scenario, seed, phase_us, "could not add interferer") ||
            !check_result(mesh_sim_set_link(world,
                                            fixture->interferer,
                                            fixture->anchor,
                                            100u,
                                            0u) == MESH_SIM_OK,
                          scenario, seed, phase_us, "could not link interferer to anchor")) {
            return false;
        }
    }
    if (!check_result(mesh_sim_init_clicker_session(world,
                                                    fixture->clicker,
                                                    &click_config) == PROTO_OK,
                      scenario, seed, phase_us, "could not start clicker session") ||
        !check_result(mesh_sim_init_anchor_session(world,
                                                   fixture->anchor,
                                                   &anchor_config) == PROTO_OK,
                      scenario, seed, phase_us, "could not start anchor session")) {
        return false;
    }
    return true;
}

static bool encode_wake_claim(struct fixture *fixture,
                              uint16_t remaining_ms,
                              uint8_t *frame,
                              size_t *frame_len,
                              uint32_t seed,
                              int64_t phase_us,
                              const char *scenario)
{
    struct uwb_wake_claim_frame claim;
    struct uwb_clicker_session *session =
        &fixture->world->roles[fixture->clicker].clicker_session;
    int ret;

    ret = uwb_clicker_build_wake_claim(session,
                                       CLICKER_ID,
                                       remaining_ms,
                                       remaining_ms,
                                       CLAIMED_DURATION_MS,
                                       &claim);
    if (!check_result(ret == PROTO_OK,
                      scenario, seed, phase_us, "wake claim build failed") ||
        !check_result(claim.attempt_index == 1u,
                      scenario, seed, phase_us, "wake claim was not attempt index 1")) {
        return false;
    }
    ret = uwb_encode_wake_claim(&claim,
                                frame,
                                UWB_WAKE_CLAIM_LEN,
                                frame_len);
    return check_result(ret == PROTO_OK && *frame_len == UWB_WAKE_CLAIM_LEN,
                        scenario, seed, phase_us, "wake claim encoding failed");
}

static bool schedule_wake_train_with_gap(struct fixture *fixture,
                                         uint64_t start_us,
                                         uint64_t duration_us,
                                         uint32_t gap_jitter_max_us,
                                         const char *scenario,
                                         uint32_t seed,
                                         int64_t phase_us,
                                         struct train_schedule *schedule)
{
    struct mesh_sim_world *world = fixture->world;
    const uint64_t close_us = start_us + duration_us;
    uint64_t at_us = start_us;

    *schedule = (struct train_schedule) {
        .first_start_us = start_us,
        .first_tx_index = UINT16_MAX,
    };
    while (at_us < close_us) {
        uint8_t frame[UWB_WAKE_CLAIM_LEN];
        uint64_t remaining_us = close_us - at_us;
        uint16_t remaining_ms = (uint16_t)((remaining_us + 999u) / 1000u);
        uint16_t tx_index;
        uint32_t jitter_us;
        uint64_t timing_airtime_us;
        size_t frame_len = 0u;

        if (!encode_wake_claim(fixture,
                               remaining_ms,
                               frame,
                               &frame_len,
                               seed,
                               phase_us,
                               scenario)) {
            return false;
        }
        timing_airtime_us = dwm3000_timing_airtime_us_ceil(
            DWM3000_TIMING_PHY_CH5_WAKE,
            frame_len);
        if (!check_result(timing_airtime_us != 0u &&
                              timing_airtime_us ==
                                  mesh_sim_frame_duration_us(
                                      MESH_SIM_PHY_CHANNEL5_WAKE,
                                      frame_len),
                          scenario, seed, phase_us,
                          "simulator wake airtime differs from production timing API")) {
            return false;
        }
        if (!check_result(mesh_sim_schedule_raw_tx(world,
                                                   fixture->clicker,
                                                   at_us,
                                                   UWB_CHANNEL_WAKE_CONTACT,
                                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                                   frame,
                                                   frame_len,
                                                   false,
                                                   &tx_index) == MESH_SIM_OK,
                          scenario, seed, phase_us,
                          "could not schedule wake claim")) {
            return false;
        }
        if (schedule->frame_count == 0u) {
            schedule->first_tx_index = tx_index;
            schedule->airtime_us = (uint32_t)timing_airtime_us;
        }
        schedule->frame_count++;
        schedule->last_end_us = world->transmissions[tx_index].end_us;
        uwb_clicker_note_wake_claim_tx(
            &world->roles[fixture->clicker].clicker_session,
            1u);

        jitter_us = mesh_sim_random(world) % (gap_jitter_max_us + 1u);
        if (!check_result(jitter_us <= gap_jitter_max_us,
                          scenario, seed, phase_us,
                          "wake claim jitter exceeded the production bound")) {
            return false;
        }
        if (jitter_us > schedule->max_jitter_us) {
            schedule->max_jitter_us = jitter_us;
        }
        at_us = schedule->last_end_us + jitter_us;
    }
    return check_result(schedule->frame_count > 0u &&
                            schedule->last_end_us > schedule->first_start_us,
                        scenario, seed, phase_us,
                        "wake train contained no complete transmissions");
}

static bool schedule_wake_train(struct fixture *fixture,
                                uint64_t start_us,
                                uint32_t seed,
                                int64_t phase_us,
                                struct train_schedule *schedule)
{
    return schedule_wake_train_with_gap(
        fixture,
        start_us,
        PRODUCTION_WAKE_TRAIN_US,
        UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US,
        "attempt_1_train",
        seed,
        phase_us,
        schedule);
}

static bool schedule_enumeration_activation_train(
    struct fixture *fixture,
    uint64_t start_us,
    uint32_t seed,
    int64_t phase_us,
    struct train_schedule *schedule)
{
    return schedule_wake_train_with_gap(
        fixture,
        start_us,
        ENUMERATION_ACTIVATION_WAKE_TRAIN_US,
        MESH_RADIO_ENUMERATION_WAKE_GAP_JITTER_MAX_US,
        "enumeration_activation_train",
        seed,
        phase_us,
        schedule);
}

static bool measure_low_duty_timing(struct low_duty_timing *timing)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    const uint32_t seed = UINT32_C(0x13579bdf);

    if (!setup_fixture(&fixture, &world, seed, 0, false) ||
        !check_result(mesh_sim_start_anchor_low_duty(&world,
                                                    fixture.anchor,
                                                    0u) == MESH_SIM_OK,
                      "measure_low_duty", seed, 0,
                      "could not start low-duty scanner") ||
        !check_result(mesh_sim_run_until(&world,
                                         PRODUCTION_WAKE_TRAIN_US + 150000u) ==
                          MESH_SIM_OK,
                      "measure_low_duty", seed, 0,
                      "low-duty scanner did not run")) {
        return false;
    }
    if (!check_result(world.rx_window_count >= 2u,
                      "measure_low_duty", seed, 0,
                      "simulator did not expose two low-duty windows")) {
        return false;
    }
    timing->first_start_us = world.rx_windows[0].start_us;
    timing->first_end_us = world.rx_windows[0].end_us;
    timing->second_start_us = world.rx_windows[1].start_us;
    timing->second_end_us = world.rx_windows[1].end_us;
    return check_result(timing->first_end_us - timing->first_start_us ==
                            MESH_RADIO_ANCHOR_SCAN_RX_US &&
                            timing->second_end_us - timing->second_start_us ==
                                MESH_RADIO_ANCHOR_SCAN_RX_US &&
                            timing->second_start_us > timing->first_end_us,
                        "measure_low_duty", seed, 0,
                        "low-duty windows do not use the production 3.5 ms RX duration");
}

static size_t accepted_wake_receptions(const struct mesh_sim_world *world)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->source_id == CLICKER_ID &&
            reception->receiver_id == ANCHOR_ID &&
            reception->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
            reception->outcome == MESH_SIM_RX_DECODED &&
            reception->protocol_status == PROTO_OK) {
            count++;
        }
    }
    return count;
}

static void run_attempt_one_case(const struct low_duty_timing *timing,
                                 uint32_t seed,
                                 const struct phase_case *phase)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct train_schedule train;
    struct uwb_anchor_session *anchor_session;
    size_t accepted_receptions;
    uint64_t start_us = timing->first_start_us + phase->offset_us;
    int64_t phase_us = (int64_t)phase->offset_us;

    if (!setup_fixture(&fixture, &world, seed, phase_us, false) ||
        !check_result(mesh_sim_start_anchor_low_duty(&world,
                                                    fixture.anchor,
                                                    0u) == MESH_SIM_OK,
                      phase->name, seed, phase_us,
                      "could not start low-duty scanner") ||
        !schedule_wake_train(&fixture, start_us, seed, phase_us, &train) ||
        !check_result(mesh_sim_run_until(&world, train.last_end_us + 1u) == MESH_SIM_OK,
                      phase->name, seed, phase_us,
                      "wake train simulation failed")) {
        return;
    }

    anchor_session = &world.roles[fixture.anchor].anchor_session;
    accepted_receptions = accepted_wake_receptions(&world);
    check_result(anchor_session->diagnostics.claims >= 1u &&
                     anchor_session->diagnostics.claims == accepted_receptions &&
                     anchor_session->state == UWB_ANCHOR_CLAIMED &&
                     anchor_session->epoch.active &&
                     anchor_session->epoch.attempt_index == 1u &&
                     accepted_receptions >= 1u,
                 phase->name,
                 seed,
                 phase_us,
                 "valid 400 ms attempt-1 train did not produce an accepted claim");
}

static void test_attempt_one_wake_trains(const struct low_duty_timing *timing,
                                         uint32_t wake_airtime_us)
{
    static const uint32_t seeds[] = {
        UINT32_C(0x00000001),
        UINT32_C(0x10203040),
        UINT32_C(0x6d2b79f5),
        UINT32_C(0xa5a5a5a5),
        UINT32_C(0xdeadbeef),
        UINT32_C(0xffffffff),
    };
    struct phase_case phases[10];
    uint64_t cycle_us = timing->second_start_us - timing->first_start_us;
    uint64_t rx_us = timing->first_end_us - timing->first_start_us;
    uint64_t before_next_by_airtime = cycle_us > wake_airtime_us ?
                                      cycle_us - wake_airtime_us : 0u;
    unsigned int failures_before = failure_count;

    phases[0] = (struct phase_case) { "rx_open", 0u };
    phases[1] = (struct phase_case) { "rx_open_plus_1", 1u };
    phases[2] = (struct phase_case) { "rx_close_minus_1", rx_us - 1u };
    phases[3] = (struct phase_case) { "rx_close", rx_us };
    phases[4] = (struct phase_case) { "rx_close_plus_1", rx_us + 1u };
    phases[5] = (struct phase_case) {
        "off_gap_midpoint", rx_us + (cycle_us - rx_us) / 2u
    };
    phases[6] = (struct phase_case) {
        "next_open_minus_airtime", before_next_by_airtime
    };
    phases[7] = (struct phase_case) {
        "next_open_minus_airtime_plus_1", before_next_by_airtime + 1u
    };
    phases[8] = (struct phase_case) { "next_open_minus_1", cycle_us - 1u };
    phases[9] = (struct phase_case) { "next_open", cycle_us };

    for (size_t seed_index = 0u; seed_index < ARRAY_SIZE(seeds); seed_index++) {
        for (size_t phase_index = 0u; phase_index < ARRAY_SIZE(phases); phase_index++) {
            run_attempt_one_case(timing, seeds[seed_index], &phases[phase_index]);
        }
    }

    if (failure_count == failures_before) {
        printf("PASS attempt_1_train seeds=%zu phases=%zu cycle_us=%" PRIu64
               " airtime_us=%" PRIu32 "\n",
               ARRAY_SIZE(seeds),
               ARRAY_SIZE(phases),
               cycle_us,
               wake_airtime_us);
    }
}

static void run_enumeration_activation_case(const struct low_duty_timing *timing,
                                            uint32_t seed,
                                            uint64_t phase_us)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct train_schedule train;

    if (!setup_fixture(&fixture, &world, seed, (int64_t)phase_us, false) ||
        !check_result(mesh_sim_start_anchor_low_duty(&world,
                                                    fixture.anchor,
                                                    0u) == MESH_SIM_OK,
                      "enumeration_activation_boundary", seed,
                      (int64_t)phase_us,
                      "could not start low-duty scanner") ||
        !schedule_enumeration_activation_train(
            &fixture,
            timing->first_start_us + phase_us,
            seed,
            (int64_t)phase_us,
            &train) ||
        !check_result(mesh_sim_run_until(&world, train.last_end_us + 1u) ==
                          MESH_SIM_OK,
                      "enumeration_activation_boundary", seed,
                      (int64_t)phase_us,
                      "enumeration activation simulation failed")) {
        return;
    }

    check_result(accepted_wake_receptions(&world) >= 1u &&
                     world.roles[fixture.anchor].anchor_session.state ==
                         UWB_ANCHOR_CLAIMED,
                 "enumeration_activation_boundary", seed,
                 (int64_t)phase_us,
                 "low-duty scan phase did not acquire and decode a complete wake");
}

static void mark_phase_interval(bool *covered,
                                bool *boundaries,
                                uint64_t cycle_us,
                                int64_t lower,
                                int64_t upper)
{
    int64_t clipped_lower = lower < 0 ? 0 : lower;
    int64_t clipped_upper = upper >= (int64_t)cycle_us ?
                            (int64_t)cycle_us - 1 : upper;

    if (clipped_lower > clipped_upper) {
        return;
    }
    for (int64_t phase = clipped_lower; phase <= clipped_upper; phase++) {
        covered[phase] = true;
    }
    for (int delta = -1; delta <= 1; delta++) {
        int64_t lower_boundary = clipped_lower + delta;
        int64_t upper_boundary = clipped_upper + delta;

        if (lower_boundary >= 0 && lower_boundary < (int64_t)cycle_us) {
            boundaries[lower_boundary] = true;
        }
        if (upper_boundary >= 0 && upper_boundary < (int64_t)cycle_us) {
            boundaries[upper_boundary] = true;
        }
    }
}

static void test_enumeration_activation_covers_every_scan_phase(
    const struct low_duty_timing *timing)
{
    static const uint32_t seeds[] = {
        UINT32_C(0x00000001),
        UINT32_C(0x6d2b79f5),
        UINT32_C(0xffffffff),
    };
    uint64_t cycle_us = timing->second_start_us - timing->first_start_us;
    uint64_t rx_us = timing->first_end_us - timing->first_start_us;
    uint64_t preamble_us = dwm3000_timing_rctu_to_us_floor(
        dwm3000_timing_preamble_rctu(DWM3000_TIMING_PHY_CH5_WAKE));
    uint64_t pac_us = dwm3000_timing_rctu_to_us_ceil(
        dwm3000_timing_pac_rctu(DWM3000_TIMING_PHY_CH5_WAKE));
    unsigned int failures_before = failure_count;
    size_t simulated_boundaries = 0u;

    if (!check_result(cycle_us > rx_us && preamble_us > pac_us,
                      "enumeration_activation_phase_coverage", 0u, 0,
                      "invalid production scan or preamble timing")) {
        return;
    }

    for (size_t seed_index = 0u; seed_index < ARRAY_SIZE(seeds); seed_index++) {
        static struct mesh_sim_world reference_world;
        struct fixture fixture;
        struct train_schedule train;
        bool *covered = calloc((size_t)cycle_us, sizeof(*covered));
        bool *boundaries = calloc((size_t)cycle_us, sizeof(*boundaries));
        uint32_t seed = seeds[seed_index];
        uint32_t observed_max_gap_us = 0u;

        if (!check_result(covered != NULL && boundaries != NULL,
                          "enumeration_activation_phase_coverage", seed, 0,
                          "could not allocate exhaustive phase bitmap") ||
            !setup_fixture(&fixture, &reference_world, seed, 0, false) ||
            !schedule_enumeration_activation_train(
                &fixture, timing->first_start_us, seed, 0, &train)) {
            free(boundaries);
            free(covered);
            continue;
        }

        for (size_t tx_offset = 0u; tx_offset < train.frame_count; tx_offset++) {
            const struct mesh_sim_transmission *tx =
                &reference_world.transmissions[train.first_tx_index + tx_offset];
            uint64_t relative_start_us = tx->start_us - train.first_start_us;

            if (tx_offset > 0u) {
                const struct mesh_sim_transmission *previous =
                    &reference_world.transmissions[
                        train.first_tx_index + tx_offset - 1u];
                uint32_t gap_us = (uint32_t)(tx->start_us - previous->end_us);

                if (gap_us > observed_max_gap_us) {
                    observed_max_gap_us = gap_us;
                }
            }
            for (uint64_t scan_index = 0u; scan_index < 5u; scan_index++) {
                int64_t scan_start_relative =
                    (int64_t)(scan_index * cycle_us);
                int64_t lower = scan_start_relative -
                    (int64_t)relative_start_us -
                    (int64_t)(preamble_us - pac_us);
                int64_t upper = scan_start_relative + (int64_t)rx_us -
                    (int64_t)relative_start_us - (int64_t)pac_us;

                mark_phase_interval(covered, boundaries, cycle_us, lower, upper);
            }
        }

        for (uint64_t phase_us = 0u; phase_us < cycle_us; phase_us++) {
            if (!covered[phase_us]) {
                check_result(false,
                             "enumeration_activation_phase_coverage",
                             seed,
                             (int64_t)phase_us,
                             "enumeration train leaves an uncovered low-duty scan phase");
                break;
            }
        }
        check_result(observed_max_gap_us <=
                         MESH_RADIO_ENUMERATION_WAKE_GAP_JITTER_MAX_US,
                     "enumeration_activation_phase_coverage", seed, 0,
                     "enumeration train exceeded its production gap bound");

        for (uint64_t phase_us = 0u; phase_us < cycle_us; phase_us++) {
            if (boundaries[phase_us]) {
                run_enumeration_activation_case(timing, seed, phase_us);
                simulated_boundaries++;
            }
        }
        free(boundaries);
        free(covered);
    }

    if (failure_count == failures_before) {
        printf("PASS enumeration_activation_phase_coverage phases=%" PRIu64
               " boundary_simulations=%zu gap_max_us=%u\n",
               cycle_us,
               simulated_boundaries,
               MESH_RADIO_ENUMERATION_WAKE_GAP_JITTER_MAX_US);
    }
}

static void test_partial_frame_is_not_accepted(void)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct mesh_sim_rx_window *window;
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    uint16_t tx_index;
    uint64_t tx_start_us;
    const uint32_t seed = UINT32_C(0x31415926);
    const int64_t phase_us = -1;
    unsigned int failures_before = failure_count;

    if (!setup_fixture(&fixture, &world, seed, phase_us, false) ||
        !check_result(mesh_sim_start_anchor_low_duty(&world,
                                                    fixture.anchor,
                                                    0u) == MESH_SIM_OK,
                      "leading_partial", seed, phase_us,
                      "could not start low-duty scanner") ||
        !check_result(mesh_sim_run_until(&world, 0u) == MESH_SIM_OK &&
                          world.rx_window_count == 1u,
                      "leading_partial", seed, phase_us,
                      "could not expose first low-duty window") ||
        !encode_wake_claim(&fixture,
                           (uint16_t)(PRODUCTION_WAKE_TRAIN_US / 1000u),
                           frame,
                           &frame_len,
                           seed,
                           phase_us,
                           "leading_partial")) {
        return;
    }

    window = &world.rx_windows[0];
    tx_start_us = window->start_us - 1u;
    if (!check_result(mesh_sim_schedule_raw_tx(&world,
                                               fixture.clicker,
                                               tx_start_us,
                                               UWB_CHANNEL_WAKE_CONTACT,
                                               MESH_SIM_PHY_CHANNEL5_WAKE,
                                               frame,
                                               frame_len,
                                               false,
                                               &tx_index) == MESH_SIM_OK,
                      "leading_partial", seed, phase_us,
                      "could not schedule boundary frame") ||
        !check_result(world.transmissions[tx_index].start_rctu < window->start_rctu,
                      "leading_partial", seed, phase_us,
                      "boundary frame did not begin before RX window") ||
        !check_result(mesh_sim_run_until(&world,
                                         world.transmissions[tx_index].end_us + 1u) ==
                          MESH_SIM_OK,
                      "leading_partial", seed, phase_us,
                      "boundary frame simulation failed")) {
        return;
    }

    check_result(world.reception_count == 1u &&
                     (world.receptions[0].outcome == MESH_SIM_RX_PREAMBLE_ONLY ||
                      world.receptions[0].outcome == MESH_SIM_RX_SFD_TIMEOUT ||
                      world.receptions[0].outcome == MESH_SIM_RX_FRAME_TIMEOUT ||
                      world.receptions[0].outcome == MESH_SIM_RX_DECODE_ERROR) &&
                     world.roles[fixture.anchor].partial_frames == 1u &&
                     world.roles[fixture.anchor].anchor_session.diagnostics.claims == 0u &&
                     world.roles[fixture.anchor].anchor_session.state == UWB_ANCHOR_IDLE,
                 "leading_partial",
                 seed,
                 phase_us,
                 "frame not fully contained by RX window was accepted");
    if (failure_count == failures_before) {
        printf("PASS leading_partial outcome=%d\n", world.receptions[0].outcome);
    }
}

static void test_collision_is_not_accepted(void)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct mesh_sim_rx_window *window;
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    uint16_t first_tx;
    uint16_t second_tx;
    uint64_t tx_start_us;
    uint64_t run_end_us;
    const uint32_t seed = UINT32_C(0xc0111de5);
    const int64_t phase_us = 0;
    unsigned int failures_before = failure_count;

    if (!setup_fixture(&fixture, &world, seed, phase_us, true) ||
        !check_result(mesh_sim_start_anchor_low_duty(&world,
                                                    fixture.anchor,
                                                    0u) == MESH_SIM_OK,
                      "collision", seed, phase_us,
                      "could not start low-duty scanner") ||
        !check_result(mesh_sim_run_until(&world, 0u) == MESH_SIM_OK &&
                          world.rx_window_count == 1u,
                      "collision", seed, phase_us,
                      "could not expose first low-duty window") ||
        !encode_wake_claim(&fixture,
                           (uint16_t)(PRODUCTION_WAKE_TRAIN_US / 1000u),
                           frame,
                           &frame_len,
                           seed,
                           phase_us,
                           "collision")) {
        return;
    }

    window = &world.rx_windows[0];
    tx_start_us = window->start_us + 1u;
    if (!check_result(mesh_sim_schedule_raw_tx(&world,
                                               fixture.clicker,
                                               tx_start_us,
                                               UWB_CHANNEL_WAKE_CONTACT,
                                               MESH_SIM_PHY_CHANNEL5_WAKE,
                                               frame,
                                               frame_len,
                                               false,
                                               &first_tx) == MESH_SIM_OK,
                      "collision", seed, phase_us,
                      "could not schedule valid claim") ||
        !check_result(mesh_sim_schedule_raw_tx(&world,
                                               fixture.interferer,
                                               tx_start_us,
                                               UWB_CHANNEL_WAKE_CONTACT,
                                               MESH_SIM_PHY_CHANNEL5_WAKE,
                                               frame,
                                               frame_len,
                                               false,
                                               &second_tx) == MESH_SIM_OK,
                      "collision", seed, phase_us,
                      "could not schedule colliding frame")) {
        return;
    }
    run_end_us = world.transmissions[first_tx].end_us >
                         world.transmissions[second_tx].end_us ?
                     world.transmissions[first_tx].end_us :
                     world.transmissions[second_tx].end_us;
    if (!check_result(mesh_sim_run_until(&world, run_end_us + 1u) == MESH_SIM_OK,
                      "collision", seed, phase_us,
                      "collision simulation failed")) {
        return;
    }

    check_result(world.reception_count == 2u &&
                     world.receptions[0].outcome == MESH_SIM_RX_COLLISION &&
                     world.receptions[1].outcome == MESH_SIM_RX_COLLISION &&
                     world.roles[fixture.anchor].collision_frames == 2u &&
                     world.roles[fixture.anchor].anchor_session.diagnostics.claims == 0u &&
                     world.roles[fixture.anchor].anchor_session.state == UWB_ANCHOR_IDLE,
                 "collision",
                 seed,
                 phase_us,
                 "colliding wake traffic counted as an accepted claim");
    if (failure_count == failures_before) {
        printf("PASS collision receptions=%zu accepted=0\n", world.reception_count);
    }
}

static size_t count_receptions_for_phy(const struct mesh_sim_world *world,
                                       enum mesh_sim_phy phy,
                                       enum mesh_sim_rx_outcome outcome)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->phy == phy && reception->outcome == outcome) {
            count++;
        }
    }
    return count;
}

static bool reception_in_window(const struct mesh_sim_reception *reception,
                                uint64_t start_us,
                                uint64_t end_us)
{
    return reception->start_us >= start_us && reception->end_us <= end_us;
}

static size_t count_wake_receptions_in_window(
    const struct mesh_sim_world *world,
    uint64_t start_us,
    uint64_t end_us,
    enum mesh_sim_rx_outcome outcome)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->source_id == CLICKER_ID &&
            reception->receiver_id == ANCHOR_ID &&
            reception->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
            reception->outcome == outcome &&
            reception_in_window(reception, start_us, end_us)) {
            count++;
        }
    }
    return count;
}

static size_t count_route_request_receptions(
    const struct mesh_sim_world *world,
    uint64_t source_id)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];

        if (reception->source_id == source_id &&
            reception->receiver_id == ANCHOR_ID &&
            reception->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL &&
            reception->outcome == MESH_SIM_RX_DECODED &&
            reception->protocol_status == PROTO_OK &&
            reception->packet.msg_type == MSG_ROUTE_REQ) {
            count++;
        }
    }
    return count;
}

static bool build_real_route_request(struct mesh_outbound *out,
                                     uint64_t target_id,
                                     uint32_t now_ms,
                                     uint32_t seed,
                                     int64_t phase_us,
                                     const char *scenario)
{
    struct mesh_relay relay;
    int ret;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    INTERFERER_ID,
                    GATEWAY_ID,
                    ROUTE_EPOCH);
    ret = mesh_relay_build_route_request_with_timing_flags(&relay,
                                                           target_id,
                                                           NULL,
                                                           0u,
                                                           0u,
                                                           0u,
                                                           out,
                                                           now_ms);
    return check_result(ret == PROTO_OK &&
                            out->packet.msg_type == MSG_ROUTE_REQ &&
                            out->packet.payload_len == out->payload_len &&
                            out->payload_len > 0u,
                        scenario,
                        seed,
                        phase_us,
                        "could not build an encoded route-request control frame");
}

static bool schedule_route_listener_probe(struct fixture *fixture,
                                          uint64_t first_end_us,
                                          uint64_t standard_start_us,
                                          uint64_t standard_end_us,
                                          bool resumed_extended,
                                          uint64_t resumed_end_us,
                                          uint32_t seed,
                                          int64_t phase_us,
                                          const char *scenario)
{
    struct mesh_sim_world *world = fixture->world;

    if (!check_result(mesh_sim_schedule_rx(world,
                                           fixture->anchor,
                                           0u,
                                           first_end_us,
                                           UWB_CHANNEL_WAKE_CONTACT,
                                           MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                           NULL) == MESH_SIM_OK,
                      scenario,
                      seed,
                      phase_us,
                      "could not arm first extended route listener slice") ||
        !check_result(mesh_sim_schedule_rx(world,
                                           fixture->anchor,
                                           standard_start_us,
                                           standard_end_us,
                                           UWB_CHANNEL_WAKE_CONTACT,
                                           MESH_SIM_PHY_CHANNEL5_WAKE,
                                           NULL) == MESH_SIM_OK,
                      scenario,
                      seed,
                      phase_us,
                      "could not arm the bounded standard wake probe")) {
        return false;
    }
    if (resumed_extended &&
        !check_result(mesh_sim_schedule_rx(world,
                                           fixture->anchor,
                                           standard_end_us +
                                               ROUTE_LISTENER_PHY_RETUNE_GUARD_US,
                                           resumed_end_us,
                                           UWB_CHANNEL_WAKE_CONTACT,
                                           MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                           NULL) == MESH_SIM_OK,
                      scenario,
                      seed,
                      phase_us,
                      "could not arm resumed extended route listener slice")) {
        return false;
    }
    return true;
}

static void test_phy_compatibility_matrix(void)
{
    const uint32_t seed = UINT32_C(0x71000001);
    unsigned int failures_before = failure_count;

    check_result(mesh_sim_phy_acquisition_compatible(
                     MESH_SIM_PHY_CHANNEL5_WAKE,
                     MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) &&
                     mesh_sim_phy_acquisition_compatible(
                         MESH_SIM_PHY_CHANNEL5_WAKE,
                         MESH_SIM_PHY_CHANNEL5_RANGE) &&
                     !mesh_sim_phy_acquisition_compatible(
                         MESH_SIM_PHY_CHANNEL5_WAKE,
                         MESH_SIM_PHY_CHANNEL9_MESH) &&
                     mesh_sim_phy_decode_compatible(
                         MESH_SIM_PHY_CHANNEL5_WAKE,
                         MESH_SIM_PHY_CHANNEL5_RANGE) &&
                     !mesh_sim_phy_decode_compatible(
                         MESH_SIM_PHY_CHANNEL5_WAKE,
                         MESH_SIM_PHY_CHANNEL5_MESH_CONTROL),
                 "route_listener_phy_profiles",
                 seed,
                 0,
                 "simulator PHY acquisition/decode compatibility matrix is wrong");
    if (failure_count == failures_before) {
        printf("PASS route_listener_phy_profiles acquisition_and_phr_matrix\n");
    }
}

static void test_route_listener_mismatch_without_click(void)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct train_schedule train;
    const uint32_t seed = UINT32_C(0x71f00d42);
    const int64_t phase_us = 1000;
    unsigned int failures_before = failure_count;

    /* A standard wake preamble has the same CH5 acquisition shape as the
     * extended route listener, but its standard PHR must fail to decode. */
    if (!setup_fixture(&fixture, &world, seed, phase_us, false) ||
        !check_result(mesh_sim_schedule_rx(
                          &world,
                          fixture.anchor,
                          0u,
                          PRODUCTION_WAKE_TRAIN_US + 10000u,
                          UWB_CHANNEL_WAKE_CONTACT,
                          MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                          NULL) == MESH_SIM_OK,
                      "route_listener_phy_mismatch",
                      seed,
                      phase_us,
                      "could not arm extended route listener") ||
        !schedule_wake_train(&fixture,
                             1000u,
                             seed,
                             phase_us,
                             &train) ||
        !check_result(mesh_sim_run_until(&world, train.last_end_us + 1u) ==
                          MESH_SIM_OK,
                      "route_listener_phy_mismatch",
                      seed,
                      phase_us,
                      "extended route listener simulation failed")) {
        return;
    }

    check_result(count_receptions_for_phy(
                     &world,
                     MESH_SIM_PHY_CHANNEL5_WAKE,
                     MESH_SIM_RX_DECODE_ERROR) > 0u &&
                     accepted_wake_receptions(&world) == 0u &&
                     world.roles[fixture.anchor].anchor_session.diagnostics.claims == 0u,
                 "route_listener_phy_mismatch",
                 seed,
                 phase_us,
                 "standard wake activity was silently discarded or accepted by the extended listener");
    if (failure_count == failures_before) {
        printf("PASS route_listener_phy_mismatch decode_errors=%zu claims=0\n",
               count_receptions_for_phy(&world,
                                        MESH_SIM_PHY_CHANNEL5_WAKE,
                                        MESH_SIM_RX_DECODE_ERROR));
    }
}

static bool run_route_listener_click_case(uint32_t seed,
                                          int64_t phase_us,
                                          bool collide,
                                          bool expect_claim)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct train_schedule train;
    const uint64_t first_end_us = UINT64_C(100000);
    const uint64_t standard_start_us =
        first_end_us + ROUTE_LISTENER_PHY_RETUNE_GUARD_US;
    const uint64_t standard_end_us =
        standard_start_us + ROUTE_LISTENER_CLICK_PROBE_US;
    uint16_t collision_tx = UINT16_MAX;
    int ret;

    if (!setup_fixture(&fixture, &world, seed, phase_us, collide) ||
        !schedule_route_listener_probe(&fixture,
                                       first_end_us,
                                       standard_start_us,
                                       standard_end_us,
                                       false,
                                       0u,
                                       seed,
                                       phase_us,
                                       collide ? "route_listener_collision" :
                                                  "route_listener_phase") ||
        !schedule_wake_train(&fixture,
                             (uint64_t)phase_us,
                             seed,
                             phase_us,
                             &train)) {
        return false;
    }

    if (collide) {
        for (size_t i = 0u; i < world.transmission_count; i++) {
            const struct mesh_sim_transmission *tx = &world.transmissions[i];

            if (tx->valid && tx->node_index == fixture.clicker &&
                tx->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
                tx->start_us >= standard_start_us &&
                tx->end_us <= standard_end_us) {
                collision_tx = (uint16_t)i;
                break;
            }
        }
        if (!check_result(collision_tx != UINT16_MAX,
                          "route_listener_collision",
                          seed,
                          phase_us,
                          "collision phase did not place a complete wake frame in the standard probe")) {
            return false;
        }
        ret = mesh_sim_schedule_raw_tx(
            &world,
            fixture.interferer,
            world.transmissions[collision_tx].start_us,
            UWB_CHANNEL_WAKE_CONTACT,
            MESH_SIM_PHY_CHANNEL5_WAKE,
            world.transmissions[collision_tx].frame,
            world.transmissions[collision_tx].frame_len,
            false,
            NULL);
        if (!check_result(ret == MESH_SIM_OK,
                          "route_listener_collision",
                          seed,
                          phase_us,
                          "could not schedule colliding standard wake frame")) {
            return false;
        }
    }

    if (!check_result(mesh_sim_run_until(&world, train.last_end_us + 1u) ==
                          MESH_SIM_OK,
                      collide ? "route_listener_collision" :
                                "route_listener_phase",
                      seed,
                      phase_us,
                      "route listener phase simulation failed")) {
        return false;
    }

    {
        struct uwb_anchor_session *anchor_session =
            &world.roles[fixture.anchor].anchor_session;
        struct mesh_runtime *runtime = &world.roles[fixture.anchor].runtime;
        size_t accepted = accepted_wake_receptions(&world);
        bool owned = anchor_session->diagnostics.claims > 0u &&
                     anchor_session->state == UWB_ANCHOR_CLAIMED &&
                     anchor_session->epoch.active &&
                     runtime->radio_owner == MESH_RUNTIME_RADIO_DS_TWR &&
                     mesh_sim_count_transitions(
                         &world,
                         MESH_SIM_TRANSITION_WAKE_CLAIM_OWNED,
                         ANCHOR_ID) > 0u;
        bool probe_claim = count_wake_receptions_in_window(
                               &world,
                               standard_start_us,
                               standard_end_us,
                               MESH_SIM_RX_DECODED) > 0u;

        if (!check_result(owned == expect_claim,
                          collide ? "route_listener_collision" :
                                    "route_listener_phase",
                          seed,
                          phase_us,
                          expect_claim ?
                              "complete standard wake frame did not transfer claim custody into DS-TWR" :
                              "wake train outside the bounded probe unexpectedly acquired a claim") ||
            !check_result(!expect_claim || (accepted > 0u && probe_claim),
                          collide ? "route_listener_collision" :
                                    "route_listener_phase",
                          seed,
                          phase_us,
                          "accepted claim was not a fully contained frame in the standard probe")) {
            return false;
        }
        if (collide &&
            !check_result(world.roles[fixture.anchor].collision_frames >= 2u &&
                              accepted > 0u,
                          "route_listener_collision",
                          seed,
                          phase_us,
                          "later repeated wake claims did not recover after the probe collision")) {
            return false;
        }
    }
    return true;
}

static void test_route_listener_phase_sweep(void)
{
    const uint32_t seed = UINT32_C(0x72000001);
    const uint64_t wake_airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE,
        UWB_WAKE_CLAIM_LEN);
    const uint64_t standard_start_us = UINT64_C(100000) +
                                       ROUTE_LISTENER_PHY_RETUNE_GUARD_US;
    const uint64_t standard_end_us = standard_start_us +
                                     ROUTE_LISTENER_CLICK_PROBE_US;
    const struct phase_case phases[] = {
        { "train_before_listener", 0u },
        { "first_frame_at_extended_close", 100000u },
        { "first_frame_in_retune_gap", 129999u },
        { "first_frame_at_probe_open", 130000u },
        { "probe_deadline_minus_one_frame", standard_end_us - wake_airtime_us - 1u },
        { "probe_deadline_exact", standard_end_us - wake_airtime_us },
        { "probe_deadline_tail_only", standard_end_us - wake_airtime_us + 1u },
        { "after_probe_deadline", standard_end_us + 1u },
    };
    const bool expected_claim[] = {
        true, true, true, true, true, true, false, false,
    };
    unsigned int failures_before = failure_count;

    for (size_t i = 0u; i < ARRAY_SIZE(phases); i++) {
        if (phases[i].offset_us > (uint64_t)INT64_MAX ||
            !run_route_listener_click_case(seed + (uint32_t)i,
                                           (int64_t)phases[i].offset_us,
                                           false,
                                           expected_claim[i])) {
            return;
        }
    }
    if (failure_count == failures_before) {
        printf("PASS route_listener_phase_sweep phases=%zu probe=[%" PRIu64
               ",%" PRIu64 "] budget_us=%" PRIu64 "\n",
               ARRAY_SIZE(phases),
               standard_start_us,
               standard_end_us,
               ROUTE_LISTENER_CLICK_PROBE_US);
    }
}

static void test_route_listener_multi_node_collision(void)
{
    static const int64_t phases[] = { 0, 75000, 129999 };
    const uint32_t seed = UINT32_C(0x73000001);
    unsigned int failures_before = failure_count;

    for (size_t i = 0u; i < ARRAY_SIZE(phases); i++) {
        if (!run_route_listener_click_case(seed + (uint32_t)i,
                                           phases[i],
                                           true,
                                           true)) {
            return;
        }
    }
    if (failure_count == failures_before) {
        printf("PASS route_listener_multi_node_collision phases=%zu recovery=1\n",
               ARRAY_SIZE(phases));
    }
}

static void test_route_listener_tail_activity_is_bounded(void)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    uint16_t tx_index;
    const uint64_t ext_end_us = UINT64_C(100000);
    const uint64_t standard_start_us =
        ext_end_us + ROUTE_LISTENER_PHY_RETUNE_GUARD_US;
    const uint64_t standard_end_us =
        standard_start_us + ROUTE_LISTENER_CLICK_PROBE_US;
    const uint32_t seed = UINT32_C(0x74000001);
    const int64_t phase_us = (int64_t)(ext_end_us -
                                      dwm3000_timing_airtime_us_ceil(
                                          DWM3000_TIMING_PHY_CH5_WAKE,
                                          UWB_WAKE_CLAIM_LEN) +
                                      1u);
    unsigned int failures_before = failure_count;

    if (!setup_fixture(&fixture, &world, seed, phase_us, false) ||
        !schedule_route_listener_probe(&fixture,
                                       ext_end_us,
                                       standard_start_us,
                                       standard_end_us,
                                       false,
                                       0u,
                                       seed,
                                       phase_us,
                                       "route_listener_tail") ||
        !encode_wake_claim(&fixture,
                           (uint16_t)(PRODUCTION_WAKE_TRAIN_US / 1000u),
                           frame,
                           &frame_len,
                           seed,
                           phase_us,
                           "route_listener_tail") ||
        !check_result(mesh_sim_schedule_raw_tx(&world,
                                               fixture.clicker,
                                               (uint64_t)phase_us,
                                               UWB_CHANNEL_WAKE_CONTACT,
                                               MESH_SIM_PHY_CHANNEL5_WAKE,
                                               frame,
                                               frame_len,
                                               false,
                                               &tx_index) == MESH_SIM_OK,
                      "route_listener_tail",
                      seed,
                      phase_us,
                      "could not schedule final extended-listener wake frame") ||
        !check_result(mesh_sim_run_until(&world,
                                         world.transmissions[tx_index].end_us + 1u) ==
                          MESH_SIM_OK,
                      "route_listener_tail",
                      seed,
                      phase_us,
                      "tail-overlap simulation failed")) {
        return;
    }

    check_result(accepted_wake_receptions(&world) == 0u &&
                     world.roles[fixture.anchor].anchor_session.diagnostics.claims == 0u &&
                     world.roles[fixture.anchor].runtime.radio_owner ==
                         MESH_RUNTIME_RADIO_NONE &&
                     world.reception_count == 1u &&
                     world.receptions[0].outcome != MESH_SIM_RX_DECODED,
                 "route_listener_tail",
                 seed,
                 phase_us,
                 "a final wake frame overlapping the extended-listener tail was falsely handed off");
    if (failure_count == failures_before) {
        printf("PASS route_listener_tail final_frame_without_later_claim=1\n");
    }
}

static void test_route_listener_malformed_activity_and_real_control(void)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct mesh_outbound route_request;
    uint8_t malformed[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    uint16_t malformed_tx = UINT16_MAX;
    uint16_t route_tx = UINT16_MAX;
    const uint64_t probe_start_us = UINT64_C(130000);
    const uint64_t probe_end_us = probe_start_us + ROUTE_LISTENER_CLICK_PROBE_US;
    const uint32_t seed = UINT32_C(0x75000001);
    const int64_t phase_us = 150000;
    unsigned int failures_before = failure_count;

    if (!setup_fixture(&fixture, &world, seed, phase_us, true) ||
        !schedule_route_listener_probe(&fixture,
                                       UINT64_C(100000),
                                       probe_start_us,
                                       probe_end_us,
                                       true,
                                       UINT64_C(600000),
                                       seed,
                                       phase_us,
                                       "route_listener_malformed") ||
        !encode_wake_claim(&fixture,
                           (uint16_t)(PRODUCTION_WAKE_TRAIN_US / 1000u),
                           malformed,
                           &frame_len,
                           seed,
                           phase_us,
                           "route_listener_malformed")) {
        return;
    }
    malformed[0] ^= 0x80u;
    if (!check_result(mesh_sim_schedule_raw_tx(&world,
                                               fixture.clicker,
                                               probe_start_us + 1000u,
                                               UWB_CHANNEL_WAKE_CONTACT,
                                               MESH_SIM_PHY_CHANNEL5_WAKE,
                                               malformed,
                                               frame_len,
                                               false,
                                               &malformed_tx) == MESH_SIM_OK,
                      "route_listener_malformed",
                      seed,
                      phase_us,
                      "could not schedule malformed standard wake activity") ||
        !build_real_route_request(&route_request,
                                  ANCHOR_ID,
                                  500u,
                                  seed,
                                  phase_us,
                                  "route_listener_malformed") ||
        !check_result(mesh_sim_schedule_packet_tx(&world,
                                                  fixture.interferer,
                                                  UINT64_C(500000),
                                                  UWB_CHANNEL_WAKE_CONTACT,
                                                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                                  &route_request.packet,
                                                  route_request.payload,
                                                  route_request.payload_len,
                                                  &route_tx) == MESH_SIM_OK,
                      "route_listener_malformed",
                      seed,
                      phase_us,
                      "could not schedule real encoded route-request activity") ||
        !check_result(mesh_sim_run_until(&world, UINT64_C(600000) + 1u) ==
                          MESH_SIM_OK,
                      "route_listener_malformed",
                      seed,
                      phase_us,
                      "malformed/control listener simulation failed")) {
        return;
    }

    check_result(count_receptions_for_phy(&world,
                                          MESH_SIM_PHY_CHANNEL5_WAKE,
                                          MESH_SIM_RX_DECODE_ERROR) >= 1u &&
                     count_route_request_receptions(&world, INTERFERER_ID) == 1u &&
                     mesh_sim_count_transitions(
                         &world,
                         MESH_SIM_TRANSITION_PACKET_QUEUED,
                         ANCHOR_ID) > 0u &&
                     world.roles[fixture.anchor].anchor_session.diagnostics.claims == 0u,
                 "route_listener_malformed",
                 seed,
                 phase_us,
                 "malformed standard activity caused a false claim or hid a valid encoded route request");
    if (failure_count == failures_before) {
        printf("PASS route_listener_malformed malformed_rejected=1 route_request_decoded=1\n");
    }
}

static uint64_t bounded_ds_twr_exchange_us(void)
{
    uint64_t wire_time_us =
        dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_RANGE,
                                       UWB_CLICK_POLL_LEN) +
        dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_RANGE,
                                       UWB_RESP_LEN) +
        dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_RANGE,
                                       UWB_FINAL_LEN) +
        (2u * (uint64_t)UWB_DS_TWR_REPLY_DELAY_US);

    return wire_time_us > UWB_RANGE_SCHEDULE_SINGLE_ANCHOR_MIN_EXCHANGE_STRIDE_US ?
           wire_time_us : UWB_RANGE_SCHEDULE_SINGLE_ANCHOR_MIN_EXCHANGE_STRIDE_US;
}

static void test_claim_holds_channel5_through_ds_twr(
    const struct low_duty_timing *timing)
{
    static struct mesh_sim_world world;
    struct fixture fixture;
    struct train_schedule train;
    struct mesh_runtime *runtime;
    uint64_t exchange_end_us;
    const uint32_t seed = UINT32_C(0x5eedc5a5);
    const int64_t phase_us = 0;

    if (!setup_fixture(&fixture, &world, seed, phase_us, false) ||
        !check_result(mesh_sim_start_anchor_low_duty(&world,
                                                    fixture.anchor,
                                                    0u) == MESH_SIM_OK,
                      "claim_channel5_ownership", seed, phase_us,
                      "could not start low-duty scanner") ||
        !schedule_wake_train(&fixture,
                             timing->first_start_us,
                             seed,
                             phase_us,
                             &train) ||
        !check_result(mesh_sim_run_until(
                          &world,
                          world.transmissions[train.first_tx_index].end_us + 1u) ==
                          MESH_SIM_OK,
                      "claim_channel5_ownership", seed, phase_us,
                      "could not run through first accepted claim") ||
        !check_result(world.roles[fixture.anchor].anchor_session.diagnostics.claims == 1u,
                      "claim_channel5_ownership", seed, phase_us,
                      "ownership check did not begin from an accepted claim")) {
        return;
    }

    runtime = &world.roles[fixture.anchor].runtime;
    exchange_end_us = train.first_start_us + PRODUCTION_WAKE_TRAIN_US +
                      bounded_ds_twr_exchange_us();

    check_result(runtime->radio_owner == MESH_RUNTIME_RADIO_DS_TWR &&
                     runtime->radio_busy_until_us >= exchange_end_us,
                 "claim_channel5_ownership",
                 seed,
                 phase_us,
                 "missing mesh_sim accepted-claim -> continuous channel-5 DS-TWR ownership seam");
}

int main(void)
{
    struct low_duty_timing timing;
    uint64_t wake_airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE,
        UWB_WAKE_CLAIM_LEN);

    check_result(uwb_clicker_wake_claim_jitter_us(0u) == 0u &&
                     uwb_clicker_wake_claim_jitter_us(
                         UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US) ==
                         UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US &&
                     wake_airtime_us > 0u && wake_airtime_us <= UINT32_MAX,
                 "production_timing_apis",
                 0u,
                 0,
                 "production jitter or wake airtime API is inconsistent");

    if (measure_low_duty_timing(&timing)) {
        test_attempt_one_wake_trains(&timing, (uint32_t)wake_airtime_us);
        test_enumeration_activation_covers_every_scan_phase(&timing);
        test_partial_frame_is_not_accepted();
        test_collision_is_not_accepted();
        test_phy_compatibility_matrix();
        test_route_listener_mismatch_without_click();
        test_route_listener_phase_sweep();
        test_route_listener_multi_node_collision();
        test_route_listener_tail_activity_is_bounded();
        test_route_listener_malformed_activity_and_real_control();
        test_claim_holds_channel5_through_ds_twr(&timing);
    }

    if (failure_count != 0u) {
        fprintf(stderr, "RESULT mesh_wake_scenarios failures=%u\n", failure_count);
        return EXIT_FAILURE;
    }
    printf("PASS mesh_wake_scenarios\n");
    return EXIT_SUCCESS;
}
