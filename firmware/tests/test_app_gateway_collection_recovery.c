#include "app_gateway_collection_recovery.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const uint64_t gateway_id = UINT64_C(0x1111222233334444);
static const uint64_t source_id = UINT64_C(0xaaaabbbbccccdddd);

static size_t make_collection_result(uint8_t *payload,
                                     size_t payload_cap,
                                     uint32_t collection_epoch,
                                     uint16_t gateway_epoch)
{
    const struct command_result_id id = {
        .gateway_id = gateway_id,
        .gateway_epoch = gateway_epoch,
        .command_seq = 91u,
        .node_id = source_id,
        .node_boot_counter = 3u,
        .result_seq = 12u,
    };
    size_t offset = 0u;

    assert(command_result_id_append_tlvs(payload,
                                         payload_cap,
                                         &offset,
                                         &id) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          payload_cap,
                          &offset,
                          TLV_COLLECTION_EPOCH_ID,
                          collection_epoch) == PROTO_OK);
    return offset;
}

static size_t make_collection_bundle(uint8_t *payload,
                                     size_t payload_cap,
                                     uint16_t bundle_id)
{
    struct command_result_id record_id = {
        .gateway_id = gateway_id,
        .gateway_epoch = 7u,
        .command_seq = 91u,
        .node_id = source_id,
        .node_boot_counter = 3u,
        .result_seq = 12u,
    };
    struct result_bundle_header bundle = {
        .gateway_id = gateway_id,
        .gateway_epoch = 7u,
        .command_seq = 91u,
        .collection_epoch_id = 44u,
        .bundle_id = bundle_id,
        .record_count = 1u,
    };
    uint8_t record_payload[96] = {0};
    uint8_t records[128] = {0};
    size_t record_payload_len = 0u;
    size_t records_len = 0u;
    size_t offset = 0u;

    assert(command_result_id_append_tlvs(record_payload,
                                         sizeof(record_payload),
                                         &record_payload_len,
                                         &record_id) == PROTO_OK);
    assert(mesh_append_command_result(record_payload,
                                      sizeof(record_payload),
                                      &record_payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(tlv_append_u32(record_payload,
                          sizeof(record_payload),
                          &record_payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          bundle.collection_epoch_id) == PROTO_OK);
    {
        const struct result_bundle_record complete_record = {
            .result_id = record_id,
            .payload_len = (uint16_t)record_payload_len,
            .payload_crc = proto_crc16_ccitt_false(record_payload,
                                                   record_payload_len),
            .payload = record_payload,
        };

        assert(result_bundle_record_append_tlv(records,
                                               sizeof(records),
                                               &records_len,
                                               &complete_record) == PROTO_OK);
    }
    bundle.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(payload,
                                            payload_cap,
                                            &offset,
                                            &bundle) == PROTO_OK);
    assert(payload_cap - offset >= records_len);
    memcpy(&payload[offset], records, records_len);
    offset += records_len;
    return offset;
}

static void test_recovery_eack_binds_one_result_after_host_receipt(void)
{
    struct app_gateway_collection_recovery state = {0};
    struct mesh_outbound outbound;
    struct gateway_collection_eack eack;
    struct gateway_collection_recovery_identity identity;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = 91u,
        .seq = 19u,
    };
    uint8_t payload[128] = {0};
    size_t payload_len = make_collection_result(payload,
                                                sizeof(payload),
                                                44u,
                                                7u);

    packet.payload_len = (uint16_t)payload_len;
    assert(app_gateway_collection_recovery_preflight(&packet,
                                                     payload,
                                                     payload_len,
                                                     gateway_id,
                                                     8u) == 0);
    assert(app_gateway_collection_recovery_reserve_host_custody(&state,
                                                                &packet,
                                                                payload,
                                                                payload_len,
                                                                gateway_id,
                                                                8u) == 0);
    assert(state.host_custody_pending && !state.active);
    assert(app_gateway_collection_recovery_begin(&state,
                                                 &packet,
                                                 payload,
                                                 payload_len,
                                                 gateway_id,
                                                 8u,
                                                 UINT32_C(0x12007)) == 0);
    assert(app_gateway_collection_recovery_matches(&state,
                                                   &packet,
                                                   payload,
                                                   payload_len));
    assert(app_gateway_collection_recovery_outbound(&state, &outbound) == 0);
    assert(outbound.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(outbound.next_hop_id == MESH_BROADCAST_ID);
    assert(outbound.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(outbound.packet.seq == 0x2007u);
    assert(gateway_collection_eack_packet_validate(&outbound.packet,
                                                   outbound.payload,
                                                   outbound.payload_len,
                                                   &eack) == PROTO_OK);
    assert(eack.gateway_id == gateway_id);
    assert(eack.gateway_epoch == 7u);
    assert(eack.command_seq == packet.session_id);
    assert(eack.collection_epoch_id == 44u);
    assert(eack.membership_epoch == 7u);
    assert(!eack.collection_open);
    assert(eack.expected_count == 1u && eack.received_count == 1u);
    assert(eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST);
    assert(gateway_collection_eack_recovery_identity(outbound.payload,
                                                     outbound.payload_len,
                                                     &identity) == PROTO_OK);
    assert(identity.recovery_attempt_id == UINT32_C(0x12007));
    assert(identity.packet_src_id == source_id);
    assert(identity.packet_seq == packet.seq);
    assert(identity.payload_len == payload_len);
}

static void test_recovery_owner_refuses_cross_packet_replacement(void)
{
    struct app_gateway_collection_recovery state = {0};
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = 91u,
        .seq = 19u,
    };
    uint8_t payload[128] = {0};
    uint8_t altered[128] = {0};
    size_t payload_len = make_collection_result(payload,
                                                sizeof(payload),
                                                44u,
                                                7u);

    packet.payload_len = (uint16_t)payload_len;
    assert(app_gateway_collection_recovery_reserve_host_custody(&state,
                                                                &packet,
                                                                payload,
                                                                payload_len,
                                                                gateway_id,
                                                                8u) == 0);
    assert(app_gateway_collection_recovery_begin(&state,
                                                 &packet,
                                                 payload,
                                                 payload_len,
                                                 gateway_id,
                                                 8u,
                                                 1u) == 0);
    assert(app_gateway_collection_recovery_begin(&state,
                                                 &packet,
                                                 payload,
                                                 payload_len,
                                                 gateway_id,
                                                 8u,
                                                 2u) == 0);
    memcpy(altered, payload, payload_len);
    altered[payload_len - 1u] ^= 0x01u;
    assert(app_gateway_collection_recovery_begin(&state,
                                                 &packet,
                                                 altered,
                                                 payload_len,
                                                 gateway_id,
                                                 8u,
                                                 2u) == -EBUSY);
    assert(app_gateway_collection_recovery_begin(&state,
                                                 &packet,
                                                 payload,
                                                 payload_len,
                                                 gateway_id,
                                                 8u,
                                                 UINT32_C(0x10000)) == -EINVAL);
    app_gateway_collection_recovery_reset(&state);
    assert(!state.active);
}

static void test_bundle_identity_is_supported_but_wrong_gateway_is_not(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = 91u,
        .seq = 22u,
    };
    uint8_t payload[128] = {0};
    size_t payload_len = make_collection_bundle(payload, sizeof(payload), 22u);

    packet.payload_len = (uint16_t)payload_len;
    assert(app_gateway_collection_recovery_preflight(&packet,
                                                     payload,
                                                     payload_len,
                                                     gateway_id,
                                                     8u) == 0);
    packet.dst_id++;
    assert(app_gateway_collection_recovery_preflight(&packet,
                                                     payload,
                                                     payload_len,
                                                     gateway_id,
                                                     8u) == -EINVAL);
}

