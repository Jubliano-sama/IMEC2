#include "mesh_sim.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_gateway_command_priority.h"
#include "gateway_command.h"
#include "mesh.h"
#include "protocol.h"
#include "survey.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xa001000000000001)
#define ANCHOR_ID UINT64_C(0xa002000000000001)
#define ANCHOR_2_ID UINT64_C(0xa002000000000002)
#define SURVEY_ID UINT32_C(0x50665006)
#define ROUTE_EPOCH UINT32_C(7)
#define TX_START_US UINT64_C(10000)
#define CHANNEL5_STANDARD_FRAME_MAX_LEN 125u
#define PHY_HOP_GAP_US UINT64_C(1000)

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

static bool survey_result_has_action(const struct mesh_relay_result *result,
                                     enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static bool model_exact_airtime_hop(struct mesh_sim_world *world,
                                    uint8_t sender,
                                    uint8_t receiver,
                                    uint64_t *at_us,
                                    uint8_t channel,
                                    enum mesh_sim_phy tx_phy,
                                    enum mesh_sim_phy rx_phy,
                                    const uint8_t *frame,
                                    size_t frame_len,
                                    uint8_t msg_type,
                                    bool expect_decode)
{
    size_t receptions_before;
    uint16_t transmission = UINT16_MAX;
    uint64_t arrival_start_us;
    uint64_t arrival_end_us;
    int ret;

    if (world == NULL || at_us == NULL || frame == NULL || frame_len == 0u) {
        CHECK(false, "exact-airtime hop received invalid arguments");
        return false;
    }
    receptions_before = world->reception_count;
    ret = mesh_sim_schedule_raw_tx(world, sender, *at_us, channel, tx_phy,
                                   frame, frame_len, false, &transmission);
    if (ret != MESH_SIM_OK) {
        CHECK(false, "exact-airtime TX scheduling failed");
        return false;
    }
    world->transmissions[transmission].protocol_msg_type = msg_type;
    arrival_start_us = *at_us + world->propagation_us[sender][receiver];
    arrival_end_us = world->transmissions[transmission].end_us +
                     world->propagation_us[sender][receiver];
    ret = mesh_sim_schedule_rx(world, receiver, arrival_start_us,
                               arrival_end_us, channel, rx_phy, NULL);
    if (ret != MESH_SIM_OK) {
        CHECK(false, "exact-airtime RX scheduling failed");
        return false;
    }
    ret = mesh_sim_run_until(world, arrival_end_us + 1u);
    if (ret != MESH_SIM_OK) {
        CHECK(false, "exact-airtime PHY simulation failed");
        return false;
    }
    *at_us = arrival_end_us + PHY_HOP_GAP_US;

    if (expect_decode) {
        if (world->reception_count != receptions_before + 1u ||
            world->receptions[receptions_before].outcome !=
                MESH_SIM_RX_DECODED) {
            CHECK(false, "fully contained matching-PHY frame did not decode");
            return false;
        }
    } else if (world->reception_count != receptions_before) {
        CHECK(false, "PHR-mismatched frame reached receiver decode");
        return false;
    }
    return true;
}

static bool model_control_wake_claim(struct mesh_sim_world *world,
                                     uint8_t sender,
                                     uint8_t receiver,
                                     uint64_t sender_id,
                                     uint64_t *at_us,
                                     uint32_t event_id)
{
    const struct uwb_wake_claim_frame claim = {
        .network_id = UINT32_C(0x494d4543),
        .clicker_id = sender_id,
        .click_event_id = event_id,
        .attempt_index = 1u,
        .priority_id = sender_id,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 5u,
        .discovery_starts_in_ms = 5u,
        .claimed_duration_ms = 20u,
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .nonce = UINT64_C(0x0102030405060708),
        .flags = FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
                 FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY,
    };
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;

    if (uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) !=
        PROTO_OK) {
        CHECK(false, "control wake-claim encoding failed");
        return false;
    }
    return model_exact_airtime_hop(world, sender, receiver, at_us,
                                    UWB_CHANNEL_WAKE_CONTACT,
                                    MESH_SIM_PHY_CHANNEL5_WAKE,
                                    MESH_SIM_PHY_CHANNEL5_WAKE,
                                    frame, frame_len, MSG_UWB_WAKE_CLAIM, true);
}

