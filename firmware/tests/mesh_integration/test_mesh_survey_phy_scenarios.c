#include "mesh_sim.h"
#include "mesh_sim_invariants.h"
#include "app_mesh_c5_priority.h"
#include "firmware_state_machines.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "survey.h"
#include "survey_pair_lease.h"
#include "survey_pair_round_runtime.h"
#include "survey_round_control.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0xa001000000000001)
#define ANCHOR_ID UINT64_C(0xa002000000000001)
#define ANCHOR_2_ID UINT64_C(0xa002000000000002)
#define ANCHOR_3_ID UINT64_C(0xa002000000000003)
#define SURVEY_ID UINT32_C(0x50665006)
#define SURVEY_OPERATION_GENERATION UINT64_C(0x0000000150665006)
#define SURVEY_ROUND_ID UINT16_C(1)
#define ROUTE_EPOCH UINT32_C(7)
#define TX_START_US UINT64_C(10000)
#define CHANNEL5_STANDARD_FRAME_MAX_LEN 125u
#define PHY_HOP_GAP_US UINT64_C(1000)
#define MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS 40u
#define MODELED_CONTROL_PHY_RETUNE_US UINT64_C(30000)
#define MODELED_WAKE_REPEAT_GAP_US UINT64_C(1000)
#define MODELED_FORCED_CONTROL_RELAY_DELAY_MS 340u
#define MODELED_FORCED_CONTROL_CLAIM_MS 400u
#define MODELED_DEEP_RELAY_LISTEN_MS 2000u
#define MODELED_LOW_DUTY_RESCHEDULE_MS 380u
#define MODELED_LOW_DUTY_RX_US UINT64_C(3500)
#define MODELED_FOLLOWUP_SCAN_MS 20u
#define MODELED_CONNECTED_GAP_RETUNE_MS 60u
#define MODELED_LEGACY_EVENT_INTERVAL_MS 520u
#define MODELED_POST_CH9_RELEASE_DELAY_MS 12u
#define MODELED_WAKE_AFTER_RECOVERY_SCAN_START_MS 5u
#define MODELED_CH9_OWNER_MS                                              \
    (MESH_RADIO_EVENT_GUARD_MS + MESH_RADIO_EVENT_WINDOW_MS +           \
     MESH_RADIO_EVENT_GUARD_MS)
#define MODELED_INTER_CH9_C5_GAP_MS                                      \
    (MODELED_CONNECTED_GAP_RETUNE_MS + MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS)
#define MODELED_PREEMPTED_CH9_WINDOWS \
    ((MODELED_DEEP_RELAY_LISTEN_MS + MESH_RADIO_EVENT_INTERVAL_MS - 1u) / \
     MESH_RADIO_EVENT_INTERVAL_MS)

_Static_assert(MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS * 1000u >
                   MODELED_CONTROL_PHY_RETUNE_US,
               "control follow-up turnaround must cover the PHY retune");
_Static_assert(MODELED_FORCED_CONTROL_RELAY_DELAY_MS <
                   MODELED_LOW_DUTY_RESCHEDULE_MS,
               "relay copy must arrive inside the measured low-duty hole");
_Static_assert(MODELED_FORCED_CONTROL_CLAIM_MS >
                   MODELED_LOW_DUTY_RESCHEDULE_MS,
               "control claim must cover the complete low-duty hole");
_Static_assert(MODELED_DEEP_RELAY_LISTEN_MS >
                   FLOOD_WAVE_MS + MESH_RADIO_WAKE_TRAIN_MS +
                       MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS,
               "deep relay listener must cover the next wake wave and payload");
_Static_assert(MODELED_LOW_DUTY_RESCHEDULE_MS ==
                   MESH_RADIO_ANCHOR_SCAN_RESCHEDULE_MS,
               "forced follow-up scenario must use the production scan interval");
_Static_assert(MODELED_LOW_DUTY_RX_US == MESH_RADIO_ANCHOR_SCAN_RX_US,
               "forced follow-up scenario must use the production scan window");
_Static_assert(MODELED_FOLLOWUP_SCAN_MS ==
                   MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS,
               "forced follow-up scenario must use the boosted scan slice");
_Static_assert(MODELED_WAKE_AFTER_RECOVERY_SCAN_START_MS * 1000u >
                   MODELED_LOW_DUTY_RX_US,
               "ordinary scan must finish before the modeled wake copy");
_Static_assert(MODELED_WAKE_AFTER_RECOVERY_SCAN_START_MS <
                   MODELED_FOLLOWUP_SCAN_MS,
               "post-Channel-9 recovery slice must still contain the wake copy");
_Static_assert(MESH_RADIO_EVENT_INTERVAL_MS ==
                   2u * (MODELED_CH9_OWNER_MS +
                         MODELED_INTER_CH9_C5_GAP_MS),
               "two channel-9 owners must retain retune plus 20 ms Channel-5 gaps");
_Static_assert(MODELED_PREEMPTED_CH9_WINDOWS <=
                   MESH_RADIO_EVENT_MAX_MISSES,
               "one bounded gateway-control contact must not expire channel-9 timing");

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

