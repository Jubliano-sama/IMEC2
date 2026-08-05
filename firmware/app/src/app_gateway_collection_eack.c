#include "app_gateway_collection_eack.h"

#include <errno.h>
#include <string.h>

static int errno_from_proto(int ret)
{
    switch (ret) {
    case PROTO_OK:
        return 0;
    case PROTO_ERR_MALFORMED:
        return -EBUSY;
    case PROTO_ERR_NOT_FOUND:
    case PROTO_ERR_STALE:
        return -EHOSTUNREACH;
    case PROTO_ERR_NO_SPACE:
        return -ENOSPC;
    default:
        return -EINVAL;
    }
}

static bool return_target_excluded(
    uint64_t next_hop_id,
    const uint64_t *excluded_next_hop_ids,
    size_t excluded_next_hop_count)
{
    for (size_t i = 0u; i < excluded_next_hop_count; i++) {
        if (excluded_next_hop_ids[i] == next_hop_id) {
            return true;
        }
    }
    return false;
}

static void result_init(struct app_gateway_collection_eack_result *result,
                        const struct gateway_collection_state *collection)
{
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->policy.mode = APP_GATEWAY_EACK_SEND_NONE;
    if (collection != NULL) {
        result->command_seq = collection->command_seq;
        result->expected_count = collection->expected_count;
        result->received_count = collection->received_count;
        result->collection_open = collection->collection_open;
    }
}

static int count_node_id_tlvs(const uint8_t *payload,
                              size_t payload_len,
                              uint16_t *node_id_count)
{
    size_t offset = 0u;
    uint16_t count = 0u;

    if ((payload == NULL && payload_len != 0u) || node_id_count == NULL) {
        return PROTO_ERR_ARG;
    }

    while (offset < payload_len) {
        uint8_t type;
        uint8_t length;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        length = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (length > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_NODE_ID) {
            if (length != sizeof(uint64_t) || count == UINT16_MAX) {
                return PROTO_ERR_MALFORMED;
            }
            count++;
        }
        offset += length;
    }

    *node_id_count = count;
    return PROTO_OK;
}

static int validate_prebuilt_eack(
    const struct mesh_outbound *eack,
    const struct gateway_collection_state *collection,
    struct gateway_collection_eack *decoded,
    uint16_t *listed_node_count)
{
    int ret;

    if (eack == NULL || collection == NULL || decoded == NULL ||
        listed_node_count == NULL ||
        eack->packet.msg_type != MSG_GATEWAY_COLLECTION_EACK ||
        eack->packet.src_id != collection->gateway_id ||
        eack->packet.dst_id != MESH_BROADCAST_ID ||
        eack->packet.session_id != collection->command_seq ||
        eack->packet.payload_len != eack->payload_len) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_collection_eack_packet_validate(&eack->packet,
                                                  eack->payload,
                                                  eack->payload_len,
                                                  decoded);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (decoded->gateway_id != collection->gateway_id ||
        decoded->gateway_epoch != collection->gateway_epoch ||
        decoded->command_seq != collection->command_seq ||
        decoded->collection_epoch_id != collection->collection_epoch_id ||
        decoded->membership_epoch != collection->membership_epoch ||
        decoded->expected_count != collection->expected_count ||
        decoded->retry_round != collection->retry_round ||
        decoded->packet_sequence != collection->eack_sequence ||
        decoded->received_count > decoded->expected_count) {
        return PROTO_ERR_MALFORMED;
    }

    return count_node_id_tlvs(eack->payload,
                              eack->payload_len,
                              listed_node_count);
}

uint64_t app_gateway_collection_eack_current_channel9_return_hop(
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan,
    uint64_t self_id)
{
    if (received_radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        current_channel9_plan == NULL ||
        previous_hop_id == 0u ||
        previous_hop_id == MESH_BROADCAST_ID ||
        previous_hop_id == self_id) {
        return 0u;
    }

    return previous_hop_id;
}

