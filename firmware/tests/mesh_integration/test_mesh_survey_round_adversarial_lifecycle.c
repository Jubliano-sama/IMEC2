#include "gateway_command.h"
#include "mesh.h"
#include "mesh_sim.h"
#include "mesh_sim_invariants.h"
#include "survey.h"
#include "survey_pair_lease.h"
#include "survey_round_control.h"
#include "uwb.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SCENARIO_SEED UINT32_C(0x5eed5307)
#define GATEWAY_ID UINT64_C(0xb001000000000001)
#define RELAY_ID UINT64_C(0xb002000000000002)
#define LEAF_ID UINT64_C(0xb003000000000003)
#define ROUTE_EPOCH UINT32_C(53)
#define SURVEY_ID_N UINT32_C(0x53005001)
#define SURVEY_ID_STALE UINT32_C(0x52004fff)
#define OPERATION_GENERATION_N UINT64_C(0x0000000253005001)
#define ROUND_ID_N UINT16_C(7)
#define ROUND_ID_NEXT UINT16_C(8)
#define C5_GUARD_US UINT64_C(500)
#define C5_HOP_DELAY_US UINT64_C(2000)
#define PROBE_DUPLICATE_DELAY_US UINT64_C(30000)
#define MAX_SCENARIO_RF_TRANSMISSIONS 64u
#define MAX_IDENTICAL_CONTROL_TRANSMISSIONS 4u
#define MAX_SCENARIO_DURATION_US UINT64_C(120000000)

struct decoded_control {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

struct peer_observation {
    uint64_t owner_id;
    uint64_t peer_id;
    size_t accepted;
    size_t duplicates;
    size_t malformed;
    size_t stale;
};

struct fixture {
    struct mesh_sim_world world;
    struct survey_pair_lease lease;
    struct survey_gateway_context gateway_survey;
    struct survey_discovery_config accepted_discovery;
    struct peer_observation relay_observation;
    struct peer_observation leaf_observation;
    uint8_t gateway;
    uint8_t relay;
    uint8_t leaf;
    size_t semantic_prepare_count;
    size_t semantic_start_count;
    size_t semantic_abort_count;
};

static struct fixture scenario;
static uint32_t scenario_seed = DEFAULT_SCENARIO_SEED;
static int failures;

#define CHECK(expression, ...) do {                                           \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL seed=0x%08" PRIx32                            \
                " line=%d now=%" PRIu64 " sim_error=%d ",                 \
                scenario_seed, __LINE__, scenario.world.now_us,              \
                scenario.world.last_error);                                  \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                   \
        failures++;                                                           \
        return false;                                                         \
    }                                                                         \
} while (0)

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static bool pair_equal(const struct survey_pair *left,
                       const struct survey_pair *right)
{
    return left->operation_generation == right->operation_generation &&
           left->survey_id == right->survey_id &&
           left->initiator_id == right->initiator_id &&
           left->responder_id == right->responder_id &&
           left->sample_count == right->sample_count;
}

static void round_commitment(uint16_t round_id,
                             uint8_t commitment[
                                 SEMANTIC_DIGEST_SHA256_LEN])
{
    memset(commitment, 0x5au, SEMANTIC_DIGEST_SHA256_LEN);
    proto_put_u16_le(commitment, round_id);
}

/*
 * Channel-5 forwarding remains application-owned.  This boundary sends an
 * encoded production envelope through the complete-airtime PHY model, parses
 * the received bytes with the production parser, and only then invokes the
 * production relay state machine.
 */
static int c5_control_hop(struct fixture *state,
                          uint8_t sender,
                          uint8_t receiver,
                          const struct mesh_outbound *outbound,
                          struct mesh_relay_result *result,
                          struct decoded_control *decoded_control)
{
    struct proto_packet decoded;
    uint8_t frame[PACKET_EXT_MAX_LEN];
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    size_t frame_len = 0u;
    size_t receptions_before;
    uint64_t start_us;
    uint32_t airtime_us;
    uint16_t transmission;
    int ret;

    if (state == NULL || outbound == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = proto_packet_encode(&outbound->packet, outbound->payload,
                              frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    start_us = state->world.now_us + C5_HOP_DELAY_US;
    if (outbound->earliest_tx_ms != 0u &&
        (uint64_t)outbound->earliest_tx_ms * 1000u > start_us) {
        start_us = (uint64_t)outbound->earliest_tx_ms * 1000u;
    }
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame_len);
    if (airtime_us == 0u || start_us < C5_GUARD_US) {
        return MESH_SIM_ERR_PROTOCOL;
    }

    receptions_before = state->world.reception_count;
    ret = mesh_sim_schedule_rx(&state->world, receiver,
                               start_us - C5_GUARD_US,
                               start_us + airtime_us + C5_GUARD_US,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, NULL);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_schedule_raw_tx(
            &state->world, sender, start_us, UWB_CHANNEL_WAKE_CONTACT,
            MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, frame, frame_len, false,
            &transmission);
    }
    if (ret == MESH_SIM_OK) {
        state->world.transmissions[transmission].protocol_msg_type =
            outbound->packet.msg_type;
        ret = mesh_sim_run_until(&state->world,
                                 start_us + airtime_us + C5_GUARD_US);
    }
    if (ret != MESH_SIM_OK ||
        state->world.reception_count != receptions_before + 1u ||
        state->world.receptions[receptions_before].outcome !=
            MESH_SIM_RX_DECODED) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    ret = proto_packet_decode(state->world.transmissions[transmission].frame,
                              state->world.transmissions[transmission].frame_len,
                              &decoded, &decoded_payload,
                              &decoded_payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (decoded_control != NULL) {
        decoded_control->packet = decoded;
        decoded_control->payload_len = decoded_payload_len;
        if (decoded_payload_len > 0u) {
            memcpy(decoded_control->payload, decoded_payload,
                   decoded_payload_len);
        }
    }
    return mesh_relay_handle_rx_with_random(
        &state->world.roles[receiver].relay, &decoded, decoded_payload,
        decoded_payload_len, state->world.roles[sender].id, 96u,
        (uint32_t)(state->world.now_us / 1000u),
        scenario_seed ^ (uint32_t)state->world.transmission_count,
        result);
}

static int send_targeted_to_leaf(struct fixture *state,
                                 const struct mesh_outbound *outbound,
                                 struct decoded_control *leaf_control,
                                 bool *delivered)
{
    struct mesh_relay_result relay_result;
    struct mesh_relay_result leaf_result;
    int ret;

    if (delivered == NULL) {
        return PROTO_ERR_ARG;
    }
    *delivered = false;
    ret = c5_control_hop(state, state->gateway, state->relay, outbound,
                         &relay_result, NULL);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!has_action(&relay_result, MESH_RELAY_ACTION_FORWARD)) {
        return PROTO_OK;
    }
    relay_result.forward.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = c5_control_hop(state, state->relay, state->leaf,
                         &relay_result.forward, &leaf_result, leaf_control);
    if (ret == PROTO_OK) {
        *delivered = has_action(&leaf_result,
                                MESH_RELAY_ACTION_DELIVER_LOCAL);
    }
    return ret;
}