static bool model_pair_prepare_control_hop(
    struct mesh_sim_world *world,
    uint8_t sender,
    uint8_t receiver,
    uint64_t sender_id,
    uint64_t *at_us,
    const struct mesh_outbound *control,
    size_t modeled_frame_len,
    bool force_standard_phr,
    bool expect_decode)
{
    uint8_t frame[PACKET_EXT_MAX_LEN] = {0};
    size_t encoded_len = 0u;
    enum mesh_sim_phy tx_phy;
    enum mesh_sim_phy rx_phy;

    if (!model_control_wake_claim(world, sender, receiver, sender_id, at_us,
                                  control->packet.seq)) {
        return false;
    }
    if (proto_packet_encode(&control->packet, control->payload, frame,
                            sizeof(frame), &encoded_len) != PROTO_OK ||
        encoded_len > modeled_frame_len ||
        modeled_frame_len > sizeof(frame)) {
        CHECK(false, "pair-prepare modeled frame length is invalid");
        return false;
    }
    memset(frame + encoded_len, 0xa5, modeled_frame_len - encoded_len);
    tx_phy = force_standard_phr ? MESH_SIM_PHY_CHANNEL5_WAKE :
        (app_mesh_c5_control_uses_extended_phr(
             control->packet.msg_type, modeled_frame_len,
             CHANNEL5_STANDARD_FRAME_MAX_LEN) ?
             MESH_SIM_PHY_CHANNEL5_MESH_CONTROL :
             MESH_SIM_PHY_CHANNEL5_WAKE);
    rx_phy = app_mesh_c5_wake_followup_uses_extended_phr(
                 FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
                 FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY) ?
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL : MESH_SIM_PHY_CHANNEL5_WAKE;

    return model_exact_airtime_hop(world, sender, receiver, at_us,
                                    UWB_CHANNEL_WAKE_CONTACT, tx_phy, rx_phy,
                                    frame, modeled_frame_len,
                                    MSG_SURVEY_PAIR_PREPARE, expect_decode);
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
        .round_count = 4u,
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

static enum mesh_sim_rx_outcome run_pair_start_skew_phy_case(
    const uint8_t *poll,
    size_t poll_len,
    uint32_t start_skew_ms,
    uint32_t attempt_offset_us,
    enum mesh_sim_rx_outcome expected_outcome)
{
    static struct mesh_sim_world world;
    uint8_t initiator = UINT8_MAX;
    uint8_t responder = UINT8_MAX;
    uint16_t window = UINT16_MAX;
    uint16_t transmission = UINT16_MAX;
    uint32_t airtime_us;
    uint64_t poll_start_us;
    uint64_t poll_end_us;
    uint64_t responder_end_us;

    mesh_sim_init(&world, UINT32_C(0x52c50000) ^ start_skew_ms ^
                  attempt_offset_us);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &initiator) == MESH_SIM_OK,
          "pair-skew initiator setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &responder) == MESH_SIM_OK,
          "pair-skew responder setup failed");
    CHECK(mesh_sim_set_link(&world, initiator, responder, 100u, 0u) ==
              MESH_SIM_OK,
          "pair-skew anchor link setup failed");

    airtime_us = mesh_sim_frame_duration_us(MESH_SIM_PHY_CHANNEL5_RANGE,
                                             poll_len);
    poll_start_us = TX_START_US + (uint64_t)start_skew_ms * 1000u +
                    attempt_offset_us;
    poll_end_us = poll_start_us + airtime_us;
    responder_end_us = TX_START_US +
        (uint64_t)SURVEY_PAIR_RESPONDER_WINDOW_MS * 1000u;
    CHECK(mesh_sim_schedule_rx(&world, responder, TX_START_US,
                               responder_end_us, UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_RANGE, &window) ==
              MESH_SIM_OK,
          "pair-skew responder RX scheduling failed");
    CHECK(mesh_sim_schedule_raw_tx(&world, initiator, poll_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_RANGE,
                                   poll, poll_len, false, &transmission) ==
              MESH_SIM_OK,
          "pair-skew initiator poll scheduling failed");
    world.transmissions[transmission].protocol_msg_type = MSG_UWB_POLL;
    CHECK(mesh_sim_run_until(&world, poll_end_us + 1u) == MESH_SIM_OK,
          "pair-skew PHY simulation failed");
    CHECK(world.reception_count == 1u,
          "pair-start timing case did not produce one poll reception");
    if (world.reception_count != 1u) {
        return MESH_SIM_RX_DECODE_ERROR;
    }
    CHECK(world.receptions[0].outcome == expected_outcome,
          expected_outcome == MESH_SIM_RX_DECODED ?
              "admitted local skew did not contain the complete poll airtime" :
              "poll beyond the bounded local window unexpectedly decoded");
    return world.receptions[0].outcome;
}

