#include "app_gateway_collection_recovery.h"

#include "semantic_digest.h"

#include <errno.h>
#include <string.h>

static int recovery_collection_identity(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch,
    struct gateway_collection_eack *eack)
{
    const uint8_t *collection_epoch_raw = NULL;
    uint8_t collection_epoch_len = 0u;
    int ret;

    if (packet == NULL || payload == NULL || eack == NULL ||
        packet->payload_len != payload_len || gateway_id == 0u ||
        current_gateway_epoch == 0u ||
        packet->src_id == 0u || packet->dst_id != gateway_id ||
        packet->session_id == 0u || packet->seq == 0u || payload_len == 0u ||
        payload_len > PACKET_EXT_MAX_PAYLOAD_LEN) {
        return -EINVAL;
    }

    memset(eack, 0, sizeof(*eack));
    switch (packet->msg_type) {
    case MSG_COMMAND_RESULT: {
        struct command_result_id result_id;

        ret = command_result_id_from_tlvs(payload, payload_len, &result_id);
        if (ret != PROTO_OK || result_id.gateway_id != gateway_id ||
            result_id.gateway_epoch == 0u ||
            result_id.command_seq != packet->session_id ||
            result_id.node_id != packet->src_id ||
            result_id.node_boot_counter == 0u || result_id.result_seq == 0u) {
            return -EBADMSG;
        }
        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_COLLECTION_EPOCH_ID,
                              &collection_epoch_raw,
                              &collection_epoch_len);
        if (ret != PROTO_OK || collection_epoch_len != sizeof(uint32_t) ||
            proto_get_u32_le(collection_epoch_raw) == 0u) {
            return -EBADMSG;
        }
        eack->gateway_id = result_id.gateway_id;
        eack->gateway_epoch = result_id.gateway_epoch;
        eack->command_seq = result_id.command_seq;
        eack->collection_epoch_id = proto_get_u32_le(collection_epoch_raw);
        break;
    }
    case MSG_RESULT_BUNDLE: {
        struct result_bundle_header bundle;

        ret = result_bundle_header_from_tlvs(payload, payload_len, &bundle);
        if (ret != PROTO_OK || bundle.gateway_id != gateway_id ||
            bundle.gateway_epoch == 0u ||
            bundle.command_seq != packet->session_id ||
            bundle.collection_epoch_id == 0u || bundle.bundle_id == 0u ||
            bundle.record_count == 0u) {
            return -EBADMSG;
        }
        eack->gateway_id = bundle.gateway_id;
        eack->gateway_epoch = bundle.gateway_epoch;
        eack->command_seq = bundle.command_seq;
        eack->collection_epoch_id = bundle.collection_epoch_id;
        break;
    }
    default:
        return -ENOENT;
    }

    /* This owner exists only after gateway reboot. A no-ledger packet from
     * the current gateway epoch is not proof of a vanished prior owner and
     * must remain source-custodied for normal admission/retry. */
    if (eack->gateway_epoch == current_gateway_epoch) {
        return -ESTALE;
    }

    /* A recovery EACK has no live membership roster. The old gateway epoch is
     * stable semantic context and satisfies the normal nonzero wire field. */
    eack->membership_epoch = eack->gateway_epoch;
    eack->expected_count = 1u;
    eack->received_count = 1u;
    eack->eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST;
    eack->retry_round = 0u;
    eack->next_retry_spread_ms = 0u;
    eack->collection_open = false;
    return 0;
}