static int commit_targeted_leaf_command(
    struct fixture *state,
    const struct decoded_control *control)
{
    if (state == NULL || control == NULL) {
        return PROTO_ERR_ARG;
    }
    return mesh_relay_commit_anchor_command_delivery(
        &state->world.roles[state->leaf].relay,
        &control->packet,
        control->payload,
        control->payload_len,
        (uint32_t)(state->world.now_us / 1000u));
}

static int send_broadcast_to_leaf(struct fixture *state,
                                  const struct mesh_outbound *outbound,
                                  struct decoded_control *leaf_control,
                                  bool *delivered)
{
    struct mesh_relay_result relay_result;
    struct mesh_relay_result leaf_result;
    int ret;

    if (delivered == NULL) {
        return PROTO_ERR_ARG;
    }
    *delivered = false;
    ret = c5_control_hop(state, state->gateway, state->relay, outbound,
                         &relay_result, NULL);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!has_action(&relay_result, MESH_RELAY_ACTION_FORWARD)) {
        return PROTO_OK;
    }
    relay_result.forward.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = c5_control_hop(state, state->relay, state->leaf,
                         &relay_result.forward, &leaf_result, leaf_control);
    if (ret == PROTO_OK) {
        *delivered = has_action(&leaf_result,
                                MESH_RELAY_ACTION_DELIVER_LOCAL);
    }
    return ret;
}

static int install_gateway_route_advertisement(struct fixture *state,
                                               uint16_t seq)
{
    struct mesh_outbound gateway_adv;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result leaf_result;
    const struct route_candidate *selected;
    int ret;

    ret = mesh_relay_build_gateway_route_adv(
        &state->world.roles[state->gateway].relay, seq,
        (uint32_t)(state->world.now_us / 1000u), &gateway_adv);
    if (ret == PROTO_OK) {
        gateway_adv.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
        ret = c5_control_hop(state, state->gateway, state->relay,
                             &gateway_adv, &relay_result, NULL);
    }
    if (ret != PROTO_OK ||
        !has_action(&relay_result,
                    MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV)) {
        return ret == PROTO_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    relay_result.gateway_route_adv.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = c5_control_hop(state, state->relay, state->leaf,
                         &relay_result.gateway_route_adv, &leaf_result, NULL);
    if (ret != PROTO_OK ||
        !has_action(&leaf_result,
                    MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV)) {
        return ret == PROTO_OK ? MESH_SIM_ERR_PROTOCOL : ret;
    }
    selected = route_selected(&state->world.roles[state->relay].relay.upstream);
    if (selected == NULL || selected->next_hop_id != GATEWAY_ID) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    selected = route_selected(&state->world.roles[state->leaf].relay.upstream);
    if (selected == NULL || selected->next_hop_id != RELAY_ID) {
        return MESH_SIM_ERR_PROTOCOL;
    }
    ret = mesh_sim_install_downlink(&state->world, state->gateway, LEAF_ID,
                                    state->relay, 2u, ROUTE_EPOCH);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_install_downlink(&state->world, state->relay, LEAF_ID,
                                        state->leaf, 1u, ROUTE_EPOCH);
    }
    return ret;
}

static int setup_fixture(struct fixture *state)
{
    int ret;

    memset(state, 0, sizeof(*state));
    mesh_sim_init(&state->world, scenario_seed);
    ret = mesh_sim_add_role(&state->world, MESH_SIM_ROLE_GATEWAY,
                            GATEWAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                            &state->gateway);
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&state->world, MESH_SIM_ROLE_ANCHOR,
                                RELAY_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &state->relay);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_add_role(&state->world, MESH_SIM_ROLE_ANCHOR,
                                LEAF_ID, GATEWAY_ID, ROUTE_EPOCH,
                                &state->leaf);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&state->world, state->gateway, state->relay,
                                96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = mesh_sim_set_link(&state->world, state->relay, state->leaf,
                                96u, 1u);
    }
    if (ret == MESH_SIM_OK) {
        ret = install_gateway_route_advertisement(state, 101u);
    }
    state->relay_observation.owner_id = RELAY_ID;
    state->relay_observation.peer_id = LEAF_ID;
    state->leaf_observation.owner_id = LEAF_ID;
    state->leaf_observation.peer_id = RELAY_ID;
    return ret;
}

