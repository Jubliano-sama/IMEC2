#include "mesh_event_owner.h"

#include <string.h>

static uint32_t payload_fingerprint(const uint8_t *payload, size_t payload_len)
{
    uint32_t hash = UINT32_C(2166136261);

    if (payload == NULL && payload_len != 0u) {
        return 0u;
    }
    for (size_t i = 0u; i < payload_len; i++) {
        hash ^= payload[i];
        hash *= UINT32_C(16777619);
    }
    return hash == 0u ? UINT32_C(1) : hash;
}

static bool sequence_is_newer(uint16_t candidate, uint16_t current)
{
    uint16_t delta = (uint16_t)(candidate - current);

    return delta != 0u && delta < UINT16_C(0x8000);
}

int mesh_event_owner_proposal_boot_nonce(const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t *boot_nonce)
{
    size_t offset = 0u;
    bool found = false;

    if (boot_nonce == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    *boot_nonce = 0u;
    while (offset < payload_len) {
        uint8_t type;
        uint8_t value_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            /* The event timing decoder owns complete TLV validation. There
             * is no boot nonce to extract here, so proposal classification
             * will reject the payload as missing its required identity. */
            return found ? PROTO_OK : PROTO_ERR_NOT_FOUND;
        }
        type = payload[offset];
        value_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if ((size_t)value_len > payload_len - offset) {
            return type == TLV_MESH_EVENT_BOOT_NONCE ?
                       PROTO_ERR_MALFORMED :
                       (found ? PROTO_OK : PROTO_ERR_NOT_FOUND);
        }
        if (type == TLV_MESH_EVENT_BOOT_NONCE) {
            if (found || value_len != sizeof(uint64_t)) {
                return PROTO_ERR_MALFORMED;
            }
            *boot_nonce = proto_get_u64_le(&payload[offset]);
            if (*boot_nonce == 0u) {
                return PROTO_ERR_MALFORMED;
            }
            found = true;
        }
        offset += value_len;
    }
    return found ? PROTO_OK : PROTO_ERR_NOT_FOUND;
}

bool mesh_event_owner_retains_session(const struct mesh_event_owner *owner,
                                      uint32_t session_id)
{
    if (owner == NULL || owner->generation == 0u || session_id == 0u) {
        return false;
    }
    if (owner->session_id == session_id) {
        return true;
    }
    for (uint8_t i = 0u; i < owner->retired_session_count; i++) {
        if (owner->retired_session_ids[i] == session_id) {
            return true;
        }
    }
    return false;
}

static void retire_current_session(struct mesh_event_owner *owner)
{
    uint64_t boot_nonce;

    if (owner == NULL || owner->generation == 0u || owner->session_id == 0u) {
        return;
    }
    boot_nonce = owner->proposal_from_peer ? owner->remote_boot_nonce : 0u;
    for (uint8_t i = 0u; i < owner->retired_session_count; i++) {
        if (owner->retired_session_ids[i] == owner->session_id &&
            owner->retired_boot_nonces[i] == boot_nonce) {
            return;
        }
    }

    owner->retired_session_ids[owner->retired_session_cursor] =
        owner->session_id;
    owner->retired_boot_nonces[owner->retired_session_cursor] = boot_nonce;
    owner->retired_session_cursor =
        (uint8_t)((owner->retired_session_cursor + 1u) %
                  MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY);
    if (owner->retired_session_count <
        MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY) {
        owner->retired_session_count++;
    }
}

static bool retired_boot_nonce(const struct mesh_event_owner *owner,
                               uint64_t boot_nonce)
{
    if (owner == NULL || boot_nonce == 0u) {
        return false;
    }
    for (uint8_t i = 0u; i < owner->retired_session_count; i++) {
        if (owner->retired_boot_nonces[i] == boot_nonce) {
            return true;
        }
    }
    return false;
}

static bool proposal_is_new_incarnation(const struct mesh_event_owner *owner,
                                        uint64_t boot_nonce)
{
    return owner != NULL && boot_nonce != 0u &&
           boot_nonce != owner->remote_boot_nonce &&
           !retired_boot_nonce(owner, boot_nonce);
}

int mesh_event_owner_begin(struct mesh_event_owner *owner,
                           uint64_t peer_id,
                           uint32_t session_id,
                           uint16_t proposal_sequence,
                           bool proposal_from_peer)
{
    return mesh_event_owner_begin_with_boot_nonce(owner,
                                                  peer_id,
                                                  session_id,
                                                  proposal_sequence,
                                                  proposal_from_peer,
                                                  0u);
}

