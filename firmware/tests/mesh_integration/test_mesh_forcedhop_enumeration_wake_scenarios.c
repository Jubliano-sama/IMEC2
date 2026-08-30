#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "mesh_sim.h"
#include "protocol.h"
#include "survey_protocol.h"
#include "uwb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_ID UINT32_C(0x494d4543)
#define ROUTE_EPOCH UINT32_C(17)
#define ENUMERATION_EPOCH UINT32_C(0xe0010001)
#define GATEWAY_ID UINT64_C(0x9000000000000001)
#define RELAY_ID UINT64_C(0xa100000000000001)
#define FORCED_ID UINT64_C(0xa200000000000001)
#define RX_GUARD_US UINT64_C(100)
#define CONTROL_TURNAROUND_US UINT64_C(60000)
#define WAKE_GAP_US UINT64_C(500)

#define CHECK(expression, message)                                         \
    do {                                                                   \
        if (!(expression)) {                                               \
            fprintf(stderr, "FAIL forced-hop enumeration wake line=%d: %s\n", \
                    __LINE__, message);                                    \
            return false;                                                  \
        }                                                                  \
    } while (0)

struct fixture {
    struct mesh_sim_world world;
    uint8_t gateway;
    uint8_t relay;
    uint8_t forced;
};

static bool setup_fixture(struct fixture *fixture)
{
    const struct uwb_anchor_config relay_config = {
        .network_id = NETWORK_ID,
        .anchor_id = RELAY_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    const struct uwb_anchor_config forced_config = {
        .network_id = NETWORK_ID,
        .anchor_id = FORCED_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };

    memset(fixture, 0, sizeof(*fixture));
    mesh_sim_init(&fixture->world, UINT32_C(0xe11e0001));
    CHECK(mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->gateway) == MESH_SIM_OK,
          "gateway setup failed");
    CHECK(mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->relay) == MESH_SIM_OK,
          "relay setup failed");
    CHECK(mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            FORCED_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->forced) == MESH_SIM_OK,
          "forced anchor setup failed");
    CHECK(mesh_sim_set_link(&fixture->world, fixture->gateway,
                            fixture->relay, 100u, 0u) == MESH_SIM_OK,
          "gateway-relay link setup failed");
    CHECK(mesh_sim_set_link(&fixture->world, fixture->relay,
                            fixture->forced, 100u, 0u) == MESH_SIM_OK,
          "relay-forced link setup failed");
    CHECK(!fixture->world.reachable[fixture->gateway][fixture->forced],
          "gateway can directly reach the forced anchor");
    CHECK(mesh_sim_init_anchor_session(&fixture->world, fixture->relay,
                                       &relay_config) == PROTO_OK,
          "relay wake session setup failed");
    CHECK(mesh_sim_init_anchor_session(&fixture->world, fixture->forced,
                                       &forced_config) == PROTO_OK,
          "forced wake session setup failed");
    CHECK(mesh_sim_start_anchor_low_duty(&fixture->world, fixture->relay,
                                         0u) == MESH_SIM_OK,
          "relay low-duty scanner setup failed");
    CHECK(mesh_sim_start_anchor_low_duty(&fixture->world, fixture->forced,
                                         0u) == MESH_SIM_OK,
          "forced low-duty scanner setup failed");
    CHECK(mesh_sim_run_until(&fixture->world, 0u) == MESH_SIM_OK,
          "low-duty scanners did not arm");
    return true;
}

static bool setup_retained_handoff_fixture(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    mesh_sim_init(&fixture->world, UINT32_C(0x5a7e0001));
    CHECK(mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->gateway) == MESH_SIM_OK,
          "handoff gateway setup failed");
    CHECK(mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->relay) == MESH_SIM_OK,
          "handoff relay setup failed");
    CHECK(mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                            FORCED_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->forced) == MESH_SIM_OK,
          "handoff forced-anchor setup failed");
    CHECK(mesh_sim_set_link(&fixture->world, fixture->gateway,
                            fixture->relay, 100u, 0u) == MESH_SIM_OK,
          "handoff gateway-relay link setup failed");
    CHECK(mesh_sim_set_link(&fixture->world, fixture->relay,
                            fixture->forced, 100u, 0u) == MESH_SIM_OK,
          "handoff relay-forced link setup failed");
    CHECK(!fixture->world.reachable[fixture->gateway][fixture->forced],
          "handoff gateway can directly reach the forced anchor");
    return true;
}