static int build_discovery_start(uint32_t survey_id,
                                 uint16_t seq,
                                 struct mesh_outbound *outbound)
{
    const struct survey_discovery_config config = {
        .operation_generation = OPERATION_GENERATION_N,
        .survey_id = survey_id,
        .start_delay_ms = 100u,
        .slot_ms = 80u,
        .slot_count = 8u,
        .round_count = SURVEY_DISCOVERY_MAX_ROUND_COUNT,
    };
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = survey_append_discovery_start_tlvs(
        outbound->payload, sizeof(outbound->payload), &payload_len, &config);
    if (ret == PROTO_OK) {
        ret = survey_init_discovery_start_packet(
            &outbound->packet, GATEWAY_ID, &config, seq,
            (uint8_t)payload_len);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static int build_pair_control(const struct survey_pair *pair,
                              uint16_t round_id,
                              enum command_id command_id,
                              uint16_t seq,
                              struct mesh_outbound *outbound)
{
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];
    size_t payload_len = 0u;
    int ret;

    if (pair == NULL || outbound == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(outbound, 0, sizeof(*outbound));
    if (command_id != CMD_SURVEY_PREPARE_PAIR) {
        ret = mesh_append_command_id(outbound->payload,
                                     sizeof(outbound->payload),
                                     &payload_len, command_id);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = survey_append_pair_tlvs(outbound->payload,
                                  sizeof(outbound->payload),
                                  &payload_len, pair);
    if (ret == PROTO_OK) {
        ret = survey_round_id_append_tlv(outbound->payload,
                                         sizeof(outbound->payload),
                                         &payload_len, round_id);
    }
    if (ret == PROTO_OK) {
        round_commitment(round_id, commitment);
        ret = survey_round_commitment_append_tlv(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            commitment);
    }
    if (ret == PROTO_OK && command_id == CMD_SURVEY_START_PAIR) {
        ret = tlv_append_u32(outbound->payload, sizeof(outbound->payload),
                             &payload_len, TLV_EXECUTE_DELAY_MS,
                             SURVEY_ROUND_START_EXECUTE_DELAY_MS);
    }
    if (ret == PROTO_OK && command_id == CMD_SURVEY_PREPARE_PAIR) {
        ret = survey_init_pair_prepare_packet(&outbound->packet, pair,
                                              GATEWAY_ID, LEAF_ID, seq,
                                              (uint8_t)payload_len);
    } else if (ret == PROTO_OK) {
        ret = mesh_init_command(&outbound->packet, GATEWAY_ID, LEAF_ID,
                                survey_operation_session_id(
                                    pair->operation_generation),
                                seq,
                                (uint8_t)payload_len);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = RELAY_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    return PROTO_OK;
}

static int parse_pair_control(const struct decoded_control *control,
                              enum command_id expected,
                              struct survey_pair *pair,
                              uint16_t *round_id)
{
    enum command_id command_id;
    int ret;

    if (control == NULL || pair == NULL || round_id == NULL) {
        return PROTO_ERR_ARG;
    }
    if (expected == CMD_SURVEY_PREPARE_PAIR) {
        if (control->packet.msg_type != MSG_SURVEY_PAIR_PREPARE) {
            return PROTO_ERR_MALFORMED;
        }
    } else {
        if (control->packet.msg_type != MSG_COMMAND) {
            return PROTO_ERR_MALFORMED;
        }
        ret = gateway_command_extract_id(control->payload,
                                         control->payload_len,
                                         &command_id);
        if (ret != PROTO_OK || command_id != expected) {
            return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
        }
    }
    ret = survey_extract_pair_tlvs(control->payload, control->payload_len,
                                   pair);
    if (ret == PROTO_OK) {
        ret = survey_round_id_extract_tlv(control->payload,
                                          control->payload_len, round_id);
    }
    if (ret != PROTO_OK ||
        control->packet.session_id !=
            survey_operation_session_id(pair->operation_generation) ||
        (pair->initiator_id != LEAF_ID && pair->responder_id != LEAF_ID)) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    return PROTO_OK;
}

static int encode_probe(uint64_t anchor_id,
                        const struct survey_discovery_config *config,
                        uint8_t round,
                        uint32_t survey_id,
                        uint64_t operation_generation,
                        bool corrupt,
                        uint8_t *frame,
                        size_t frame_cap,
                        size_t *frame_len)
{
    struct uwb_survey_discovery_probe_frame probe = {
        .network_id = UINT32_C(0x494d4543),
        .survey_id = survey_id,
        .operation_generation = operation_generation,
        .anchor_id = anchor_id,
        .slot_count = config->slot_count,
        .flags = FLAG_DIAGNOSTIC,
    };
    int ret;

    probe.anchor_slot = survey_discovery_opportunity_slot(
        anchor_id, config->survey_id, round, config->slot_count);
    ret = uwb_encode_survey_discovery_probe(&probe, frame, frame_cap,
                                            frame_len);
    if (ret == PROTO_OK && corrupt && *frame_len > 0u) {
        frame[*frame_len - 1u] ^= UINT8_C(0x80);
    }
    return ret;
}

static int schedule_probe(struct fixture *state,
                          uint8_t sender,
                          uint8_t receiver,
                          uint64_t start_us,
                          const uint8_t *frame,
                          size_t frame_len,
                          bool receive)
{
    uint64_t arrival_start_us;
    uint64_t arrival_end_us;
    uint16_t transmission;
    int ret = mesh_sim_schedule_raw_tx(
        &state->world, sender, start_us, UWB_CHANNEL_WAKE_CONTACT,
        MESH_SIM_PHY_CHANNEL5_WAKE, frame, frame_len, false, &transmission);

    if (ret != MESH_SIM_OK) {
        return ret;
    }
    state->world.transmissions[transmission].protocol_msg_type =
        MSG_UWB_SURVEY_DISCOVERY_PROBE;
    if (!receive) {
        return MESH_SIM_OK;
    }
    arrival_start_us = start_us + state->world.propagation_us[sender][receiver];
    arrival_end_us = state->world.transmissions[transmission].end_us +
                     state->world.propagation_us[sender][receiver];
    return mesh_sim_schedule_rx(&state->world, receiver,
                                arrival_start_us, arrival_end_us,
                                UWB_CHANNEL_WAKE_CONTACT,
                                MESH_SIM_PHY_CHANNEL5_WAKE, NULL);
}

static void note_probe_receptions(struct fixture *state,
                                  size_t reception_begin,
                                  uint8_t round,
                                  const uint8_t *relay_frame,
                                  size_t relay_frame_len,
                                  const uint8_t *leaf_frame,
                                  size_t leaf_frame_len,
                                  const uint8_t *stale_frame,
                                  size_t stale_frame_len)
{
    bool round_relay_seen = false;

    for (size_t i = reception_begin; i < state->world.reception_count; i++) {
        const struct mesh_sim_reception *rx = &state->world.receptions[i];
        const uint8_t *frame;
        size_t frame_len;
        struct uwb_survey_discovery_probe_frame decoded;
        struct peer_observation *observation;
        int ret;

        if (rx->source_id == RELAY_ID) {
            frame = relay_frame;
            frame_len = relay_frame_len;
            observation = &state->leaf_observation;
            if (round == 3u && round_relay_seen && stale_frame != NULL) {
                frame = stale_frame;
                frame_len = stale_frame_len;
            }
            round_relay_seen = true;
        } else {
            frame = leaf_frame;
            frame_len = leaf_frame_len;
            observation = &state->relay_observation;
        }
        if (rx->outcome != MESH_SIM_RX_DECODED) {
            observation->malformed++;
            continue;
        }
        ret = uwb_decode_survey_discovery_probe(frame, frame_len, &decoded);
        if (ret != PROTO_OK) {
            observation->malformed++;
            continue;
        }
        if (decoded.survey_id != state->accepted_discovery.survey_id ||
            decoded.operation_generation !=
                state->accepted_discovery.operation_generation) {
            observation->stale++;
            continue;
        }
        if (decoded.anchor_id != observation->peer_id ||
            decoded.anchor_slot != survey_discovery_opportunity_slot(
                decoded.anchor_id, decoded.survey_id, round,
                state->accepted_discovery.slot_count)) {
            observation->malformed++;
            continue;
        }
        if (observation->accepted == 0u) {
            observation->accepted++;
        } else {
            observation->duplicates++;
        }
    }
}

static bool run_discovery_announce_matrix(struct fixture *state)
{
    struct mesh_outbound start;
    struct mesh_outbound malformed;
    struct decoded_control leaf_control;
    struct survey_discovery_config decoded_config;
    uint64_t discovery_base_us;
    bool delivered;

    CHECK(build_discovery_start(SURVEY_ID_N, 110u, &start) == PROTO_OK &&
          send_broadcast_to_leaf(state, &start, &leaf_control, &delivered) ==
              PROTO_OK && delivered,
          "survey discovery START did not cross the real two-hop flood");
    CHECK(survey_extract_discovery_start_tlvs(
              leaf_control.payload, leaf_control.payload_len,
              &decoded_config) == PROTO_OK &&
          decoded_config.survey_id == SURVEY_ID_N &&
          decoded_config.round_count == SURVEY_DISCOVERY_MAX_ROUND_COUNT,
          "leaf did not parse the production discovery schedule");
    state->accepted_discovery = decoded_config;

    CHECK(send_broadcast_to_leaf(state, &start, NULL, &delivered) == PROTO_OK &&
              !delivered,
          "exact duplicate START reached semantic delivery twice");

    malformed = start;
    malformed.packet.seq++;
    malformed.payload_len--;
    malformed.packet.payload_len = malformed.payload_len;
    CHECK(send_broadcast_to_leaf(state, &malformed, &leaf_control,
                                 &delivered) == PROTO_OK && !delivered &&
          state->accepted_discovery.survey_id == SURVEY_ID_N,
          "malformed START was delivered or changed the accepted discovery generation");

    discovery_base_us = state->world.now_us +
        (uint64_t)state->accepted_discovery.start_delay_ms * 1000u;
    for (uint8_t round = 0u;
         round < state->accepted_discovery.round_count; round++) {
        struct survey_discovery_attempt_schedule relay_schedule;
        struct survey_discovery_attempt_schedule leaf_schedule;
        uint8_t relay_frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
        uint8_t leaf_frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
        uint8_t stale_frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
        size_t relay_frame_len = 0u;
        size_t leaf_frame_len = 0u;
        size_t stale_frame_len = 0u;
        size_t reception_begin = state->world.reception_count;
        uint64_t relay_tx_us;
        uint64_t leaf_tx_us;
        uint64_t round_end_us;
        bool relay_receive = true;
        bool leaf_receive = true;

        CHECK(survey_discovery_schedule_attempt(
                  &state->accepted_discovery, RELAY_ID, round, 0u,
                  &relay_schedule) == PROTO_OK &&
              survey_discovery_schedule_attempt(
                  &state->accepted_discovery, LEAF_ID, round, 0u,
                  &leaf_schedule) == PROTO_OK,
              "round %u production probe schedule failed", round);
        CHECK(relay_schedule.tx_ms != leaf_schedule.tx_ms,
              "round %u fixture aliases both production slots", round);
        CHECK(encode_probe(RELAY_ID, &state->accepted_discovery, round,
                           SURVEY_ID_N, OPERATION_GENERATION_N,
                           round == 2u, relay_frame,
                           sizeof(relay_frame), &relay_frame_len) == PROTO_OK &&
              encode_probe(LEAF_ID, &state->accepted_discovery, round,
                           SURVEY_ID_N, OPERATION_GENERATION_N,
                           false, leaf_frame,
                           sizeof(leaf_frame), &leaf_frame_len) == PROTO_OK,
              "round %u probe encoding failed", round);
        relay_tx_us = discovery_base_us +
                      (uint64_t)relay_schedule.tx_ms * 1000u;
        leaf_tx_us = discovery_base_us +
                     (uint64_t)leaf_schedule.tx_ms * 1000u;

        /* Round zero loses the leaf-to-relay announce. */
        leaf_receive = round != 0u;
        if (round == 3u) {
            /* Insert in reverse time order; the scheduler must replay by time. */
            CHECK(schedule_probe(state, state->leaf, state->relay,
                                 leaf_tx_us, leaf_frame, leaf_frame_len,
                                 leaf_receive) == MESH_SIM_OK &&
                  schedule_probe(state, state->relay, state->leaf,
                                 relay_tx_us, relay_frame, relay_frame_len,
                                 relay_receive) == MESH_SIM_OK,
                  "reordered round-three scheduling failed");
        } else {
            CHECK(schedule_probe(state, state->relay, state->leaf,
                                 relay_tx_us, relay_frame, relay_frame_len,
                                 relay_receive) == MESH_SIM_OK &&
                  schedule_probe(state, state->leaf, state->relay,
                                 leaf_tx_us, leaf_frame, leaf_frame_len,
                                 leaf_receive) == MESH_SIM_OK,
                  "round %u probe scheduling failed", round);
        }
        if (round == 1u) {
            CHECK(schedule_probe(
                      state, state->leaf, state->relay,
                      leaf_tx_us + PROBE_DUPLICATE_DELAY_US,
                      leaf_frame, leaf_frame_len, true) == MESH_SIM_OK,
                  "duplicate discovery probe scheduling failed");
        }
        if (round == 3u) {
            struct survey_discovery_attempt_schedule delayed;

            CHECK(survey_discovery_schedule_attempt(
                      &state->accepted_discovery, RELAY_ID, round,
                      relay_schedule.latest_tx_start_ms + 1u, &delayed) ==
                      PROTO_ERR_BUSY,
                  "expired round-three slot was treated as a new opportunity");
            CHECK(encode_probe(RELAY_ID, &state->accepted_discovery, round,
                               SURVEY_ID_N, OPERATION_GENERATION_N - 1u,
                               false, stale_frame,
                               sizeof(stale_frame), &stale_frame_len) ==
                      PROTO_OK &&
                  schedule_probe(
                      state, state->relay, state->leaf,
                      relay_tx_us + PROBE_DUPLICATE_DELAY_US,
                      stale_frame, stale_frame_len, true) == MESH_SIM_OK,
                  "stale delayed probe scheduling failed");
        }
        round_end_us = discovery_base_us +
            (uint64_t)relay_schedule.window_end_ms * 1000u;
        CHECK(mesh_sim_run_until(&state->world, round_end_us) == MESH_SIM_OK,
              "round %u radio replay failed", round);
        note_probe_receptions(state, reception_begin, round,
                              relay_frame, relay_frame_len,
                              leaf_frame, leaf_frame_len,
                              round == 3u ? stale_frame : NULL,
                              stale_frame_len);
    }

    CHECK(state->relay_observation.accepted == 1u &&
              state->relay_observation.duplicates == 3u &&
              state->relay_observation.malformed == 0u &&
              state->relay_observation.stale == 0u,
          "relay announce ledger disagrees accepted=%zu dup=%zu malformed=%zu stale=%zu",
          state->relay_observation.accepted,
          state->relay_observation.duplicates,
          state->relay_observation.malformed,
          state->relay_observation.stale);
    CHECK(state->leaf_observation.accepted == 1u &&
              state->leaf_observation.duplicates == 2u &&
              state->leaf_observation.malformed == 1u &&
              state->leaf_observation.stale == 1u,
          "leaf announce ledger disagrees accepted=%zu dup=%zu malformed=%zu stale=%zu",
          state->leaf_observation.accepted,
          state->leaf_observation.duplicates,
          state->leaf_observation.malformed,
          state->leaf_observation.stale);
    return true;
}

static int note_report_roundtrip(struct survey_gateway_context *context,
                                 uint32_t survey_id,
                                 uint64_t operation_generation,
                                 uint64_t anchor_id,
                                 uint64_t peer_id,
                                 uint16_t seq)
{
    const struct survey_reachability_entry entry = {
        .peer_id = peer_id,
        .rssi_dbm = -61,
        .quality = 91u,
    };
    struct proto_packet packet;
    struct proto_packet decoded_packet;
    struct survey_reachability_entry decoded_entries[2];
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t frame[PACKET_EXT_MAX_LEN];
    const uint8_t *decoded_payload = NULL;
    size_t payload_len = 0u;
    size_t frame_len = 0u;
    size_t decoded_payload_len = 0u;
    size_t decoded_count = 0u;
    uint32_t decoded_survey_id = 0u;
    uint64_t decoded_operation_generation = 0u;
    uint64_t decoded_anchor_id = 0u;
    const uint8_t *boot_raw = NULL;
    const uint8_t *status_raw = NULL;
    uint8_t boot_len = 0u;
    uint8_t status_len = 0u;
    int ret;

    ret = survey_append_reach_report_tlvs(
        payload, sizeof(payload), &payload_len, survey_id, anchor_id,
        &entry, 1u);
    if (ret == PROTO_OK) {
        ret = survey_operation_generation_append_tlv(
            payload, sizeof(payload), &payload_len, operation_generation);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(payload,
                             sizeof(payload),
                             &payload_len,
                             TLV_NODE_BOOT_COUNTER,
                             1u);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u16(payload, sizeof(payload), &payload_len,
                             TLV_COMMAND_STATUS, COMMAND_OK);
    }
    if (ret == PROTO_OK) {
        ret = survey_init_discovery_report_packet(
            &packet, anchor_id, GATEWAY_ID, survey_id,
            operation_generation, 1u, seq,
            (uint8_t)payload_len);
    }
    if (ret == PROTO_OK) {
        ret = proto_packet_encode(&packet, payload, frame, sizeof(frame),
                                  &frame_len);
    }
    if (ret == PROTO_OK) {
        ret = proto_packet_decode(frame, frame_len, &decoded_packet,
                                  &decoded_payload, &decoded_payload_len);
    }
    if (ret == PROTO_OK) {
        ret = survey_extract_reach_report_tlvs(
            decoded_payload, decoded_payload_len, &decoded_survey_id,
            &decoded_anchor_id, decoded_entries, 2u, &decoded_count);
    }
    if (ret == PROTO_OK) {
        ret = survey_operation_generation_extract_tlv(
            decoded_payload,
            decoded_payload_len,
            &decoded_operation_generation);
    }
    if (ret == PROTO_OK) {
        ret = tlv_find_unique(decoded_payload,
                              decoded_payload_len,
                              TLV_COMMAND_STATUS,
                              &status_raw,
                              &status_len);
    }
    if (ret == PROTO_OK) {
        ret = tlv_find_unique(decoded_payload,
                              decoded_payload_len,
                              TLV_NODE_BOOT_COUNTER,
                              &boot_raw,
                              &boot_len);
    }
    if (ret != PROTO_OK || decoded_packet.msg_type !=
            MSG_SURVEY_DISCOVERY_REPORT ||
        decoded_packet.src_id != decoded_anchor_id ||
        boot_len != sizeof(uint32_t) ||
        decoded_packet.session_id != proto_get_u32_le(boot_raw) ||
        status_len != sizeof(uint16_t) ||
        proto_get_u16_le(status_raw) != COMMAND_OK ||
        decoded_count != 1u) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    if (decoded_operation_generation != context->operation_generation) {
        return PROTO_ERR_STALE;
    }
    return survey_gateway_note_reach_report(
        context, decoded_survey_id, decoded_anchor_id,
        decoded_entries, decoded_count);
}

static bool run_report_idempotence_and_pair_plan(struct fixture *state,
                                                 struct survey_pair *pair)
{
    struct survey_gateway_context accepted;
    struct survey_reachability_entry invalid = {
        .peer_id = RELAY_ID,
        .rssi_dbm = -60,
        .quality = 90u,
    };

    CHECK(survey_gateway_begin_operation(&state->gateway_survey,
                                         SURVEY_ID_N,
                                         OPERATION_GENERATION_N,
                                         1u) == PROTO_OK,
          "gateway survey context did not start");
    CHECK(note_report_roundtrip(&state->gateway_survey,
                                SURVEY_ID_N,
                                OPERATION_GENERATION_N - 1u,
                                RELAY_ID,
                                LEAF_ID,
                                200u) == PROTO_ERR_STALE &&
              state->gateway_survey.report_count == 0u,
          "stale report generation changed the current survey");
    CHECK(note_report_roundtrip(&state->gateway_survey,
                                SURVEY_ID_STALE,
                                OPERATION_GENERATION_N,
                                RELAY_ID,
                                LEAF_ID,
                                201u) == PROTO_ERR_STALE &&
              state->gateway_survey.report_count == 0u,
          "stale report changed the current survey");
    CHECK(note_report_roundtrip(&state->gateway_survey,
                                SURVEY_ID_N,
                                OPERATION_GENERATION_N,
                                RELAY_ID,
                                LEAF_ID,
                                202u) == PROTO_OK &&
              note_report_roundtrip(&state->gateway_survey,
                                    SURVEY_ID_N,
                                    OPERATION_GENERATION_N,
                                    LEAF_ID,
                                    RELAY_ID,
                                    203u) == PROTO_OK &&
              state->gateway_survey.report_count == 2u,
          "valid directed observations were not committed");

    accepted = state->gateway_survey;
    CHECK(note_report_roundtrip(&state->gateway_survey,
                                SURVEY_ID_N,
                                OPERATION_GENERATION_N,
                                RELAY_ID,
                                LEAF_ID,
                                204u) == PROTO_OK &&
              memcmp(&state->gateway_survey, &accepted,
                     sizeof(state->gateway_survey)) == 0,
          "exact duplicate discovery report changed gateway state");
    CHECK(note_report_roundtrip(&state->gateway_survey,
                                SURVEY_ID_N,
                                OPERATION_GENERATION_N,
                                RELAY_ID,
                                GATEWAY_ID,
                                205u) ==
              PROTO_ERR_MALFORMED &&
              memcmp(&state->gateway_survey, &accepted,
                     sizeof(state->gateway_survey)) == 0,
          "conflicting duplicate discovery report changed gateway state");
    CHECK(survey_gateway_note_reach_report(
              &state->gateway_survey, SURVEY_ID_N, RELAY_ID,
              &invalid, 1u) == PROTO_ERR_MALFORMED &&
              state->gateway_survey.report_count == 2u,
          "self-referential report mutated gateway state");
    CHECK(survey_gateway_plan_pairs(&state->gateway_survey) == PROTO_OK &&
              state->gateway_survey.pair_count == 1u &&
              survey_gateway_pair_at(&state->gateway_survey, 0u, pair) ==
                  PROTO_OK &&
              pair->operation_generation == OPERATION_GENERATION_N &&
              ((pair->initiator_id == RELAY_ID &&
                pair->responder_id == LEAF_ID) ||
               (pair->initiator_id == LEAF_ID &&
                pair->responder_id == RELAY_ID)),
          "adversarial announce set did not produce one bounded pair");
    return true;
}

/*
 * Zephyr owns the final application ingress dispatch.  These adapters model
 * that narrow boundary after real frame decoding, then call the production
 * lease owner directly so stale-operation and exact-once semantics remain the
 * code under test.
 */
static enum survey_pair_lease_decision apply_prepare(
    struct fixture *state,
    const struct decoded_control *control,
    uint32_t lease_ms)
{
    struct survey_pair pair;
    struct survey_pair_control_id id;
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t round_id;

    if (parse_pair_control(control, CMD_SURVEY_PREPARE_PAIR,
                           &pair, &round_id) != PROTO_OK) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    if (survey_round_commitment_extract_tlv(
            control->payload,
            control->payload_len,
            commitment) != PROTO_OK) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    id = (struct survey_pair_control_id) {
        .session_id = control->packet.session_id,
        .command_seq = control->packet.seq,
    };
    return survey_pair_lease_prepare_round_bound(
        &state->lease, &pair, round_id, commitment, &id,
        (uint32_t)(state->world.now_us / 1000u), lease_ms);
}

static enum survey_pair_lease_decision apply_start(
    struct fixture *state,
    const struct decoded_control *control)
{
    struct survey_pair pair;
    struct survey_pair_control_id id;
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];
    const uint8_t *execute_delay_value = NULL;
    uint8_t execute_delay_len = 0u;
    uint32_t execute_delay_ms;
    uint32_t remaining_delay_ms;
    uint32_t now_ms;
    uint16_t round_id;

    if (parse_pair_control(control, CMD_SURVEY_START_PAIR,
                           &pair, &round_id) != PROTO_OK) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    if (survey_round_commitment_extract_tlv(
            control->payload,
            control->payload_len,
            commitment) != PROTO_OK ||
        tlv_find_unique(control->payload, control->payload_len,
                        TLV_EXECUTE_DELAY_MS, &execute_delay_value,
                        &execute_delay_len) != PROTO_OK ||
        execute_delay_len != sizeof(execute_delay_ms)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    execute_delay_ms = proto_get_u32_le(execute_delay_value);
    if (execute_delay_ms != SURVEY_ROUND_START_EXECUTE_DELAY_MS ||
        control->packet.message_age_ms >= execute_delay_ms) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    remaining_delay_ms = execute_delay_ms - control->packet.message_age_ms;
    now_ms = (uint32_t)(state->world.now_us / 1000u);
    id = (struct survey_pair_control_id) {
        .session_id = control->packet.session_id,
        .command_seq = control->packet.seq,
    };
    return survey_pair_lease_start_round_bound_at(
        &state->lease, &pair, round_id, commitment, &id,
        now_ms, now_ms + remaining_delay_ms);
}

static bool apply_abort(struct fixture *state,
                        const struct decoded_control *control)
{
    struct survey_pair pair;
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint16_t round_id;

    if (parse_pair_control(control, CMD_SURVEY_ABORT,
                           &pair, &round_id) != PROTO_OK) {
        return false;
    }
    if (survey_round_commitment_extract_tlv(
            control->payload,
            control->payload_len,
            commitment) != PROTO_OK) {
        return false;
    }
    return survey_pair_lease_abort_matching_round_bound(
        &state->lease,
        &pair,
        control->packet.session_id,
        round_id,
        commitment);
}

static bool run_pair_control_and_reset_matrix(struct fixture *state,
                                              const struct survey_pair *planned)
{
    struct survey_pair pair = {
        .operation_generation = OPERATION_GENERATION_N,
        .initiator_id = LEAF_ID,
        .responder_id = RELAY_ID,
        .survey_id = SURVEY_ID_N,
        .sample_count = planned->sample_count,
    };
    struct survey_pair next_pair = pair;
    struct mesh_outbound prepare;
    struct mesh_outbound malformed;
    struct mesh_outbound start;
    struct mesh_outbound abort;
    struct decoded_control decoded;
    struct survey_pair running_pair;
    struct survey_pair_control_id start_id;
    uint32_t original_deadline;
    uint32_t start_deadline;
    bool delivered;

    next_pair.operation_generation = OPERATION_GENERATION_N + 1u;
    survey_pair_lease_reset(&state->lease);
    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_PREPARE_PAIR,
                             210u, &prepare) == PROTO_OK,
          "pair PREPARE build failed");
    malformed = prepare;
    malformed.packet.seq++;
    malformed.payload_len--;
    malformed.packet.payload_len = malformed.payload_len;
    CHECK(send_targeted_to_leaf(state, &malformed, &decoded, &delivered) ==
              PROTO_OK && !delivered &&
          state->lease.phase == SURVEY_PAIR_LEASE_IDLE,
          "malformed PREPARE was delivered or acquired pair ownership");

    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_START_PAIR,
                             212u, &start) == PROTO_OK &&
          send_targeted_to_leaf(state, &start, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_start(state, &decoded) == SURVEY_PAIR_LEASE_INVALID_STATE &&
          state->lease.phase == SURVEY_PAIR_LEASE_IDLE,
          "reordered START acquired ownership before PREPARE");
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "reordered START result admission did not commit transport replay");

