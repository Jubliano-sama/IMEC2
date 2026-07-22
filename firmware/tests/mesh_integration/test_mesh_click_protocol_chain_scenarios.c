#include "app_gateway_ble_stream.h"
#include "gateway_ble_transport.h"
#include "mesh_sim.h"
#include "mesh_sim_invariants.h"
#include "report.h"
#include "uwb.h"
#include "uwb_session.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * This test deliberately keeps the world in static storage.  The simulator
 * owns sizeable telemetry arrays and putting it on the test stack hides
 * failures behind a host-side stack overflow.
 *
 * This is a joined native protocol-component chain, not a Zephyr role-handler
 * test. Raw UWB frames cross exact simulator airtime windows, then the test
 * explicitly invokes the production codecs and session owners for the next
 * state transition.
 */
#define CLICK_NETWORK_ID UINT32_C(0x494d4543)
#define CLICKER_ID UINT64_C(0xc100000000000001)
#define SOURCE_ID UINT64_C(0xc200000000000002)
#define RELAY1_ID UINT64_C(0xc300000000000003)
#define RELAY2_ID UINT64_C(0xc400000000000004)
#define GATEWAY_ID UINT64_C(0xc500000000000005)
#define CLICK_EVENT_ID UINT32_C(0x22070001)
#define CLICK_NONCE UINT64_C(0x0102030405060708)
#define ROUTE_EPOCH UINT32_C(22)
#define PHY_HOP_GAP_US UINT64_C(1000)
#define DIRECT_TX_PREPARE_US UINT64_C(20000)
#define DIRECT_PAYLOAD_SERVICE_US UINT64_C(50000)
#define DIRECT_ACK_SERVICE_US UINT64_C(40000)
#define DIRECT_RX_GUARD_US UINT64_C(1)
#define MAX_MESH_STEPS 240u

static int failures;

#define CHECK(expr, ...) do {                                                \
    if (!(expr)) {                                                           \
        fprintf(stderr, "FAIL line=%d ", __LINE__);                         \
        fprintf(stderr, __VA_ARGS__);                                       \
        fputc('\n', stderr);                                                 \
        failures++;                                                          \
        return false;                                                        \
    }                                                                        \
} while (0)