static uint64_t first_low_duty_start(const struct mesh_sim_world *world,
                                     uint8_t node_index)
{
    for (size_t i = 0u; i < world->rx_window_count; i++) {
        if (world->rx_windows[i].node_index == node_index &&
            world->rx_windows[i].periodic_low_duty) {
            return world->rx_windows[i].start_us;
        }
    }
    return UINT64_MAX;
}

static bool schedule_wake_train(struct mesh_sim_world *world,
                                uint8_t sender,
                                uint64_t start_us,
                                uint32_t duration_ms,
                                uint32_t event_id,
                                uint64_t *last_end_us)
{
    const uint64_t close_us = start_us + (uint64_t)duration_ms * 1000u;
    const uint64_t source_id = world->roles[sender].id;
    uint64_t at_us = start_us;
    size_t sent = 0u;

    while (at_us < close_us) {
        struct uwb_wake_claim_frame claim = {0};
        uint8_t frame[UWB_WAKE_CLAIM_LEN];
        uint64_t remaining_us = close_us - at_us;
        uint16_t remaining_ms =
            (uint16_t)((remaining_us + UINT64_C(999)) / UINT64_C(1000));
        uint16_t claimed_ms = (uint16_t)(remaining_ms + 500u);
        uint16_t tx_index = UINT16_MAX;
        size_t frame_len = 0u;

        if (claimed_ms > UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS) {
            claimed_ms = UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS;
        }
        claim.network_id = NETWORK_ID;
        claim.clicker_id = source_id;
        claim.click_event_id = event_id;
        claim.attempt_index = 1u;
        claim.priority_id = source_id;
        claim.wake_channel = UWB_CHANNEL_WAKE_CONTACT;
        claim.ranging_channel = UWB_CHANNEL_WAKE_CONTACT;
        claim.wake_train_ends_in_ms = remaining_ms;
        claim.discovery_starts_in_ms = remaining_ms;
        claim.claimed_duration_ms = claimed_ms;
        claim.min_anchor_count = 1u;
        claim.max_anchor_count = 1u;
        claim.nonce = source_id ^ event_id ^ UINT64_C(0x1020304050607080);
        claim.flags = FLAG_CONTROL_FOLLOWUP | FLAG_ROUTE_SETUP |
                      FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;

        CHECK(uwb_encode_wake_claim(&claim, frame, sizeof(frame),
                                    &frame_len) == PROTO_OK,
              "wake claim encode failed");
        CHECK(mesh_sim_schedule_raw_tx(world, sender, at_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_WAKE,
                                       frame, frame_len, false,
                                       &tx_index) == MESH_SIM_OK,
              "wake claim schedule failed");
        *last_end_us = world->transmissions[tx_index].end_us;
        at_us = *last_end_us + WAKE_GAP_US;
        sent++;
    }
    return sent > 0u;
}

static bool build_enumeration_claim(uint8_t *frame,
                                    size_t frame_cap,
                                    size_t *frame_len)
{
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = UINT32_C(0xe11e0001),
        .seq = 1u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;
    int ret;

    ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                         TLV_COMMAND_ID, CMD_ASSIGN_DISCOVERY_SLOTS);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
            payload, sizeof(payload), &payload_len,
            DISCOVERY_ASSIGNMENT_PHASE_CLAIM, ENUMERATION_EPOCH);
    }
    if (ret != PROTO_OK) {
        return false;
    }
    packet.payload_len = (uint16_t)payload_len;
    return proto_packet_encode(&packet, payload,
                               frame, frame_cap, frame_len) == PROTO_OK;
}