static void test_pair_start_skew_is_local_and_route_depth_independent(void)
{
    static const uint32_t phy_sweep_step_ms = 37u;
    const struct uwb_range_header header = {
        .type = MSG_UWB_POLL,
        .seq = 1u,
        .round_index = 0u,
        .network_id = UINT32_C(0x494d4543),
        .session_id = SURVEY_ID,
        .session_nonce = UINT64_C(0x5066500600000001),
        .initiator_short_addr =
            uwb_session_short_addr_from_id(ANCHOR_ID),
        .responder_short_addr =
            uwb_session_short_addr_from_id(ANCHOR_2_ID),
        .flags = FLAG_DIAGNOSTIC,
        .initiator_id = ANCHOR_ID,
        .responder_id = ANCHOR_2_ID,
    };
    uint8_t poll[UWB_POLL_LEN] = {0};
    size_t poll_len = 0u;
    uint32_t poll_airtime_us;
    uint32_t latest_poll_offset_us;

    CHECK(uwb_encode_poll(&header, poll, sizeof(poll), &poll_len) == PROTO_OK &&
              poll_len == UWB_POLL_LEN,
          "pair-skew poll encoding failed");
    poll_airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_RANGE, poll_len);
    CHECK(poll_airtime_us < SURVEY_PAIR_INITIATOR_TIMEOUT_MS * 1000u,
          "initiator DS-TWR timeout cannot contain one complete poll frame");
    latest_poll_offset_us = SURVEY_PAIR_INITIATOR_TIMEOUT_MS * 1000u -
                            poll_airtime_us;

    CHECK(SURVEY_PAIR_RESPONDER_WINDOW_MS ==
              SURVEY_PAIR_START_SKEW_MARGIN_MS +
                  SURVEY_PAIR_INITIATOR_TIMEOUT_MS,
          "responder window is not exactly local skew plus DS-TWR timeout");
    CHECK(SURVEY_PAIR_RESPONDER_WINDOW_MS <
              survey_pair_control_timeout_ms(1u),
          "one-hop command timeout leaked into the local responder window");
    CHECK(SURVEY_PAIR_RESPONDER_WINDOW_MS <
              survey_pair_control_timeout_ms(SURVEY_DEFAULT_TTL),
          "multi-hop command timeout leaked into the local responder window");

    for (uint32_t skew_ms = 0u;
         skew_ms < SURVEY_PAIR_START_SKEW_MARGIN_MS;
         skew_ms += phy_sweep_step_ms) {
        if (run_pair_start_skew_phy_case(poll, poll_len, skew_ms,
                                         latest_poll_offset_us,
                                         MESH_SIM_RX_DECODED) !=
            MESH_SIM_RX_DECODED) {
            break;
        }
    }
    run_pair_start_skew_phy_case(poll, poll_len,
                                 SURVEY_PAIR_START_SKEW_MARGIN_MS,
                                 latest_poll_offset_us,
                                 MESH_SIM_RX_DECODED);
    run_pair_start_skew_phy_case(poll, poll_len,
                                 SURVEY_PAIR_START_SKEW_MARGIN_MS,
                                 latest_poll_offset_us + 1u,
                                 MESH_SIM_RX_FRAME_TIMEOUT);
}