int app_gateway_collection_eack_prepare(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_eack_input *input,
    struct app_gateway_collection_eack_result *result)
{
    uint16_t missing_count = 0u;
    uint8_t eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST;
    struct gateway_collection_eack decoded_eack = {0};
    uint16_t listed_node_count = 0u;
    int ret;

    if (input != NULL) {
        result_init(result, input->collection);
    } else {
        result_init(result, NULL);
    }
    if (eack == NULL || input == NULL || input->collection == NULL) {
        return -EINVAL;
    }

    if (input->use_prebuilt_eack) {
        ret = validate_prebuilt_eack(eack,
                                     input->collection,
                                     &decoded_eack,
                                     &listed_node_count);
        if (ret != PROTO_OK) {
            return errno_from_proto(ret);
        }
        eack_format = decoded_eack.eack_format;
        if (eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST) {
            missing_count = listed_node_count;
        }
        if (result != NULL) {
            result->command_seq = decoded_eack.command_seq;
            result->expected_count = decoded_eack.expected_count;
            result->received_count = decoded_eack.received_count;
            result->collection_open = decoded_eack.collection_open;
        }
        ret = PROTO_OK;
    } else if (input->expected_node_id_count == input->collection->expected_count) {
        eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST;
        ret = gateway_collection_prepare_missing_eack_outbound(
            input->collection,
            input->expected_node_ids,
            input->expected_node_id_count,
            eack,
            &missing_count);
        if (ret == PROTO_ERR_NO_SPACE) {
            missing_count = 0u;
            eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST;
            ret = gateway_collection_prepare_eack_outbound(
                input->collection,
                EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                eack);
        }
    } else {
        ret = gateway_collection_prepare_eack_outbound(
            input->collection,
            EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
            eack);
    }
    if (result != NULL) {
        result->eack_format = eack_format;
        result->missing_count = missing_count;
    }
    if (ret != PROTO_OK) {
        return errno_from_proto(ret);
    }

    return 0;
}

int app_gateway_collection_recovery_eack_prepare(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_recovery_eack_input *input)
{
    struct gateway_collection_eack recovery = {0};
    uint32_t sequence_mix;
    size_t payload_len = 0u;
    int ret;

    if (eack == NULL || input == NULL ||
        input->gateway_id == 0u ||
        input->command_seq == 0u ||
        input->collection_epoch_id == 0u ||
        input->packet_src_id == 0u ||
        input->recovery_attempt_id == 0u ||
        input->packet_seq == 0u ||
        input->payload_len == 0u ||
        input->payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }

    sequence_mix = input->recovery_attempt_id ^
                   (input->recovery_attempt_id >> 16);
    recovery.gateway_id = input->gateway_id;
    recovery.gateway_epoch = input->gateway_epoch;
    recovery.command_seq = input->command_seq;
    recovery.collection_epoch_id = input->collection_epoch_id;
    recovery.membership_epoch =
        input->gateway_epoch == 0u ? 1u : input->gateway_epoch;
    recovery.expected_count = 1u;
    recovery.received_count = 1u;
    recovery.packet_sequence = (uint16_t)sequence_mix;
    if (recovery.packet_sequence == 0u) {
        recovery.packet_sequence = 1u;
    }
    /*
     * A recovery EACK is terminal authority only for the exact immutable
     * packet whose durable receipt was proven.  The listed node is the packet
     * owner (including the bundling relay for RESULT_BUNDLE), while sequence,
     * length, and SHA-256 distinguish every redriven packet from other custody
     * in the same collection.
     */
    recovery.eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST;
    recovery.retry_round = 0u;
    recovery.next_retry_spread_ms = 0u;
    recovery.collection_open = false;

    memset(eack, 0, sizeof(*eack));
    ret = gateway_collection_eack_append_tlvs(
        eack->payload,
        sizeof(eack->payload),
        &payload_len,
        &recovery);
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(
            eack->payload,
            sizeof(eack->payload),
            &payload_len,
            TLV_COLLECTION_RECOVERY_ATTEMPT_ID,
            input->recovery_attempt_id);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u64(eack->payload,
                             sizeof(eack->payload),
                             &payload_len,
                             TLV_NODE_ID,
                             input->packet_src_id);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u16(eack->payload,
                             sizeof(eack->payload),
                             &payload_len,
                             TLV_RESULT_SEQ,
                             input->packet_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u16(eack->payload,
                             sizeof(eack->payload),
                             &payload_len,
                             TLV_PAYLOAD_LEN,
                             input->payload_len);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_bytes(eack->payload,
                               sizeof(eack->payload),
                               &payload_len,
                               TLV_RESULT_SHA256_COMMITMENT,
                               input->payload_digest,
                               SEMANTIC_DIGEST_SHA256_LEN);
    }
    if (ret != PROTO_OK) {
        return errno_from_proto(ret);
    }

    eack->packet.msg_type = MSG_GATEWAY_COLLECTION_EACK;
    eack->packet.src_id = input->gateway_id;
    eack->packet.dst_id = MESH_BROADCAST_ID;
    eack->packet.session_id = input->command_seq;
    eack->packet.seq = recovery.packet_sequence;
    eack->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    eack->packet.payload_len = (uint16_t)payload_len;
    eack->payload_len = (uint16_t)payload_len;
    eack->next_hop_id = MESH_BROADCAST_ID;
    eack->radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    return gateway_collection_eack_packet_validate(
               &eack->packet,
               eack->payload,
               eack->payload_len,
               NULL) == PROTO_OK ?
           0 : -EBADMSG;
}