static bool build_survey_start(uint8_t *frame,
                               size_t frame_cap,
                               size_t *frame_len)
{
    struct survey_control control = {
        .phase = SURVEY_PHASE_NEIGHBOR_START,
        .identity = {
            .generation = UINT32_C(0x5a7e0001),
            .assignment = {
                .assignment_epoch = ENUMERATION_EPOCH,
                .table_command_seq = UINT32_C(0xe0010002),
                .slot_span = 2u,
                .max_hop_count = 2u,
            },
        },
        .start_delay_ms = 15000u,
        .self_stop_delay_ms = SURVEY_INITIAL_SELF_EXPIRY_MS,
        .start_delay_present = true,
        .self_stop_delay_present = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = UINT32_C(0x5a7e0001),
        .seq = 2u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    memset(control.identity.assignment.table_commitment.bytes,
           0xa5,
           sizeof(control.identity.assignment.table_commitment.bytes));
    if (survey_control_append_tlvs(payload, sizeof(payload), &payload_len,
                                   &control) != PROTO_OK) {
        return false;
    }
    packet.payload_len = (uint16_t)payload_len;
    return proto_packet_encode(&packet, payload,
                               frame, frame_cap, frame_len) == PROTO_OK;
}

static bool schedule_control_rx(struct mesh_sim_world *world,
                                uint8_t receiver,
                                uint64_t tx_start_us,
                                size_t frame_len)
{
    uint32_t airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame_len);

    if (airtime_us == 0u || tx_start_us < RX_GUARD_US) {
        return false;
    }
    return mesh_sim_schedule_rx(world, receiver,
                                tx_start_us - RX_GUARD_US,
                                tx_start_us + airtime_us + RX_GUARD_US,
                                UWB_CHANNEL_WAKE_CONTACT,
                                MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                NULL) == MESH_SIM_OK;
}

static size_t decoded_from_to(const struct mesh_sim_world *world,
                              uint64_t source_id,
                              uint64_t receiver_id,
                              size_t first_reception)
{
    size_t decoded = 0u;

    for (size_t i = first_reception; i < world->reception_count; i++) {
        const struct mesh_sim_reception *rx = &world->receptions[i];

        if (rx->source_id == source_id && rx->receiver_id == receiver_id &&
            rx->outcome == MESH_SIM_RX_DECODED) {
            decoded++;
        }
    }
    return decoded;
}

static bool run_until_ok(struct mesh_sim_world *world, uint64_t end_us,
                         const char *phase)
{
    int ret = mesh_sim_run_until(world, end_us);

    if (ret != MESH_SIM_OK) {
        fprintf(stderr,
                "SIM ERROR phase=%s status=%d source=%s:%u event=%u object=%u time_us=%llu\n",
                phase, ret,
                world->last_error_file == NULL ? "?" : world->last_error_file,
                world->last_error_line,
                world->last_error_event_valid ?
                    world->last_error_event_type : 0u,
                world->last_error_event_valid ?
                    world->last_error_event_object_index : 0u,
                (unsigned long long)(world->last_error_event_valid ?
                    world->last_error_event_time_us : world->now_us));
        return false;
    }
    return true;
}