    CHECK(send_targeted_to_leaf(state, &prepare, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_prepare(state, &decoded, SURVEY_PAIR_PREPARED_LEASE_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "valid PREPARE did not acquire the leaf lease");
    state->semantic_prepare_count++;
    original_deadline = state->lease.prepared_deadline_ms;
    CHECK(send_targeted_to_leaf(state, &prepare, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_prepare(state, &decoded, SURVEY_PAIR_PREPARED_LEASE_MS) ==
              SURVEY_PAIR_LEASE_DUPLICATE &&
          state->lease.prepared_deadline_ms == original_deadline &&
          state->semantic_prepare_count == 1u,
          "exact duplicate PREPARE refreshed or re-applied state");
    /* PREPARE has a dedicated relay/app retry owner.  The relay deliberately
     * redelivers exact PREPARE retries so a transient result-admission failure
     * can recover; command-result transport commit does not apply to this
     * packet type.  Semantic admission above remains idempotent. */

    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_START_PAIR,
                             214u, &start) == PROTO_OK,
          "post-PREPARE START rebuild failed");
    CHECK(send_targeted_to_leaf(state, &start, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_start(state, &decoded) == SURVEY_PAIR_LEASE_ACCEPTED,
          "valid START did not enter START_PENDING");
    state->semantic_start_count++;
    start_id = (struct survey_pair_control_id) {
        .session_id = decoded.packet.session_id,
        .command_seq = decoded.packet.seq,
    };
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "accepted START result admission did not commit transport replay");

    start_deadline = state->lease.start_execution_deadline_ms;
    CHECK(!survey_pair_lease_ready_snapshot(&state->lease, NULL) &&
          survey_pair_lease_release_start(&state->lease, &start_id) &&
          survey_pair_lease_ready_snapshot(&state->lease, &running_pair) &&
          pair_equal(&running_pair, &pair) &&
          !survey_pair_lease_mark_running_at(&state->lease,
                                             start_deadline - 1u,
                                             NULL, NULL) &&
          survey_pair_lease_mark_running_at(&state->lease, start_deadline,
                                            &running_pair, NULL) &&
          !survey_pair_lease_mark_running_at(&state->lease, start_deadline,
                                             NULL, NULL),
          "START custody/deadline barrier did not release exactly once");

    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_ABORT,
                             216u, &abort) == PROTO_OK &&
          send_targeted_to_leaf(state, &abort, &decoded, &delivered) ==
              PROTO_OK && delivered && apply_abort(state, &decoded) &&
          state->lease.phase == SURVEY_PAIR_LEASE_ABORTING,
          "matching ABORT did not own running cleanup");
    state->semantic_abort_count++;
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "accepted ABORT result admission did not commit transport replay");
    CHECK(send_targeted_to_leaf(state, &abort, NULL, &delivered) ==
              PROTO_OK && !delivered &&
          state->semantic_abort_count == 1u,
          "exact duplicate ABORT reached cleanup twice");
    abort.packet.seq++;
    CHECK(send_targeted_to_leaf(state, &abort, &decoded, &delivered) ==
              PROTO_OK && delivered && !apply_abort(state, &decoded) &&
          survey_pair_lease_finish(&state->lease) &&
          !survey_pair_lease_finish(&state->lease) &&
          state->lease.phase == SURVEY_PAIR_LEASE_IDLE,
          "ABORT cleanup did not terminalize exactly once");
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "duplicate-state ABORT result admission did not commit transport replay");

    /* Reset while N owns START but before its result releases execution. */
    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_PREPARE_PAIR,
                             220u, &prepare) == PROTO_OK &&
          send_targeted_to_leaf(state, &prepare, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_prepare(state, &decoded, SURVEY_PAIR_PREPARED_LEASE_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "reset-boundary operation N PREPARE failed");
    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_START_PAIR,
                             221u, &start) == PROTO_OK &&
          send_targeted_to_leaf(state, &start, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_start(state, &decoded) == SURVEY_PAIR_LEASE_ACCEPTED,
          "reset-boundary operation N START failed");
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "reset-boundary START result admission did not commit transport replay");
    CHECK(mesh_sim_reset_role(&state->world, state->leaf) == MESH_SIM_OK,
          "leaf reset injection failed");
    survey_pair_lease_reset(&state->lease);
    CHECK(state->lease.phase == SURVEY_PAIR_LEASE_IDLE &&
              state->world.roles[state->leaf].tx_queue_count == 0u &&
              state->world.roles[state->leaf].relay.pending.state ==
                  MESH_RELAY_TX_IDLE,
          "reset left protocol or communication ownership live");
    CHECK(install_gateway_route_advertisement(state, 301u) == MESH_SIM_OK,
          "gateway route advertisement did not repair the reset leaf route");

    CHECK(build_pair_control(&next_pair, ROUND_ID_NEXT,
                             CMD_SURVEY_PREPARE_PAIR, 310u, &prepare) ==
              PROTO_OK &&
          send_targeted_to_leaf(state, &prepare, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_prepare(state, &decoded, SURVEY_PAIR_PREPARED_LEASE_MS) ==
              SURVEY_PAIR_LEASE_ACCEPTED,
          "operation N+1 PREPARE failed after reset recovery");
    original_deadline = state->lease.prepared_deadline_ms;

    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_START_PAIR,
                             311u, &start) == PROTO_OK &&
          send_targeted_to_leaf(state, &start, &decoded, &delivered) ==
              PROTO_OK && delivered &&
          apply_start(state, &decoded) ==
              SURVEY_PAIR_LEASE_STALE &&
          state->lease.phase == SURVEY_PAIR_LEASE_PREPARED &&
          state->lease.pair.survey_id == SURVEY_ID_N &&
          state->lease.pair.operation_generation ==
              OPERATION_GENERATION_N + 1u &&
          state->lease.round_id == ROUND_ID_NEXT &&
          state->lease.prepared_deadline_ms == original_deadline,
          "delayed START from N mutated N+1");
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "stale START result admission did not commit transport replay");
    CHECK(build_pair_control(&pair, ROUND_ID_N, CMD_SURVEY_ABORT,
                             313u, &abort) == PROTO_OK &&
          send_targeted_to_leaf(state, &abort, &decoded, &delivered) ==
              PROTO_OK && delivered && !apply_abort(state, &decoded) &&
          state->lease.pair.survey_id == SURVEY_ID_N &&
          state->lease.pair.operation_generation ==
              OPERATION_GENERATION_N + 1u &&
          state->lease.round_id == ROUND_ID_NEXT,
          "delayed ABORT from N cancelled N+1");
    CHECK(commit_targeted_leaf_command(state, &decoded) == PROTO_OK,
          "stale ABORT result admission did not commit transport replay");

    CHECK(survey_pair_lease_expire(&state->lease, original_deadline) &&
          state->lease.phase == SURVEY_PAIR_LEASE_IDLE &&
          survey_pair_lease_invariant(&state->lease),
          "N+1 failed to terminate at its tested lease bound");
    return true;
}

