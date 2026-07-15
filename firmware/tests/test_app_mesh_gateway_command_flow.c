#include "app_mesh_gateway_command_flow.h"
#include "survey_gateway_transaction.h"

#include <assert.h>
#include <string.h>

#define GATEWAY_ID UINT64_C(0x9000)
#define ANCHOR_ID UINT64_C(0xa100)

static size_t broadcast_command_payload(uint8_t *payload, size_t payload_cap)
{
    size_t payload_len = 0u;

    assert(mesh_append_command_id(payload,
                                  payload_cap,
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         payload_cap,
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         payload_cap,
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          payload_cap,
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          61u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          payload_cap,
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          17u) == PROTO_OK);
    return payload_len;
}

static void test_prepare_anchor_receive_and_result_identity(void)
{
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1234),
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 17u,
        .seq = 61u,
    };
    struct app_mesh_gateway_command_flow flow;
    struct app_mesh_gateway_command_anchor_state anchor_state = {0};
    struct gateway_command_options options;
    struct mesh_outbound result = {0};
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool broadcast;
    bool expired;
    bool duplicate;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = broadcast_command_payload(payload, sizeof(payload));

    command.payload_len = (uint16_t)payload_len;
    assert(app_mesh_gateway_command_flow_prepare(&command,
                                                 payload,
                                                 payload_len,
                                                 GATEWAY_ID,
                                                 100u,
                                                 61u,
                                                 &flow) == PROTO_OK);
    assert(flow.command_id == CMD_GET_STATUS);
    assert(flow.outbound.packet.dst_id == MESH_BROADCAST_ID);
    assert(flow.outbound.next_hop_id == MESH_BROADCAST_ID);

    assert(app_mesh_gateway_command_flow_anchor_receive(
               &anchor_state,
               &flow.outbound.packet,
               flow.outbound.payload,
               flow.outbound.payload_len,
               101u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(command_id == CMD_GET_STATUS);
    assert(broadcast);
    assert(!expired);
    assert(!duplicate);
    app_mesh_gateway_command_flow_anchor_remember(&anchor_state,
                                                   &flow.outbound.packet,
                                                   &options,
                                                   101u);
    assert(app_mesh_gateway_command_flow_anchor_receive(
               &anchor_state,
               &flow.outbound.packet,
               flow.outbound.payload,
               flow.outbound.payload_len,
               102u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(duplicate);

    payload_len = 0u;
    assert(mesh_append_command_result(result.payload,
                                      sizeof(result.payload),
                                      &payload_len,
                                      flow.command_id,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    result.payload_len = (uint16_t)payload_len;
    assert(app_mesh_gateway_command_flow_init_result(&result,
                                                      &flow.outbound.packet,
                                                      ANCHOR_ID,
                                                      GATEWAY_ID,
                                                      false) == PROTO_OK);
    assert(app_mesh_gateway_command_flow_result_matches(&flow.outbound.packet,
                                                         &result.packet));
    assert(app_mesh_gateway_command_flow_decode_result(flow.command_id,
                                                        result.payload,
                                                        result.payload_len,
                                                        &status,
                                                        &reason) == PROTO_OK);
    assert(status == COMMAND_OK);
    assert(reason == 0u);
}

static void test_malformed_then_valid_survey_result_preserves_transaction(void)
{
    const struct survey_pair pair = {
        .survey_id = 77u,
        .initiator_id = ANCHOR_ID,
        .responder_id = UINT64_C(0xa200),
        .sample_count = 4u,
    };
    const struct node_transaction_key key = {
        .requester_id = GATEWAY_ID,
        .responder_id = ANCHOR_ID,
        .session_id = 77u,
        .transaction_id = 51u,
        .operation_id = CMD_SURVEY_PREPARE_PAIR,
    };
    struct survey_gateway_transaction transaction;
    enum survey_gateway_transaction_result result;
    enum node_transaction_action action;
    enum command_status status = COMMAND_OK;
    uint8_t reason = 0u;
    uint8_t payload[32];
    size_t payload_len = 0u;
    uint32_t result_fingerprint;

    survey_gateway_transaction_init(&transaction);
    assert(survey_gateway_transaction_load_pair(&transaction, &pair) == 0);
    assert(survey_gateway_transaction_begin(
               &transaction, &key, CMD_SURVEY_PREPARE_PAIR,
               610u, 710u, 810u, 5000u, 10u) == 0);

    assert(mesh_append_command_id(payload, sizeof(payload), &payload_len,
                                  CMD_SURVEY_PREPARE_PAIR) == PROTO_OK);
    assert(app_mesh_gateway_command_flow_decode_result(
               CMD_SURVEY_PREPARE_PAIR, payload, payload_len,
               &status, &reason) != PROTO_OK);
    assert(transaction.active.state == NODE_TRANSACTION_ACTIVE);
    assert(transaction.active.accepted_result_fingerprint == 0u);
    assert(transaction.active.result_token == 0u);
    assert(transaction.prepared_mask == 0u);
    assert(transaction.recent_next == 0u);

    payload_len = 0u;
    assert(mesh_append_command_result(payload, sizeof(payload), &payload_len,
                                      CMD_SURVEY_PREPARE_PAIR,
                                      COMMAND_OK, 0u) == PROTO_OK);
    assert(app_mesh_gateway_command_flow_decode_result(
               CMD_SURVEY_PREPARE_PAIR, payload, payload_len,
               &status, &reason) == PROTO_OK);
    result_fingerprint = node_transaction_fingerprint_bytes(
        0u, payload, payload_len);
    assert(survey_gateway_transaction_reconcile_result(
               &transaction, &key, 610u, result_fingerprint,
               result_fingerprint, status, 11u, &result, &action) == 0);
    assert(result == SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK);
    assert(transaction.active.state == NODE_TRANSACTION_SUCCEEDED);
    assert(transaction.active.accepted_result_fingerprint ==
           result_fingerprint);
    assert(transaction.prepared_mask ==
           SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK);
}

int main(void)
{
    test_prepare_anchor_receive_and_result_identity();
    test_malformed_then_valid_survey_result_preserves_transaction();
    return 0;
}
