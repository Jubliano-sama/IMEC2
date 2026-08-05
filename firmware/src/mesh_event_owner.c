#include "mesh_event_owner.h"

#include <string.h>

static bool sequence_is_newer(uint16_t candidate, uint16_t current)
{
    uint16_t delta = (uint16_t)(candidate - current);

    return delta != 0u && delta < UINT16_C(0x8000);
}

static bool session_is_newer(uint32_t candidate, uint32_t current)
{
    uint32_t delta = candidate - current;

    return delta != 0u && delta < UINT32_C(0x80000000);
}

int mesh_event_owner_proposal_boot_nonce(const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t *boot_nonce)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (boot_nonce == NULL || (payload == NULL && payload_len != 0u)) {
        return PROTO_ERR_ARG;
    }
    *boot_nonce = 0u;
    if (payload_len == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_MESH_EVENT_BOOT_NONCE,
                          &value,
                          &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *boot_nonce = proto_get_u64_le(value);
    return *boot_nonce == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

enum mesh_event_proposal_arbitration
mesh_event_owner_arbitrate_reciprocal_proposal(
    uint64_t local_id,
    uint64_t peer_id,
    bool local_proposal_pending,
    uint64_t local_proposal_peer_id,
    const struct mesh_event_owner *owner)
{
    bool pending_to_peer;
    bool installed_local_proposal;

    if (local_id == 0u || peer_id == 0u || local_id == peer_id) {
        return MESH_EVENT_PROPOSAL_NO_RECIPROCAL;
    }
    pending_to_peer = local_proposal_pending &&
                      local_proposal_peer_id == peer_id;
    installed_local_proposal =
        owner != NULL && owner->active && owner->peer_id == peer_id &&
        !owner->proposal_from_peer;
    if (!pending_to_peer && !installed_local_proposal) {
        return MESH_EVENT_PROPOSAL_NO_RECIPROCAL;
    }
    return local_id < peer_id ? MESH_EVENT_PROPOSAL_KEEP_LOCAL :
                                MESH_EVENT_PROPOSAL_ACCEPT_REMOTE;
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
    bool session_retained = false;

    if (owner == NULL || owner->generation == 0u || owner->session_id == 0u) {
        return;
    }
    for (uint8_t i = 0u; i < owner->retired_session_count; i++) {
        if (owner->retired_session_ids[i] == owner->session_id) {
            session_retained = true;
            break;
        }
    }

    if (!session_retained) {
        owner->retired_session_ids[owner->retired_session_cursor] =
            owner->session_id;
        owner->retired_session_cursor =
            (uint8_t)((owner->retired_session_cursor + 1u) %
                      MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY);
        if (owner->retired_session_count <
            MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY) {
            owner->retired_session_count++;
        }
    }
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
    bool new_incarnation = false;
    bool reset_peer_history = false;

    if (owner == NULL || peer_id == 0u || session_id == 0u ||
        proposal_sequence == 0u) {
        return PROTO_ERR_ARG;
    }

    if (owner->generation != 0u && owner->peer_id != peer_id) {
        /*
         * A live owner is never replaceable by another peer. Once inactive,
         * its slot may be rebound; cross-incarnation safety comes from never
         * replacing a live owner with an unorderable boot nonce.
         */
        if (owner->active) {
            return PROTO_ERR_NO_SPACE;
        }
        reset_peer_history = true;
    }
    if (owner->generation != 0u && !reset_peer_history) {
        new_incarnation =
            proposal_from_peer &&
            remote_boot_nonce != 0u &&
            remote_boot_nonce != owner->remote_boot_nonce;
        if (new_incarnation && owner->active) {
            /*
             * Random boot nonces have no monotonic ordering. A peer reboot
             * therefore waits for supervision/route teardown of the prior
             * owner instead of replacing live Channel-9 timing immediately.
             */
            return PROTO_ERR_STALE;
        }
        if (!new_incarnation &&
            (mesh_event_owner_retains_session(owner, session_id) ||
             (proposal_from_peer && owner->remote_proposal_seen &&
              !session_is_newer(session_id,
                                owner->remote_proposal_session_id)))) {
            return PROTO_ERR_STALE;
        }
        retire_current_session(owner);
    } else if (owner->generation == 0u || reset_peer_history) {
        memset(owner->retired_session_ids, 0,
               sizeof(owner->retired_session_ids));
        owner->retired_session_count = 0u;
        owner->retired_session_cursor = 0u;
        owner->remote_proposal_sequence = 0u;
        owner->remote_proposal_session_id = 0u;
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
    memset(owner->proposal_payload_digest, 0,
           sizeof(owner->proposal_payload_digest));
    memset(owner->local_payload_digest, 0,
           sizeof(owner->local_payload_digest));
    memset(owner->remote_payload_digest, 0,
           sizeof(owner->remote_payload_digest));
    owner->proposal_sequence = proposal_sequence;
    owner->local_sequence = 0u;
    owner->remote_sequence = 0u;
    owner->local_message_type = 0u;
    owner->remote_message_type = 0u;
    owner->active = true;
    owner->terminal = false;
    owner->proposal_from_peer = proposal_from_peer;
    owner->proposal_payload_digest_valid = false;
    if (proposal_from_peer) {
        owner->remote_proposal_session_id = session_id;
        owner->remote_proposal_sequence = proposal_sequence;
        owner->remote_boot_nonce = remote_boot_nonce;
        owner->remote_proposal_seen = true;
    }
    owner->local_control_seen = false;
    owner->remote_control_seen = false;
    return PROTO_OK;
}

int mesh_event_owner_bind_remote_proposal_digest(
    struct mesh_event_owner *owner,
    uint32_t session_id,
    uint16_t proposal_sequence,
    const uint8_t proposal_payload_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (owner == NULL || proposal_payload_digest == NULL || !owner->active ||
        !owner->proposal_from_peer || session_id == 0u ||
        proposal_sequence == 0u || owner->session_id != session_id ||
        owner->proposal_sequence != proposal_sequence) {
        return PROTO_ERR_ARG;
    }
    memcpy(owner->proposal_payload_digest,
           proposal_payload_digest,
           sizeof(owner->proposal_payload_digest));
    owner->proposal_payload_digest_valid = true;
    return PROTO_OK;
}

uint16_t mesh_event_owner_next_local_sequence(
    const struct mesh_event_owner *owner)
{
    uint16_t sequence;

    if (owner == NULL || !owner->active || owner->session_id == 0u ||
        owner->proposal_sequence == 0u) {
        return 0u;
    }
    sequence = owner->local_control_seen ?
                   owner->local_sequence :
                   owner->proposal_sequence;
    sequence++;
    return sequence == 0u ? 1u : sequence;
}

static enum mesh_event_owner_decision classify_proposal(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool reciprocal_local_proposal_proven)
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
    /* EVENT_PROPOSE is incarnation-scoped on the wire. A missing nonce is
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
    if (boot_nonce != owner->remote_boot_nonce) {
        /*
         * A new peer incarnation may restart its proposal sequence only after
         * the previous owner is inactive. This protects live timing without
         * an unbounded random-nonce history. The only live exception requires
         * explicit caller proof that this is still the bounded reciprocal
         * window for a locally proposed owner. Stable device ID then lets only
         * the higher endpoint accept the lower endpoint's proposal.
         */
        return owner->active &&
                       !(reciprocal_local_proposal_proven &&
                         !owner->proposal_from_peer &&
                         owner->remote_boot_nonce == 0u &&
                         local_id > previous_hop_id) ?
                   MESH_EVENT_OWNER_STALE :
                   MESH_EVENT_OWNER_APPLY;
    }
    if (packet->session_id == owner->session_id) {
        uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

        if (!owner->proposal_from_peer ||
            packet->seq != owner->proposal_sequence ||
            !owner->proposal_payload_digest_valid ||
            !semantic_digest_sha256(payload, payload_len, digest)) {
            return MESH_EVENT_OWNER_CONFLICT;
        }
        return semantic_digest_equal(
                   digest,
                   owner->proposal_payload_digest,
                   sizeof(digest)) ?
                   MESH_EVENT_OWNER_DUPLICATE :
                   MESH_EVENT_OWNER_CONFLICT;
    }
    if (mesh_event_owner_retains_session(owner, packet->session_id) ||
        (owner->remote_proposal_seen &&
         !session_is_newer(packet->session_id,
                           owner->remote_proposal_session_id))) {
        return MESH_EVENT_OWNER_STALE;
    }
    return MESH_EVENT_OWNER_APPLY;
}