struct click_fixture {
    struct mesh_sim_world world;
    uint8_t clicker;
    uint8_t source;
    uint8_t relay1;
    uint8_t relay2;
    uint8_t gateway;
    uint16_t relay1_relay2_connection;
    uint16_t source_relay1_connection;
};

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .event_interval_ms = 360u,
        .event_window_ms = 25u,
        .first_event_time_ms = first_event_ms,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static int exact_air_hop(struct mesh_sim_world *world,
                         uint8_t sender,
                         uint8_t receiver,
                         uint64_t *at_us,
                         uint8_t channel,
                         enum mesh_sim_phy phy,
                         const uint8_t *frame,
                         size_t frame_len,
                         uint8_t msg_type)
{
    size_t before;
    uint16_t tx_index = UINT16_MAX;
    uint64_t arrival_start;
    uint64_t arrival_end;
    int ret;

    if (world == NULL || at_us == NULL || frame == NULL || frame_len == 0u) {
        return MESH_SIM_ERR_ARG;
    }
    before = world->reception_count;
    ret = mesh_sim_schedule_raw_tx(world, sender, *at_us, channel, phy,
                                   frame, frame_len, false, &tx_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    world->transmissions[tx_index].protocol_msg_type = msg_type;
    arrival_start = *at_us + world->propagation_us[sender][receiver];
    arrival_end = world->transmissions[tx_index].end_us +
                  world->propagation_us[sender][receiver];
    ret = mesh_sim_schedule_rx(world, receiver, arrival_start, arrival_end,
                               channel, phy, NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_run_until(world, arrival_end + 1u);
    }
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    if (world->reception_count != before + 1u ||
        world->receptions[before].outcome != MESH_SIM_RX_DECODED) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    *at_us = arrival_end + PHY_HOP_GAP_US;
    return MESH_SIM_OK;
}

static bool setup_fixture(struct click_fixture *fixture)
{
    struct uwb_clicker_config clicker_config = {
        .network_id = CLICK_NETWORK_ID,
        .clicker_id = CLICKER_ID,
        .click_event_id = CLICK_EVENT_ID,
        .nonce = CLICK_NONCE,
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 3u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    struct uwb_anchor_config anchor_config = {
        .network_id = CLICK_NETWORK_ID,
        .anchor_id = SOURCE_ID,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
    };
    struct mesh_event_params source_relay1 = connection_params(7100u);
    struct mesh_event_params relay1_relay2 = connection_params(7200u);
    int ret;

    memset(fixture, 0, sizeof(*fixture));
    mesh_sim_init(&fixture->world, UINT32_C(0x2207cafe));
    ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_CLICKER,
                            CLICKER_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &fixture->clicker);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                                SOURCE_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &fixture->source);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                                RELAY1_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &fixture->relay1);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_ANCHOR,
                                RELAY2_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &fixture->relay2);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&fixture->world, MESH_SIM_ROLE_GATEWAY,
                                GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &fixture->gateway);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&fixture->world, fixture->clicker,
                                fixture->source, 98u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&fixture->world, fixture->source,
                                fixture->relay1, 96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&fixture->world, fixture->relay1,
                                fixture->relay2, 96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&fixture->world, fixture->relay2,
                                fixture->gateway, 96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_init_clicker_session(&fixture->world,
                                            fixture->clicker,
                                            &clicker_config);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_init_anchor_session(&fixture->world,
                                           fixture->source,
                                           &anchor_config);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_route(&fixture->world, fixture->source,
                                     fixture->relay1, 2u, ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_route(&fixture->world, fixture->relay1,
                                     fixture->relay2, 1u, ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_relay_note_direct_gateway_route(
            &fixture->world.roles[fixture->relay2].relay, 0u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_downlink(&fixture->world, fixture->gateway,
                                        SOURCE_ID, fixture->relay2, 3u,
                                        ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_downlink(&fixture->world, fixture->relay2,
                                        SOURCE_ID, fixture->relay1, 2u,
                                        ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_downlink(&fixture->world, fixture->relay1,
                                        SOURCE_ID, fixture->source, 1u,
                                        ROUTE_EPOCH);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_connection(&fixture->world, fixture->source,
                                      fixture->relay1, &source_relay1,
                                      true, &fixture->source_relay1_connection);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_connection(&fixture->world, fixture->relay1,
                                      fixture->relay2, &relay1_relay2,
                                      true, &fixture->relay1_relay2_connection);
    }
    CHECK(ret == MESH_SIM_OK, "fixture setup failed ret=%d", ret);
    return true;
}

static bool run_click_uwb_path(struct click_fixture *fixture,
                               struct proto_packet *report_packet,
                               uint8_t *report_payload,
                               size_t *report_payload_len)
{
    struct mesh_sim_world *world = &fixture->world;
    struct mesh_sim_role_instance *clicker = &world->roles[fixture->clicker];
    struct mesh_sim_role_instance *source = &world->roles[fixture->source];
    struct uwb_wake_claim_frame claim;
    struct uwb_wake_claim_frame decoded_claim;
    struct uwb_discover_frame discover;
    struct uwb_discover_frame decoded_discover;
    struct uwb_discovery_reply_frame reply;
    struct uwb_discovery_reply_frame decoded_reply;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_schedule_frame decoded_schedule;
    struct uwb_range_step step;
    struct uwb_range_exchange_identity identity;
    struct uwb_response_frame response;
    struct uwb_final_frame final;
    struct uwb_report_frame range_report;
    struct uwb_range_header poll_header;
    struct uwb_range_header decoded_poll;
    uint8_t frame[UWB_RANGE_SCHEDULE_MAX_LEN];
    uint8_t poll[UWB_POLL_LEN];
    uint8_t response_bytes[UWB_RESP_LEN];
    uint8_t final_bytes[UWB_FINAL_LEN];
    uint8_t report_bytes[UWB_REPORT_LEN];
    size_t frame_len;
    size_t written;
    uint64_t at_us = 5000u;
    uint16_t rx_window;
    int ret;

    /* A bounded explicit receive window models the scanner's wake train. */
    ret = mesh_sim_schedule_rx(world, fixture->source, 0u, 500000u,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE, &rx_window);
    CHECK(ret == MESH_SIM_OK, "wake RX window failed ret=%d", ret);

    for (uint16_t i = 0u; i < 5u; i++) {
        ret = uwb_clicker_build_wake_claim(&clicker->clicker_session,
                                           CLICKER_ID,
                                           (uint16_t)(400u - (i * 70u)),
                                           420u, 1200u, &claim);
        CHECK(ret == PROTO_OK, "wake claim build failed ret=%d", ret);
        ret = uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len);
        CHECK(ret == PROTO_OK && frame_len == UWB_WAKE_CLAIM_LEN,
              "wake claim encode failed ret=%d len=%zu", ret, frame_len);
        ret = uwb_decode_wake_claim(frame, frame_len, &decoded_claim);
        CHECK(ret == PROTO_OK && decoded_claim.click_event_id == CLICK_EVENT_ID,
              "wake claim decode failed ret=%d", ret);
        ret = mesh_sim_schedule_raw_tx(world, fixture->clicker, at_us,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_WAKE,
                                       frame, frame_len, false, NULL);
        CHECK(ret == MESH_SIM_OK, "wake train TX failed ret=%d", ret);
        at_us += 80000u;
    }
    uwb_clicker_note_wake_claim_tx(&clicker->clicker_session, 5u);
    ret = mesh_sim_run_until(world, at_us + 1u);
    CHECK(ret == MESH_SIM_OK, "wake train run failed ret=%d", ret);
    /* Finish the scanner window before the clicker sends discovery. */
    ret = mesh_sim_run_until(world, 500001u);
    CHECK(ret == MESH_SIM_OK, "wake scanner handoff failed ret=%d", ret);
    CHECK(source->anchor_session.epoch.active &&
              source->runtime.radio_owner == MESH_RUNTIME_RADIO_DS_TWR &&
              mesh_sim_count_transitions(world,
                                         MESH_SIM_TRANSITION_WAKE_CLAIM_OWNED,
                                         SOURCE_ID) > 0u,
          "wake claim did not own the channel-5 train");

    ret = uwb_clicker_build_discover(&clicker->clicker_session, &discover);
    CHECK(ret == PROTO_OK, "discover build failed ret=%d", ret);
    ret = uwb_encode_discover(&discover, frame, sizeof(frame), &frame_len);
    CHECK(ret == PROTO_OK, "discover encode failed ret=%d", ret);
    ret = uwb_decode_discover(frame, frame_len, &decoded_discover);
    CHECK(ret == PROTO_OK, "discover decode failed ret=%d", ret);
    at_us = world->now_us + 5000u;
    ret = exact_air_hop(world, fixture->clicker, fixture->source, &at_us,
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame, frame_len,
                        MSG_UWB_DISCOVER);
    CHECK(ret == MESH_SIM_OK, "discover airtime hop failed ret=%d", ret);
    ret = uwb_anchor_build_discovery_reply(&source->anchor_session,
                                           &decoded_discover, 97u, 3300u,
                                           &reply);
    CHECK(ret == PROTO_OK, "discovery reply build failed ret=%d", ret);
    ret = uwb_encode_discovery_reply(&reply, frame, sizeof(frame), &frame_len);
    CHECK(ret == PROTO_OK, "discovery reply encode failed ret=%d", ret);
    ret = uwb_decode_discovery_reply(frame, frame_len, &decoded_reply);
    CHECK(ret == PROTO_OK, "discovery reply decode failed ret=%d", ret);
    at_us = world->now_us + 5000u;
    ret = exact_air_hop(world, fixture->source, fixture->clicker, &at_us,
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame, frame_len,
                        MSG_UWB_DISCOVERY_REPLY);
    CHECK(ret == MESH_SIM_OK, "discovery reply airtime failed ret=%d", ret);
    ret = uwb_clicker_note_discovery_reply(&clicker->clicker_session,
                                           &decoded_reply);
    CHECK(ret == PROTO_OK && clicker->clicker_session.candidate_count == 1u,
          "discovery reply was not admitted ret=%d", ret);

    ret = uwb_clicker_build_range_schedule(&clicker->clicker_session,
                                           UWB_DS_TWR_REPLY_DELAY_US,
                                           10u, 50u, &schedule);
    CHECK(ret == PROTO_OK, "range schedule build failed ret=%d", ret);
    ret = uwb_encode_range_schedule(&schedule, frame, sizeof(frame), &frame_len);
    CHECK(ret == PROTO_OK, "range schedule encode failed ret=%d", ret);
    ret = uwb_decode_range_schedule(frame, frame_len, &decoded_schedule);
    CHECK(ret == PROTO_OK, "range schedule decode failed ret=%d", ret);
    ret = uwb_anchor_accept_range_schedule(&source->anchor_session,
                                           &decoded_schedule,
                                           (uint32_t)(world->now_us / 1000u),
                                           100u);
    CHECK(ret == PROTO_OK && source->anchor_session.scheduled,
          "range schedule was not admitted ret=%d", ret);

    ret = uwb_clicker_next_range_step(&clicker->clicker_session, &step);
    CHECK(ret == PROTO_OK && step.anchor_id == SOURCE_ID,
          "range step selection failed ret=%d", ret);
    identity = (struct uwb_range_exchange_identity) {
        .network_id = CLICK_NETWORK_ID,
        .clicker_id = CLICKER_ID,
        .click_event_id = CLICK_EVENT_ID,
        .attempt_index = clicker->clicker_session.attempt_index,
        .nonce = CLICK_NONCE,
        .anchor_id = SOURCE_ID,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .seq = step.seq,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    CHECK(uwb_anchor_accepts_range_exchange(&source->anchor_session,
                                            &identity),
          "anchor rejected scheduled range identity");
    poll_header = (struct uwb_range_header) {
        .type = MSG_UWB_POLL,
        .seq = step.seq,
        .round_index = step.round_index,
        .network_id = CLICK_NETWORK_ID,
        .session_id = CLICK_EVENT_ID,
        .session_nonce = CLICK_NONCE,
        .initiator_short_addr = uwb_session_short_addr_from_id(CLICKER_ID),
        .responder_short_addr = uwb_session_short_addr_from_id(SOURCE_ID),
        .flags = FLAG_COUNT_AS_CLICK,
        .initiator_id = CLICKER_ID,
        .responder_id = SOURCE_ID,
    };
    response.header = poll_header;
    response.header.type = MSG_UWB_RESP;
    response.poll_rx_ts_32 = 0x11223344u;
    response.resp_tx_ts_32 = 0x1122aabbu;
    final.header = poll_header;
    final.header.type = MSG_UWB_FINAL;
    final.poll_tx_ts_32 = 0x11223344u;
    final.resp_rx_ts_32 = 0x1122aabbu;
    final.final_tx_ts_32 = 0x55667788u;
    final.diagnostic_flags = 0u;
    final.clicker_clock_offset_raw = 0;
    range_report.header = poll_header;
    range_report.header.type = MSG_UWB_REPORT;
    range_report.distance_mm = 1234;
    range_report.quality = 94u;
    range_report.status = RANGE_OK;
    range_report.rsl_dbm = -52;

    ret = uwb_encode_poll(&poll_header, poll, sizeof(poll), &written);
    CHECK(ret == PROTO_OK && written == sizeof(poll), "POLL encode failed ret=%d", ret);
    at_us = world->now_us + 3000u;
    ret = exact_air_hop(world, fixture->clicker, fixture->source, &at_us,
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_RANGE, poll, written,
                        MSG_UWB_POLL);
    CHECK(ret == MESH_SIM_OK, "POLL airtime failed ret=%d", ret);
    ret = uwb_decode_poll(poll, written, &decoded_poll);
    CHECK(ret == PROTO_OK, "POLL parser failed ret=%d", ret);

    ret = uwb_encode_response(&response, response_bytes, sizeof(response_bytes),
                              &written);
    CHECK(ret == PROTO_OK && written == sizeof(response_bytes),
          "RESP encode failed ret=%d", ret);
    at_us += UWB_DS_TWR_REPLY_DELAY_US - PHY_HOP_GAP_US;
    ret = exact_air_hop(world, fixture->source, fixture->clicker, &at_us,
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_RANGE, response_bytes, written,
                        MSG_UWB_RESP);
    CHECK(ret == MESH_SIM_OK, "RESP airtime failed ret=%d", ret);
    {
        struct uwb_response_frame decoded_response;
        ret = uwb_decode_response(response_bytes, written, &decoded_response);
        CHECK(ret == PROTO_OK && decoded_response.header.seq == step.seq,
              "RESP parser failed ret=%d", ret);
    }
    ret = uwb_encode_final(&final, final_bytes, sizeof(final_bytes), &written);
    CHECK(ret == PROTO_OK && written == sizeof(final_bytes),
          "FINAL encode failed ret=%d", ret);
    at_us += UWB_DS_TWR_REPLY_DELAY_US - PHY_HOP_GAP_US;
    ret = exact_air_hop(world, fixture->clicker, fixture->source, &at_us,
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_RANGE, final_bytes, written,
                        MSG_UWB_FINAL);
    CHECK(ret == MESH_SIM_OK, "FINAL airtime failed ret=%d", ret);
    {
        struct uwb_final_frame decoded_final;
        ret = uwb_decode_final(final_bytes, written, &decoded_final);
        CHECK(ret == PROTO_OK && decoded_final.header.seq == step.seq,
              "FINAL parser failed ret=%d", ret);
    }
    ret = uwb_encode_report(&range_report, report_bytes, sizeof(report_bytes),
                            &written);
    CHECK(ret == PROTO_OK && written == sizeof(report_bytes),
          "REPORT encode failed ret=%d", ret);
    ret = exact_air_hop(world, fixture->source, fixture->clicker, &at_us,
                        UWB_CHANNEL_WAKE_CONTACT,
                        MESH_SIM_PHY_CHANNEL5_RANGE, report_bytes, written,
                        MSG_UWB_REPORT);
    CHECK(ret == MESH_SIM_OK, "REPORT airtime failed ret=%d", ret);
    {
        struct uwb_report_frame decoded_report;
        ret = uwb_decode_report(report_bytes, written, &decoded_report);
        CHECK(ret == PROTO_OK && decoded_report.status == RANGE_OK,
              "REPORT parser failed ret=%d", ret);
    }
    ret = uwb_anchor_note_range_result(&source->anchor_session, RANGE_OK);
    CHECK(ret == PROTO_OK, "anchor result accounting failed ret=%d", ret);
    ret = uwb_clicker_record_range_result(&clicker->clicker_session, &step,
                                         RANGE_OK);
    CHECK(ret == PROTO_OK, "clicker result accounting failed ret=%d", ret);
    ret = uwb_clicker_next_range_step(&clicker->clicker_session, &step);
    CHECK(ret == PROTO_ERR_NOT_FOUND &&
              clicker->clicker_session.state == UWB_CLICKER_SUCCEEDED,
          "clicker did not settle after successful DS-TWR ret=%d", ret);

    {
        struct range_report_fields fields = {
            .clicker_id = CLICKER_ID,
            .anchor_id = SOURCE_ID,
            .event_seq = CLICK_EVENT_ID,
            .timestamp_ms = world->now_us / 1000u,
            .distance_mm = range_report.distance_mm,
            .quality = range_report.quality,
            .range_status = RANGE_OK,
            .omit_rsl = true,
            .omit_cir = true,
        };
        size_t length = 0u;
        ret = report_append_range_tlvs(report_payload,
                                       UWB_MESH_MAX_PAYLOAD_LEN,
                                       &length, &fields);
        CHECK(ret == PROTO_OK && length <= UINT8_MAX,
              "click report TLV build failed ret=%d len=%zu", ret, length);
        ret = report_init_click_packet(report_packet, SOURCE_ID, GATEWAY_ID,
                                       CLICK_EVENT_ID, 1u, (uint8_t)length);
        CHECK(ret == PROTO_OK, "click report packet build failed ret=%d", ret);
        *report_payload_len = length;
    }
    return true;
}

/* Deliberately model the unachievable production release combination. */
static bool test_release_contract_gap(void)
{
    struct uwb_clicker_session clicker;
    struct uwb_clicker_config cfg = {
        .network_id = CLICK_NETWORK_ID,
        .clicker_id = CLICKER_ID,
        .click_event_id = CLICK_EVENT_ID + 1u,
        .nonce = CLICK_NONCE + 1u,
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    struct uwb_wake_claim_frame claim;
    struct uwb_discover_frame discover;
    struct uwb_range_release_frame release;

    CHECK(uwb_clicker_session_start(&clicker, &cfg) == PROTO_OK,
          "release gap fixture did not start");
    CHECK(uwb_clicker_build_wake_claim(&clicker, CLICKER_ID, 50u, 50u, 100u,
                                       &claim) == PROTO_OK,
          "release gap claim failed");
    CHECK(uwb_clicker_build_discover(&clicker, &discover) == PROTO_OK,
          "release gap discover failed");
    CHECK(uwb_clicker_build_range_release(
              &clicker, UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
              &release) == PROTO_ERR_NOT_FOUND,
          "zero discovered anchors unexpectedly produced a release");
    cfg.min_anchor_count = 2u;
    CHECK(uwb_clicker_session_start(&clicker, &cfg) == PROTO_ERR_MALFORMED,
          "count-as-click accepted min_anchor_count=2");
    return true;
}

static bool test_partial_and_collision_boundaries(void)
{
    static struct mesh_sim_world world;
    uint8_t tx_node;
    uint8_t rx_node;
    uint8_t interferer;
    uint8_t frame[UWB_WAKE_CLAIM_LEN] = {0};
    uint16_t tx1;
    uint16_t tx2;
    uint64_t end;
    int ret;

    mesh_sim_init(&world, UINT32_C(0x2207bad));
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, SOURCE_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &rx_node);
    CHECK(ret == MESH_SIM_OK, "boundary receiver setup failed ret=%d", ret);
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY1_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &tx_node);
    CHECK(ret == MESH_SIM_OK, "boundary sender setup failed ret=%d", ret);
    ret = mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY2_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &interferer);
    CHECK(ret == MESH_SIM_OK, "boundary interferer setup failed ret=%d", ret);
    CHECK(mesh_sim_set_link(&world, tx_node, rx_node, 100u, 1u) == MESH_SIM_OK &&
              mesh_sim_set_link(&world, interferer, rx_node, 100u, 1u) == MESH_SIM_OK,
          "boundary links failed");
    ret = mesh_sim_schedule_raw_tx(&world, tx_node, 1000u,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   frame, sizeof(frame), false, &tx1);
    CHECK(ret == MESH_SIM_OK, "partial TX setup failed ret=%d", ret);
    CHECK(mesh_sim_schedule_rx(&world, rx_node,
                               1000u + 50u,
                               world.transmissions[tx1].end_us - 50u,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE, NULL) == MESH_SIM_OK,
          "partial RX setup failed");
    end = world.transmissions[tx1].end_us + 1u;
    CHECK(mesh_sim_run_until(&world, end) == MESH_SIM_OK &&
              world.reception_count == 1u &&
              world.receptions[0].outcome != MESH_SIM_RX_DECODED,
          "partial frame was accepted outcome=%d", world.receptions[0].outcome);

    mesh_sim_init(&world, UINT32_C(0x2207bad2));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, SOURCE_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &rx_node) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY1_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &tx_node) == MESH_SIM_OK &&
              mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY2_ID,
                                GATEWAY_ID, ROUTE_EPOCH, &interferer) == MESH_SIM_OK,
          "collision fixture setup failed");
    CHECK(mesh_sim_set_link(&world, tx_node, rx_node, 100u, 1u) == MESH_SIM_OK &&
              mesh_sim_set_link(&world, interferer, rx_node, 100u, 1u) == MESH_SIM_OK,
          "collision links failed");
    CHECK(mesh_sim_schedule_rx(&world, rx_node, 1000u, 100000u,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_WAKE, NULL) == MESH_SIM_OK,
          "collision RX setup failed");
    CHECK(mesh_sim_schedule_raw_tx(&world, tx_node, 1001u,
                                   UWB_CHANNEL_WAKE_CONTACT,
                                   MESH_SIM_PHY_CHANNEL5_WAKE,
                                   frame, sizeof(frame), false, &tx1) == MESH_SIM_OK &&
              mesh_sim_schedule_raw_tx(&world, interferer, 1001u,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       MESH_SIM_PHY_CHANNEL5_WAKE,
                                       frame, sizeof(frame), false, &tx2) == MESH_SIM_OK,
          "collision TX setup failed");
    end = world.transmissions[tx1].end_us > world.transmissions[tx2].end_us ?
          world.transmissions[tx1].end_us : world.transmissions[tx2].end_us;
    CHECK(mesh_sim_run_until(&world, end + 1u) == MESH_SIM_OK &&
              world.reception_count == 2u &&
              world.receptions[0].outcome == MESH_SIM_RX_COLLISION &&
              world.receptions[1].outcome == MESH_SIM_RX_COLLISION,
          "collision was not explicit receptions=%zu", world.reception_count);
    return true;
}