static size_t max_identical_control_transmissions(
    const struct mesh_sim_world *world,
    struct proto_packet *most_repeated)
{
    size_t maximum = 0u;

    for (size_t i = 0u; i < world->transmission_count; i++) {
        struct proto_packet first;
        const uint8_t *payload = NULL;
        size_t payload_len = 0u;
        size_t count = 0u;

        if (proto_packet_decode(world->transmissions[i].frame,
                                world->transmissions[i].frame_len,
                                &first, &payload, &payload_len) != PROTO_OK) {
            continue;
        }
        for (size_t j = 0u; j < world->transmission_count; j++) {
            struct proto_packet candidate;

            if (proto_packet_decode(world->transmissions[j].frame,
                                    world->transmissions[j].frame_len,
                                    &candidate, &payload,
                                    &payload_len) == PROTO_OK &&
                candidate.msg_type == first.msg_type &&
                candidate.src_id == first.src_id &&
                candidate.dst_id == first.dst_id &&
                candidate.session_id == first.session_id &&
                candidate.seq == first.seq) {
                count++;
            }
        }
        if (count > maximum) {
            maximum = count;
            if (most_repeated != NULL) {
                *most_repeated = first;
            }
        }
    }
    return maximum;
}

static bool run_scenario(void)
{
    struct mesh_sim_invariant_report report;
    struct proto_packet most_repeated = {0};
    struct survey_pair planned_pair;
    size_t identical_max;

    CHECK(setup_fixture(&scenario) == MESH_SIM_OK,
          "forced multihop fixture or Here-I-Am setup failed");
    CHECK(!scenario.world.reachable[scenario.gateway][scenario.leaf],
          "fixture accidentally permits direct gateway-to-leaf RF");
    if (!run_discovery_announce_matrix(&scenario) ||
        !run_report_idempotence_and_pair_plan(&scenario, &planned_pair) ||
        !run_pair_control_and_reset_matrix(&scenario, &planned_pair)) {
        return false;
    }

    identical_max = max_identical_control_transmissions(&scenario.world,
                                                         &most_repeated);
    CHECK(scenario.world.transmission_count <= MAX_SCENARIO_RF_TRANSMISSIONS,
          "RF traffic exceeded bound actual=%zu bound=%u",
          scenario.world.transmission_count,
          MAX_SCENARIO_RF_TRANSMISSIONS);
    /* One exact duplicate is deliberately carried over both physical hops. */
    CHECK(identical_max <= MAX_IDENTICAL_CONTROL_TRANSMISSIONS,
          "identical control traffic repeated without bound count=%zu type=%u src=%" PRIu64
          " dst=%" PRIu64 " session=%" PRIu32 " seq=%u",
          identical_max, most_repeated.msg_type, most_repeated.src_id,
          most_repeated.dst_id, most_repeated.session_id,
          most_repeated.seq);
    CHECK(scenario.world.now_us <= MAX_SCENARIO_DURATION_US,
          "scenario exceeded liveness bound now=%" PRIu64,
          scenario.world.now_us);
    CHECK(mesh_sim_check_invariants(&scenario.world, &report) == MESH_SIM_OK,
          "runtime invariant=%s node=%zu object=%zu reason=%s",
          mesh_sim_invariant_name(report.code), report.node_index,
          report.object_index,
          report.description == NULL ? "unknown" : report.description);
    CHECK(mesh_sim_check_settled(&scenario.world, &report) == MESH_SIM_OK,
          "network did not settle invariant=%s node=%zu reason=%s",
          mesh_sim_invariant_name(report.code), report.node_index,
          report.description == NULL ? "unknown" : report.description);
    CHECK(scenario.semantic_prepare_count == 1u &&
              scenario.semantic_start_count == 1u &&
              scenario.semantic_abort_count == 1u,
          "semantic exact-once counters disagreed prepare=%zu start=%zu abort=%zu",
          scenario.semantic_prepare_count, scenario.semantic_start_count,
          scenario.semantic_abort_count);
    return true;
}

static bool parse_seed(const char *text, uint32_t *seed)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || seed == NULL || text[0] == '\0') {
        return false;
    }
    value = strtoull(text, &end, 0);
    if (end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *seed = (uint32_t)value;
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
        if (!parse_seed(argv[2], &scenario_seed)) {
            fprintf(stderr, "invalid seed: %s\n", argv[2]);
            return 2;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--seed 0..0xffffffff]\n", argv[0]);
        return 2;
    }
    if (!run_scenario()) {
        fprintf(stderr, "replay: %s --seed 0x%08" PRIx32 "\n",
                argv[0], scenario_seed);
        return EXIT_FAILURE;
    }
    printf("PASS survey_round_adversarial_lifecycle seed=0x%08" PRIx32 " "
           "rf=%zu max_identical=%zu now_us=%" PRIu64 "\n",
           scenario_seed, scenario.world.transmission_count,
           max_identical_control_transmissions(&scenario.world, NULL),
           scenario.world.now_us);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