static bool recovery_packet_identity_matches(
    const struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (state == NULL || packet == NULL ||
        (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        packet->src_id != state->identity.packet_src_id ||
        packet->seq != state->identity.packet_seq ||
        payload_len != state->identity.payload_len ||
        !semantic_digest_sha256(payload, payload_len, digest)) {
        return false;
    }
    return semantic_digest_equal(state->identity.payload_digest,
                                 digest,
                                 sizeof(digest));
}

static bool recovery_identity_matches(
    const struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return state != NULL && state->active &&
           recovery_packet_identity_matches(state,
                                            packet,
                                            payload,
                                            payload_len);
}

int app_gateway_collection_recovery_preflight(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch)
{
    struct gateway_collection_eack eack;

    return recovery_collection_identity(packet,
                                        payload,
                                        payload_len,
                                        gateway_id,
                                        current_gateway_epoch,
                                        &eack);
}

int app_gateway_collection_recovery_reserve_host_custody(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch)
{
    struct gateway_collection_eack eack;
    struct gateway_collection_recovery_identity identity;
    int ret;

    if (state == NULL) {
        return -EINVAL;
    }
    if (state->active || state->host_custody_pending) {
        return recovery_packet_identity_matches(state,
                                                packet,
                                                payload,
                                                payload_len) ?
               0 : -EBUSY;
    }
    ret = recovery_collection_identity(packet,
                                       payload,
                                       payload_len,
                                       gateway_id,
                                       current_gateway_epoch,
                                       &eack);
    if (ret < 0) {
        return ret;
    }
    memset(&identity, 0, sizeof(identity));
    if (!semantic_digest_sha256(payload, payload_len, identity.payload_digest)) {
        return -EIO;
    }
    identity.packet_src_id = packet->src_id;
    identity.packet_seq = packet->seq;
    identity.payload_len = (uint16_t)payload_len;

    memset(state, 0, sizeof(*state));
    state->identity = identity;
    state->host_custody_pending = true;
    return 0;
}

int app_gateway_collection_recovery_cancel_host_custody(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (state == NULL || state->active || !state->host_custody_pending ||
        !recovery_packet_identity_matches(state, packet, payload, payload_len)) {
        return -ESTALE;
    }
    memset(state, 0, sizeof(*state));
    return 0;
}

int app_gateway_collection_recovery_begin(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint16_t current_gateway_epoch,
    uint32_t recovery_attempt_id)
{
    struct gateway_collection_eack eack;
    struct gateway_collection_recovery_identity identity = {0};
    uint8_t eack_payload[PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN];
    struct proto_packet eack_packet = {0};
    size_t offset = 0u;
    int ret;

    if (state == NULL || recovery_attempt_id == 0u ||
        (uint16_t)recovery_attempt_id == 0u) {
        return -EINVAL;
    }
    if (state->active) {
        return recovery_identity_matches(state, packet, payload, payload_len) ?
               0 : -EBUSY;
    }
    if (!state->host_custody_pending) {
        return -ESTALE;
    }
    if (!recovery_packet_identity_matches(state, packet, payload, payload_len)) {
        return -EBUSY;
    }
    ret = recovery_collection_identity(packet,
                                       payload,
                                       payload_len,
                                       gateway_id,
                                       current_gateway_epoch,
                                       &eack);
    if (ret < 0) {
        return ret;
    }
    if (!semantic_digest_sha256(payload, payload_len, identity.payload_digest)) {
        return -EIO;
    }

    identity.packet_src_id = packet->src_id;
    identity.recovery_attempt_id = recovery_attempt_id;
    identity.packet_seq = packet->seq;
    identity.payload_len = (uint16_t)payload_len;
    eack.packet_sequence = (uint16_t)recovery_attempt_id;

    /* Keep the pre-receipt reservation intact until the entire frozen EACK
     * validates.  The BLE head has already been accepted at this boundary;
     * clearing its reservation on a local construction error would admit a
     * successor collection with no owner for that accepted stale packet. */
    ret = gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &offset,
                                               &eack);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    ret = tlv_append_u32(eack_payload,
                         sizeof(eack_payload),
                         &offset,
                         TLV_COLLECTION_RECOVERY_ATTEMPT_ID,
                         identity.recovery_attempt_id);
    if (ret == PROTO_OK) {
        ret = tlv_append_u64(eack_payload,
                             sizeof(eack_payload),
                             &offset,
                             TLV_NODE_ID,
                             identity.packet_src_id);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u16(eack_payload,
                             sizeof(eack_payload),
                             &offset,
                             TLV_RESULT_SEQ,
                             identity.packet_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u16(eack_payload,
                             sizeof(eack_payload),
                             &offset,
                             TLV_PAYLOAD_LEN,
                             identity.payload_len);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_bytes(eack_payload,
                               sizeof(eack_payload),
                               &offset,
                               TLV_RESULT_SHA256_COMMITMENT,
                               identity.payload_digest,
                               sizeof(identity.payload_digest));
    }
    if (ret != PROTO_OK ||
        offset != PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN) {
        return -EBADMSG;
    }

    eack_packet = (struct proto_packet) {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .flags = 0u,
        .src_id = gateway_id,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = eack.command_seq,
        .seq = eack.packet_sequence,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = (uint16_t)offset,
    };
    if (gateway_collection_eack_packet_validate(&eack_packet,
                                                eack_payload,
                                                offset,
                                                NULL) != PROTO_OK) {
        return -EBADMSG;
    }

    memset(state, 0, sizeof(*state));
    state->eack_packet = eack_packet;
    memcpy(state->eack_payload, eack_payload, sizeof(eack_payload));
    state->eack_payload_len = (uint16_t)offset;
    state->identity = identity;
    state->active = true;
    return 0;
}

int app_gateway_collection_recovery_outbound(
    const struct app_gateway_collection_recovery *state,
    struct mesh_outbound *outbound)
{
    if (state == NULL || outbound == NULL || !state->active ||
        state->eack_payload_len !=
            PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN ||
        gateway_collection_eack_packet_validate(&state->eack_packet,
                                                state->eack_payload,
                                                state->eack_payload_len,
                                                NULL) != PROTO_OK) {
        return -EINVAL;
    }

    memset(outbound, 0, sizeof(*outbound));
    outbound->packet = state->eack_packet;
    memcpy(outbound->payload,
           state->eack_payload,
           state->eack_payload_len);
    outbound->payload_len = state->eack_payload_len;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    return 0;
}

bool app_gateway_collection_recovery_outbound_matches(
    const struct app_gateway_collection_recovery *state,
    const struct mesh_outbound *outbound)
{
    const struct proto_packet *expected;
    const struct proto_packet *actual;

    if (state == NULL || outbound == NULL || !state->active ||
        state->eack_payload_len !=
            PROTO_GATEWAY_COLLECTION_RECOVERY_EACK_PAYLOAD_LEN ||
        outbound->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        outbound->next_hop_id != MESH_BROADCAST_ID ||
        outbound->payload_len != state->eack_payload_len) {
        return false;
    }

    expected = &state->eack_packet;
    actual = &outbound->packet;
    if (actual->msg_type != expected->msg_type ||
        actual->flags != expected->flags ||
        actual->src_id != expected->src_id ||
        actual->dst_id != expected->dst_id ||
        actual->session_id != expected->session_id ||
        actual->seq != expected->seq || actual->ttl != expected->ttl ||
        actual->payload_len != expected->payload_len) {
        return false;
    }

    /* Flood transmission updates packet age at every physical attempt.  It is
     * transit accounting, not recovery identity: the frozen envelope fields
     * and payload commitment above remain unchanged across retries. */

    return memcmp(outbound->payload,
                  state->eack_payload,
                  state->eack_payload_len) == 0;
}

bool app_gateway_collection_recovery_matches(
    const struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return state != NULL &&
           (state->active || state->host_custody_pending) &&
           recovery_packet_identity_matches(state,
                                            packet,
                                            payload,
                                            payload_len);
}

int app_gateway_collection_recovery_finish_host_delivery(
    struct app_gateway_collection_recovery *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (state == NULL || !state->active ||
        !state->flood_progress.complete ||
        !recovery_packet_identity_matches(state, packet, payload, payload_len)) {
        return -ESTALE;
    }
    memset(state, 0, sizeof(*state));
    return 0;
}

void app_gateway_collection_recovery_reset(
    struct app_gateway_collection_recovery *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}
