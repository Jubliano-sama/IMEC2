#include "app_gateway_eack_retry.h"

#include <string.h>

static bool collection_identity_valid(
    const struct gateway_collection_state *collection)
{
    return collection != NULL && collection->gateway_id != 0u &&
           collection->command_seq != 0u &&
           collection->collection_epoch_id != 0u &&
           collection->eack_sequence != 0u;
}

static struct app_gateway_eack_retry_identity collection_identity(
    const struct gateway_collection_state *collection)
{
    return (struct app_gateway_eack_retry_identity) {
        .gateway_id = collection->gateway_id,
        .command_seq = collection->command_seq,
        .collection_epoch_id = collection->collection_epoch_id,
        .eack_round = collection->retry_round,
        .eack_sequence = collection->eack_sequence,
    };
}

static bool identity_equal(
    const struct app_gateway_eack_retry_identity *lhs,
    const struct app_gateway_eack_retry_identity *rhs)
{
    return lhs->gateway_id == rhs->gateway_id &&
           lhs->command_seq == rhs->command_seq &&
           lhs->collection_epoch_id == rhs->collection_epoch_id &&
           lhs->eack_round == rhs->eack_round &&
           lhs->eack_sequence == rhs->eack_sequence;
}

static bool packet_equal(const struct proto_packet *lhs,
                         const struct proto_packet *rhs)
{
    return lhs->msg_type == rhs->msg_type &&
           lhs->flags == rhs->flags &&
           lhs->src_id == rhs->src_id &&
           lhs->dst_id == rhs->dst_id &&
           lhs->session_id == rhs->session_id &&
           lhs->seq == rhs->seq &&
           lhs->ttl == rhs->ttl &&
           lhs->payload_len == rhs->payload_len &&
           lhs->message_age_ms == rhs->message_age_ms;
}

static bool eack_packet_matches_collection(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_collection_state *collection)
{
    struct gateway_collection_eack decoded;

    if (!collection_identity_valid(collection) ||
        gateway_collection_eack_packet_validate(packet,
                                                payload,
                                                payload_len,
                                                &decoded) != PROTO_OK) {
        return false;
    }

    return decoded.gateway_id == collection->gateway_id &&
           decoded.gateway_epoch == collection->gateway_epoch &&
           decoded.command_seq == collection->command_seq &&
           decoded.collection_epoch_id == collection->collection_epoch_id &&
           decoded.membership_epoch == collection->membership_epoch &&
           decoded.expected_count == collection->expected_count &&
           decoded.packet_sequence == collection->eack_sequence &&
           decoded.retry_round == collection->retry_round;
}

static bool eack_matches_collection(
    const struct mesh_outbound *eack,
    const struct gateway_collection_state *collection)
{
    return eack != NULL &&
           eack_packet_matches_collection(&eack->packet,
                                          eack->payload,
                                          eack->payload_len,
                                          collection);
}

bool app_gateway_eack_retry_snapshot_active(
    const struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection)
{
    struct app_gateway_eack_retry_identity identity;

    if (state == NULL || !state->active || !state->snapshot.valid ||
        !collection_identity_valid(collection)) {
        return false;
    }

    identity = collection_identity(collection);
    return identity_equal(&state->identity, &identity);
}

int app_gateway_eack_retry_freeze(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    const struct mesh_outbound *eack)
{
    struct app_gateway_eack_retry_identity identity;

    if (state == NULL || !eack_matches_collection(eack, collection)) {
        return PROTO_ERR_ARG;
    }

    identity = collection_identity(collection);
    if (state->active && !identity_equal(&state->identity, &identity)) {
        app_gateway_eack_retry_reset(state);
    }
    if (!state->active) {
        state->identity = identity;
        state->active = true;
    }

    if (state->snapshot.valid) {
        if (gateway_collection_eack_custody_validate(&state->snapshot) != PROTO_OK ||
            !packet_equal(&state->snapshot.packet, &eack->packet) ||
            state->snapshot.payload_len != eack->payload_len ||
            memcmp(state->snapshot.payload,
                   eack->payload,
                   eack->payload_len) != 0) {
            return PROTO_ERR_MALFORMED;
        }
        return PROTO_OK;
    }

    return gateway_collection_eack_custody_capture(&state->snapshot,
                                                   &eack->packet,
                                                   eack->payload,
                                                   eack->payload_len);
}

int app_gateway_eack_retry_restore(
    const struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    struct mesh_outbound *eack)
{
    if (eack == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!app_gateway_eack_retry_snapshot_active(state, collection)) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (gateway_collection_eack_custody_validate(&state->snapshot) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }

    memset(eack, 0, sizeof(*eack));
    eack->packet = state->snapshot.packet;
    memcpy(eack->payload,
           state->snapshot.payload,
           state->snapshot.payload_len);
    eack->payload_len = state->snapshot.payload_len;
    eack->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    eack->next_hop_id = MESH_BROADCAST_ID;

    return eack_matches_collection(eack, collection) ?
           PROTO_OK : PROTO_ERR_MALFORMED;
}