static void preempt_ch9_for_survey(void *survey_work)
{
    struct gateway_priority_fixture fixture = {
        .ch9_rx_active = true,
    };
    struct fw_radio_handoff_sm handoff;
    struct fw_transition transition;
    struct fw_event event = {
        .operation_id = 1u,
        .generation = 1u,
        .target = FW_MACHINE_RADIO,
        .source = FW_EVENT_SOURCE_SERVICE,
        .type = FW_EVENT_RADIO_PREEMPT_REQUESTED,
        .payload.value = 1u,
    };

    fw_radio_handoff_sm_init(&handoff);
    CHECK(fw_radio_handoff_sm_handle(&handoff, &event, &transition) ==
              FW_SM_APPLIED,
          "three-sample survey priority request failed");
    CHECK(transition.effect.type == FW_EFFECT_RADIO_REQUEST_ABORT,
          "survey priority request did not request receive abort");
    survey_request_ch9_abort(&fixture);
    CHECK(fixture.event_count == 1u &&
              fixture.events[0] == GATEWAY_PRIORITY_ABORT_RX &&
              fixture.scheduled_work == NULL,
          "survey work ran before channel-9 reached a safe boundary");
    CHECK(handoff.state == FW_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY,
          "survey priority state did not wait for channel-9 completion");

    /* Models dwm3000_driver_receive_frame_continuous() returning -ECANCELED. */
    fixture.ch9_rx_active = false;
    event.type = FW_EVENT_RADIO_SAFE_BOUNDARY;
    CHECK(fw_radio_handoff_sm_handle(&handoff, &event, &transition) ==
              FW_SM_APPLIED &&
              transition.effect.type == FW_EFFECT_RADIO_SCHEDULE_PENDING,
          "three-sample survey did not resume at the channel-9 safe boundary");
    CHECK(survey_schedule_after_safe_boundary(&fixture, survey_work) == 0,
          "three-sample survey scheduling effect failed");
    event.type = FW_EVENT_EFFECT_SUCCEEDED;
    CHECK(fw_radio_handoff_sm_handle(&handoff, &event, &transition) ==
              FW_SM_APPLIED && handoff.state == FW_RADIO_HANDOFF_IDLE,
          "three-sample survey handoff did not finish after scheduling");
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
    } else {
        const struct mesh_sim_reception *reception =
            world->reception_count == receptions_before + 1u ?
                &world->receptions[receptions_before] : NULL;

        CHECK(reception != NULL &&
                  reception->outcome != MESH_SIM_RX_DECODED &&
                  reception->protocol_status != PROTO_OK,
              "PHR-mismatched frame was not recorded as a rejected RF activity");
        if (reception == NULL || reception->outcome == MESH_SIM_RX_DECODED ||
            reception->protocol_status == PROTO_OK) {
            return false;
        }
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
        .operation_generation = SURVEY_OPERATION_GENERATION,
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
              "incompatible survey-start PHY unexpectedly reached the receiver");
        return;
    }
    CHECK(world.reception_count == 1u,
          "survey-start scenario did not produce exactly one RX outcome");
    if (world.reception_count == 1u) {
        CHECK((world.receptions[0].outcome == MESH_SIM_RX_DECODED) ==
                  expect_decode &&
                  (expect_decode ||
                   world.receptions[0].protocol_status != PROTO_OK),
              expect_decode ?
                  "fully contained extended-PHR survey start did not decode" :
                  "survey-start activity was not recorded as a rejected RF frame");
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

static uint16_t model_survey_sample_alignment(
    uint32_t initiator_spacing_ms,
    uint32_t later_responder_window_ms,
    uint32_t responder_deadline_shift_ms,
    uint16_t poll_loss_mask,
    uint16_t response_loss_mask,
    uint16_t *wrong_sequence_mask)
{
    const uint64_t responder_started_ms = 100u;
    const uint64_t initiator_started_ms =
        responder_started_ms + SURVEY_PAIR_START_SKEW_MARGIN_MS;
    const uint64_t first_window_deadline_ms =
        responder_started_ms + SURVEY_PAIR_RESPONDER_WINDOW_MS +
        responder_deadline_shift_ms;
    uint16_t expected_sample = 0u;
    uint16_t usable_mask = 0u;
    uint16_t consumed_mask = 0u;
    uint64_t expected_window_deadline_ms = first_window_deadline_ms;

    for (uint16_t emitted_sample = 0u;
         emitted_sample < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
         emitted_sample++) {
        const uint16_t emitted_bit = (uint16_t)(1u << emitted_sample);
        const uint64_t poll_started_ms = initiator_started_ms +
            (uint64_t)emitted_sample * initiator_spacing_ms;

        while (expected_sample < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT &&
               poll_started_ms >= expected_window_deadline_ms) {
            expected_sample++;
            expected_window_deadline_ms = first_window_deadline_ms +
                (uint64_t)expected_sample * later_responder_window_ms;
        }
        if ((poll_loss_mask & emitted_bit) != 0u) {
            continue;
        }
        if (expected_sample != emitted_sample) {
            consumed_mask |= emitted_bit;
            continue;
        }
        if ((response_loss_mask & emitted_bit) != 0u) {
            /* The responder remains on this sequence until its deadline. */
            continue;
        }

        usable_mask |= emitted_bit;
        expected_sample++;
        expected_window_deadline_ms = first_window_deadline_ms +
            (uint64_t)expected_sample * later_responder_window_ms;
    }

    if (wrong_sequence_mask != NULL) {
        *wrong_sequence_mask = consumed_mask;
    }
    return usable_mask;
}

static void test_sample_cells_isolate_early_errors_and_lost_polls(void)
{
    const uint64_t responder_role_start_ms = 100u;
    const uint64_t pair_execution_ms =
        responder_role_start_ms + SURVEY_PAIR_START_SKEW_MARGIN_MS;
    const uint64_t first_cell_end_ms =
        pair_execution_ms + SURVEY_PAIR_INITIATOR_TIMEOUT_MS;
    const uint64_t second_cell_start_ms =
        pair_execution_ms + SURVEY_PAIR_SAMPLE_CELL_MS;
    const uint16_t all_samples_mask =
        (uint16_t)((1u << SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) - 1u);
    const uint16_t sample_1 = 1u << 0;
    const uint16_t sample_3 = 1u << 2;
    uint16_t wrong_sequence_mask = 0u;
    uint16_t usable_mask;

    CHECK(SURVEY_PAIR_SAMPLE_CELL_MS ==
              SURVEY_PAIR_INITIATOR_TIMEOUT_MS +
                  SURVEY_PAIR_SAMPLE_GAP_MS,
          "survey sample cell drifted from timeout plus inter-sample gap");
    CHECK(SURVEY_PAIR_BATCH_WINDOW_MS ==
              SURVEY_PAIR_RESPONDER_WINDOW_MS +
                  (SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT - 1u) *
                      SURVEY_PAIR_SAMPLE_CELL_MS,
          "survey pair batch window does not cover every fixed sample cell");
    CHECK(SURVEY_PAIR_BATCH_WINDOW_MS <
              survey_pair_control_timeout_ms(1u),
          "one-hop pair control deadline cannot contain the RF batch");
    CHECK(responder_role_start_ms + SURVEY_PAIR_RESPONDER_WINDOW_MS ==
              first_cell_end_ms &&
              second_cell_start_ms - first_cell_end_ms ==
                  SURVEY_PAIR_SAMPLE_GAP_MS,
          "immutable responder and initiator grids do not share cell boundaries");

    for (uint16_t failed_sample = 0u;
         failed_sample < SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
         failed_sample++) {
        const uint16_t failed_bit = (uint16_t)(1u << failed_sample);
        const uint16_t expected_usable =
            (uint16_t)(all_samples_mask & ~failed_bit);

        usable_mask = model_survey_sample_alignment(
            SURVEY_PAIR_SAMPLE_CELL_MS,
            SURVEY_PAIR_SAMPLE_CELL_MS,
            0u,
            failed_bit,
            0u,
            &wrong_sequence_mask);
        CHECK(usable_mask == expected_usable && wrong_sequence_mask == 0u,
              "one lost poll cascaded outside its fixed sample cell");

        usable_mask = model_survey_sample_alignment(
            SURVEY_PAIR_SAMPLE_CELL_MS,
            SURVEY_PAIR_SAMPLE_CELL_MS,
            0u,
            0u,
            failed_bit,
            &wrong_sequence_mask);
        CHECK(usable_mask == expected_usable && wrong_sequence_mask == 0u,
              "one lost response cascaded outside its fixed sample cell");
    }

    usable_mask = model_survey_sample_alignment(
        SURVEY_PAIR_SAMPLE_CELL_MS,
        SURVEY_PAIR_SAMPLE_CELL_MS,
        0u,
        sample_3,
        sample_1,
        &wrong_sequence_mask);
    CHECK(usable_mask ==
              (uint16_t)(all_samples_mask & ~(sample_1 | sample_3)) &&
              wrong_sequence_mask == 0u,
          "separate first-response and sample-3-poll losses cascaded");

    /*
     * A worker admitted after sample 1's RF cell closes must skip sequence 1,
     * then wait for sequence 2's immutable start. Treating that skip as one
     * lost poll proves it cannot be emitted into the responder's next window.
     */
    CHECK(first_cell_end_ms + 1u < second_cell_start_ms,
          "sample gap leaves no bounded admission point for an expired cell");
    usable_mask = model_survey_sample_alignment(
        SURVEY_PAIR_SAMPLE_CELL_MS,
        SURVEY_PAIR_SAMPLE_CELL_MS,
        0u,
        sample_1,
        0u,
        &wrong_sequence_mask);
    CHECK(usable_mask == (uint16_t)(all_samples_mask & ~sample_1) &&
              wrong_sequence_mask == 0u,
          "skipping an expired first cell shifted later sequence ownership");

    usable_mask = model_survey_sample_alignment(
        SURVEY_PAIR_SAMPLE_GAP_MS,
        SURVEY_PAIR_SAMPLE_CELL_MS,
        0u,
        0u,
        sample_1,
        &wrong_sequence_mask);
    CHECK(usable_mask == 0u &&
              wrong_sequence_mask == (uint16_t)(all_samples_mask & ~sample_1),
          "gap-only initiator pacing no longer demonstrates the sequence cascade");

    usable_mask = model_survey_sample_alignment(
        SURVEY_PAIR_SAMPLE_CELL_MS,
        SURVEY_PAIR_RESPONDER_WINDOW_MS,
        0u,
        sample_3,
        sample_1,
        &wrong_sequence_mask);
    CHECK(usable_mask == (1u << 1) &&
              (wrong_sequence_mask & ((1u << 3) | (1u << 4))) ==
                  ((1u << 3) | (1u << 4)),
          "full skew window on later samples no longer demonstrates tail consumption");

    usable_mask = model_survey_sample_alignment(
        SURVEY_PAIR_SAMPLE_CELL_MS,
        SURVEY_PAIR_SAMPLE_CELL_MS,
        100u,
        0u,
        sample_1,
        &wrong_sequence_mask);
    CHECK(usable_mask == 0u &&
              wrong_sequence_mask == (uint16_t)(all_samples_mask & ~sample_1),
          "local responder-entry jitter no longer demonstrates grid shift cascade");
}

static bool model_c5_interval_overlaps_pair_quiet_window(
    uint64_t rf_start_ms,
    uint64_t rf_end_ms,
    uint64_t pair_release_ms,
    uint16_t sample_count)
{
    const uint64_t preemption_lead_ms =
        SURVEY_DISCOVERY_TRANSPORT_PREEMPT_BUDGET_MS +
        SURVEY_PAIR_START_SKEW_MARGIN_MS;
    const uint64_t quiet_start_ms = pair_release_ms - preemption_lead_ms;
    const uint64_t quiet_end_ms = pair_release_ms +
        ((uint64_t)(sample_count - 1u) * SURVEY_PAIR_SAMPLE_CELL_MS) +
        SURVEY_PAIR_INITIATOR_TIMEOUT_MS;

    /* Radio reservations are half-open: ending exactly at the quiet boundary
     * is safe, while even one millisecond of complete wake/flood overlap is
     * rejected. */
    return rf_start_ms < quiet_end_ms && quiet_start_ms < rf_end_ms;
}

static uint64_t model_c5_start_after_pair_quiet_window(
    uint64_t requested_start_ms,
    uint64_t complete_airtime_ms,
    uint64_t pair_release_ms,
    uint16_t sample_count)
{
    const uint64_t quiet_end_ms = pair_release_ms +
        ((uint64_t)(sample_count - 1u) * SURVEY_PAIR_SAMPLE_CELL_MS) +
        SURVEY_PAIR_INITIATOR_TIMEOUT_MS;

    return model_c5_interval_overlaps_pair_quiet_window(
               requested_start_ms,
               requested_start_ms + complete_airtime_ms,
               pair_release_ms,
               sample_count) ?
        quiet_end_ms : requested_start_ms;
}

static void test_gateway_c5_quiet_window_preserves_immutable_pair_release(void)
{
    const uint16_t sample_count = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
    const uint64_t start_origin_ms = UINT64_C(5000);
    const uint64_t pair_release_ms =
        start_origin_ms + SURVEY_ROUND_START_EXECUTE_DELAY_MS;
    const uint64_t preemption_lead_ms =
        SURVEY_DISCOVERY_TRANSPORT_PREEMPT_BUDGET_MS +
        SURVEY_PAIR_START_SKEW_MARGIN_MS;
    const uint64_t quiet_start_ms = pair_release_ms - preemption_lead_ms;
    const uint64_t quiet_end_ms = pair_release_ms +
        ((uint64_t)(sample_count - 1u) * SURVEY_PAIR_SAMPLE_CELL_MS) +
        SURVEY_PAIR_INITIATOR_TIMEOUT_MS;
    const uint64_t complete_wake_flood_ms =
        MESH_RADIO_WAKE_TRAIN_MS + MESH_RADIO_EVENT_RETUNE_GUARD_MS +
        FLOOD_WAVE_MS;
    uint64_t scheduled_start_ms;
    uint64_t frozen_release_ms = pair_release_ms;

    CHECK(preemption_lead_ms == UINT64_C(2000),
          "pair Channel-5 cutoff drifted from derived endpoint preemption lead");
    CHECK(quiet_end_ms - pair_release_ms == UINT64_C(790),
          "pair Channel-5 quiet tail does not cover the final sample cell");
    CHECK(complete_wake_flood_ms <= preemption_lead_ms,
          "derived pair preemption lead cannot contain one complete wake/flood");

    CHECK(!model_c5_interval_overlaps_pair_quiet_window(
              quiet_start_ms - complete_wake_flood_ms,
              quiet_start_ms,
              pair_release_ms,
              sample_count),
          "complete Channel-5 RF ending at the cutoff was rejected");
    CHECK(model_c5_interval_overlaps_pair_quiet_window(
              quiet_start_ms - complete_wake_flood_ms + 1u,
              quiet_start_ms + 1u,
              pair_release_ms,
              sample_count),
          "wake/flood tail was allowed to enter the pair quiet interval");
    CHECK(model_c5_interval_overlaps_pair_quiet_window(
              pair_release_ms,
              pair_release_ms + complete_wake_flood_ms,
              pair_release_ms,
              sample_count),
          "Channel-5 RF was allowed during the fixed sample grid");
    CHECK(!model_c5_interval_overlaps_pair_quiet_window(
              quiet_end_ms,
              quiet_end_ms + complete_wake_flood_ms,
              pair_release_ms,
              sample_count),
          "Channel-5 RF after the final cell deadline was rejected");

    /* Model the delayed responder-START redrive seen in the F1DD trace. The
     * retry moves past the RF batch; its already-frozen execution release must
     * not move with the retry. */
    scheduled_start_ms = model_c5_start_after_pair_quiet_window(
        quiet_start_ms,
        complete_wake_flood_ms,
        pair_release_ms,
        sample_count);
    CHECK(scheduled_start_ms == quiet_end_ms,
          "delayed survey START redrive was not deferred past the RF batch");
    CHECK(frozen_release_ms == pair_release_ms &&
              pair_release_ms ==
                  start_origin_ms + SURVEY_ROUND_START_EXECUTE_DELAY_MS,
          "deferring a START redrive moved the immutable pair release");

    CHECK(SURVEY_ROUND_START_INITIATOR_SEND_CUTOFF_MS == UINT32_C(13000) &&
              survey_round_start_initiator_send_allowed(
                  (uint32_t)(quiet_start_ms - start_origin_ms - 1u)) &&
              !survey_round_start_initiator_send_allowed(
                  (uint32_t)(quiet_start_ms - start_origin_ms)) &&
              !survey_round_start_initiator_send_allowed(
                  (uint32_t)(quiet_start_ms - start_origin_ms + 1u)),
          "initiator START admission cutoff is not strict at pair preemption");
}

static int build_pair_prepare_control(struct mesh_outbound *control,
                                      uint64_t target_id,
                                      uint64_t next_hop_id,
                                      uint16_t seq)
{
    const struct survey_pair pair = {
        .operation_generation = SURVEY_OPERATION_GENERATION,
        .initiator_id = ANCHOR_ID,
        .responder_id = ANCHOR_2_ID,
        .survey_id = SURVEY_ID,
        .sample_count = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    };
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    size_t payload_len = 0u;
    int ret;

    if (control == NULL ||
        (target_id != pair.initiator_id && target_id != pair.responder_id)) {
        return PROTO_ERR_ARG;
    }
    memset(control, 0, sizeof(*control));
    ret = survey_append_pair_tlvs(control->payload, sizeof(control->payload),
                                  &payload_len, &pair);
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(
            control->payload, sizeof(control->payload), &payload_len,
            SURVEY_ROUND_ID);
    }
    if (ret == PROTO_OK) {
        proto_put_u16_le(round_commitment, SURVEY_ROUND_ID);
        ret = survey_round_commitment_append_tlv(
            control->payload, sizeof(control->payload), &payload_len,
            round_commitment);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_init_pair_prepare_packet(&control->packet, &pair, GATEWAY_ID,
                                          target_id, seq,
                                          (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
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
    CHECK(gateway_command_pending_claim_result(
              &pending,
              &result_out.packet,
              (uint32_t)(at_us / 1000u),
              NULL,
              NULL) ==
              GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED,
          "physically delivered 0x21 did not complete pending 0x52");
    CHECK(!pending.active,
          "completed pair-control waiter remained active");
}

static void test_pair_prepare_phr_and_complete_airtime_sweep(void)
{
    static const size_t frame_lengths[] = {124u, 125u, 126u};

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

struct followup_opportunity_result {
    size_t trigger_mismatches[2];
    size_t control_wakes_decoded[2];
    size_t controls_decoded[2];
    size_t controls_rejected[2];
};

static int build_pair_start_control(struct mesh_outbound *control,
                                    uint64_t target_id,
                                    uint16_t seq)
{
    const struct survey_pair pair = {
        .initiator_id = ANCHOR_ID,
        .responder_id = ANCHOR_2_ID,
        .survey_id = SURVEY_ID,
        .sample_count = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
    };
    size_t payload_len = 0u;
    int ret;

    if (control == NULL ||
        (target_id != pair.initiator_id && target_id != pair.responder_id)) {
        return PROTO_ERR_ARG;
    }
    memset(control, 0, sizeof(*control));
    ret = mesh_append_command_id(control->payload,
                                 sizeof(control->payload),
                                 &payload_len,
                                 CMD_SURVEY_START_PAIR);
    if (ret == PROTO_OK) {
        ret = survey_append_pair_tlvs(control->payload,
                                      sizeof(control->payload),
                                      &payload_len,
                                      &pair);
    }
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    control->packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID,
        .dst_id = target_id,
        .session_id = SURVEY_ID,
        .seq = seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = (uint16_t)payload_len,
    };
    control->payload_len = (uint16_t)payload_len;
    control->next_hop_id = target_id;
    control->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static bool encode_control_followup_wake_from(uint64_t sender_id,
                                              uint16_t sequence,
                                              uint16_t claimed_duration_ms,
                                              uint8_t *frame,
                                              size_t frame_cap,
                                              size_t *frame_len)
{
    const uint8_t flags =
        FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
    const struct uwb_wake_claim_frame claim = {
        .network_id = UINT32_C(0x494d4543),
        .clicker_id = sender_id,
        .click_event_id = sequence,
        .attempt_index = 1u,
        .priority_id = sender_id,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 5u,
        .discovery_starts_in_ms = 5u,
        .claimed_duration_ms = claimed_duration_ms,
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .nonce = UINT64_C(0x0102030405060708),
        .flags = flags,
    };

    CHECK(app_mesh_c5_wake_followup_is_control(flags),
          "modeled wake is not a CONTROL_FOLLOWUP");
    CHECK(!app_mesh_c5_wake_claim_preempts_mesh(flags),
          "modeled non-click CONTROL_FOLLOWUP acquired click priority");
    CHECK(app_mesh_c5_wake_followup_uses_extended_phr(flags),
          "modeled control wake does not select an extended-PHR follower");
    return uwb_encode_wake_claim(&claim, frame, frame_cap, frame_len) ==
           PROTO_OK;
}

static bool encode_control_followup_wake(uint16_t sequence,
                                         uint8_t *frame,
                                         size_t frame_cap,
                                         size_t *frame_len)
{
    return encode_control_followup_wake_from(GATEWAY_ID,
                                             sequence,
                                             20u,
                                             frame,
                                             frame_cap,
                                             frame_len);
}

static void test_forced_control_followup_covers_relay_gap(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound control;
    uint8_t direct_wake[UWB_WAKE_CLAIM_LEN] = {0};
    uint8_t relay_wake[UWB_WAKE_CLAIM_LEN] = {0};
    uint8_t control_frame[PACKET_EXT_MAX_LEN] = {0};
    size_t direct_wake_len = 0u;
    size_t relay_wake_len = 0u;
    size_t control_frame_len = 0u;
    uint8_t gateway = UINT8_MAX;
    uint8_t relay = UINT8_MAX;
    uint8_t forced = UINT8_MAX;
    uint16_t direct_tx = UINT16_MAX;
    uint16_t control_tx = UINT16_MAX;
    uint16_t relay_tx = UINT16_MAX;
    uint16_t relay_control_tx = UINT16_MAX;
    uint64_t direct_end_us;
    uint64_t control_start_us;
    uint64_t relay_start_us;
    uint64_t relay_end_us;
    uint64_t relay_accepted_at_us = 0u;
    uint64_t exclusive_listener_end_us;
    uint64_t followup_end_us;
    uint64_t ordinary_scan_start_us;
    size_t direct_wakes_decoded = 0u;
    size_t direct_payloads_decoded = 0u;
    size_t relay_wakes_decoded = 0u;
    size_t relay_payloads_decoded = 0u;
    size_t boosted_retries_during_exclusive = 0u;
    const uint8_t control_flags =
        FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
        FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;

    mesh_sim_init(&world, UINT32_C(0xf0c53400));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK,
          "forced-followup gateway setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay) == MESH_SIM_OK,
          "forced-followup relay setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &forced) == MESH_SIM_OK,
          "forced-followup target setup failed");
    CHECK(mesh_sim_set_link(&world, gateway, forced, 100u, 0u) == MESH_SIM_OK &&
              mesh_sim_set_link(&world, relay, forced, 100u, 0u) == MESH_SIM_OK,
          "forced-followup links failed");
    CHECK(encode_control_followup_wake_from(
              GATEWAY_ID, 201u, MODELED_FORCED_CONTROL_CLAIM_MS,
              direct_wake, sizeof(direct_wake), &direct_wake_len) &&
              encode_control_followup_wake_from(
                  ANCHOR_2_ID, 201u, MODELED_FORCED_CONTROL_CLAIM_MS,
                  relay_wake, sizeof(relay_wake), &relay_wake_len),
          "forced-followup wake encoding failed");
    CHECK(build_pair_start_control(&control, ANCHOR_ID, 201u) == PROTO_OK &&
              proto_packet_encode(&control.packet,
                                  control.payload,
                                  control_frame,
                                  sizeof(control_frame),
                                  &control_frame_len) == PROTO_OK,
          "forced-followup direct payload encoding failed");

    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   gateway,
                                   TX_START_US,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   direct_wake,
                                   direct_wake_len,
                                   false,
                                   &direct_tx) == MESH_SIM_OK,
          "forced-followup direct wake scheduling failed");
    direct_end_us = world.transmissions[direct_tx].end_us;
    control_start_us = direct_end_us +
                       MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS * 1000u;
    relay_start_us = direct_end_us +
                     MODELED_FORCED_CONTROL_RELAY_DELAY_MS * 1000u;
    followup_end_us = direct_end_us +
                      MODELED_FORCED_CONTROL_CLAIM_MS * 1000u;
    ordinary_scan_start_us = direct_end_us +
                             MODELED_LOW_DUTY_RESCHEDULE_MS * 1000u;

    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   gateway,
                                   control_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   control_frame,
                                   control_frame_len,
                                   false,
                                   &control_tx) == MESH_SIM_OK,
          "forced-followup direct payload scheduling failed");
    world.transmissions[control_tx].protocol_msg_type =
        control.packet.msg_type;
    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   relay,
                                   relay_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   relay_wake,
                                   relay_wake_len,
                                   false,
                                   &relay_tx) == MESH_SIM_OK,
          "forced-followup relay wake scheduling failed");
    relay_end_us = world.transmissions[relay_tx].end_us;
    CHECK(relay_end_us < ordinary_scan_start_us &&
              ordinary_scan_start_us + MODELED_LOW_DUTY_RX_US <
                  followup_end_us,
          "relay copy no longer occupies the measured 380 ms low-duty hole");

    CHECK(mesh_sim_schedule_rx(&world,
                               forced,
                               TX_START_US,
                               direct_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE,
                               NULL) == MESH_SIM_OK,
          "forced-followup initial Channel-5 RX scheduling failed");
    for (uint64_t slice_start_us =
             direct_end_us + MODELED_FOLLOWUP_SCAN_MS * 1000u;
         slice_start_us < followup_end_us;
         slice_start_us += 2u * MODELED_FOLLOWUP_SCAN_MS * 1000u) {
        const uint64_t candidate_end_us =
            slice_start_us + MODELED_FOLLOWUP_SCAN_MS * 1000u;
        const uint64_t slice_end_us =
            candidate_end_us < followup_end_us ? candidate_end_us :
                                                  followup_end_us;

        CHECK(mesh_sim_schedule_rx(&world,
                                   forced,
                                   slice_start_us,
                                   slice_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   NULL) == MESH_SIM_OK,
              "forced-followup boosted Channel-5 slice scheduling failed");
        if (slice_start_us <= relay_start_us &&
            relay_end_us <= slice_end_us) {
            relay_accepted_at_us = slice_end_us;
            break;
        }
    }
    CHECK(relay_accepted_at_us != 0u,
          "boosted scan did not contain the complete allowed relay wake");
    if (relay_accepted_at_us == 0u) {
        return;
    }
    exclusive_listener_end_us = relay_accepted_at_us +
                                MODELED_DEEP_RELAY_LISTEN_MS * 1000u;
    CHECK(mesh_sim_schedule_rx(&world,
                               forced,
                               relay_accepted_at_us,
                               exclusive_listener_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                               NULL) == MESH_SIM_OK,
          "allowed relay wake did not acquire the exclusive control listener");
    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   relay,
                                   relay_accepted_at_us +
                                       MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS *
                                           1000u,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   control_frame,
                                   control_frame_len,
                                   false,
                                   &relay_control_tx) == MESH_SIM_OK,
          "forced-followup relayed payload scheduling failed");
    world.transmissions[relay_control_tx].protocol_msg_type =
        control.packet.msg_type;
    for (size_t i = 0u; i < world.rx_window_count; i++) {
        const struct mesh_sim_rx_window *window = &world.rx_windows[i];

        if (window->valid && window->node_index == forced &&
            window->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
            window->start_us >= relay_accepted_at_us &&
            window->start_us < exclusive_listener_end_us) {
            boosted_retries_during_exclusive++;
        }
    }
    CHECK(relay_accepted_at_us + MODELED_FOLLOWUP_SCAN_MS * 1000u <
              exclusive_listener_end_us,
          "modeled boosted retry no longer falls inside exclusive ownership");
    CHECK(boosted_retries_during_exclusive == 0u,
          "accepted relay wake left boosted scan retries inside exclusive ownership");
    CHECK(mesh_sim_run_until(&world, exclusive_listener_end_us + 1u) ==
              MESH_SIM_OK,
          "forced-followup PHY simulation failed");

    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &world.receptions[i];

        if (rx->receiver_id != ANCHOR_ID) {
            continue;
        }
        if (rx->source_id == GATEWAY_ID &&
            rx->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
            rx->outcome == MESH_SIM_RX_DECODED) {
            direct_wakes_decoded++;
        } else if (rx->source_id == GATEWAY_ID &&
                   rx->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL &&
                   rx->outcome == MESH_SIM_RX_DECODED) {
            direct_payloads_decoded++;
        } else if (rx->source_id == ANCHOR_2_ID &&
                   rx->phy == MESH_SIM_PHY_CHANNEL5_WAKE &&
                   rx->outcome == MESH_SIM_RX_DECODED) {
            relay_wakes_decoded++;
        } else if (rx->source_id == ANCHOR_2_ID &&
                   rx->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL &&
                   rx->outcome == MESH_SIM_RX_DECODED) {
            relay_payloads_decoded++;
        }
    }

    CHECK(direct_wakes_decoded == 1u,
          "forced target did not physically decode the direct gateway wake");
    CHECK(direct_payloads_decoded == 0u,
          "boosted standard scanner decoded the direct extended payload");
    CHECK(relay_wakes_decoded == 1u,
          "bounded boosted scanner missed the 340 ms relay wake");
    CHECK(relay_payloads_decoded == 1u,
          "exclusive listener missed the allowed relayed control payload");
    CHECK(!app_mesh_c5_route_wake_claim_allowed(
              GATEWAY_ID, GATEWAY_ID, control_flags, false, true) &&
              app_mesh_c5_connected_gap_rx_action(false, false, false) ==
                  APP_MESH_C5_CONNECTED_GAP_RX_CONTINUE,
          "forced target did not ignore the direct gateway wake and continue");
    CHECK(!app_mesh_c5_gateway_control_copy_allowed(
              GATEWAY_ID, GATEWAY_ID, GATEWAY_ID, true) &&
              app_mesh_c5_gateway_control_copy_allowed(
                  GATEWAY_ID, ANCHOR_2_ID, GATEWAY_ID, true),
          "forced target direct/relayed payload admission drifted");
    CHECK(app_mesh_c5_route_wake_claim_allowed(
              ANCHOR_2_ID, GATEWAY_ID, control_flags, false, true) &&
              app_mesh_c5_connected_gap_rx_action(false, true, false) ==
                  APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL,
          "forced target did not admit the allowed relay wake");
    CHECK(app_mesh_c5_connected_gap_rx_action(true, true, false) ==
              APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK,
          "a click claim did not retain priority over an allowed relay wake");
}

