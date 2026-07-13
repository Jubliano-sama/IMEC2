#include "mesh_sim.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_gateway_command_priority.h"
#include "protocol.h"
#include "survey.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GATEWAY_ID UINT64_C(0xa001000000000001)
#define ANCHOR_ID UINT64_C(0xa002000000000001)
#define ANCHOR_2_ID UINT64_C(0xa002000000000002)
#define SURVEY_ID UINT32_C(0x50665006)
#define ROUTE_EPOCH UINT32_C(7)
#define TX_START_US UINT64_C(10000)

static unsigned int failures;

enum gateway_priority_event {
    GATEWAY_PRIORITY_ABORT_RX = 1,
    GATEWAY_PRIORITY_SCHEDULE_SURVEY,
};

struct gateway_priority_fixture {
    enum gateway_priority_event events[2];
    size_t event_count;
    void *scheduled_work;
    bool ch9_rx_active;
    bool abort_requested;
};

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            failures++;                                                       \
        }                                                                      \
    } while (0)

static void survey_request_ch9_abort(void *ctx)
{
    struct gateway_priority_fixture *fixture = ctx;

    CHECK(fixture->ch9_rx_active,
          "survey command did not preempt an active channel-9 receive");
    fixture->events[fixture->event_count++] = GATEWAY_PRIORITY_ABORT_RX;
    fixture->abort_requested = true;
}

static int survey_schedule_after_safe_boundary(void *ctx, void *work)
{
    struct gateway_priority_fixture *fixture = ctx;

    CHECK(fixture->abort_requested,
          "survey command scheduled without a receive-abort request");
    CHECK(!fixture->ch9_rx_active,
          "survey command scheduled before channel-9 released the radio");
    fixture->events[fixture->event_count++] =
        GATEWAY_PRIORITY_SCHEDULE_SURVEY;
    fixture->scheduled_work = work;
    return 0;
}

static void survey_clear_ch9_abort(void *ctx)
{
    struct gateway_priority_fixture *fixture = ctx;

    fixture->abort_requested = false;
}

static void preempt_ch9_for_survey(void *survey_work)
{
    struct gateway_priority_fixture fixture = {
        .ch9_rx_active = true,
    };
    const struct app_mesh_gateway_command_priority_ops ops = {
        .gateway_role = true,
        .request_receive_abort = survey_request_ch9_abort,
        .reschedule_now = survey_schedule_after_safe_boundary,
        .clear_receive_abort = survey_clear_ch9_abort,
        .ctx = &fixture,
    };
    struct app_mesh_gateway_command_priority priority = {0};

    CHECK(app_mesh_gateway_command_priority_request(
              &priority, &ops, survey_work) == 0,
          "three-sample survey priority request failed");
    CHECK(fixture.event_count == 1u &&
              fixture.events[0] == GATEWAY_PRIORITY_ABORT_RX &&
              fixture.scheduled_work == NULL,
          "survey work ran before channel-9 reached a safe boundary");
    CHECK(app_mesh_gateway_command_priority_waiting_for_safe_boundary(&priority),
          "survey priority state did not wait for channel-9 completion");

    /* Models dwm3000_driver_receive_frame_continuous() returning -ECANCELED. */
    fixture.ch9_rx_active = false;
    CHECK(app_mesh_gateway_command_priority_acknowledge_safe_boundary(
              &priority, &ops) == 0,
          "three-sample survey did not resume at the channel-9 safe boundary");
    CHECK(fixture.event_count == 2u &&
              fixture.events[1] == GATEWAY_PRIORITY_SCHEDULE_SURVEY &&
              fixture.scheduled_work == survey_work,
          "survey safe-boundary handoff order drifted");
}

static enum mesh_sim_phy survey_start_tx_phy(size_t frame_len)
{
    return app_mesh_c5_control_uses_extended_phr(
               MSG_SURVEY_DISCOVERY_START, frame_len, 125u) ?
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL : MESH_SIM_PHY_CHANNEL5_WAKE;
}