int mesh_event_owner_begin_with_boot_nonce(
    struct mesh_event_owner *owner,
    uint64_t peer_id,
    uint32_t session_id,
    uint16_t proposal_sequence,
    bool proposal_from_peer,
    uint64_t remote_boot_nonce)
{
    uint32_t generation;
    bool new_incarnation;

    if (owner == NULL || peer_id == 0u || session_id == 0u ||
        proposal_sequence == 0u) {
        return PROTO_ERR_ARG;
    }

    new_incarnation = proposal_from_peer &&
                      proposal_is_new_incarnation(owner, remote_boot_nonce);
    if (owner->generation != 0u && owner->peer_id == peer_id) {
        if (proposal_from_peer && remote_boot_nonce != 0u &&
            remote_boot_nonce != owner->remote_boot_nonce &&
            retired_boot_nonce(owner, remote_boot_nonce)) {
            return PROTO_ERR_STALE;
        }
        if (!new_incarnation &&
            (mesh_event_owner_retains_session(owner, session_id) ||
             (proposal_from_peer && owner->active &&
              owner->remote_proposal_seen &&
              !sequence_is_newer(proposal_sequence,
                                 owner->remote_proposal_sequence)))) {
            return PROTO_ERR_STALE;
        }
        retire_current_session(owner);
    } else {
        memset(owner->retired_session_ids, 0,
               sizeof(owner->retired_session_ids));
        memset(owner->retired_boot_nonces, 0,
               sizeof(owner->retired_boot_nonces));
        owner->retired_session_count = 0u;
        owner->retired_session_cursor = 0u;
        owner->remote_proposal_sequence = 0u;
        owner->remote_boot_nonce = 0u;
        owner->remote_proposal_seen = false;
    }

    generation = owner->generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    owner->peer_id = peer_id;
    owner->session_id = session_id;
    owner->generation = generation;
    owner->local_payload_fingerprint = 0u;
    owner->remote_payload_fingerprint = 0u;
    owner->proposal_sequence = proposal_sequence;
    owner->local_sequence = 0u;
    owner->remote_sequence = 0u;
    owner->local_payload_len = 0u;
    owner->remote_payload_len = 0u;
    owner->local_message_type = 0u;
    owner->remote_message_type = 0u;
    owner->active = true;
    owner->terminal = false;
    owner->proposal_from_peer = proposal_from_peer;
    if (proposal_from_peer) {
        owner->remote_proposal_sequence = proposal_sequence;
        owner->remote_boot_nonce = remote_boot_nonce;
        owner->remote_proposal_seen = true;
    }
    owner->local_control_seen = false;
    owner->remote_control_seen = false;
    return PROTO_OK;
}

enum mesh_event_owner_decision mesh_event_owner_classify_proposal(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint64_t boot_nonce = 0u;
    int nonce_ret;

    if (packet == NULL || local_id == 0u || previous_hop_id == 0u ||
        packet->msg_type != MSG_MESH_EVENT_PROPOSE || packet->seq == 0u ||
        packet->session_id == 0u || packet->src_id != previous_hop_id ||
        packet->dst_id != local_id || packet->payload_len != payload_len ||
        (payload == NULL && payload_len != 0u)) {
        return MESH_EVENT_OWNER_INVALID;
    }
    nonce_ret = mesh_event_owner_proposal_boot_nonce(payload, payload_len,
                                                     &boot_nonce);
    /* EVENT_PROPOSE is incarnation-scoped on the wire.  A missing nonce is
     * not a legacy zero value: accepting it would let a reset/replay proposal
     * reach the session and timing replacement path without an owner identity.
     * Keep this check local to proposal classification so UPDATE/END remain
     * governed by their independent control-sequence state. */
    if (nonce_ret != PROTO_OK) {
        return MESH_EVENT_OWNER_INVALID;
    }
    if (owner == NULL || owner->generation == 0u) {
        return MESH_EVENT_OWNER_APPLY;
    }
    if (owner->peer_id != previous_hop_id) {
        return MESH_EVENT_OWNER_STALE;
    }
    if (boot_nonce != 0u && boot_nonce != owner->remote_boot_nonce &&
        retired_boot_nonce(owner, boot_nonce)) {
        return MESH_EVENT_OWNER_STALE;
    }
    if (proposal_is_new_incarnation(owner, boot_nonce)) {
        return MESH_EVENT_OWNER_APPLY;
    }
    if (packet->session_id == owner->session_id) {
        return owner->proposal_from_peer &&
               packet->seq == owner->proposal_sequence ?
                   MESH_EVENT_OWNER_DUPLICATE : MESH_EVENT_OWNER_CONFLICT;
    }
    if (mesh_event_owner_retains_session(owner, packet->session_id) ||
        (owner->active && owner->remote_proposal_seen &&
         !sequence_is_newer(packet->seq,
                            owner->remote_proposal_sequence))) {
        return MESH_EVENT_OWNER_STALE;
    }
    return MESH_EVENT_OWNER_APPLY;
}