struct post_ch9_recovery_model {
    bool channel9_owner_active;
    bool recovery_pending;
    uint8_t completed_owner_windows;
    uint8_t worker_conflicts_observed;
    uint8_t attempted_scans;
};

static void model_post_ch9_owner_begin(struct post_ch9_recovery_model *model)
{
    CHECK(model != NULL && !model->channel9_owner_active,
          "post-Channel-9 model acquired overlapping owner windows");
    if (model != NULL) {
        model->channel9_owner_active = true;
    }
}

static void model_post_ch9_owner_release(struct post_ch9_recovery_model *model)
{
    CHECK(model != NULL && model->channel9_owner_active,
          "post-Channel-9 model released an inactive owner");
    if (model != NULL) {
        model->channel9_owner_active = false;
        model->recovery_pending = true;
        model->completed_owner_windows++;
    }
}

static uint32_t model_post_ch9_scan_window_ms(
    struct post_ch9_recovery_model *model)
{
    if (model == NULL) {
        return 0u;
    }
    if (model->channel9_owner_active) {
        model->worker_conflicts_observed++;
        return 0u;
    }
    return model->recovery_pending ? MODELED_FOLLOWUP_SCAN_MS :
                                     (uint32_t)(MODELED_LOW_DUTY_RX_US / 1000u);
}

static void model_post_ch9_scan_complete(
    struct post_ch9_recovery_model *model,
    bool rx_attempted)
{
    if (model != NULL && rx_attempted) {
        model->attempted_scans++;
        model->recovery_pending = false;
    }
}