static enum mesh_sim_phy survey_start_rx_phy(void)
{
    return app_mesh_c5_wake_followup_uses_extended_phr(
               FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
               FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY) ?
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL : MESH_SIM_PHY_CHANNEL5_WAKE;
}

static void run_survey_start_phy_case(bool mutate_tx_to_standard_wake,
                                      uint32_t rx_end_trim_us,
                                      bool expect_decode,
                                      bool expect_reception)
{
    static const struct survey_discovery_config config = {
        .survey_id = SURVEY_ID,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    static struct mesh_sim_world world;
    struct proto_packet packet;
    uint8_t payload[64] = {0};
    uint8_t frame[PACKET_EXT_MAX_LEN] = {0};
    size_t payload_len = 0u;
    size_t frame_len = 0u;
    uint8_t gateway = UINT8_MAX;
    uint8_t anchor = UINT8_MAX;
    uint16_t window = UINT16_MAX;
    uint16_t transmission = UINT16_MAX;
    uint32_t airtime_us;
    uint64_t rx_end_us;
    enum mesh_sim_phy tx_phy;
    enum mesh_sim_phy rx_phy;
    int ret;

    mesh_sim_init(&world, UINT32_C(0x54c50001));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &anchor) == MESH_SIM_OK,
          "anchor setup failed");
    CHECK(mesh_sim_set_link(&world, gateway, anchor, 100u, 0u) == MESH_SIM_OK,
          "gateway-anchor link setup failed");
    CHECK(survey_append_discovery_start_tlvs(payload, sizeof(payload),
                                              &payload_len, &config) == PROTO_OK,
          "survey-start TLV encoding failed");
    CHECK(survey_init_discovery_start_packet(&packet, GATEWAY_ID, &config,
                                              1u, (uint8_t)payload_len) == PROTO_OK,
          "survey-start packet setup failed");
    CHECK(packet.msg_type == MSG_SURVEY_DISCOVERY_START,
          "scenario is not exercising survey-start 0x54");

    CHECK(proto_packet_encode(&packet, payload, frame, sizeof(frame),
                              &frame_len) == PROTO_OK,
          "survey-start frame encoding failed");
    tx_phy = mutate_tx_to_standard_wake ? MESH_SIM_PHY_CHANNEL5_WAKE :
                                         survey_start_tx_phy(frame_len);
    rx_phy = survey_start_rx_phy();
    if (!mutate_tx_to_standard_wake) {
        CHECK(tx_phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              "production survey-start TX policy did not select extended PHR");
        CHECK(rx_phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              "production survey-start RX policy did not select extended PHR");
    }
    airtime_us = mesh_sim_frame_duration_us(tx_phy, frame_len);
    CHECK(airtime_us > rx_end_trim_us, "invalid RX containment mutation");
    rx_end_us = TX_START_US + airtime_us - rx_end_trim_us;
    CHECK(mesh_sim_schedule_rx(&world, anchor, TX_START_US, rx_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               rx_phy,
                               &window) == MESH_SIM_OK,
          "survey-start RX scheduling failed");
    CHECK(mesh_sim_schedule_raw_tx(&world, gateway, TX_START_US,
                                   UWB_CHANNEL_WAKE_CONTACT, tx_phy,
                                   frame, frame_len, false,
                                   &transmission) == MESH_SIM_OK,
          "survey-start TX scheduling failed");
    world.transmissions[transmission].protocol_msg_type = packet.msg_type;
    ret = mesh_sim_run_until(&world, TX_START_US + airtime_us + 100u);
    CHECK(ret == MESH_SIM_OK, "survey-start simulation failed");

    if (!expect_reception) {
        CHECK(world.reception_count == 0u,
              "PHY-mismatched survey start unexpectedly reached RX decode");
        return;
    }
    CHECK(world.reception_count == 1u,
          "survey-start scenario did not produce exactly one RX outcome");
    if (world.reception_count == 1u) {
        CHECK((world.receptions[0].outcome == MESH_SIM_RX_DECODED) ==
                  expect_decode,
              expect_decode ?
                  "fully contained extended-PHR survey start did not decode" :
                  "partially contained survey start incorrectly decoded");
    }
}