static bool enumeration_reaches_forced_anchor(bool relay_wake)
{
    static struct fixture fixture;
    uint8_t command_frame[PACKET_EXT_MAX_LEN];
    uint64_t gateway_wake_end_us = 0u;
    uint64_t relay_wake_end_us = 0u;
    uint64_t relay_scan_start_us;
    uint64_t gateway_control_start_us;
    uint64_t relay_control_start_us;
    uint16_t tx_index = UINT16_MAX;
    size_t command_len = 0u;
    size_t first_reception;

    CHECK(setup_fixture(&fixture), "fixture setup failed");
    relay_scan_start_us = first_low_duty_start(&fixture.world,
                                                fixture.relay);
    CHECK(relay_scan_start_us != UINT64_MAX,
          "relay low-duty window was not exposed");
    CHECK(build_enumeration_claim(command_frame, sizeof(command_frame),
                                  &command_len),
          "enumeration CLAIM frame build failed");

    CHECK(schedule_wake_train(
              &fixture.world, fixture.gateway, relay_scan_start_us + 1u,
              MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS,
              UINT32_C(0xe001), &gateway_wake_end_us),
          "gateway activation train schedule failed");
    CHECK(run_until_ok(&fixture.world, gateway_wake_end_us + 1u,
                       "gateway-wake"),
          "gateway activation train simulation failed");
    CHECK(fixture.world.roles[fixture.relay]
                  .anchor_session.diagnostics.claims > 0u,
          "direct relay did not wake from the gateway train");
    CHECK(fixture.world.roles[fixture.forced]
                  .anchor_session.diagnostics.claims == 0u,
          "RF-hidden anchor heard the direct gateway train");

    gateway_control_start_us = gateway_wake_end_us + CONTROL_TURNAROUND_US;
    CHECK(schedule_control_rx(&fixture.world, fixture.relay,
                              gateway_control_start_us, command_len),
          "relay control listener schedule failed");
    CHECK(mesh_sim_schedule_raw_tx(
              &fixture.world, fixture.gateway, gateway_control_start_us,
              UWB_CHANNEL_WAKE_CONTACT,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              command_frame, command_len, false, &tx_index) == MESH_SIM_OK,
          "gateway enumeration CLAIM schedule failed");
    CHECK(run_until_ok(
              &fixture.world,
              fixture.world.transmissions[tx_index].end_us + RX_GUARD_US + 1u,
              "gateway-claim"),
          "gateway enumeration CLAIM simulation failed");
    CHECK(decoded_from_to(&fixture.world, GATEWAY_ID, RELAY_ID, 0u) > 0u,
          "direct relay did not decode the gateway enumeration CLAIM");

    if (relay_wake) {
        uint64_t relay_wake_start_us =
            fixture.world.roles[fixture.relay].runtime.radio_busy_until_us +
            1000u;

        /* The control relay takes over the awake radio instead of returning
         * to its periodic scanner before it transmits downstream. */
        fixture.world.roles[fixture.relay]
            .resume_low_duty_after_ds_twr = false;
        CHECK(schedule_wake_train(
                  &fixture.world, fixture.relay, relay_wake_start_us,
                  MESH_RADIO_WAKE_TRAIN_MS, UINT32_C(0xe002),
                  &relay_wake_end_us),
              "relay activation train schedule failed");
        CHECK(run_until_ok(&fixture.world, relay_wake_end_us + 1u,
                           "relay-wake"),
              "relay activation train simulation failed");
        CHECK(fixture.world.roles[fixture.forced]
                      .anchor_session.diagnostics.claims > 0u,
              "forced anchor did not wake from the relayed train");
        relay_control_start_us = relay_wake_end_us + CONTROL_TURNAROUND_US;
        CHECK(schedule_control_rx(&fixture.world, fixture.forced,
                                  relay_control_start_us, command_len),
              "forced-anchor control listener schedule failed");
    } else {
        relay_control_start_us = fixture.world.now_us + CONTROL_TURNAROUND_US;
    }

    first_reception = fixture.world.reception_count;
    CHECK(mesh_sim_schedule_raw_tx(
              &fixture.world, fixture.relay, relay_control_start_us,
              UWB_CHANNEL_WAKE_CONTACT,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              command_frame, command_len, false, &tx_index) == MESH_SIM_OK,
          "relayed enumeration CLAIM schedule failed");
    CHECK(run_until_ok(
              &fixture.world,
              fixture.world.transmissions[tx_index].end_us + RX_GUARD_US + 1u,
              "relay-claim"),
          "relayed enumeration CLAIM simulation failed");
    return decoded_from_to(&fixture.world, RELAY_ID, FORCED_ID,
                           first_reception) > 0u;
}