static enum mesh_event_owner_decision classify_owned_control(
    const struct mesh_event_owner *owner,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool local_control)
{
    uint32_t fingerprint;
    uint32_t previous_fingerprint;
    uint16_t previous_sequence;
    uint16_t previous_payload_len;
    uint8_t previous_message_type;
    bool control_seen;

    if (owner == NULL || packet == NULL || packet->seq == 0u ||
        packet->session_id == 0u || packet->payload_len != payload_len ||
        (payload == NULL && payload_len != 0u) ||
        (packet->msg_type != MSG_MESH_EVENT_UPDATE &&
         packet->msg_type != MSG_MESH_EVENT_END) ||
        (packet->msg_type == MSG_MESH_EVENT_END && payload_len != 0u)) {
        return MESH_EVENT_OWNER_INVALID;
    }
    if (owner->session_id != packet->session_id) {
        return MESH_EVENT_OWNER_STALE;
    }

    if (local_control) {
        control_seen = owner->local_control_seen;
        previous_sequence = owner->local_sequence;
        previous_payload_len = owner->local_payload_len;
        previous_message_type = owner->local_message_type;
        previous_fingerprint = owner->local_payload_fingerprint;
    } else {
        control_seen = owner->remote_control_seen;
        previous_sequence = owner->remote_sequence;
        previous_payload_len = owner->remote_payload_len;
        previous_message_type = owner->remote_message_type;
        previous_fingerprint = owner->remote_payload_fingerprint;
    }
    fingerprint = payload_fingerprint(payload, payload_len);
    if (control_seen && packet->seq == previous_sequence) {
        if (packet->msg_type == previous_message_type &&
            payload_len == previous_payload_len &&
            fingerprint == previous_fingerprint) {
            return MESH_EVENT_OWNER_DUPLICATE;
        }
        return MESH_EVENT_OWNER_CONFLICT;
    }
    if (!owner->active || owner->terminal ||
        (control_seen &&
         !sequence_is_newer(packet->seq, previous_sequence))) {
        return MESH_EVENT_OWNER_STALE;
    }
    return MESH_EVENT_OWNER_APPLY;
}

enum mesh_event_owner_decision mesh_event_owner_classify(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (owner == NULL || packet == NULL || local_id == 0u ||
        previous_hop_id == 0u || owner->peer_id != previous_hop_id ||
        packet->src_id != previous_hop_id || packet->dst_id != local_id) {
        return MESH_EVENT_OWNER_STALE;
    }
    return classify_owned_control(owner, packet, payload, payload_len, false);
}

enum mesh_event_owner_decision mesh_event_owner_classify_local(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (owner == NULL || packet == NULL || local_id == 0u ||
        owner->peer_id == 0u || packet->src_id != local_id ||
        packet->dst_id != owner->peer_id) {
        return MESH_EVENT_OWNER_STALE;
    }
    return classify_owned_control(owner, packet, payload, payload_len, true);
}

static void commit_owned_control(struct mesh_event_owner *owner,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 bool local_control)
{
    if (local_control) {
        owner->local_sequence = packet->seq;
        owner->local_message_type = packet->msg_type;
        owner->local_payload_len = (uint16_t)payload_len;
        owner->local_payload_fingerprint =
            payload_fingerprint(payload, payload_len);
        owner->local_control_seen = true;
    } else {
        owner->remote_sequence = packet->seq;
        owner->remote_message_type = packet->msg_type;
        owner->remote_payload_len = (uint16_t)payload_len;
        owner->remote_payload_fingerprint =
            payload_fingerprint(payload, payload_len);
        owner->remote_control_seen = true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        owner->active = false;
        owner->terminal = true;
    }
}

int mesh_event_owner_commit(struct mesh_event_owner *owner,
                            uint64_t local_id,
                            uint64_t previous_hop_id,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len)
{
    if (mesh_event_owner_classify(owner, local_id, previous_hop_id, packet,
                                  payload, payload_len) !=
        MESH_EVENT_OWNER_APPLY) {
        return PROTO_ERR_STALE;
    }

    commit_owned_control(owner, packet, payload, payload_len, false);
    return PROTO_OK;
}

int mesh_event_owner_commit_local(struct mesh_event_owner *owner,
                                  uint64_t local_id,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
    if (mesh_event_owner_classify_local(owner, local_id, packet, payload,
                                        payload_len) !=
        MESH_EVENT_OWNER_APPLY) {
        return PROTO_ERR_STALE;
    }
    commit_owned_control(owner, packet, payload, payload_len, true);
    return PROTO_OK;
}

void mesh_event_owner_abandon(struct mesh_event_owner *owner)
{
    if (owner == NULL) {
        return;
    }
    owner->active = false;
    owner->terminal = false;
}

bool mesh_event_owner_generation_matches(const struct mesh_event_owner *owner,
                                         uint32_t generation)
{
    return owner != NULL && owner->active && generation != 0u &&
           owner->generation == generation;
}
