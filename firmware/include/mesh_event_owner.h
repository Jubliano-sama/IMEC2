#ifndef MESH_EVENT_OWNER_H
#define MESH_EVENT_OWNER_H

#include "protocol.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_event_owner_decision {
    MESH_EVENT_OWNER_APPLY = 0,
    MESH_EVENT_OWNER_DUPLICATE,
    MESH_EVENT_OWNER_STALE,
    MESH_EVENT_OWNER_CONFLICT,
    MESH_EVENT_OWNER_INVALID,
};

enum mesh_event_proposal_arbitration {
    MESH_EVENT_PROPOSAL_NO_RECIPROCAL = 0,
    MESH_EVENT_PROPOSAL_KEEP_LOCAL,
    MESH_EVENT_PROPOSAL_ACCEPT_REMOTE,
};

#define MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY 8u

/*
 * A PROPOSE session is the wire-visible connection operation identity.  The
 * local generation protects delayed local work; UPDATE and END use the same
 * session and a monotonically advancing packet sequence starting strictly
 * after the proposal sequence.
 */
struct mesh_event_owner {
    uint64_t peer_id;
    uint8_t proposal_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t local_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t remote_payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t session_id;
    uint64_t remote_boot_nonce;
    uint32_t generation;
    uint32_t remote_proposal_session_id;
    uint16_t proposal_sequence;
    uint16_t local_sequence;
    uint16_t remote_sequence;
    uint8_t local_message_type;
    uint8_t remote_message_type;
    uint32_t retired_session_ids[MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY];
    uint8_t retired_session_count;
    uint8_t retired_session_cursor;
    uint16_t remote_proposal_sequence;
    bool active;
    bool terminal;
    bool proposal_from_peer;
    bool proposal_payload_digest_valid;
    bool remote_proposal_seen;
    bool local_control_seen;
    bool remote_control_seen;
};

int mesh_event_owner_begin(struct mesh_event_owner *owner,
                           uint64_t peer_id,
                           uint32_t session_id,
                           uint16_t proposal_sequence,
                           bool proposal_from_peer);

/* Begin an owner while binding a peer-originated proposal to its boot
 * incarnation. Wire EVENT_PROPOSE classification requires a nonzero nonce;
 * zero is retained here only for locally initiated owners and old fixtures
 * that do not represent a received proposal. */
int mesh_event_owner_begin_with_boot_nonce(
    struct mesh_event_owner *owner,
    uint64_t peer_id,
    uint32_t session_id,
    uint16_t proposal_sequence,
    bool proposal_from_peer,
    uint64_t remote_boot_nonce);

/*
 * Bind the full semantic payload identity after a received proposal becomes
 * the active owner. This survives short response/retry caches for the complete
 * owner lifetime, so a same-key byte mutation remains a conflict.
 */
int mesh_event_owner_bind_remote_proposal_digest(
    struct mesh_event_owner *owner,
    uint32_t session_id,
    uint16_t proposal_sequence,
    const uint8_t proposal_payload_digest[SEMANTIC_DIGEST_SHA256_LEN]);

/*
 * Allocate UPDATE/END sequence values in the owner's peer-scoped direction.
 * The value is committed only by mesh_event_owner_commit_local().
 */
uint16_t mesh_event_owner_next_local_sequence(
    const struct mesh_event_owner *owner);

/* Extract the per-boot nonce from a PROPOSE payload. A missing nonce returns
 * PROTO_ERR_NOT_FOUND; proposal classification treats that as invalid. */
int mesh_event_owner_proposal_boot_nonce(const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t *boot_nonce);

/*
 * Resolve reciprocal proposals using stable hardware identity.  The lower
 * device ID is the sole proposer; a pending proposal to another peer and an
 * owner created from a peer proposal are not reciprocal work.
 */
enum mesh_event_proposal_arbitration
mesh_event_owner_arbitrate_reciprocal_proposal(
    uint64_t local_id,
    uint64_t peer_id,
    bool local_proposal_pending,
    uint64_t local_proposal_peer_id,
    const struct mesh_event_owner *owner);

/*
 * Classify a PROPOSE before reserving or replacing channel-9 timing. The
 * payload must carry one nonzero TLV_MESH_EVENT_BOOT_NONCE; missing or zero
 * incarnations are invalid. A prior operation retained for this peer is
 * stale, and an older 32-bit operation session from the same boot
 * incarnation remains stale after teardown and bounded-history rollover. The
 * proposal packet sequence remains exact identity, not cross-session
 * freshness. Fresh replacements remain legal.
 */
enum mesh_event_owner_decision mesh_event_owner_classify_proposal(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

/*
 * Classify a proposal after the caller has independently proved a live
 * reciprocal local proposal. This is the only path that may let the lower-ID
 * peer's nonzero boot incarnation replace an active locally proposed owner.
 */
enum mesh_event_owner_decision
mesh_event_owner_classify_reciprocal_proposal(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

bool mesh_event_owner_retains_session(const struct mesh_event_owner *owner,
                                      uint32_t session_id);

enum mesh_event_owner_decision mesh_event_owner_classify(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

enum mesh_event_owner_decision mesh_event_owner_classify_local(
    const struct mesh_event_owner *owner,
    uint64_t local_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);

int mesh_event_owner_commit(struct mesh_event_owner *owner,
                            uint64_t local_id,
                            uint64_t previous_hop_id,
                            const struct proto_packet *packet,
                            const uint8_t *payload,
                            size_t payload_len);

int mesh_event_owner_commit_local(struct mesh_event_owner *owner,
                                  uint64_t local_id,
                                  const struct proto_packet *packet,
                                  const uint8_t *payload,
                                  size_t payload_len);

void mesh_event_owner_abandon(struct mesh_event_owner *owner);
bool mesh_event_owner_generation_matches(const struct mesh_event_owner *owner,
                                         uint32_t generation);

#ifdef __cplusplus
}
#endif

#endif