static bool survey_start_reuses_retained_enumeration_handoff(void)
{
    static struct fixture fixture;
    uint8_t command_frame[PACKET_EXT_MAX_LEN];
    const uint64_t handoff_start_us = UINT64_C(1000);
    const uint64_t handoff_end_us = handoff_start_us +
        (uint64_t)SURVEY_ENUMERATION_HANDOFF_HOLD_MS * 1000u;
    const uint64_t gateway_control_start_us = handoff_end_us -
        UINT64_C(1000000);
    uint64_t relay_control_start_us;
    uint16_t gateway_tx_index = UINT16_MAX;
    uint16_t relay_tx_index = UINT16_MAX;
    size_t command_len = 0u;
    size_t first_reception;

    CHECK(setup_retained_handoff_fixture(&fixture),
          "retained-handoff fixture setup failed");
    CHECK(build_survey_start(command_frame, sizeof(command_frame),
                             &command_len),
          "survey START frame build failed");
    CHECK(mesh_sim_schedule_raw_tx(
              &fixture.world, fixture.gateway, gateway_control_start_us,
              UWB_CHANNEL_WAKE_CONTACT,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              command_frame, command_len, false,
              &gateway_tx_index) == MESH_SIM_OK,
          "wake-free gateway survey START schedule failed");
    CHECK(mesh_sim_schedule_rx(
              &fixture.world, fixture.relay,
              handoff_start_us,
              fixture.world.transmissions[gateway_tx_index].end_us +
                  RX_GUARD_US,
              UWB_CHANNEL_WAKE_CONTACT,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              NULL) == MESH_SIM_OK,
          "relay retained enumeration listener schedule failed");
    CHECK(mesh_sim_schedule_rx(
              &fixture.world, fixture.forced,
              handoff_start_us,
              handoff_end_us,
              UWB_CHANNEL_WAKE_CONTACT,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              NULL) == MESH_SIM_OK,
          "forced-anchor retained enumeration listener schedule failed");
    CHECK(run_until_ok(
              &fixture.world,
              fixture.world.transmissions[gateway_tx_index].end_us +
                  RX_GUARD_US + 1u,
              "wake-free-gateway-survey-start"),
          "gateway survey START simulation failed");
    CHECK(decoded_from_to(&fixture.world, GATEWAY_ID, RELAY_ID, 0u) > 0u,
          "relay did not decode wake-free survey START near handoff expiry");

    relay_control_start_us = fixture.world.now_us +
        (uint64_t)DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS * 1000u;
    CHECK(relay_control_start_us < handoff_end_us,
          "relay control bound exceeds enumeration handoff");
    first_reception = fixture.world.reception_count;
    CHECK(mesh_sim_schedule_raw_tx(
              &fixture.world, fixture.relay, relay_control_start_us,
              UWB_CHANNEL_WAKE_CONTACT,
              MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
              command_frame, command_len, false,
              &relay_tx_index) == MESH_SIM_OK,
          "wake-free relayed survey START schedule failed");
    CHECK(run_until_ok(
              &fixture.world,
              fixture.world.transmissions[relay_tx_index].end_us +
                  RX_GUARD_US + 1u,
              "wake-free-relayed-survey-start"),
          "relayed survey START simulation failed");
    CHECK(decoded_from_to(&fixture.world, RELAY_ID, FORCED_ID,
                          first_reception) > 0u,
          "forced anchor did not decode wake-free relayed survey START");
    CHECK(fixture.world.roles[fixture.relay]
                  .anchor_session.diagnostics.claims == 0u &&
              fixture.world.roles[fixture.forced]
                  .anchor_session.diagnostics.claims == 0u,
          "survey START unexpectedly used a wake claim");
    return true;
}

int main(void)
{
    bool without_relay_wake = enumeration_reaches_forced_anchor(false);
    bool with_relay_wake = enumeration_reaches_forced_anchor(true);
    bool survey_without_wake =
        survey_start_reuses_retained_enumeration_handoff();

    printf("TRACE forced-hop enumeration without_relay_wake=%u "
           "with_relay_wake=%u survey_without_wake=%u\n",
           without_relay_wake ? 1u : 0u,
           with_relay_wake ? 1u : 0u,
           survey_without_wake ? 1u : 0u);
    if (without_relay_wake || !with_relay_wake || !survey_without_wake) {
        fprintf(stderr,
                "RESULT forced-hop enumeration/survey wake without_relay=%u "
                "with_relay=%u survey_without_wake=%u\n",
                without_relay_wake ? 1u : 0u,
                with_relay_wake ? 1u : 0u,
                survey_without_wake ? 1u : 0u);
        return 1;
    }
    puts("PASS forced-hop enumeration wake is retained for wake-free survey START");
    return 0;
}