int app_gateway_eack_retry_export_custody(
    const struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    struct gateway_collection_eack_custody_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!app_gateway_eack_retry_snapshot_active(state, collection)) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (gateway_collection_eack_custody_validate(&state->snapshot) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }

    *snapshot = state->snapshot;
    return PROTO_OK;
}

int app_gateway_eack_retry_import_custody(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    const struct gateway_collection_eack_custody_snapshot *snapshot)
{
    bool snapshot_is_state_storage;
    int ret;

    if (state == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = gateway_collection_eack_custody_validate(snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (!eack_packet_matches_collection(&snapshot->packet,
                                        snapshot->payload,
                                        snapshot->payload_len,
                                        collection)) {
        return PROTO_ERR_MALFORMED;
    }

    snapshot_is_state_storage = snapshot == &state->snapshot;
    if (snapshot_is_state_storage) {
        memset(&state->rf_retry, 0, sizeof(state->rf_retry));
        memset(&state->c5_flood_progress, 0, sizeof(state->c5_flood_progress));
        memset(&state->identity, 0, sizeof(state->identity));
        memset(state->failed_channel9_next_hop_ids,
               0,
               sizeof(state->failed_channel9_next_hop_ids));
        state->failed_channel9_next_hop_count = 0u;
        state->force_c5_recovery = false;
        state->active = false;
    } else {
        app_gateway_eack_retry_reset(state);
        state->snapshot = *snapshot;
    }
    state->identity = collection_identity(collection);
    state->active = true;
    return PROTO_OK;
}

static uint32_t mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16;
    return value;
}

static struct app_mesh_rf_retry_key retry_key(
    const struct app_gateway_eack_retry_identity *identity)
{
    return (struct app_mesh_rf_retry_key) {
        .source_id = identity->gateway_id,
        .destination_id = MESH_BROADCAST_ID,
        .session_id = identity->command_seq,
        .sequence = identity->eack_sequence,
        .message_type = MSG_GATEWAY_COLLECTION_EACK,
        .operation = APP_MESH_RF_RETRY_OPERATION_COLLECTION_EACK,
    };
}

uint32_t app_gateway_eack_retry_note_failure(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    uint32_t fresh_entropy)
{
    struct app_gateway_eack_retry_identity identity;
    struct app_mesh_rf_retry_key key;

    if (state == NULL || !collection_identity_valid(collection)) {
        return 0u;
    }

    identity = collection_identity(collection);
    if (!state->active || !identity_equal(&state->identity, &identity)) {
        app_gateway_eack_retry_reset(state);
        state->identity = identity;
        state->active = true;
    }

    key = retry_key(&identity);
    return app_mesh_rf_retry_next_delay_ms(
        &state->rf_retry,
        &key,
        APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD,
        fresh_entropy ^ mix32(identity.collection_epoch_id));
}

void app_gateway_eack_retry_note_failed_channel9_target(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection,
    uint64_t next_hop_id)
{
    struct app_gateway_eack_retry_identity identity;

    if (state == NULL || !state->active ||
        !collection_identity_valid(collection) || next_hop_id == 0u ||
        next_hop_id == MESH_BROADCAST_ID) {
        return;
    }
    identity = collection_identity(collection);
    if (!identity_equal(&state->identity, &identity)) {
        return;
    }
    for (uint8_t i = 0u; i < state->failed_channel9_next_hop_count; i++) {
        if (state->failed_channel9_next_hop_ids[i] == next_hop_id) {
            return;
        }
    }
    if (state->failed_channel9_next_hop_count >=
        APP_GATEWAY_EACK_RETRY_FAILED_HOP_CAP) {
        state->force_c5_recovery = true;
        return;
    }
    state->failed_channel9_next_hop_ids[state->failed_channel9_next_hop_count] =
        next_hop_id;
    state->failed_channel9_next_hop_count++;
}

void app_gateway_eack_retry_note_success(
    struct app_gateway_eack_retry_state *state,
    const struct gateway_collection_state *collection)
{
    struct app_gateway_eack_retry_identity identity;

    if (state == NULL || !state->active ||
        !collection_identity_valid(collection)) {
        return;
    }

    identity = collection_identity(collection);
    if (!identity_equal(&state->identity, &identity)) {
        return;
    }

    (void)app_gateway_eack_retry_commit_success(state);
}

int app_gateway_eack_retry_commit_success(
    struct app_gateway_eack_retry_state *state)
{
    struct app_mesh_rf_retry_key key;

    if (state == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!state->active) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (state->identity.gateway_id == 0u ||
        state->identity.command_seq == 0u ||
        state->identity.collection_epoch_id == 0u ||
        state->identity.eack_sequence == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    key = retry_key(&state->identity);
    app_mesh_rf_retry_note_success(&state->rf_retry, &key);
    memset(state, 0, sizeof(*state));
    return PROTO_OK;
}

void app_gateway_eack_retry_reset(
    struct app_gateway_eack_retry_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}
