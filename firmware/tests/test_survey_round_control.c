#include "survey_round_control.h"

#include "mesh_radio_timing.h"
#include "mesh_relay.h"
#include "survey.h"

#include <assert.h>
#include <string.h>

#define GATEWAY_ID 0x1111222233334444ull
#define SURVEY_ID 0xAABBCCDDu
#define ROUND_ID 0x1234u

static void test_round_id_optional_parser_and_encoding(void)
{
    uint8_t payload[16] = {TLV_COMMAND_ID, 2u, 1u, 0u};
    size_t payload_len = 4u;
    uint16_t round_id = UINT16_MAX;

    assert(TLV_SURVEY_ROUND_ID == 0xAFu);
    assert(CMD_SURVEY_GO == 0x0105u);
    assert(survey_round_id_extract_tlv(payload,
                                       payload_len,
                                       &round_id) == PROTO_OK);
    assert(round_id == SURVEY_LEGACY_ROUND_ID);
    assert(survey_round_id_append_tlv(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      ROUND_ID) == PROTO_OK);
    assert(survey_round_id_extract_tlv(payload,
                                       payload_len,
                                       &round_id) == PROTO_OK);
    assert(round_id == ROUND_ID);
    assert(survey_round_id_append_tlv(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      SURVEY_LEGACY_ROUND_ID) ==
           PROTO_ERR_MALFORMED);
}

static void test_go_payload_round_trip_and_parser_rejections(void)
{
    const struct survey_round_go go = {
        .survey_id = SURVEY_ID,
        .round_id = ROUND_ID,
    };
    struct survey_round_go decoded = {0};
    uint8_t payload[32] = {0};
    size_t payload_len = 0u;

    assert(survey_round_go_append_tlvs(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       &go) == PROTO_OK);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_OK);
    assert(decoded.survey_id == go.survey_id);
    assert(decoded.round_id == go.round_id);

    payload[2u] = (uint8_t)(CMD_SURVEY_START_PAIR & 0xffu);
    payload[3u] = (uint8_t)(CMD_SURVEY_START_PAIR >> 8);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);
    payload[2u] = (uint8_t)(CMD_SURVEY_GO & 0xffu);
    payload[3u] = (uint8_t)(CMD_SURVEY_GO >> 8);
    payload[payload_len - 2u] = 0u;
    payload[payload_len - 1u] = 0u;
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len,
                                     &decoded) == PROTO_ERR_MALFORMED);
    assert(survey_round_go_from_tlvs(payload,
                                     payload_len - PROTO_TLV_U16_ENCODED_LEN,
                                     &decoded) == PROTO_ERR_MALFORMED);
}

static void test_go_packet_initializer(void)
{
    struct proto_packet packet;

    memset(&packet, 0xa5, sizeof(packet));
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       7u,
                                       16u) == PROTO_OK);
    assert(packet.msg_type == MSG_COMMAND);
    assert(packet.flags == 0u);
    assert(packet.src_id == GATEWAY_ID);
    assert(packet.dst_id == 0u);
    assert(packet.session_id == SURVEY_ID);
    assert(packet.seq == 7u);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 16u);
    assert(packet.message_age_ms == 0u);

    assert(survey_round_go_init_packet(NULL,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       1u,
                                       0u) == PROTO_ERR_ARG);
    assert(survey_round_go_init_packet(&packet,
                                       0u,
                                       SURVEY_ID,
                                       1u,
                                       0u) == PROTO_ERR_MALFORMED);
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       0u,
                                       1u,
                                       0u) == PROTO_ERR_MALFORMED);
    assert(survey_round_go_init_packet(&packet,
                                       GATEWAY_ID,
                                       SURVEY_ID,
                                       0u,
                                       0u) == PROTO_ERR_MALFORMED);
}

static void test_go_execute_delay_scales_by_complete_forward_horizon(void)
{
    assert(survey_round_go_execute_delay_ms(0u) ==
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(1u) ==
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(2u) ==
           2u * SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(3u) ==
           3u * SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
}

static void test_go_execute_delay_covers_first_receiver_forward(void)
{
    const uint32_t first_receiver_forward_horizon_ms =
        FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS +
        MESH_RADIO_WAKE_TRAIN_MS +
        FLOOD_RELAY_BURST_MS +
        FLOOD_POST_ROOT_GUARD_MS;

    /*
     * Local GO delivery happens after the relay core has synchronously built
     * and sent the first broadcast forward. Even a directly reached anchor
     * must therefore retain enough GO delay for that complete worst case.
     */
    assert(SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS >=
           first_receiver_forward_horizon_ms);
    assert(SURVEY_ROUND_GO_BASE_EXECUTE_DELAY_MS >=
           SURVEY_ROUND_GO_PER_HOP_EXECUTE_DELAY_MS);
    assert(survey_round_go_execute_delay_ms(0u) >=
           first_receiver_forward_horizon_ms);
    assert(survey_round_go_execute_delay_ms(1u) >=
           first_receiver_forward_horizon_ms);
}

int main(void)
{
    test_round_id_optional_parser_and_encoding();
    test_go_payload_round_trip_and_parser_rejections();
    test_go_packet_initializer();
    test_go_execute_delay_scales_by_complete_forward_horizon();
    test_go_execute_delay_covers_first_receiver_forward();
    return 0;
}
