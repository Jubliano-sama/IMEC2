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

int app_gateway_collection_eack_send(
    struct mesh_outbound *eack,
    const struct app_gateway_collection_eack_input *input,
    const struct app_gateway_eack_policy_ops *ops,
    struct app_gateway_collection_eack_result *result)
{
    uint64_t return_next_hop_ids[APP_GATEWAY_COLLECTION_EACK_RETURN_TARGET_CAP] = {0};
    uint16_t missing_count = 0u;
    size_t return_target_count;
    uint8_t eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST;
    uint64_t current_channel9_next_hop_id;
    int ret;

    if (input != NULL) {
        result_init(result, input->collection);
    } else {
        result_init(result, NULL);
    }
    if (eack == NULL || input == NULL || input->collection == NULL || ops == NULL) {
        return -EINVAL;
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

    if (input->expected_node_id_count == input->collection->expected_count) {
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

    return_target_count = gateway_collection_return_candidates(
        input->collection,
        return_next_hop_ids,
        sizeof(return_next_hop_ids) / sizeof(return_next_hop_ids[0]));
    if (result != NULL) {
        result->return_target_count = return_target_count;
    }

    return app_gateway_eack_send_to_candidates_with_current_channel9(
        eack,
        current_channel9_next_hop_id,
        input->current_channel9_plan,
        return_next_hop_ids,
        return_target_count,
        ops,
        result == NULL ? NULL : &result->policy);
}