static void test_post_channel9_recovery_scan_captures_live_wake_train(void)
{
    static struct mesh_sim_world world;
    struct post_ch9_recovery_model recovery = {0};
    uint8_t wake_frame[UWB_WAKE_CLAIM_LEN] = {0};
    size_t wake_frame_len = 0u;
    uint8_t sender = UINT8_MAX;
    uint8_t old_receiver = UINT8_MAX;
    uint8_t recovery_receiver = UINT8_MAX;
    uint16_t wake_tx = UINT16_MAX;
    const uint64_t first_ch9_owner_start_us = UINT64_C(740000);
    const uint64_t first_ch9_owner_end_us =
        first_ch9_owner_start_us +
        MODELED_CH9_OWNER_MS * UINT64_C(1000);
    const uint64_t second_ch9_owner_start_us =
        first_ch9_owner_end_us +
        MODELED_INTER_CH9_C5_GAP_MS * UINT64_C(1000);
    const uint64_t second_ch9_owner_end_us =
        second_ch9_owner_start_us +
        MODELED_CH9_OWNER_MS * UINT64_C(1000);
    const uint64_t scan_start_us =
        second_ch9_owner_end_us +
        MODELED_POST_CH9_RELEASE_DELAY_MS * UINT64_C(1000);
    const uint64_t wake_start_us =
        scan_start_us +
        MODELED_WAKE_AFTER_RECOVERY_SCAN_START_MS * UINT64_C(1000);
    uint32_t deferred_scan_ms;
    uint32_t recovery_scan_ms;
    size_t physical_ch9_owner_windows = 0u;
    size_t old_decoded = 0u;
    size_t recovery_decoded = 0u;

    mesh_sim_init(&world, UINT32_C(0xf0c53402));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &sender) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &old_receiver) ==
                  MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_3_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &recovery_receiver) ==
                  MESH_SIM_OK,
          "post-Channel-9 recovery roles failed");
    CHECK(mesh_sim_set_link(&world, sender, old_receiver, 100u, 0u) ==
                  MESH_SIM_OK &&
              mesh_sim_set_link(&world, sender, recovery_receiver, 100u, 0u) ==
                  MESH_SIM_OK,
          "post-Channel-9 recovery links failed");
    CHECK(first_ch9_owner_end_us +
                  MODELED_INTER_CH9_C5_GAP_MS * UINT64_C(1000) ==
              second_ch9_owner_start_us,
          "post-Channel-9 owner windows lost the exact 60 ms retune plus 20 ms receive gap");
    CHECK(mesh_sim_schedule_rx(&world,
                               recovery_receiver,
                               first_ch9_owner_start_us,
                               first_ch9_owner_end_us,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   recovery_receiver,
                                   second_ch9_owner_start_us,
                                   second_ch9_owner_end_us,
                                   UWB_CHANNEL_MESH_PAYLOAD,
                                   MESH_SIM_PHY_CHANNEL9_MESH,
                                   NULL) == MESH_SIM_OK,
          "post-Channel-9 physical owner scheduling failed");

    CHECK(mesh_sim_run_until(&world, first_ch9_owner_start_us) == MESH_SIM_OK,
          "first Channel-9 owner did not start");
    model_post_ch9_owner_begin(&recovery);
    CHECK(mesh_sim_run_until(&world, first_ch9_owner_end_us) == MESH_SIM_OK,
          "first Channel-9 owner did not finish");
    model_post_ch9_owner_release(&recovery);
    CHECK(recovery.recovery_pending,
          "first Channel-9 release did not retain recovery debt");

    CHECK(mesh_sim_run_until(&world, second_ch9_owner_start_us) == MESH_SIM_OK,
          "second Channel-9 owner did not start");
    model_post_ch9_owner_begin(&recovery);
    CHECK(recovery.recovery_pending,
          "back-to-back Channel-9 owner cleared pending recovery");
    CHECK(mesh_sim_run_until(&world, second_ch9_owner_end_us) == MESH_SIM_OK,
          "second Channel-9 owner did not finish");
    model_post_ch9_owner_release(&recovery);
    CHECK(recovery.completed_owner_windows == 2u &&
              recovery.recovery_pending,
          "two completed Channel-9 owners did not retain one recovery scan");

    deferred_scan_ms = model_post_ch9_scan_window_ms(&recovery);
    model_post_ch9_scan_complete(&recovery, false);
    CHECK(deferred_scan_ms == MODELED_FOLLOWUP_SCAN_MS &&
              recovery.recovery_pending &&
              recovery.attempted_scans == 0u,
          "a non-attempted post-Channel-9 worker consumed recovery debt");
    CHECK(scan_start_us > second_ch9_owner_end_us,
          "recovery worker still overlaps a live Channel-9 owner");
    recovery_scan_ms = model_post_ch9_scan_window_ms(&recovery);
    CHECK(recovery_scan_ms == MODELED_FOLLOWUP_SCAN_MS &&
              recovery.worker_conflicts_observed == 0u,
          "first released scan depended on observing a live Channel-9 conflict");

    CHECK(encode_control_followup_wake_from(
              ANCHOR_2_ID, 401u, MODELED_FORCED_CONTROL_CLAIM_MS,
              wake_frame, sizeof(wake_frame), &wake_frame_len),
          "post-Channel-9 recovery wake encoding failed");
    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   sender,
                                   wake_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   wake_frame,
                                   wake_frame_len,
                                   false,
                                   &wake_tx) == MESH_SIM_OK,
          "post-Channel-9 recovery wake scheduling failed");
    CHECK(mesh_sim_schedule_rx(&world,
                               old_receiver,
                               scan_start_us,
                               scan_start_us + MODELED_LOW_DUTY_RX_US,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE,
                               NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(
                  &world,
                  recovery_receiver,
                  scan_start_us,
                  scan_start_us + recovery_scan_ms * UINT64_C(1000),
                  UWB_CHANNEL_WAKE_CONTACT,
                  MESH_SIM_PHY_CHANNEL5_WAKE,
                  NULL) == MESH_SIM_OK,
          "post-Channel-9 recovery scan scheduling failed");
    CHECK(world.transmissions[wake_tx].end_us <
              scan_start_us + MODELED_FOLLOWUP_SCAN_MS * UINT64_C(1000),
          "20 ms recovery scan no longer contains the complete wake frame");
    CHECK(mesh_sim_run_until(
              &world,
              scan_start_us + MODELED_FOLLOWUP_SCAN_MS * UINT64_C(1000) + 1u) ==
              MESH_SIM_OK,
          "post-Channel-9 recovery simulation failed");
    model_post_ch9_scan_complete(&recovery, true);

    for (size_t i = 0u; i < world.rx_window_count; i++) {
        const struct mesh_sim_rx_window *window = &world.rx_windows[i];

        if (window->valid && window->node_index == recovery_receiver &&
            window->phy == MESH_SIM_PHY_CHANNEL9_MESH) {
            physical_ch9_owner_windows++;
        }
    }

    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &world.receptions[i];

        if (rx->source_id != ANCHOR_2_ID ||
            rx->outcome != MESH_SIM_RX_DECODED) {
            continue;
        }
        if (rx->receiver_id == ANCHOR_ID) {
            old_decoded++;
        } else if (rx->receiver_id == ANCHOR_3_ID) {
            recovery_decoded++;
        }
    }

    CHECK(old_decoded == 0u,
          "observed 3 ms post-Channel-9 scan unexpectedly decoded the later wake");
    CHECK(recovery_decoded == 1u,
          "20 ms post-Channel-9 recovery scan missed the live wake train");
    CHECK(physical_ch9_owner_windows == 2u,
          "scenario did not execute two real Channel-9 owner windows");
    CHECK(recovery.attempted_scans == 1u && !recovery.recovery_pending,
          "real attempted recovery scan did not consume the pending bit once");
}

static uint32_t model_two_connection_gap_scan_ms(uint32_t event_interval_ms)
{
    uint32_t half_phase_ms;
    uint32_t physical_gap_ms;

    if ((event_interval_ms & 1u) != 0u) {
        return 0u;
    }
    half_phase_ms = event_interval_ms / 2u;
    if (half_phase_ms <= MODELED_CH9_OWNER_MS) {
        return 0u;
    }
    physical_gap_ms = half_phase_ms - MODELED_CH9_OWNER_MS;
    return app_mesh_c5_connected_gap_window_ms(
        &(const struct app_mesh_c5_connected_gap_timing) {
            .next_channel9_delay_ms = physical_gap_ms,
            .scan_cap_ms = MODELED_FOLLOWUP_SCAN_MS,
            .min_scan_ms = MODELED_FOLLOWUP_SCAN_MS,
            .retune_margin_ms = MODELED_CONNECTED_GAP_RETUNE_MS,
        });
}