struct ble_send_context {
    struct gateway_ble_link *link;
    int last_status;
    uint32_t calls;
};

static int send_stream_record(const uint8_t *record, size_t record_len, void *ctx)
{
    struct ble_send_context *send = ctx;

    (void)record;
    send->calls++;
    send->last_status = gateway_ble_link_try_notify(send->link, record_len);
    return send->last_status;
}

static bool test_ble_credit_refusal_and_drain(const struct proto_packet *packet,
                                              const uint8_t *payload,
                                              size_t payload_len)
{
    struct gateway_ble_stream_state stream;
    struct gateway_ble_stream_diagnostics diagnostics;
    struct gateway_ble_link link;
    struct ble_send_context send;
    uint8_t completed;
    int ret;

    gateway_ble_stream_init(&stream);
    gateway_ble_link_init(&link, 30000u, 1u);
    CHECK(gateway_ble_link_connect(&link, 0u, 247u, true) ==
              GATEWAY_BLE_LINK_OK, "BLE link connect failed");
    CHECK(gateway_ble_stream_enqueue_packet(&stream, packet, payload,
                                             payload_len, 1u, 1u, true) == 1,
          "gateway stream enqueue failed");
    /* Occupy the sole controller credit so the first drain is a real refusal. */
    CHECK(gateway_ble_link_try_notify(&link, 1u) == GATEWAY_BLE_LINK_OK,
          "BLE credit priming failed");
    send = (struct ble_send_context) {.link = &link};
    CHECK(gateway_ble_stream_drain(&stream, send_stream_record, &send, 2u,
                                   true, 1u) == 0u &&
              send.last_status == GATEWAY_BLE_LINK_ERR_NO_CREDIT &&
              gateway_ble_stream_depth(&stream) == 1u,
          "stream dropped record on transient NO_CREDIT");
    ret = gateway_ble_link_run_connection_event(&link, link.next_event_us,
                                                &completed);
    CHECK(ret == GATEWAY_BLE_LINK_OK && completed == 1u,
          "BLE connection event did not release credit ret=%d completed=%u",
          ret, completed);
    CHECK(gateway_ble_stream_drain(&stream, send_stream_record, &send, 31u,
                                   true, 1u) == 1u &&
              gateway_ble_stream_depth(&stream) == 0u,
          "stream did not drain after credit recovery");
    CHECK(gateway_ble_stream_drain(&stream, send_stream_record, &send, 32u,
                                   true, 1u) == 0u,
          "stream redelivered an already retired record");
    gateway_ble_stream_get_diagnostics(&stream, 32u, &diagnostics);
    CHECK(diagnostics.packets_sent == 1u && diagnostics.enqueue_attempts == 1u,
          "BLE stream accounting was not exactly once sent=%" PRIu32,
          diagnostics.packets_sent);
    return true;
}

