#include "mesh.h"
#include "mesh_relay.h"
#include "survey.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_ANCHOR_ID UINT64_C(0x1111222233334444)
#define TEST_GATEWAY_ID UINT64_C(0x9999888877776666)
#define TEST_OPERATION_GENERATION UINT64_C(0x1234567887654321)
#define TEST_SURVEY_ID UINT32_C(0x55667788)

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return (result->actions & action) != 0u;
}

static size_t build_discovery_report(struct proto_packet *packet,
                                     uint8_t *payload,
                                     size_t payload_capacity,
                                     uint32_t boot_incarnation)
{
    size_t payload_len = 0u;

    assert(packet != NULL);
    assert(payload != NULL);
    assert(boot_incarnation != 0u);
    assert(survey_append_reach_report_tlvs(payload,
                                           payload_capacity,
                                           &payload_len,
                                           TEST_SURVEY_ID,
                                           TEST_ANCHOR_ID,
                                           NULL,
                                           0u) == PROTO_OK);
    assert(survey_operation_generation_append_tlv(
               payload,
               payload_capacity,
               &payload_len,
               TEST_OPERATION_GENERATION) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          payload_capacity,
                          &payload_len,
                          TLV_NODE_BOOT_COUNTER,
                          boot_incarnation) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          payload_capacity,
                          &payload_len,
                          TLV_COMMAND_STATUS,
                          COMMAND_OK) == PROTO_OK);

    assert(payload_len <= UINT8_MAX);
    assert(survey_init_discovery_report_packet(
               packet,
               TEST_ANCHOR_ID,
               TEST_GATEWAY_ID,
               TEST_SURVEY_ID,
               TEST_OPERATION_GENERATION,
               boot_incarnation,
               1u,
               (uint8_t)payload_len) == PROTO_OK);
    return payload_len;
}

static void expect_fresh_gateway_delivery(
    struct mesh_relay *gateway,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    struct mesh_relay_result result;

    assert(mesh_packet_rx_semantics_validate(packet,
                                             payload,
                                             payload_len,
                                             TEST_ANCHOR_ID,
                                             TEST_GATEWAY_ID,
                                             TEST_GATEWAY_ID) == PROTO_OK);
    assert(mesh_relay_handle_rx(gateway,
                                packet,
                                payload,
                                payload_len,
                                TEST_ANCHOR_ID,
                                90u,
                                received_at_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);

    assert(mesh_relay_commit_gateway_delivery(gateway,
                                              packet,
                                              payload,
                                              payload_len,
                                              TEST_ANCHOR_ID,
                                              received_at_ms + 1u,
                                              &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}

static void expect_exact_retry_is_duplicate_safe(
    struct mesh_relay *gateway,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    struct mesh_relay_result result;

    assert(mesh_relay_handle_rx(gateway,
                                packet,
                                payload,
                                payload_len,
                                TEST_ANCHOR_ID,
                                90u,
                                received_at_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}

int main(void)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct proto_packet before_reboot;
    struct proto_packet after_reboot;
    uint8_t before_payload[SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN];
    uint8_t after_payload[SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN];
    size_t before_payload_len;
    size_t after_payload_len;
    uint16_t discovery_sequence = UINT16_MAX - 1u;

    /*
     * A boot-local transport sequence must never alias an earlier report.
     * Exhaustion is sticky until a reboot supplies a new boot incarnation.
     */
    assert(survey_discovery_sequence_next(&discovery_sequence) == UINT16_MAX);
    assert(discovery_sequence == UINT16_MAX);
    assert(survey_discovery_sequence_next(&discovery_sequence) == 0u);
    assert(discovery_sequence == UINT16_MAX);
    assert(survey_discovery_sequence_next(&discovery_sequence) == 0u);
    assert(survey_discovery_sequence_next(NULL) == 0u);

    before_payload_len = build_discovery_report(&before_reboot,
                                                before_payload,
                                                sizeof(before_payload),
                                                41u);
    after_payload_len = build_discovery_report(&after_reboot,
                                               after_payload,
                                               sizeof(after_payload),
                                               42u);
    assert(before_reboot.seq == after_reboot.seq);
    assert(before_reboot.session_id != after_reboot.session_id);

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_ID,
                    77u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    expect_fresh_gateway_delivery(&gateway,
                                  &before_reboot,
                                  before_payload,
                                  before_payload_len,
                                  1000u);
    expect_exact_retry_is_duplicate_safe(&gateway,
                                         &before_reboot,
                                         before_payload,
                                         before_payload_len,
                                         1010u);

    /*
     * An exact operation may be redriven after the anchor reset. The new boot
     * report must pass relay/history admission so its mandatory incarnation
     * reaches the gateway's pre-host survey callback.
     */
    expect_fresh_gateway_delivery(&gateway,
                                  &after_reboot,
                                  after_payload,
                                  after_payload_len,
                                  1020u);
    expect_exact_retry_is_duplicate_safe(&gateway,
                                         &after_reboot,
                                         after_payload,
                                         after_payload_len,
                                         1030u);

    puts("survey reboot mesh identity tests passed");
    return 0;
}