static void test_retained_ch9_retry_owner_keeps_survey_control_gap(void)
{
    static const struct survey_discovery_config config = {
        .survey_id = SURVEY_ID,
        .operation_generation = SURVEY_OPERATION_GENERATION,
        .start_delay_ms = 90000u,
        .slot_ms = 40u,
        .slot_count = 6u,
        .round_count = 4u,
    };
    static struct mesh_sim_world world;
    struct route_candidate parent_route = {
        .next_hop_id = ANCHOR_2_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = 1u,
        .last_success_ms = 1u,
        .hop_count = 1u,
        .link_quality = 94u,
        .valid = true,
    };
    struct mesh_outbound retry_owner = {0};
    struct mesh_pending_tx pending_before;
    struct proto_packet survey_start = {0};
    struct fw_radio_activity_capture capture = {
        .relay_tx_active = true,
        .ch9_ack_wait_active = true,
        .c5_tx_intent = FW_C5_TX_INTENT_BACKGROUND,
    };
    struct fw_radio_activity_runtime activity_runtime;
    struct fw_radio_activity_decision decision;
    uint8_t command_result_payload[32] = {0};
    uint8_t survey_payload[96] = {0};
    uint8_t survey_frame[PACKET_EXT_MAX_LEN] = {0};
    uint8_t wake_frame[UWB_WAKE_CLAIM_LEN] = {0};
    size_t command_result_payload_len = 0u;
    size_t survey_payload_len = 0u;
    size_t survey_frame_len = 0u;
    size_t wake_frame_len = 0u;
    uint8_t relay = UINT8_MAX;
    uint8_t forced = UINT8_MAX;
    uint16_t object_index = UINT16_MAX;
    const uint64_t first_owner_start_us = UINT64_C(10000);
    const uint64_t first_owner_end_us =
        first_owner_start_us + MODELED_CH9_OWNER_MS * UINT64_C(1000);
    const uint64_t first_scan_start_us =
        first_owner_end_us +
        MODELED_CONNECTED_GAP_RETUNE_MS * UINT64_C(1000);
    const uint64_t first_scan_end_us =
        first_scan_start_us + MODELED_FOLLOWUP_SCAN_MS * UINT64_C(1000);
    const uint64_t second_owner_start_us =
        first_owner_start_us +
        (MESH_RADIO_EVENT_INTERVAL_MS / 2u) * UINT64_C(1000);
    const uint64_t second_owner_end_us =
        second_owner_start_us + MODELED_CH9_OWNER_MS * UINT64_C(1000);
    const uint64_t second_scan_start_us =
        second_owner_end_us +
        MODELED_CONNECTED_GAP_RETUNE_MS * UINT64_C(1000);
    const uint64_t second_scan_end_us =
        second_scan_start_us + MODELED_FOLLOWUP_SCAN_MS * UINT64_C(1000);
    const uint64_t wake_start_us = first_scan_end_us + 1u;
    const uint64_t wake_end_us =
        wake_start_us + MESH_RADIO_WAKE_TRAIN_MS * UINT64_C(1000);
    uint64_t wake_tx_us;
    uint64_t payload_tx_us;
    uint64_t payload_listener_end_us;
    uint32_t production_scan_ms;
    uint32_t wake_airtime_us;
    size_t wake_decoded = 0u;
    size_t survey_payloads_decoded = 0u;
    bool state_changed = false;

    CHECK(model_two_connection_gap_scan_ms(
              MODELED_LEGACY_EVENT_INTERVAL_MS) == 0u,
          "legacy 520 ms cadence unexpectedly contains retune plus C5 RX");
    production_scan_ms = model_two_connection_gap_scan_ms(
        MESH_RADIO_EVENT_INTERVAL_MS);
    CHECK(production_scan_ms == MODELED_FOLLOWUP_SCAN_MS,
          "production cadence does not expose the exact 20 ms C5 scan");

    /* A full wake beginning just after one scan still contains the next
     * complete scan for every possible phase of the two-link cadence. */
    for (uint32_t phase_us = 0u;
         phase_us < (MESH_RADIO_EVENT_INTERVAL_MS / 2u) * 1000u;
         phase_us++) {
        const uint64_t next_scan_us = phase_us == 0u ? 0u :
            (MESH_RADIO_EVENT_INTERVAL_MS / 2u) * UINT64_C(1000);

        CHECK(next_scan_us +
                  MODELED_FOLLOWUP_SCAN_MS * UINT64_C(1000) <=
              (uint64_t)phase_us +
                  MESH_RADIO_WAKE_TRAIN_MS * UINT64_C(1000),
              "400 ms wake lost its guaranteed complete connected-gap scan");
        if (failures != 0u) {
            break;
        }
    }

    mesh_sim_init(&world, UINT32_C(0xf0c56400));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &forced) ==
                  MESH_SIM_OK,
          "retained-owner control-gap roles failed");
    CHECK(mesh_sim_set_link(&world, relay, forced, 100u, 0u) == MESH_SIM_OK,
          "retained-owner control-gap link failed");
    CHECK(route_upsert_candidate(&world.roles[forced].relay.upstream,
                                 &parent_route) == PROTO_OK,
          "retained owner parent route setup failed");
    CHECK(mesh_append_command_result(command_result_payload,
                                     sizeof(command_result_payload),
                                     &command_result_payload_len,
                                     CMD_SURVEY_PREPARE_PAIR,
                                     COMMAND_OK,
                                     0u) == PROTO_OK &&
              mesh_init_command_result(&retry_owner.packet,
                                       ANCHOR_ID,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       UINT16_C(0x6401),
                                       (uint8_t)command_result_payload_len,
                                       true) == PROTO_OK,
          "retained command-result owner setup failed");
    memcpy(retry_owner.payload,
           command_result_payload,
           command_result_payload_len);
    retry_owner.payload_len = (uint16_t)command_result_payload_len;
    CHECK(mesh_relay_start_tx(&world.roles[forced].relay,
                              &retry_owner.packet,
                              retry_owner.payload,
                              retry_owner.payload_len,
                              1u,
                              &retry_owner) == PROTO_OK,
          "retained command-result owner did not acquire custody");
    world.roles[forced].relay.pending.radio_channel =
        UWB_CHANNEL_MESH_PAYLOAD;
    world.roles[forced].relay.pending.state =
        MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    world.roles[forced].relay.pending.retry_after_ms = UINT32_C(0x12345678);
    pending_before = world.roles[forced].relay.pending;

    fw_radio_activity_runtime_init(&activity_runtime);
    CHECK(fw_radio_activity_decide(&capture,
                                   &activity_runtime,
                                   &decision,
                                   &state_changed) == 0 &&
              decision.state == FW_RADIO_ACTIVITY_MESH_RX &&
              !decision.c5_tx_allowed && decision.uwb_rx_allowed,
          "generic C5 received a blanket live-ACK bypass");
    capture.c5_tx_intent = FW_C5_TX_INTENT_GATEWAY_SURVEY_CONTROL;
    CHECK(fw_radio_activity_decide(&capture,
                                   &activity_runtime,
                                   &decision,
                                   &state_changed) == 0 &&
              decision.c5_tx_allowed,
          "validated gateway survey control lost its narrow ACK exception");

    CHECK(survey_append_discovery_start_tlvs(
              survey_payload,
              sizeof(survey_payload),
              &survey_payload_len,
              &config) == PROTO_OK &&
              survey_init_discovery_start_packet(
                  &survey_start,
                  GATEWAY_ID,
                  &config,
                  UINT16_C(0x6402),
                  (uint8_t)survey_payload_len) == PROTO_OK &&
              proto_packet_encode(&survey_start,
                                  survey_payload,
                                  survey_frame,
                                  sizeof(survey_frame),
                                  &survey_frame_len) == PROTO_OK &&
              encode_control_followup_wake_from(
                  ANCHOR_2_ID,
                  survey_start.seq,
                  MESH_RADIO_WAKE_TRAIN_MS,
                  wake_frame,
                  sizeof(wake_frame),
                  &wake_frame_len),
          "exact relayed survey-start wave setup failed");
    CHECK(survey_start.msg_type == MSG_SURVEY_DISCOVERY_START &&
              survey_start_tx_phy(survey_frame_len) ==
                  MESH_SIM_PHY_CHANNEL5_MESH_CONTROL &&
              app_mesh_c5_route_wake_claim_allowed(
                  ANCHOR_2_ID,
                  GATEWAY_ID,
                  FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
                      FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY,
                  false,
                  true) &&
              app_mesh_c5_connected_gap_rx_action(false, true, false) ==
                  APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_ROUTE_CONTROL,
          "modeled control is not an exact allowed relayed 0x54");

    CHECK(mesh_sim_schedule_rx(&world,
                               forced,
                               first_owner_start_us,
                               first_owner_end_us,
                               UWB_CHANNEL_MESH_PAYLOAD,
                               MESH_SIM_PHY_CHANNEL9_MESH,
                               NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   forced,
                                   second_owner_start_us,
                                   second_owner_end_us,
                                   UWB_CHANNEL_MESH_PAYLOAD,
                                   MESH_SIM_PHY_CHANNEL9_MESH,
                                   NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   forced,
                                   first_scan_start_us,
                                   first_scan_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   forced,
                                   second_scan_start_us,
                                   second_scan_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   NULL) == MESH_SIM_OK,
          "retained-owner CH9/C5 reservation scheduling failed");

    wake_airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_WAKE, wake_frame_len);
    CHECK((2u * wake_airtime_us) +
              (uint32_t)MODELED_WAKE_REPEAT_GAP_US <=
              MODELED_FOLLOWUP_SCAN_MS * 1000u,
          "20 ms connected-gap scan cannot contain a worst-aligned wake copy");
    wake_tx_us = wake_start_us;
    while (wake_tx_us + wake_airtime_us <= wake_end_us) {
        CHECK(mesh_sim_schedule_raw_tx(&world,
                                       relay,
                                       wake_tx_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_WAKE,
                                       wake_frame,
                                       wake_frame_len,
                                       false,
                                       &object_index) == MESH_SIM_OK,
              "bounded control wake copy scheduling failed");
        world.transmissions[object_index].protocol_msg_type =
            MSG_UWB_WAKE_CLAIM;
        wake_tx_us = world.transmissions[object_index].end_us +
                     MODELED_WAKE_REPEAT_GAP_US;
    }

    payload_tx_us = wake_end_us +
                    MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS * UINT64_C(1000);
    for (uint8_t copy = 0u; copy < 4u; copy++) {
        CHECK(mesh_sim_schedule_raw_tx(&world,
                                       relay,
                                       payload_tx_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                       survey_frame,
                                       survey_frame_len,
                                       false,
                                       &object_index) == MESH_SIM_OK,
              "survey-start payload copy scheduling failed");
        world.transmissions[object_index].protocol_msg_type =
            MSG_SURVEY_DISCOVERY_START;
        payload_tx_us = world.transmissions[object_index].end_us +
                        MODELED_WAKE_REPEAT_GAP_US;
    }
    payload_listener_end_us = payload_tx_us;
    CHECK(mesh_sim_schedule_rx(&world,
                               forced,
                               second_scan_end_us,
                               payload_listener_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                               NULL) == MESH_SIM_OK,
          "survey control follow-up listener scheduling failed");
    CHECK(mesh_sim_run_until(&world, payload_listener_end_us + 1u) ==
              MESH_SIM_OK,
          "retained-owner survey-control PHY simulation failed");

    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &world.receptions[i];

        if (rx->receiver_id != ANCHOR_ID ||
            rx->source_id != ANCHOR_2_ID ||
            rx->outcome != MESH_SIM_RX_DECODED) {
            continue;
        }
        if (rx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            wake_decoded++;
        } else if (rx->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) {
            survey_payloads_decoded++;
        }
    }
    CHECK(wake_decoded > 0u,
          "400 ms wake missed every legal two-link connected-gap scan");
    CHECK(survey_payloads_decoded == 4u,
          "allowed control follow-up did not receive all four exact 0x54 copies");
    CHECK(memcmp(&world.roles[forced].relay.pending,
                 &pending_before,
                 sizeof(pending_before)) == 0,
          "C5 survey observation mutated or lost retained CH9 retry custody");
    CHECK(app_mesh_c5_connected_gap_rx_action(false, false, false) ==
              APP_MESH_C5_CONNECTED_GAP_RX_CONTINUE,
          "generic C5 activity escaped the bounded observation loop");
}

static void test_forced_control_followup_reopens_for_deeper_relay(void)
{
    static struct mesh_sim_world world;
    struct mesh_outbound hop1_control;
    struct mesh_outbound hop2_control;
    uint8_t hop1_wake[UWB_WAKE_CLAIM_LEN] = {0};
    uint8_t hop2_wake[UWB_WAKE_CLAIM_LEN] = {0};
    uint8_t hop1_frame[PACKET_EXT_MAX_LEN] = {0};
    uint8_t hop2_frame[PACKET_EXT_MAX_LEN] = {0};
    size_t hop1_wake_len = 0u;
    size_t hop2_wake_len = 0u;
    size_t hop1_frame_len = 0u;
    size_t hop2_frame_len = 0u;
    uint8_t relay1 = UINT8_MAX;
    uint8_t relay2 = UINT8_MAX;
    uint8_t forced = UINT8_MAX;
    uint16_t hop1_wake_tx = UINT16_MAX;
    uint16_t hop1_payload_tx = UINT16_MAX;
    uint16_t hop2_wake_tx = UINT16_MAX;
    uint16_t hop2_payload_tx = UINT16_MAX;
    uint64_t hop1_wake_start_us = TX_START_US;
    uint64_t hop1_wake_end_us;
    uint64_t hop1_payload_start_us;
    uint64_t hop1_payload_end_us;
    uint64_t hop2_wake_start_us;
    uint64_t hop2_wake_end_us;
    uint64_t hop2_payload_start_us;
    uint64_t hop2_payload_end_us;
    uint64_t listen_end_us;
    size_t hop1_wakes_decoded = 0u;
    size_t hop1_payloads_decoded = 0u;
    size_t hop2_wakes_decoded = 0u;
    size_t hop2_payloads_decoded = 0u;

    mesh_sim_init(&world, UINT32_C(0xf0c53401));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay1) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_3_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &relay2) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &forced) == MESH_SIM_OK,
          "deep forced-followup role setup failed");
    CHECK(mesh_sim_set_link(&world, relay1, forced, 100u, 0u) == MESH_SIM_OK &&
              mesh_sim_set_link(&world, relay2, forced, 100u, 0u) == MESH_SIM_OK,
          "deep forced-followup links failed");
    CHECK(encode_control_followup_wake_from(
              ANCHOR_2_ID, 301u, MODELED_DEEP_RELAY_LISTEN_MS,
              hop1_wake, sizeof(hop1_wake), &hop1_wake_len) &&
              encode_control_followup_wake_from(
                  ANCHOR_3_ID, 302u, MODELED_DEEP_RELAY_LISTEN_MS,
                  hop2_wake, sizeof(hop2_wake), &hop2_wake_len),
          "deep forced-followup wake encoding failed");
    CHECK(build_pair_start_control(&hop1_control, ANCHOR_ID, 301u) == PROTO_OK,
          "deep forced-followup control setup failed");
    hop1_control.packet.ttl = (uint8_t)(MESH_DEFAULT_TTL - 1u);
    hop2_control = hop1_control;
    hop2_control.packet.ttl = (uint8_t)(MESH_DEFAULT_TTL - 2u);
    CHECK(proto_packet_encode(&hop1_control.packet,
                              hop1_control.payload,
                              hop1_frame,
                              sizeof(hop1_frame),
                              &hop1_frame_len) == PROTO_OK &&
              proto_packet_encode(&hop2_control.packet,
                                  hop2_control.payload,
                                  hop2_frame,
                                  sizeof(hop2_frame),
                                  &hop2_frame_len) == PROTO_OK,
          "deep forced-followup payload encoding failed");
    CHECK((uint8_t)(MESH_DEFAULT_TTL - hop1_control.packet.ttl) == 1u &&
              (uint8_t)(MESH_DEFAULT_TTL - hop2_control.packet.ttl) == 2u,
          "deep forced-followup hop policy drifted");

    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   relay1,
                                   hop1_wake_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   hop1_wake,
                                   hop1_wake_len,
                                   false,
                                   &hop1_wake_tx) == MESH_SIM_OK,
          "deep forced-followup first wake scheduling failed");
    hop1_wake_end_us = world.transmissions[hop1_wake_tx].end_us;
    hop1_payload_start_us = hop1_wake_start_us +
                            (MESH_RADIO_WAKE_TRAIN_MS +
                             MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS) * 1000u;
    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   relay1,
                                   hop1_payload_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   hop1_frame,
                                   hop1_frame_len,
                                   false,
                                   &hop1_payload_tx) == MESH_SIM_OK,
          "deep forced-followup first payload scheduling failed");
    hop1_payload_end_us = world.transmissions[hop1_payload_tx].end_us;

    hop2_wake_start_us = hop1_wake_start_us + FLOOD_WAVE_MS * 1000u;
    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   relay2,
                                   hop2_wake_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   hop2_wake,
                                   hop2_wake_len,
                                   false,
                                   &hop2_wake_tx) == MESH_SIM_OK,
          "deep forced-followup second wake scheduling failed");
    hop2_wake_end_us = world.transmissions[hop2_wake_tx].end_us;
    hop2_payload_start_us = hop2_wake_start_us +
                            (MESH_RADIO_WAKE_TRAIN_MS +
                             MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS) * 1000u;
    CHECK(mesh_sim_schedule_raw_tx(&world,
                                   relay2,
                                   hop2_payload_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   hop2_frame,
                                   hop2_frame_len,
                                   false,
                                   &hop2_payload_tx) == MESH_SIM_OK,
          "deep forced-followup second payload scheduling failed");
    hop2_payload_end_us = world.transmissions[hop2_payload_tx].end_us;
    listen_end_us = hop1_wake_start_us +
                    (MESH_RADIO_WAKE_TRAIN_MS +
                     MODELED_DEEP_RELAY_LISTEN_MS) * 1000u;

    CHECK(mesh_sim_schedule_rx(&world,
                               forced,
                               hop1_wake_start_us,
                               hop1_wake_end_us,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE,
                               NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   forced,
                                   hop1_payload_start_us,
                                   hop1_payload_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   forced,
                                   hop1_payload_end_us,
                                   hop2_wake_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   NULL) == MESH_SIM_OK &&
              mesh_sim_schedule_rx(&world,
                                   forced,
                                   hop2_payload_start_us,
                                   hop2_payload_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   NULL) == MESH_SIM_OK,
          "deep forced-followup PHY handoff scheduling failed");
    CHECK(hop2_payload_end_us < listen_end_us,
          "deep forced-followup second payload escaped the bounded listener");
    CHECK(!mesh_sim_phy_decode_compatible(
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              MESH_SIM_PHY_CHANNEL5_WAKE),
          "extended listener could decode the second standard wake");
    CHECK(mesh_sim_run_until(&world, listen_end_us + 1u) == MESH_SIM_OK,
          "deep forced-followup PHY simulation failed");

    for (size_t i = 0u; i < world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &world.receptions[i];

        if (rx->receiver_id != ANCHOR_ID ||
            rx->outcome != MESH_SIM_RX_DECODED) {
            continue;
        }
        if (rx->source_id == ANCHOR_2_ID &&
            rx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            hop1_wakes_decoded++;
        } else if (rx->source_id == ANCHOR_2_ID &&
                   rx->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) {
            hop1_payloads_decoded++;
        } else if (rx->source_id == ANCHOR_3_ID &&
                   rx->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            hop2_wakes_decoded++;
        } else if (rx->source_id == ANCHOR_3_ID &&
                   rx->phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) {
            hop2_payloads_decoded++;
        }
    }

    CHECK(hop1_wakes_decoded == 1u && hop1_payloads_decoded == 1u,
          "forced target did not decode and reject the first relay copy");
    CHECK(hop2_wakes_decoded == 1u && hop2_payloads_decoded == 1u,
          "forced target did not reopen standard PHY for the deeper relay copy");
}