static bool test_click_path(void)
{
    static struct click_fixture fixture;
    struct proto_packet report_packet;
    uint8_t report_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t report_payload_len = 0u;
    struct mesh_sim_invariant_report invariant;
    int ret;

    CHECK(setup_fixture(&fixture), "fixture setup failed");
    CHECK(run_click_uwb_path(&fixture, &report_packet, report_payload,
                             &report_payload_len), "UWB click path failed");
    CHECK(mesh_sim_queue_originated(&fixture.world, fixture.source,
                                    &report_packet, report_payload,
                                    report_payload_len) == MESH_SIM_OK,
          "click report was not queued at source");

    for (uint32_t step = 0u; step < MAX_MESH_STEPS; step++) {
        bool direct = false;
        int action_ret;

        if (fixture.world.roles[fixture.gateway].delivery_count == 1u &&
            fixture.world.roles[fixture.source].tx_queue_count == 0u &&
            fixture.world.roles[fixture.relay1].tx_queue_count == 0u &&
            fixture.world.roles[fixture.relay2].tx_queue_count == 0u &&
            fixture.world.roles[fixture.source].relay.pending.state ==
                MESH_RELAY_TX_IDLE &&
            fixture.world.roles[fixture.relay1].relay.pending.state ==
                MESH_RELAY_TX_IDLE &&
            fixture.world.roles[fixture.relay2].relay.pending.state ==
                MESH_RELAY_TX_IDLE) {
            break;
        }
        for (size_t i = 0u; i < fixture.world.role_count; i++) {
            const struct mesh_pending_tx *pending =
                &fixture.world.roles[i].relay.pending;
            uint32_t deadline_ms = 0u;
            if (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
                pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD) {
                deadline_ms = pending->gateway_ack_deadline_ms;
            } else if (pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
                       pending->state == MESH_RELAY_TX_WAIT_RESULT_GRANT) {
                deadline_ms = pending->retry_after_ms;
            }
            if (deadline_ms != 0u) {
                uint64_t at = (uint64_t)deadline_ms * 1000u;
                if (at < fixture.world.now_us) {
                    at = fixture.world.now_us;
                }
                action_ret = mesh_sim_schedule_relay_tick(&fixture.world,
                                                           (uint8_t)i, at);
                CHECK(action_ret == MESH_SIM_OK,
                      "relay deadline scheduling failed ret=%d", action_ret);
            }
        }
        for (size_t i = 0u; i < MESH_SIM_TX_QUEUE_CAPACITY; i++) {
            if (fixture.world.roles[fixture.relay2].tx_queue[i].valid &&
                fixture.world.roles[fixture.relay2].tx_queue[i].outbound.next_hop_id ==
                    GATEWAY_ID) {
                direct = true;
                break;
            }
        }
        if (direct) {
            uint16_t tx_index;
            uint16_t ack_index;
            uint64_t ready = fixture.world.now_us;
            uint64_t payload_start;
            uint64_t payload_deadline;
            uint64_t payload_arrival_end;
            uint64_t gateway_rx_end;
            uint64_t ack_start;
            uint64_t ack_end;
            if (fixture.world.roles[fixture.relay2].dwm3000.cpu_busy_until_us > ready) {
                ready = fixture.world.roles[fixture.relay2].dwm3000.cpu_busy_until_us;
            }
            if (fixture.world.roles[fixture.gateway].dwm3000.cpu_busy_until_us > ready) {
                ready = fixture.world.roles[fixture.gateway].dwm3000.cpu_busy_until_us;
            }
            CHECK(mesh_sim_run_until(&fixture.world, ready) == MESH_SIM_OK,
                  "direct turn runtime readiness failed");
            payload_start = fixture.world.now_us + DIRECT_TX_PREPARE_US;
            payload_deadline = payload_start + DIRECT_PAYLOAD_SERVICE_US;
            CHECK(mesh_sim_direct_gateway_start_queued_tx(
                      &fixture.world, fixture.relay2, payload_start,
                      payload_deadline, &tx_index) == MESH_SIM_OK,
                  "direct gateway payload start failed");
            payload_arrival_end = fixture.world.transmissions[tx_index].end_us +
                                  fixture.world.propagation_us[fixture.relay2][fixture.gateway];
            gateway_rx_end = payload_arrival_end + DIRECT_RX_GUARD_US;
            CHECK(mesh_sim_direct_gateway_arm_rx(&fixture.world, fixture.gateway,
                                                 payload_start, gateway_rx_end) == MESH_SIM_OK &&
                      mesh_sim_run_until(&fixture.world, gateway_rx_end) == MESH_SIM_OK,
                  "direct gateway RX failed");
            CHECK(fixture.world.roles[fixture.gateway].delivery_count == 1u,
                  "gateway did not deliver click report");
            CHECK(mesh_sim_run_until(&fixture.world,
                                     fixture.world.roles[fixture.gateway].dwm3000.cpu_busy_until_us) ==
                      MESH_SIM_OK,
                  "gateway ACK preparation failed");
            ack_start = fixture.world.now_us + 50000u;
            ack_end = ack_start + DIRECT_ACK_SERVICE_US;
            ret = mesh_sim_direct_gateway_schedule_ack(
                &fixture.world, fixture.gateway, fixture.relay2,
                ack_start, ack_end, &ack_index);
            if (ret == MESH_SIM_OK) {
                ret = mesh_sim_run_until(&fixture.world, ack_end);
            }
            CHECK(ret == MESH_SIM_OK, "direct gateway ACK failed ret=%d now=%" PRIu64,
                  ret, fixture.world.now_us);
        } else {
            struct mesh_sim_connection_action action;
            uint16_t selected = UINT16_MAX;
            uint64_t best = UINT64_MAX;
            for (uint16_t i = 0u; i < fixture.world.connection_count; i++) {
                action_ret = mesh_sim_connection_next_action(&fixture.world, i,
                                                              &action);
                CHECK(action_ret == MESH_SIM_OK, "connection action failed");
                if (action.kind != MESH_SIM_CONNECTION_ACTION_NONE &&
                    action.end_us < best) {
                    best = action.end_us;
                    selected = i;
                }
            }
            CHECK(selected != UINT16_MAX, "mesh path had no bounded next action");
            action_ret = mesh_sim_connection_next_action(&fixture.world,
                                                          selected, &action);
            CHECK(action_ret == MESH_SIM_OK, "selected connection action failed");
            if (!action.already_scheduled) {
                action_ret = mesh_sim_schedule_next_connection_event(
                    &fixture.world, selected, false);
                CHECK(action_ret == MESH_SIM_OK,
                      "connection event scheduling failed ret=%d", action_ret);
            }
            CHECK(mesh_sim_run_until(&fixture.world, action.end_us) == MESH_SIM_OK,
                  "connection event run failed");
        }
        ret = mesh_sim_check_invariants(&fixture.world, &invariant);
        CHECK(ret == MESH_SIM_OK, "mesh invariant failed code=%d node=%zu",
              invariant.code, invariant.node_index);
    }
    CHECK(fixture.world.roles[fixture.gateway].delivery_count == 1u,
          "mesh path did not settle exactly once");
    CHECK(fixture.world.connection_count == 2u,
          "final gateway turn incorrectly created a direct connection");
    CHECK(fixture.world.roles[fixture.relay2].relay.pending.state ==
              MESH_RELAY_TX_IDLE,
          "relay2 retained custody after gateway ACK");
    CHECK(fixture.world.roles[fixture.source].runtime.radio_owner ==
              MESH_RUNTIME_RADIO_NONE &&
              !fixture.world.roles[fixture.source].resume_low_duty_after_ds_twr &&
              fixture.world.roles[fixture.source].radio_state ==
                  MESH_SIM_RADIO_SLEEP,
          "source did not release DS-TWR ownership and resume sleep");
    CHECK(fixture.world.roles[fixture.gateway].gateway_semantic_commit_count ==
              1u,
          "gateway did not commit click custody exactly once");
    return test_ble_credit_refusal_and_drain(&report_packet, report_payload,
                                             report_payload_len);
}

int main(void)
{
    bool ok = true;

    ok = test_click_path() && ok;
    ok = test_release_contract_gap() && ok;
    ok = test_partial_and_collision_boundaries() && ok;
    if (failures != 0 || !ok) {
        fprintf(stderr, "mesh click end-to-end: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS mesh click protocol chain wake/range/report/relay/BLE scenarios");
    return 0;
}