static void test_current_epoch_never_claims_reboot_recovery(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = 91u,
        .seq = 19u,
    };
    uint8_t payload[128] = {0};
    size_t payload_len = make_collection_result(payload,
                                                sizeof(payload),
                                                44u,
                                                8u);

    packet.payload_len = (uint16_t)payload_len;
    assert(app_gateway_collection_recovery_preflight(&packet,
                                                     payload,
                                                     payload_len,
                                                     gateway_id,
                                                     8u) == -ESTALE);
}

static void test_pre_receipt_reservation_blocks_replacement_until_retirement(void)
{
    struct app_gateway_collection_recovery state = {0};
    struct proto_packet packet_a = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = 91u,
        .seq = 19u,
    };
    struct proto_packet packet_b = packet_a;
    uint8_t payload_a[128] = {0};
    uint8_t payload_b[128] = {0};
    size_t payload_a_len = make_collection_result(payload_a,
                                                  sizeof(payload_a),
                                                  44u,
                                                  7u);
    size_t payload_b_len = make_collection_result(payload_b,
                                                  sizeof(payload_b),
                                                  45u,
                                                  7u);

    packet_a.payload_len = (uint16_t)payload_a_len;
    packet_b.seq = 20u;
    packet_b.payload_len = (uint16_t)payload_b_len;
    assert(app_gateway_collection_recovery_reserve_host_custody(
               &state,
               &packet_a,
               payload_a,
               payload_a_len,
               gateway_id,
               8u) == 0);
    /* Packet B represents the next stale source record queued behind A. It
     * cannot replace A while A waits for its GUI receipt. */
    assert(app_gateway_collection_recovery_reserve_host_custody(
               &state,
               &packet_b,
               payload_b,
               payload_b_len,
               gateway_id,
               8u) == -EBUSY);
    assert(app_gateway_collection_recovery_begin(&state,
                                                 &packet_a,
                                                 payload_a,
                                                 payload_a_len,
                                                 gateway_id,
                                                 8u,
                                                 1u) == 0);
    /* The full recovery flood and BLE retirement are still outstanding, so B
     * remains behind A even after A's receipt has started the EACK. */
    assert(app_gateway_collection_recovery_reserve_host_custody(
               &state,
               &packet_b,
               payload_b,
               payload_b_len,
               gateway_id,
               8u) == -EBUSY);
    assert(app_gateway_collection_recovery_finish_host_delivery(
               &state, &packet_a, payload_a, payload_a_len) == -ESTALE);
    state.flood_progress.complete = true;
    assert(app_gateway_collection_recovery_finish_host_delivery(
               &state, &packet_a, payload_a, payload_a_len) == 0);
    assert(!state.host_custody_pending && !state.active);
    assert(app_gateway_collection_recovery_reserve_host_custody(
               &state,
               &packet_b,
               payload_b,
               payload_b_len,
               gateway_id,
               8u) == 0);
}

int main(void)
{
    test_recovery_eack_binds_one_result_after_host_receipt();
    test_recovery_owner_refuses_cross_packet_replacement();
    test_bundle_identity_is_supported_but_wrong_gateway_is_not();
    test_current_epoch_never_claims_reboot_recovery();
    test_pre_receipt_reservation_blocks_replacement_until_retirement();
    puts("app_gateway_collection_recovery tests passed");
    return 0;
}