static bool model_two_anchor_control_opportunity(
    struct mesh_sim_world *world,
    uint8_t gateway,
    const uint8_t anchors[2],
    uint64_t *at_us,
    const struct mesh_outbound *control,
    uint32_t turnaround_ms,
    bool control_listener_holds_extended,
    bool truncate_initiator_control,
    struct followup_opportunity_result *result)
{
    uint8_t wake_frame[UWB_WAKE_CLAIM_LEN] = {0};
    uint8_t control_frame[PACKET_EXT_MAX_LEN] = {0};
    size_t wake_frame_len = 0u;
    size_t control_frame_len = 0u;
    size_t receptions_before;
    uint16_t trigger_tx = UINT16_MAX;
    uint16_t probe_tx = UINT16_MAX;
    uint16_t control_tx = UINT16_MAX;
    uint64_t trigger_start_us;
    uint64_t trigger_end_us;
    uint64_t probe_start_us;
    uint64_t probe_end_us;
    uint64_t control_start_us;
    uint64_t control_end_us;
    enum mesh_sim_phy control_rx_phy;

    if (world == NULL || anchors == NULL || at_us == NULL ||
        control == NULL || result == NULL ||
        !encode_control_followup_wake(control->packet.seq,
                                      wake_frame,
                                      sizeof(wake_frame),
                                      &wake_frame_len) ||
        proto_packet_encode(&control->packet,
                            control->payload,
                            control_frame,
                            sizeof(control_frame),
                            &control_frame_len) != PROTO_OK) {
        CHECK(false, "control opportunity encoding failed");
        return false;
    }
    CHECK(app_mesh_c5_control_uses_extended_phr(
              control->packet.msg_type,
              control_frame_len,
              CHANNEL5_STANDARD_FRAME_MAX_LEN),
          "survey control copy did not select extended PHR");
    CHECK(!mesh_sim_phy_decode_compatible(
              MESH_SIM_PHY_CHANNEL5_WAKE,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL),
          "standard and extended Channel 5 PHRs became decode-compatible");

    receptions_before = world->reception_count;
    trigger_start_us = *at_us;
    CHECK(mesh_sim_schedule_raw_tx(world,
                                   gateway,
                                   trigger_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   wake_frame,
                                   wake_frame_len,
                                   false,
                                   &trigger_tx) == MESH_SIM_OK,
          "control wake trigger scheduling failed");
    trigger_end_us = world->transmissions[trigger_tx].end_us;
    for (uint8_t receiver = 0u; receiver < 2u; receiver++) {
        CHECK(mesh_sim_schedule_rx(world,
                                   anchors[receiver],
                                   trigger_start_us,
                                   trigger_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   NULL) == MESH_SIM_OK,
              "extended listener trigger scheduling failed");
    }

    probe_start_us = trigger_end_us + MODELED_WAKE_REPEAT_GAP_US;
    CHECK(mesh_sim_schedule_raw_tx(world,
                                   gateway,
                                   probe_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   wake_frame,
                                   wake_frame_len,
                                   false,
                                   &probe_tx) == MESH_SIM_OK,
          "control wake probe scheduling failed");
    probe_end_us = world->transmissions[probe_tx].end_us;
    for (uint8_t receiver = 0u; receiver < 2u; receiver++) {
        CHECK(mesh_sim_schedule_rx(world,
                                   anchors[receiver],
                                   probe_start_us,
                                   probe_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   NULL) == MESH_SIM_OK,
              "standard control-wake probe scheduling failed");
    }

    control_start_us = probe_end_us + (uint64_t)turnaround_ms * 1000u;
    CHECK(mesh_sim_schedule_raw_tx(world,
                                   gateway,
                                   control_start_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                   control_frame,
                                   control_frame_len,
                                   false,
                                   &control_tx) == MESH_SIM_OK,
          "extended survey control scheduling failed");
    world->transmissions[control_tx].protocol_msg_type =
        control->packet.msg_type;
    control_end_us = world->transmissions[control_tx].end_us;
    control_rx_phy = control_listener_holds_extended ||
                         turnaround_ms * 1000u >=
                             MODELED_CONTROL_PHY_RETUNE_US ?
                     MESH_SIM_PHY_CHANNEL5_MESH_CONTROL :
                     MESH_SIM_PHY_CHANNEL5_WAKE;
    for (uint8_t receiver = 0u; receiver < 2u; receiver++) {
        uint64_t rx_end_us = control_end_us;

        if (truncate_initiator_control && receiver == 0u) {
            rx_end_us--;
        }
        CHECK(mesh_sim_schedule_rx(world,
                                   anchors[receiver],
                                   control_start_us,
                                   rx_end_us,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   control_rx_phy,
                                   NULL) == MESH_SIM_OK,
              "survey control receive scheduling failed");
    }
    CHECK(mesh_sim_run_until(world, control_end_us + 1u) == MESH_SIM_OK,
          "two-anchor control opportunity simulation failed");

    memset(result, 0, sizeof(*result));
    for (size_t i = receptions_before; i < world->reception_count; i++) {
        const struct mesh_sim_reception *reception = &world->receptions[i];
        uint8_t receiver;

        if (reception->receiver_id == ANCHOR_ID) {
            receiver = 0u;
        } else if (reception->receiver_id == ANCHOR_2_ID) {
            receiver = 1u;
        } else {
            continue;
        }
        if (reception->phy == MESH_SIM_PHY_CHANNEL5_WAKE) {
            if (reception->outcome == MESH_SIM_RX_DECODED) {
                result->control_wakes_decoded[receiver]++;
            } else {
                result->trigger_mismatches[receiver]++;
            }
        } else if (reception->phy ==
                   MESH_SIM_PHY_CHANNEL5_MESH_CONTROL) {
            if (reception->outcome == MESH_SIM_RX_DECODED) {
                result->controls_decoded[receiver]++;
            } else {
                result->controls_rejected[receiver]++;
            }
        }
    }
    for (uint8_t receiver = 0u; receiver < 2u; receiver++) {
        bool expect_control_decode =
            control_rx_phy == MESH_SIM_PHY_CHANNEL5_MESH_CONTROL &&
            !(truncate_initiator_control && receiver == 0u);

        CHECK(result->trigger_mismatches[receiver] == 1u,
              "extended listener did not record one standard-PHR trigger mismatch");
        CHECK(result->control_wakes_decoded[receiver] == 1u,
              "standard probe did not decode one non-click control wake");
        CHECK(result->controls_decoded[receiver] +
                  result->controls_rejected[receiver] == 1u,
              "extended control did not produce one physical receive outcome");
        CHECK((result->controls_decoded[receiver] == 1u) ==
                  expect_control_decode,
              expect_control_decode ?
                  "guarded complete-airtime control copy did not decode" :
                  "wrong-PHR or partial-airtime control copy falsely decoded");
    }
    *at_us = control_end_us + FLOOD_RELAY_REPEAT_MS * 1000u;
    return true;
}

static void run_two_anchor_followup_turnaround_case(
    uint32_t turnaround_ms,
    bool sender_guarded,
    bool control_listener_holds_extended)
{
    static struct mesh_sim_world world;
    struct mesh_outbound prepare_initiator;
    struct mesh_outbound prepare_responder;
    struct mesh_outbound start_responder;
    struct mesh_outbound start_initiator;
    struct followup_opportunity_result opportunity;
    uint8_t anchors[2] = {UINT8_MAX, UINT8_MAX};
    uint8_t gateway = UINT8_MAX;
    uint64_t at_us = TX_START_US;
    size_t initiator_start_decoded = 0u;
    size_t initiator_start_rejected = 0u;

    mesh_sim_init(&world, UINT32_C(0xc5f01100) ^ turnaround_ms);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK,
          "turnaround gateway setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &anchors[0]) == MESH_SIM_OK,
          "turnaround initiator setup failed");
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, ANCHOR_2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &anchors[1]) == MESH_SIM_OK,
          "turnaround responder setup failed");
    CHECK(mesh_sim_set_link(&world, gateway, anchors[0], 100u, 0u) ==
              MESH_SIM_OK &&
              mesh_sim_set_link(&world, gateway, anchors[1], 100u, 0u) ==
              MESH_SIM_OK,
          "turnaround gateway-anchor links failed");
    CHECK(build_pair_prepare_control(&prepare_initiator,
                                     ANCHOR_ID,
                                     ANCHOR_ID,
                                     101u) == PROTO_OK &&
              build_pair_prepare_control(&prepare_responder,
                                         ANCHOR_2_ID,
                                         ANCHOR_2_ID,
                                         102u) == PROTO_OK &&
              build_pair_start_control(&start_responder,
                                       ANCHOR_2_ID,
                                       103u) == PROTO_OK &&
              build_pair_start_control(&start_initiator,
                                       ANCHOR_ID,
                                       104u) == PROTO_OK,
          "turnaround pair-control setup failed");

    if (!model_two_anchor_control_opportunity(
            &world, gateway, anchors, &at_us, &prepare_initiator,
            MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS, false, false,
            &opportunity) ||
        !model_two_anchor_control_opportunity(
            &world, gateway, anchors, &at_us, &prepare_responder,
            MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS, false, false,
            &opportunity) ||
        !model_two_anchor_control_opportunity(
            &world, gateway, anchors, &at_us, &start_responder,
            MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS, false, false,
            &opportunity)) {
        return;
    }
    CHECK(opportunity.controls_decoded[0] == 1u &&
              opportunity.controls_decoded[1] == 1u,
          "both anchors did not physically decode responder START traffic");

    for (uint8_t copy = 0u; copy < FLOOD_RELAY_REPEAT_COUNT; copy++) {
        bool truncate_first_guarded =
            (sender_guarded || control_listener_holds_extended) &&
            copy == 0u;

        if (!model_two_anchor_control_opportunity(
                &world, gateway, anchors, &at_us, &start_initiator,
                turnaround_ms, control_listener_holds_extended,
                truncate_first_guarded, &opportunity)) {
            return;
        }
        initiator_start_decoded += opportunity.controls_decoded[0];
        initiator_start_rejected += opportunity.controls_rejected[0];
    }

    if (sender_guarded || control_listener_holds_extended) {
        CHECK(initiator_start_decoded >= 1u,
              "guarded listener schedule did not contain any complete initiator START copy");
        CHECK(initiator_start_rejected >= 1u,
              "one-microsecond-short START window was falsely decoded");
    } else {
        CHECK(initiator_start_decoded == 0u &&
                  initiator_start_rejected == FLOOD_RELAY_REPEAT_COUNT,
              "zero-turnaround schedule did not miss all four extended-PHR START copies");
    }
}

static void test_two_anchor_control_followup_turnaround(void)
{
    run_two_anchor_followup_turnaround_case(0u, false, false);
    run_two_anchor_followup_turnaround_case(
        MODELED_CONTROL_FOLLOWUP_TURNAROUND_MS, true, false);
    run_two_anchor_followup_turnaround_case(0u, false, true);
}

static bool range_header_matches(const struct uwb_range_header *actual,
                                 const struct uwb_range_header *expected)
{
    return actual != NULL && expected != NULL &&
           actual->type == expected->type &&
           actual->seq == expected->seq &&
           actual->round_index == expected->round_index &&
           actual->network_id == expected->network_id &&
           actual->session_id == expected->session_id &&
           actual->session_nonce == expected->session_nonce &&
           actual->initiator_short_addr == expected->initiator_short_addr &&
           actual->responder_short_addr == expected->responder_short_addr &&
           actual->flags == expected->flags &&
           actual->initiator_id == expected->initiator_id &&
           actual->responder_id == expected->responder_id;
}