static void test_two_anchor_survey_lifecycle(void)
{
    static const struct survey_discovery_config config = {
        .survey_id = SURVEY_ID,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
    };
    static struct mesh_sim_world world;
    static struct survey_gateway_context gateway_context;
    struct survey_gateway_auto_context auto_context;
    struct survey_gateway_auto_action action;
    struct survey_discovery_attempt_schedule schedules[2];
    struct proto_packet start_packet;
    struct proto_packet report_packets[2];
    struct survey_reachability_entry reports[2] = {0};
    uint64_t anchor_ids[2] = {ANCHOR_ID, ANCHOR_2_ID};
    const enum survey_gateway_auto_stage expected_stages[4] = {
        SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR,
        SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_RESPONDER,
        SURVEY_GATEWAY_AUTO_START_INITIATOR,
    };
    uint8_t start_payload[64] = {0};
    uint8_t start_frame[PACKET_EXT_MAX_LEN] = {0};
    uint8_t report_payloads[2][64] = {{0}};
    uint8_t probe_frames[2][UWB_SURVEY_DISCOVERY_PROBE_LEN] = {{0}};
    size_t start_payload_len = 0u;
    size_t start_frame_len = 0u;
    size_t report_payload_lens[2] = {0u};
    size_t probe_frame_lens[2] = {0u};
    uint8_t gateway = UINT8_MAX;
    uint8_t anchors[2] = {UINT8_MAX, UINT8_MAX};
    uint16_t object_index;
    uint64_t start_tx_us = UINT64_C(10000);
    uint64_t probe_tx_us[2];
    uint64_t probe_end_us[2];
    uint32_t start_airtime_us;
    enum mesh_sim_phy start_tx_phy;
    enum mesh_sim_phy start_rx_phy;
    bool pair_launched = false;
    bool pair_skipped = false;
    int survey_work;

    preempt_ch9_for_survey(&survey_work);

    while (survey_discovery_opportunity_slot(
               anchor_ids[0], SURVEY_ID, 0u, config.slot_count) ==
           survey_discovery_opportunity_slot(
               anchor_ids[1], SURVEY_ID, 0u, config.slot_count)) {
        anchor_ids[1]++;
    }
    reports[0] = (struct survey_reachability_entry) {
        .peer_id = anchor_ids[1], .rssi_dbm = -48, .quality = 91u,
    };
    reports[1] = (struct survey_reachability_entry) {
        .peer_id = anchor_ids[0], .rssi_dbm = -49, .quality = 90u,
    };

    mesh_sim_init(&world, UINT32_C(0x54c50002));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK,
          "lifecycle gateway setup failed");
    for (uint8_t i = 0u; i < 2u; i++) {
        CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, anchor_ids[i],
                                GATEWAY_ID, ROUTE_EPOCH, &anchors[i]) == MESH_SIM_OK,
              "lifecycle anchor setup failed");
        CHECK(mesh_sim_set_link(&world, gateway, anchors[i], 100u, 0u) == MESH_SIM_OK,
              "lifecycle gateway-anchor link failed");
    }
    CHECK(mesh_sim_set_link(&world, anchors[0], anchors[1], 100u, 0u) == MESH_SIM_OK,
          "lifecycle anchor pair link failed");

    CHECK(survey_append_discovery_start_tlvs(start_payload, sizeof(start_payload),
                                              &start_payload_len, &config) == PROTO_OK,
          "lifecycle survey-start TLV encoding failed");
    CHECK(survey_init_discovery_start_packet(&start_packet, GATEWAY_ID, &config,
                                              2u,
                                              (uint8_t)start_payload_len) == PROTO_OK,
          "lifecycle survey-start packet setup failed");
    CHECK(proto_packet_encode(&start_packet, start_payload, start_frame,
                              sizeof(start_frame), &start_frame_len) == PROTO_OK,
          "lifecycle survey-start frame encoding failed");
    start_tx_phy = survey_start_tx_phy(start_frame_len);
    start_rx_phy = survey_start_rx_phy();
    CHECK(start_tx_phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL &&
              start_rx_phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
          "lifecycle survey-start policies disagree with extended PHR");
    start_airtime_us = mesh_sim_frame_duration_us(
        start_tx_phy, start_frame_len);
    for (uint8_t i = 0u; i < 2u; i++) {
        CHECK(mesh_sim_schedule_rx(&world, anchors[i], start_tx_us,
                                   start_tx_us + start_airtime_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   start_rx_phy,
                                   &object_index) == MESH_SIM_OK,
              "lifecycle survey-start RX scheduling failed");
    }
    CHECK(mesh_sim_schedule_raw_tx(&world, gateway, start_tx_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   start_tx_phy,
                                   start_frame, start_frame_len, false,
                                   &object_index) == MESH_SIM_OK,
          "lifecycle survey-start TX scheduling failed");
    world.transmissions[object_index].protocol_msg_type =
        MSG_SURVEY_DISCOVERY_START;

    for (uint8_t i = 0u; i < 2u; i++) {
        struct uwb_survey_discovery_probe_frame probe = {
            .network_id = UINT32_C(0x494d4543),
            .survey_id = SURVEY_ID,
            .anchor_id = anchor_ids[i],
            .slot_count = config.slot_count,
            .flags = FLAG_DIAGNOSTIC,
        };

        CHECK(survey_discovery_schedule_attempt(&config, anchor_ids[i], 0u, 0u,
                                                 &schedules[i]) == PROTO_OK,
              "production survey probe scheduling failed");
        CHECK(!schedules[i].deferred,
              "unblocked lifecycle probe unexpectedly deferred");
        probe.anchor_slot = survey_discovery_opportunity_slot(
            anchor_ids[i], SURVEY_ID, 0u, config.slot_count);
        CHECK(uwb_encode_survey_discovery_probe(&probe, probe_frames[i],
                                                sizeof(probe_frames[i]),
                                                &probe_frame_lens[i]) == PROTO_OK,
              "survey probe encoding failed");
        probe_tx_us[i] = UINT64_C(1000000) +
                         (uint64_t)schedules[i].tx_ms * 1000u;
        probe_end_us[i] = probe_tx_us[i] + mesh_sim_frame_duration_us(
            MESH_SIM_PHY_CHANNEL5_WAKE, probe_frame_lens[i]);
    }
    CHECK(probe_end_us[0] <= probe_tx_us[1] ||
              probe_end_us[1] <= probe_tx_us[0],
          "chosen lifecycle anchors collide in their production probe slots");
    for (uint8_t sender = 0u; sender < 2u; sender++) {
        uint8_t receiver = (uint8_t)(1u - sender);

        CHECK(mesh_sim_schedule_rx(&world, anchors[receiver], probe_tx_us[sender],
                                   probe_end_us[sender],
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   &object_index) == MESH_SIM_OK,
              "peer probe RX scheduling failed");
        CHECK(mesh_sim_schedule_raw_tx(&world, anchors[sender], probe_tx_us[sender],
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_WAKE,
                                       probe_frames[sender], probe_frame_lens[sender],
                                       false, &object_index) == MESH_SIM_OK,
              "peer probe TX scheduling failed");
        world.transmissions[object_index].protocol_msg_type =
            MSG_UWB_SURVEY_DISCOVERY_PROBE;
    }
    CHECK(mesh_sim_run(&world) == MESH_SIM_OK,
          "two-anchor survey radio lifecycle failed");
    CHECK(world.reception_count == 4u,
          "survey start plus two peer probes did not all reach their receivers");
    for (size_t i = 0u; i < world.reception_count; i++) {
        CHECK(world.receptions[i].outcome == MESH_SIM_RX_DECODED,
              "survey lifecycle contained frame did not decode");
    }

    CHECK(survey_gateway_begin(&gateway_context, SURVEY_ID, 3u) == PROTO_OK,
          "gateway survey context setup failed");
    CHECK(survey_gateway_note_reach_report(&gateway_context, SURVEY_ID + 1u,
                                           ANCHOR_ID, &reports[0], 1u) ==
              PROTO_ERR_STALE && gateway_context.report_count == 0u,
          "stale survey report changed gateway state");
    for (uint8_t i = 0u; i < 2u; i++) {
        uint32_t parsed_survey_id = 0u;
        uint64_t parsed_anchor_id = 0u;
        struct survey_reachability_entry parsed_entry;
        size_t parsed_count = 0u;

        CHECK(survey_append_reach_report_tlvs(
                  report_payloads[i], sizeof(report_payloads[i]),
                  &report_payload_lens[i], SURVEY_ID, anchor_ids[i],
                  &reports[i], 1u) == PROTO_OK,
              "0x55 report TLV encoding failed");
        CHECK(survey_init_discovery_report_packet(
                  &report_packets[i], anchor_ids[i], GATEWAY_ID, SURVEY_ID,
                  (uint16_t)(10u + i),
                  (uint8_t)report_payload_lens[i]) == PROTO_OK &&
                  report_packets[i].msg_type == MSG_SURVEY_DISCOVERY_REPORT,
              "0x55 report packet setup failed");
        CHECK(survey_extract_reach_report_tlvs(
                  report_payloads[i], report_payload_lens[i], &parsed_survey_id,
                  &parsed_anchor_id, &parsed_entry, 1u, &parsed_count) == PROTO_OK &&
                  parsed_survey_id == report_packets[i].session_id &&
                  parsed_anchor_id == report_packets[i].src_id && parsed_count == 1u,
              "0x55 report identity or payload round trip failed");
        CHECK(survey_gateway_note_reach_report(
                  &gateway_context, parsed_survey_id, parsed_anchor_id,
                  &parsed_entry, parsed_count) == PROTO_OK,
              "valid 0x55 report was rejected");
    }
    CHECK(survey_gateway_plan_pairs(&gateway_context) == PROTO_OK &&
              gateway_context.report_count == 2u &&
              gateway_context.pair_count == 1u &&
              gateway_context.pairs[0].sample_count == 3u,
          "two mutual reports did not plan exactly one survey pair");

    CHECK(survey_gateway_auto_begin(&auto_context) == PROTO_OK,
          "survey auto context setup failed");
    for (uint8_t i = 0u; i < 4u; i++) {
        enum command_id expected_command = i < 2u ?
            CMD_SURVEY_PREPARE_PAIR : CMD_SURVEY_START_PAIR;
        uint64_t expected_target =
            expected_stages[i] == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR ||
                    expected_stages[i] == SURVEY_GATEWAY_AUTO_START_INITIATOR ?
                gateway_context.pairs[0].initiator_id :
                gateway_context.pairs[0].responder_id;

        CHECK(survey_gateway_auto_next_action(&auto_context, &gateway_context,
                                               &action) == PROTO_OK &&
                  !action.complete && action.stage == expected_stages[i] &&
                  action.command_id == expected_command &&
                  action.target_id == expected_target,
              "survey prepare/start action order drifted");
        CHECK(survey_gateway_auto_mark_waiting(&auto_context) == PROTO_OK,
              "survey auto action did not enter waiting state");
        CHECK(survey_gateway_auto_note_result(
                  &auto_context, action.command_id, action.target_id, SURVEY_ID,
                  COMMAND_OK, &pair_launched, &pair_skipped) == PROTO_OK &&
                  !pair_skipped && pair_launched == (i == 3u),
              "survey auto action result did not advance deterministically");
    }
    CHECK(survey_gateway_auto_next_action(&auto_context, &gateway_context,
                                           &action) == PROTO_OK && action.complete,
          "survey auto lifecycle did not complete after one pair");
}

int main(void)
{
    run_survey_start_phy_case(false, 0u, true, true);
    run_survey_start_phy_case(false, 1u, false, true);
    run_survey_start_phy_case(true, 0u, false, false);
    test_two_anchor_survey_lifecycle();

    if (failures != 0u) {
        fprintf(stderr, "RESULT mesh_survey_phy_scenarios failures=%u\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS mesh_survey_phy_scenarios\n");
    return EXIT_SUCCESS;
}