static int build_pair_prepare_control(struct mesh_outbound *control,
                                      uint64_t target_id,
                                      uint64_t next_hop_id,
                                      uint16_t seq)
{
    const struct survey_pair pair = {
        .initiator_id = target_id,
        .responder_id = ANCHOR_2_ID,
        .survey_id = SURVEY_ID,
        .sample_count = 3u,
    };
    size_t payload_len = 0u;
    int ret;

    if (control == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(control, 0, sizeof(*control));
    ret = survey_append_pair_tlvs(control->payload, sizeof(control->payload),
                                  &payload_len, &pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_init_pair_prepare_packet(&control->packet, &pair, GATEWAY_ID,
                                          seq, (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    control->packet.dst_id = target_id;
    control->payload_len = (uint16_t)payload_len;
    control->next_hop_id = next_hop_id;
    control->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static bool model_command_result_hop(struct mesh_sim_world *world,
                                     uint8_t sender,
                                     uint8_t receiver,
                                     uint64_t *at_us,
                                     const struct mesh_outbound *result)
{
    uint8_t frame[PACKET_EXT_MAX_LEN] = {0};
    size_t frame_len = 0u;

    if (result == NULL ||
        proto_packet_encode(&result->packet, result->payload, frame,
                            sizeof(frame), &frame_len) != PROTO_OK) {
        CHECK(false, "0x21 command-result frame encoding failed");
        return false;
    }
    return model_exact_airtime_hop(world, sender, receiver, at_us,
                                    UWB_CHANNEL_MESH_PAYLOAD,
                                    MESH_SIM_PHY_CHANNEL9_MESH,
                                    MESH_SIM_PHY_CHANNEL9_MESH,
                                    frame, frame_len, MSG_COMMAND_RESULT, true);
}

static bool relay_receive_outbound(struct mesh_relay *receiver,
                                   const struct mesh_outbound *outbound,
                                   uint64_t previous_hop_id,
                                   uint32_t now_ms,
                                   struct mesh_relay_result *result)
{
    if (mesh_relay_handle_rx_with_random(receiver, &outbound->packet,
                                         outbound->payload,
                                         outbound->payload_len,
                                         previous_hop_id, 100u, now_ms,
                                         now_ms ^ outbound->packet.seq,
                                         result) != PROTO_OK) {
        CHECK(false, "decoded survey control/result was rejected by relay");
        return false;
    }
    return true;
}

static void run_pair_prepare_hardware_case(bool relayed,
                                           size_t modeled_frame_len,
                                           bool force_standard_phr)
{
    static struct mesh_sim_world world;
    const uint64_t relay_id = ANCHOR_2_ID;
    const uint64_t target_id = ANCHOR_ID;
    struct mesh_outbound control;
    struct mesh_outbound result_out = {0};
    struct mesh_relay_result relay_result;
    const struct route_candidate *selected;
    struct gateway_command_pending pending = {0};
    uint8_t result_payload[16] = {0};
    size_t result_payload_len = 0u;
    uint8_t gateway = UINT8_MAX;
    uint8_t relay = UINT8_MAX;
    uint8_t target = UINT8_MAX;
    uint8_t first_hop;
    uint64_t previous_hop_id = GATEWAY_ID;
    uint64_t at_us = TX_START_US;
    uint16_t seq = (uint16_t)(modeled_frame_len +
                              (relayed ? 1000u : 0u) +
                              (force_standard_phr ? 2000u : 0u));
    bool expect_control_decode = !force_standard_phr;

    mesh_sim_init(&world, (uint32_t)seq ^ UINT32_C(0x52c50000));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK,
          "pair-control gateway setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, relay_id,
                            GATEWAY_ID, ROUTE_EPOCH, &relay) == MESH_SIM_OK,
          "pair-control relay setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, target_id,
                            GATEWAY_ID, ROUTE_EPOCH, &target) == MESH_SIM_OK,
          "pair-control target setup failed");
    first_hop = relayed ? relay : target;
    CHECK(mesh_sim_set_link(&world, gateway, first_hop, 100u, 0u) ==
              MESH_SIM_OK,
          "pair-control first-hop link setup failed");
    if (relayed) {
        CHECK(mesh_sim_set_link(&world, relay, target, 100u, 0u) ==
                  MESH_SIM_OK,
              "pair-control relay-target link setup failed");
        CHECK(mesh_sim_install_route(&world, relay, gateway, 0u,
                                     ROUTE_EPOCH) == MESH_SIM_OK,
              "pair-control relay upstream route setup failed");
        CHECK(mesh_sim_install_downlink(&world, relay, target_id, target, 1u,
                                        ROUTE_EPOCH) == MESH_SIM_OK,
              "pair-control relay downlink setup failed");
    }
    CHECK(mesh_sim_install_downlink(&world, gateway, target_id, first_hop,
                                    relayed ? 2u : 1u,
                                    ROUTE_EPOCH) == MESH_SIM_OK,
          "pair-control gateway downlink setup failed");
    CHECK(build_pair_prepare_control(&control, target_id,
                                     relayed ? relay_id : target_id,
                                     seq) == PROTO_OK,
          "pair-control 0x52 packet setup failed");
    CHECK(control.packet.msg_type == MSG_SURVEY_PAIR_PREPARE,
          "pair-control scenario is not exercising 0x52");
    CHECK(gateway_command_pending_start(&pending, &control.packet,
                                        CMD_SURVEY_PREPARE_PAIR, 0u,
                                        GATEWAY_COMMAND_RESULT_TIMEOUT_MS) ==
              PROTO_OK,
          "pair-control pending waiter setup failed");

    if (!model_pair_prepare_control_hop(
            &world, gateway, first_hop, GATEWAY_ID, &at_us, &control,
            modeled_frame_len, force_standard_phr, expect_control_decode)) {
        return;
    }
    if (!expect_control_decode) {
        CHECK(pending.active,
              "wrong-PHR 0x52 unexpectedly completed its pending waiter");
        return;
    }

    if (!relay_receive_outbound(&world.roles[first_hop].relay, &control,
                                previous_hop_id, (uint32_t)(at_us / 1000u),
                                &relay_result)) {
        return;
    }
    if (relayed) {
        CHECK(survey_result_has_action(&relay_result,
                                       MESH_RELAY_ACTION_FORWARD),
              "decoded multihop 0x52 did not enter relay forwarding");
        control = relay_result.forward;
        control.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
        previous_hop_id = relay_id;
        if (!model_pair_prepare_control_hop(
                &world, relay, target, relay_id, &at_us, &control,
                modeled_frame_len, false, true) ||
            !relay_receive_outbound(&world.roles[target].relay, &control,
                                    previous_hop_id,
                                    (uint32_t)(at_us / 1000u),
                                    &relay_result)) {
            return;
        }
    }
    CHECK(survey_result_has_action(&relay_result,
                                   MESH_RELAY_ACTION_DELIVER_LOCAL),
          "matching-PHR 0x52 did not reach target local delivery");
    CHECK(route_selected(&world.roles[target].relay.upstream) == NULL,
          "pair-control target unexpectedly had a preseeded upstream route");
    CHECK(!world.roles[target].relay.route_discovery.active,
          "pair-control target started route discovery before local delivery");
    CHECK(mesh_relay_note_gateway_control_reverse_route(
              &world.roles[target].relay, &control.packet,
              previous_hop_id, 100u, SURVEY_DEFAULT_TTL,
              (uint32_t)(at_us / 1000u)) == PROTO_OK,
          "accepted 0x52 did not seed its physical reverse first hop");
    selected = route_selected(&world.roles[target].relay.upstream);
    CHECK(selected != NULL,
          "accepted 0x52 left the command result without an upstream route");
    CHECK(selected->next_hop_id == previous_hop_id,
          "accepted 0x52 seeded a route through the wrong physical sender");
    CHECK(selected->hop_count == (relayed ? 1u : 0u),
          "accepted 0x52 derived the wrong reverse hop count");
    CHECK(!world.roles[target].relay.route_discovery.active,
          "accepted 0x52 unnecessarily started route discovery");

    CHECK(mesh_append_command_result(result_payload, sizeof(result_payload),
                                     &result_payload_len,
                                     CMD_SURVEY_PREPARE_PAIR,
                                     COMMAND_OK, 0u) == PROTO_OK,
          "0x21 result payload setup failed");
    CHECK(mesh_init_command_result(&result_out.packet, target_id, GATEWAY_ID,
                                   SURVEY_ID, seq,
                                   (uint8_t)result_payload_len,
                                   false) == PROTO_OK,
          "0x21 result packet setup failed");
    memcpy(result_out.payload, result_payload, result_payload_len);
    CHECK(mesh_relay_start_tx(&world.roles[target].relay,
                              &result_out.packet,
                              result_payload,
                              result_payload_len,
                              (uint32_t)(at_us / 1000u),
                              &result_out) == PROTO_OK,
          "accepted 0x52 could not immediately start its 0x21 result");
    CHECK(result_out.next_hop_id == previous_hop_id,
          "0x21 result did not use the reverse first hop from accepted 0x52");
    CHECK(!world.roles[target].relay.route_discovery.active,
          "0x21 result triggered route discovery despite its reverse route");
    result_out.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    CHECK(result_out.packet.msg_type == MSG_COMMAND_RESULT,
          "pair-control result is not 0x21");

    if (!model_command_result_hop(&world, target,
                                  relayed ? relay : gateway,
                                  &at_us, &result_out)) {
        return;
    }
    if (relayed) {
        if (!relay_receive_outbound(&world.roles[relay].relay, &result_out,
                                    target_id, (uint32_t)(at_us / 1000u),
                                    &relay_result)) {
            return;
        }
        CHECK(survey_result_has_action(&relay_result,
                                       MESH_RELAY_ACTION_FORWARD),
              "multihop 0x21 did not enter relay forwarding");
        result_out = relay_result.forward;
        result_out.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        if (!model_command_result_hop(&world, relay, gateway, &at_us,
                                      &result_out)) {
            return;
        }
    }
    if (!relay_receive_outbound(&world.roles[gateway].relay, &result_out,
                                relayed ? relay_id : target_id,
                                (uint32_t)(at_us / 1000u), &relay_result)) {
        return;
    }
    CHECK(survey_result_has_action(&relay_result,
                                   MESH_RELAY_ACTION_DELIVER_LOCAL),
          "physically delivered 0x21 did not reach gateway local delivery");
    CHECK(gateway_command_pending_complete_result(&pending,
                                                   &result_out.packet),
          "physically delivered 0x21 did not complete pending 0x52");
    CHECK(!pending.active,
          "completed pair-control waiter remained active");
}

static void test_pair_prepare_phr_and_complete_airtime_sweep(void)
{
    static const size_t frame_lengths[] = {91u, 124u, 125u, 126u};

    for (size_t relayed = 0u; relayed < 2u; relayed++) {
        for (size_t length = 0u;
             length < sizeof(frame_lengths) / sizeof(frame_lengths[0]);
             length++) {
            run_pair_prepare_hardware_case(relayed != 0u,
                                           frame_lengths[length], false);
            if (frame_lengths[length] <= CHANNEL5_STANDARD_FRAME_MAX_LEN) {
                run_pair_prepare_hardware_case(relayed != 0u,
                                               frame_lengths[length], true);
            }
        }
    }
}

static void test_two_anchor_survey_lifecycle(void)
{
    static const struct survey_discovery_config config = {
        .survey_id = SURVEY_ID,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
        .round_count = 4u,
    };
    static struct mesh_sim_world world;
    static struct survey_gateway_context gateway_context;
    struct survey_gateway_auto_context auto_context;
    struct survey_gateway_auto_action action;
    struct survey_pair planned_pair;
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
              gateway_context.pair_count == 1u,
          "two mutual reports did not plan exactly one survey pair");
    CHECK(survey_gateway_pair_at(&gateway_context, 0u, &planned_pair) ==
              PROTO_OK && planned_pair.survey_id == SURVEY_ID &&
              planned_pair.sample_count == 3u,
          "planned survey pair did not reconstruct context-wide fields");

    CHECK(survey_gateway_auto_begin(&auto_context) == PROTO_OK,
          "survey auto context setup failed");
    for (uint8_t i = 0u; i < 4u; i++) {
        enum command_id expected_command = i < 2u ?
            CMD_SURVEY_PREPARE_PAIR : CMD_SURVEY_START_PAIR;
        uint64_t expected_target =
            expected_stages[i] == SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR ||
                    expected_stages[i] == SURVEY_GATEWAY_AUTO_START_INITIATOR ?
                planned_pair.initiator_id : planned_pair.responder_id;

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
    test_pair_start_skew_is_local_and_route_depth_independent();
    test_pair_prepare_phr_and_complete_airtime_sweep();
    test_two_anchor_survey_lifecycle();

    if (failures != 0u) {
        fprintf(stderr, "RESULT mesh_survey_phy_scenarios failures=%u\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS mesh_survey_phy_scenarios\n");
    return EXIT_SUCCESS;
}