static bool model_unreceived_airtime_tx(struct mesh_sim_world *world,
                                        uint8_t sender,
                                        uint64_t *at_us,
                                        const uint8_t *frame,
                                        size_t frame_len,
                                        uint8_t msg_type)
{
    const size_t receptions_before = world->reception_count;
    uint16_t transmission = UINT16_MAX;
    uint64_t end_us;

    if (mesh_sim_schedule_raw_tx(world, sender, *at_us,
                                 UWB_CHANNEL_WAKE_CONTACT,
                                 MESH_SIM_PHY_CHANNEL5_RANGE,
                                 frame, frame_len, false,
                                 &transmission) != MESH_SIM_OK) {
        CHECK(false, "lost-FINAL TX scheduling failed");
        return false;
    }
    world->transmissions[transmission].protocol_msg_type = msg_type;
    end_us = world->transmissions[transmission].end_us;
    if (mesh_sim_run_until(world, end_us + 1u) != MESH_SIM_OK) {
        CHECK(false, "lost-FINAL PHY simulation failed");
        return false;
    }
    CHECK(world->reception_count == receptions_before,
          "frame without a receive window unexpectedly decoded");
    *at_us = end_us + 1u;
    return true;
}

static int model_single_pair_round_commitment(
    const struct survey_pair *pair,
    uint16_t round_id,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    const struct survey_round_plan_identity identity = {
        .operation_generation = pair->operation_generation,
        .survey_id = pair->survey_id,
        .operation_session_id =
            survey_operation_session_id(pair->operation_generation),
        .execute_delay_ms = SURVEY_ROUND_START_EXECUTE_DELAY_MS,
        .observation_window_ms = SURVEY_PAIR_RESPONDER_WINDOW_MS,
        .round_id = round_id,
        .max_parallel_pairs = 1u,
        .max_reruns = 1u,
    };
    const struct survey_round_plan_entry entry = {
        .pair = *pair,
        .lane_index = 0u,
        .plan_pair_index = 0u,
        .reruns_started = 0u,
    };

    return survey_round_commitment_compute(&identity, &entry, 1u,
                                           commitment);
}