enum mesh_event_owner_decision mesh_event_owner_classify_proposal(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return classify_proposal(owner, local_id, previous_hop_id, packet,
                             payload, payload_len, false);
}

enum mesh_event_owner_decision
mesh_event_owner_classify_reciprocal_proposal(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return classify_proposal(owner, local_id, previous_hop_id, packet,
                             payload, payload_len, true);
}

static enum mesh_event_owner_decision classify_owned_control(
    const struct mesh_event_owner *owner,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool local_control)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    const uint8_t *previous_digest;
    uint16_t previous_sequence;
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
        previous_message_type = owner->local_message_type;
        previous_digest = owner->local_payload_digest;
    } else {
        control_seen = owner->remote_control_seen;
        previous_sequence = owner->remote_sequence;
        previous_message_type = owner->remote_message_type;
        previous_digest = owner->remote_payload_digest;
    }
    if (!semantic_digest_sha256(payload, payload_len, digest)) {
        return MESH_EVENT_OWNER_INVALID;
    }
    if (control_seen && packet->seq == previous_sequence) {
        if (packet->msg_type == previous_message_type &&
            semantic_digest_equal(digest, previous_digest, sizeof(digest))) {
            return MESH_EVENT_OWNER_DUPLICATE;
        }
        return MESH_EVENT_OWNER_CONFLICT;
    }
    if (!owner->active || owner->terminal ||
        (!control_seen &&
         !sequence_is_newer(packet->seq, owner->proposal_sequence)) ||
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

static bool commit_owned_control(struct mesh_event_owner *owner,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 bool local_control)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (!semantic_digest_sha256(payload, payload_len, digest)) {
        return false;
    }
    if (local_control) {
        owner->local_sequence = packet->seq;
        owner->local_message_type = packet->msg_type;
        memcpy(owner->local_payload_digest, digest, sizeof(digest));
        owner->local_control_seen = true;
    } else {
        owner->remote_sequence = packet->seq;
        owner->remote_message_type = packet->msg_type;
        memcpy(owner->remote_payload_digest, digest, sizeof(digest));
        owner->remote_control_seen = true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        owner->active = false;
        owner->terminal = true;
    }
    return true;
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

    return commit_owned_control(owner, packet, payload, payload_len, false) ?
               PROTO_OK : PROTO_ERR_ARG;
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
    return commit_owned_control(owner, packet, payload, payload_len, true) ?
               PROTO_OK : PROTO_ERR_ARG;
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