int app_gateway_collection_eack_send(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_eack_input *input,
    const struct app_gateway_eack_policy_ops *ops,
    struct app_gateway_collection_eack_result *result)
{
    uint64_t return_next_hop_ids[APP_GATEWAY_COLLECTION_EACK_RETURN_TARGET_CAP] = {0};
    size_t return_target_count;
    size_t eligible_return_target_count = 0u;
    uint64_t current_channel9_next_hop_id;
    int ret;

    if (input != NULL) {
        result_init(result, input->collection);
    } else {
        result_init(result, NULL);
    }
    if (eack == NULL || input == NULL || input->collection == NULL || ops == NULL ||
        (input->excluded_channel9_next_hop_count != 0u &&
         input->excluded_channel9_next_hop_ids == NULL)) {
        return -EINVAL;
    }

    ret = app_gateway_collection_eack_prepare(eack, input, result);
    if (ret < 0) {
        return ret;
    }

    current_channel9_next_hop_id =
        app_gateway_collection_eack_current_channel9_return_hop(
            input->previous_hop_id,
            input->received_radio_channel,
            input->current_channel9_plan,
            input->self_id);
    if (result != NULL) {
        result->current_channel9_next_hop_id = current_channel9_next_hop_id;
    }

    return_target_count = gateway_collection_return_candidates(
        input->collection,
        return_next_hop_ids,
        sizeof(return_next_hop_ids) / sizeof(return_next_hop_ids[0]));
    if (result != NULL) {
        result->return_target_count = return_target_count;
    }
    for (size_t i = 0u; !input->force_c5_recovery && i < return_target_count; i++) {
        if (!return_target_excluded(
                return_next_hop_ids[i],
                input->excluded_channel9_next_hop_ids,
                input->excluded_channel9_next_hop_count)) {
            return_next_hop_ids[eligible_return_target_count] =
                return_next_hop_ids[i];
            eligible_return_target_count++;
        }
    }

    return app_gateway_eack_send_to_candidates_with_current_channel9(
        eack,
        current_channel9_next_hop_id,
        input->current_channel9_plan,
        return_next_hop_ids,
        eligible_return_target_count,
        ops,
        result == NULL ? NULL : &result->policy);
}