static void test_planned_pair_runs_full_bounded_exchange(
    const struct survey_pair *planned_pair)
{
    static struct mesh_sim_world world;
    static struct survey_gateway_context plan;
    static struct survey_pair_round_runtime round_runtime;
    const uint16_t round_id = 1u;
    const struct survey_pair_round_metadata metadata = {
        .round_index = 0u,
        .pair_index_in_round = 0u,
        .pair_count_in_round = 1u,
    };
    struct survey_pair_lease initiator_lease;
    struct survey_pair_lease responder_lease;
    struct mesh_sim_invariant_report invariant = {0};
    struct survey_pair_control_id initiator_prepare;
    struct survey_pair_control_id responder_prepare;
    struct survey_pair_control_id initiator_start;
    struct survey_pair_control_id responder_start;
    struct survey_pair next_pair;
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    uint32_t operation_session_id;
    uint8_t initiator = UINT8_MAX;
    uint8_t responder = UINT8_MAX;
    uint64_t at_us = TX_START_US;
    size_t lane_index = SIZE_MAX;
    bool accepted_new = false;

    if (planned_pair == NULL ||
        survey_pair_validate(planned_pair) != PROTO_OK) {
        CHECK(false, "planned pair exchange received an invalid pair");
        return;
    }
    operation_session_id =
        survey_operation_session_id(planned_pair->operation_generation);
    initiator_prepare = (struct survey_pair_control_id) {
        .session_id = operation_session_id,
        .command_seq = 10u,
    };
    responder_prepare = (struct survey_pair_control_id) {
        .session_id = operation_session_id,
        .command_seq = 11u,
    };
    initiator_start = (struct survey_pair_control_id) {
        .session_id = operation_session_id,
        .command_seq = 12u,
    };
    responder_start = (struct survey_pair_control_id) {
        .session_id = operation_session_id,
        .command_seq = 13u,
    };
    if (operation_session_id == 0u ||
        model_single_pair_round_commitment(
            planned_pair, round_id, round_commitment) != PROTO_OK) {
        CHECK(false,
              "planned pair lacked a valid generation-bound round identity");
        return;
    }
    CHECK(planned_pair->sample_count <=
              SURVEY_PAIR_ROUND_RUNTIME_MAX_RESULT_SAMPLES,
          "planned pair exceeds bounded gateway result storage");

    survey_pair_lease_reset(&initiator_lease);
    survey_pair_lease_reset(&responder_lease);
    CHECK(survey_pair_lease_prepare_round_bound(
              &initiator_lease, planned_pair, round_id, round_commitment,
              &initiator_prepare, 10u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "initiator did not accept planned PREPARE");
    CHECK(survey_pair_lease_prepare_round_bound(
              &responder_lease, planned_pair, round_id, round_commitment,
              &responder_prepare, 10u, SURVEY_PAIR_PREPARED_LEASE_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "responder did not accept planned PREPARE");
    CHECK(survey_pair_lease_start_round_bound_at(
              &responder_lease, planned_pair, round_id, round_commitment,
              &responder_start, 11u,
              11u + SURVEY_ROUND_START_EXECUTE_DELAY_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "responder did not accept planned START");
    CHECK(survey_pair_lease_start_round_bound_at(
              &initiator_lease, planned_pair, round_id, round_commitment,
              &initiator_start, 11u,
              11u + SURVEY_ROUND_START_EXECUTE_DELAY_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "initiator did not accept planned START");
    CHECK(survey_pair_lease_release_start(&responder_lease, &responder_start) &&
              survey_pair_lease_release_start(&initiator_lease,
                                               &initiator_start),
          "START result custody did not release both endpoint leases");
    CHECK(!survey_pair_lease_mark_running_at(
              &responder_lease,
              10u + SURVEY_ROUND_START_EXECUTE_DELAY_MS,
              NULL, NULL) &&
              !survey_pair_lease_mark_running_at(
                  &initiator_lease,
                  10u + SURVEY_ROUND_START_EXECUTE_DELAY_MS,
                  NULL, NULL),
          "START released an endpoint before its synchronized deadline");
    CHECK(survey_pair_lease_mark_running_at(
              &responder_lease,
              11u + SURVEY_ROUND_START_EXECUTE_DELAY_MS,
              NULL, NULL) &&
              survey_pair_lease_mark_running_at(
                  &initiator_lease,
                  11u + SURVEY_ROUND_START_EXECUTE_DELAY_MS,
                  NULL, NULL),
          "START did not release both endpoints at its synchronized deadline");

    CHECK(survey_gateway_begin_operation(
              &plan, planned_pair->survey_id,
              planned_pair->operation_generation,
              planned_pair->sample_count) == PROTO_OK,
          "gateway pair plan did not initialize");
    plan.node_ids[0] = planned_pair->initiator_id;
    plan.node_ids[1] = planned_pair->responder_id;
    plan.node_count = 2u;
    plan.pair_count = 1u;
    plan.pairs_planned = true;
    plan.pairs[0] = (struct survey_gateway_pair_entry) {
        .initiator_index = 0u,
        .responder_index = 1u,
    };
    CHECK(survey_pair_round_runtime_begin(&round_runtime, &plan, &metadata,
                                           1u, 1u, 1u) == PROTO_OK &&
              survey_pair_round_runtime_load_next_batch(&round_runtime) ==
                  PROTO_OK,
          "gateway did not load the planned pair round");
    CHECK(survey_pair_round_runtime_note_prepared(
              &round_runtime, 0u,
              SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK &&
              survey_pair_round_runtime_note_prepared(
                  &round_runtime, 0u,
                  SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK,
          "gateway round did not record both PREPARE results");
    CHECK(survey_pair_round_runtime_note_started(
              &round_runtime, 0u,
              SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK &&
              survey_pair_round_runtime_note_started(
                  &round_runtime, 0u,
                  SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK &&
              survey_pair_round_runtime_mark_observing(&round_runtime, 0u) ==
                  PROTO_OK,
          "gateway round did not cross the START observation barrier");

    {
        struct survey_sample stale_sample = {
            .pair = *planned_pair,
            .round_id = round_runtime.batch_sequence,
            .sample_index = 0u,
            .distance_mm = 3400,
            .quality = 90u,
            .range_status = RANGE_OK,
        };
        const struct survey_pair_round_lane *lane =
            survey_pair_round_runtime_lane(&round_runtime, 0u);
        enum survey_pair_round_lane_state state_before = lane->state;
        uint16_t usable_before = lane->usable_result_mask;

        stale_sample.pair.operation_generation++;
        CHECK(survey_pair_round_runtime_note_sample(
                  &round_runtime, planned_pair->responder_id, &stale_sample,
                  NULL, NULL) == PROTO_ERR_STALE,
              "wrong-generation sample was admitted to the active round");
        lane = survey_pair_round_runtime_lane(&round_runtime, 0u);
        CHECK(lane->state == state_before &&
                  lane->usable_result_mask == usable_before,
              "stale survey sample mutated the active round");
    }

    mesh_sim_init(&world, UINT32_C(0x5066f00d));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            planned_pair->initiator_id, GATEWAY_ID,
                            ROUTE_EPOCH, &initiator) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                                planned_pair->responder_id, GATEWAY_ID,
                                ROUTE_EPOCH, &responder) == MESH_SIM_OK &&
              mesh_sim_set_link(&world, initiator, responder, 100u, 0u) ==
                  MESH_SIM_OK,
          "planned pair radio setup failed");

    for (uint16_t sample_index = 0u;
         sample_index < planned_pair->sample_count;
         sample_index++) {
        const uint8_t attempt_count = sample_index == 0u ? 2u : 1u;

        for (uint8_t attempt = 0u; attempt < attempt_count; attempt++) {
            const uint8_t seq = (uint8_t)(sample_index + 1u);
            const uint32_t timestamp_base = UINT32_C(0x10000000) +
                (uint32_t)sample_index * UINT32_C(0x00100000) +
                (uint32_t)attempt * UINT32_C(0x00010000);
            const struct uwb_range_header identity = {
                .type = MSG_UWB_POLL,
                .seq = seq,
                .round_index = 0u,
                .network_id = UINT32_C(0x494d4543),
                .session_id = operation_session_id,
                .session_nonce = survey_sample_nonce(planned_pair,
                                                      sample_index),
                .initiator_short_addr = uwb_session_short_addr_from_id(
                    planned_pair->initiator_id),
                .responder_short_addr = uwb_session_short_addr_from_id(
                    planned_pair->responder_id),
                .flags = FLAG_DIAGNOSTIC,
                .initiator_id = planned_pair->initiator_id,
                .responder_id = planned_pair->responder_id,
            };
            struct uwb_range_header decoded_poll = {0};
            struct uwb_response_frame response = {
                .header = identity,
                .poll_rx_ts_32 = timestamp_base + 100u,
                .resp_tx_ts_32 = timestamp_base + 8100u,
            };
            struct uwb_final_frame final = {
                .header = identity,
                .poll_tx_ts_32 = timestamp_base,
                .resp_rx_ts_32 = timestamp_base + 8200u,
                .final_tx_ts_32 = timestamp_base + 16200u,
            };
            struct uwb_report_frame report = {
                .header = identity,
                .distance_mm = sample_index == 0u ?
                    1 : 3200 + (int32_t)sample_index * 10,
                .quality = (uint8_t)(92u - sample_index),
                .status = RANGE_OK,
                .rsl_dbm = -54,
            };
            struct uwb_response_frame decoded_response = {0};
            struct uwb_final_frame decoded_final = {0};
            struct uwb_report_frame decoded_report = {0};
            struct survey_sample sample;
            uint8_t poll[UWB_POLL_LEN];
            uint8_t response_bytes[UWB_RESP_LEN];
            uint8_t final_bytes[UWB_FINAL_LEN];
            uint8_t report_bytes[UWB_REPORT_LEN];
            size_t poll_len = 0u;
            size_t response_len = 0u;
            size_t final_len = 0u;
            size_t report_len = 0u;

            response.header.type = MSG_UWB_RESP;
            final.header.type = MSG_UWB_FINAL;
            report.header.type = MSG_UWB_REPORT;
            if (uwb_encode_poll(&identity, poll, sizeof(poll), &poll_len) !=
                    PROTO_OK ||
                uwb_encode_response(&response, response_bytes,
                                    sizeof(response_bytes), &response_len) !=
                    PROTO_OK ||
                uwb_encode_final(&final, final_bytes, sizeof(final_bytes),
                                 &final_len) != PROTO_OK ||
                uwb_encode_report(&report, report_bytes,
                                  sizeof(report_bytes), &report_len) !=
                    PROTO_OK) {
                CHECK(false, "production survey range frame encoding failed");
                return;
            }

            if (!model_exact_airtime_hop(
                    &world, initiator, responder, &at_us,
                    UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_RANGE,
                    MESH_SIM_PHY_CHANNEL5_RANGE, poll, poll_len,
                    MSG_UWB_POLL, true) ||
                uwb_decode_poll(poll, poll_len, &decoded_poll) != PROTO_OK ||
                !range_header_matches(&decoded_poll, &identity)) {
                CHECK(false, "responder did not decode the exact survey POLL");
                return;
            }
            at_us += UWB_DS_TWR_REPLY_DELAY_US - PHY_HOP_GAP_US;
            if (!model_exact_airtime_hop(
                    &world, responder, initiator, &at_us,
                    UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_RANGE,
                    MESH_SIM_PHY_CHANNEL5_RANGE, response_bytes, response_len,
                    MSG_UWB_RESP, true) ||
                uwb_decode_response(response_bytes, response_len,
                                    &decoded_response) != PROTO_OK ||
                !range_header_matches(&decoded_response.header,
                                      &response.header)) {
                CHECK(false, "initiator did not decode the exact survey RESP");
                return;
            }
            at_us += UWB_DS_TWR_REPLY_DELAY_US - PHY_HOP_GAP_US;

            if (sample_index == 0u && attempt == 0u) {
                if (!model_unreceived_airtime_tx(
                        &world, initiator, &at_us, final_bytes, final_len,
                        MSG_UWB_FINAL)) {
                    return;
                }
                at_us += (uint64_t)SURVEY_PAIR_INITIATOR_TIMEOUT_MS * 1000u;
                continue;
            }
            if (!model_exact_airtime_hop(
                    &world, initiator, responder, &at_us,
                    UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_RANGE,
                    MESH_SIM_PHY_CHANNEL5_RANGE, final_bytes, final_len,
                    MSG_UWB_FINAL, true) ||
                uwb_decode_final(final_bytes, final_len, &decoded_final) !=
                    PROTO_OK ||
                !range_header_matches(&decoded_final.header, &final.header)) {
                CHECK(false, "responder did not decode the exact survey FINAL");
                return;
            }
            if (!model_exact_airtime_hop(
                    &world, responder, initiator, &at_us,
                    UWB_CHANNEL_WAKE_CONTACT, MESH_SIM_PHY_CHANNEL5_RANGE,
                    MESH_SIM_PHY_CHANNEL5_RANGE, report_bytes, report_len,
                    MSG_UWB_REPORT, true) ||
                uwb_decode_report(report_bytes, report_len, &decoded_report) !=
                    PROTO_OK ||
                !range_header_matches(&decoded_report.header,
                                      &report.header)) {
                CHECK(false, "initiator did not decode the exact survey REPORT");
                return;
            }

            sample = (struct survey_sample) {
                .pair = *planned_pair,
                .round_id = round_runtime.batch_sequence,
                .sample_index = sample_index,
                .distance_mm = decoded_report.distance_mm,
                .quality = decoded_report.quality,
                .range_status = decoded_report.status,
            };
            CHECK(survey_sample_distance_usable(&sample),
                  "positive short-range survey sample was rejected");
            CHECK(survey_pair_round_runtime_note_sample(
                      &round_runtime, planned_pair->responder_id, &sample,
                      &lane_index, &accepted_new) == PROTO_OK &&
                      lane_index == 0u && accepted_new,
                  "gateway did not accept a decoded survey sample once");
            if (sample_index == 0u) {
                CHECK(survey_pair_round_runtime_note_sample(
                          &round_runtime, planned_pair->responder_id, &sample,
                          &lane_index, &accepted_new) == PROTO_OK &&
                          !accepted_new,
                      "duplicate decoded survey sample was not idempotent");
            }
        }
    }

    CHECK(world.transmission_count ==
              (size_t)planned_pair->sample_count * 4u + 3u &&
              world.reception_count == world.transmission_count - 1u,
          "lost-FINAL retry traffic exceeded the exact tested bound");
    CHECK(world.now_us - TX_START_US <
              ((uint64_t)planned_pair->sample_count + 1u) *
                  SURVEY_PAIR_RESPONDER_WINDOW_MS * 1000u,
          "survey exchange exceeded its local bounded-time envelope");
    CHECK(survey_pair_round_lane_results_complete(
              survey_pair_round_runtime_lane(&round_runtime, 0u)),
          "gateway did not settle all planned survey samples");
    CHECK(survey_pair_round_runtime_require_cleanup(
              &round_runtime, 0u, SURVEY_PAIR_ROUND_ENDPOINT_BOTH_MASK,
              SURVEY_PAIR_ROUND_CLEANUP_SUCCESS) == PROTO_OK &&
              survey_pair_round_runtime_note_cleanup_complete(
                  &round_runtime, 0u,
                  SURVEY_PAIR_ROUND_ENDPOINT_INITIATOR_MASK) == PROTO_OK &&
              survey_pair_round_runtime_note_cleanup_complete(
                  &round_runtime, 0u,
                  SURVEY_PAIR_ROUND_ENDPOINT_RESPONDER_MASK) == PROTO_OK &&
              survey_pair_round_runtime_batch_complete(&round_runtime) &&
              survey_pair_round_runtime_complete(&round_runtime),
          "completed survey round retained gateway ownership");
    CHECK(survey_pair_lease_finish(&initiator_lease) &&
              survey_pair_lease_finish(&responder_lease) &&
              survey_pair_lease_invariant(&initiator_lease) &&
              survey_pair_lease_invariant(&responder_lease),
          "completed survey endpoints did not release their leases");

    next_pair = *planned_pair;
    next_pair.operation_generation++;
    {
        const uint16_t next_round_id = round_id + 1u;
        const uint32_t next_operation_session_id =
            survey_operation_session_id(next_pair.operation_generation);
        const struct survey_pair_control_id next_prepare = {
            .session_id = next_operation_session_id,
            .command_seq = 1u,
        };
        uint8_t next_round_commitment[SEMANTIC_DIGEST_SHA256_LEN] = {0};

        CHECK(next_operation_session_id != 0u &&
                  next_operation_session_id != operation_session_id &&
                  model_single_pair_round_commitment(
                      &next_pair, next_round_id,
                      next_round_commitment) == PROTO_OK,
              "next operation lacked a distinct generation-bound identity");
        CHECK(survey_pair_lease_prepare_round_bound(
                  &initiator_lease, &next_pair, next_round_id,
                  next_round_commitment, &next_prepare, 20u,
                  SURVEY_PAIR_PREPARED_LEASE_MS) ==
                  SURVEY_PAIR_LEASE_ACCEPTED,
              "next survey operation could not acquire the released lease");
        CHECK(survey_pair_lease_start_round_bound_at(
                  &initiator_lease, planned_pair, round_id,
                  round_commitment, &initiator_start, 21u,
                  21u + SURVEY_ROUND_START_EXECUTE_DELAY_MS) ==
                  SURVEY_PAIR_LEASE_STALE &&
                  initiator_lease.phase == SURVEY_PAIR_LEASE_PREPARED &&
                  initiator_lease.pair.operation_generation ==
                      next_pair.operation_generation &&
                  initiator_lease.pair.survey_id == planned_pair->survey_id,
              "stale START from operation N mutated operation N+1");
        CHECK(survey_pair_lease_abort(&initiator_lease),
              "next survey lease did not clean up after stale-control check");
    }

    CHECK(mesh_sim_check_settled(&world, &invariant) == MESH_SIM_OK,
          "survey PHY exchange left radio, work, queue, or custody ownership");
}

static void test_two_anchor_survey_lifecycle(void)
{
    static const struct survey_discovery_config config = {
        .survey_id = SURVEY_ID,
        .operation_generation = SURVEY_OPERATION_GENERATION,
        .start_delay_ms = 2000u,
        .slot_ms = 40u,
        .slot_count = 6u,
        .round_count = 4u,
    };
    static struct mesh_sim_world world;
    static struct survey_gateway_context gateway_context;
    struct survey_pair planned_pair;
    struct survey_discovery_attempt_schedule schedules[2];
    struct proto_packet start_packet;
    struct proto_packet report_packets[2];
    struct survey_reachability_entry reports[2] = {0};
    uint64_t anchor_ids[2] = {ANCHOR_ID, ANCHOR_2_ID};
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
    const uint32_t operation_session_id =
        survey_operation_session_id(config.operation_generation);
    uint32_t start_airtime_us;
    enum mesh_sim_phy start_tx_phy;
    enum mesh_sim_phy start_rx_phy;
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
            .operation_generation = config.operation_generation,
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

    CHECK(operation_session_id != 0u &&
              survey_gateway_begin_operation(&gateway_context,
                                             SURVEY_ID,
                                             config.operation_generation,
                                             SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) ==
                  PROTO_OK,
          "gateway survey context setup failed");
    CHECK(survey_gateway_note_reach_report(&gateway_context, SURVEY_ID + 1u,
                                           ANCHOR_ID, &reports[0], 1u) ==
              PROTO_ERR_STALE && gateway_context.report_count == 0u,
          "stale survey report changed gateway state");
    for (uint8_t i = 0u; i < 2u; i++) {
        uint32_t parsed_survey_id = 0u;
        uint64_t parsed_operation_generation = 0u;
        uint64_t parsed_anchor_id = 0u;
        struct survey_reachability_entry parsed_entry;
        const uint8_t *status_raw = NULL;
        size_t parsed_count = 0u;
        uint8_t status_len = 0u;

        CHECK(survey_append_reach_report_tlvs(
                  report_payloads[i], sizeof(report_payloads[i]),
                  &report_payload_lens[i], SURVEY_ID, anchor_ids[i],
                  &reports[i], 1u) == PROTO_OK,
              "0x55 report TLV encoding failed");
        CHECK(survey_operation_generation_append_tlv(
                  report_payloads[i],
                  sizeof(report_payloads[i]),
                  &report_payload_lens[i],
                  config.operation_generation) == PROTO_OK &&
                  tlv_append_u32(report_payloads[i],
                                 sizeof(report_payloads[i]),
                                 &report_payload_lens[i],
                                 TLV_NODE_BOOT_COUNTER,
                                 (uint32_t)i + 1u) == PROTO_OK &&
                  tlv_append_u16(report_payloads[i],
                                 sizeof(report_payloads[i]),
                                 &report_payload_lens[i],
                                 TLV_COMMAND_STATUS,
                                 COMMAND_OK) == PROTO_OK,
              "production report generation/status encoding failed");
        CHECK(survey_init_discovery_report_packet(
                  &report_packets[i], anchor_ids[i], GATEWAY_ID, SURVEY_ID,
                  config.operation_generation,
                  (uint32_t)i + 1u,
                  (uint16_t)(10u + i),
                  (uint8_t)report_payload_lens[i]) == PROTO_OK &&
                  report_packets[i].msg_type == MSG_SURVEY_DISCOVERY_REPORT,
              "0x55 report packet setup failed");
        CHECK(survey_extract_reach_report_tlvs(
                  report_payloads[i], report_payload_lens[i], &parsed_survey_id,
                  &parsed_anchor_id, &parsed_entry, 1u, &parsed_count) == PROTO_OK &&
                  survey_operation_generation_extract_tlv(
                      report_payloads[i],
                      report_payload_lens[i],
                      &parsed_operation_generation) == PROTO_OK &&
                  tlv_find_unique(report_payloads[i],
                                  report_payload_lens[i],
                                  TLV_COMMAND_STATUS,
                                  &status_raw,
                                  &status_len) == PROTO_OK &&
                  status_len == sizeof(uint16_t) &&
                  proto_get_u16_le(status_raw) == COMMAND_OK &&
                  parsed_survey_id == SURVEY_ID &&
                  parsed_operation_generation == config.operation_generation &&
                  report_packets[i].session_id == (uint32_t)i + 1u &&
                  parsed_anchor_id == report_packets[i].src_id &&
                  parsed_count == 1u,
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
              planned_pair.operation_generation ==
                  config.operation_generation &&
              planned_pair.sample_count ==
                  SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
          "planned survey pair did not reconstruct context-wide fields");

    test_planned_pair_runs_full_bounded_exchange(&planned_pair);
}

int main(void)
{
    run_survey_start_phy_case(false, 0u, true, true);
    run_survey_start_phy_case(false, 1u, false, true);
    run_survey_start_phy_case(true, 0u, false, true);
    test_pair_start_skew_is_local_and_route_depth_independent();
    test_sample_cells_isolate_early_errors_and_lost_polls();
    test_gateway_c5_quiet_window_preserves_immutable_pair_release();
    test_pair_prepare_phr_and_complete_airtime_sweep();
    test_forced_control_followup_covers_relay_gap();
    test_post_channel9_recovery_scan_captures_live_wake_train();
    test_retained_ch9_retry_owner_keeps_survey_control_gap();
    test_forced_control_followup_reopens_for_deeper_relay();
    test_two_anchor_control_followup_turnaround();
    test_two_anchor_survey_lifecycle();

    if (failures != 0u) {
        fprintf(stderr, "RESULT mesh_survey_phy_scenarios failures=%u\n", failures);
        return EXIT_FAILURE;
    }
    printf("PASS mesh_survey_phy_scenarios\n");
    return EXIT_SUCCESS;
}
