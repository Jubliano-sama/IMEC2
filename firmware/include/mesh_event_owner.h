#ifndef MESH_EVENT_OWNER_H
#define MESH_EVENT_OWNER_H

#include "protocol.h"

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

#define MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY 8u

/*
 * A PROPOSE session is the wire-visible connection operation identity.  The
 * local generation protects delayed local work; UPDATE and END use the same
 * session and a monotonically advancing packet sequence.
 */
struct mesh_event_owner {
    uint64_t peer_id;
    uint32_t session_id;
    uint32_t generation;
    uint32_t local_payload_fingerprint;
    uint32_t remote_payload_fingerprint;
    uint16_t proposal_sequence;
    uint16_t local_sequence;
    uint16_t remote_sequence;
    uint16_t local_payload_len;
    uint16_t remote_payload_len;
    uint8_t local_message_type;
    uint8_t remote_message_type;
    uint32_t retired_session_ids[MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY];
    uint8_t retired_session_count;
    uint8_t retired_session_cursor;
    uint16_t remote_proposal_sequence;
    bool active;
    bool terminal;
    bool proposal_from_peer;
    bool remote_proposal_seen;
    bool local_control_seen;
    bool remote_control_seen;
};

int mesh_event_owner_begin(struct mesh_event_owner *owner,
                           uint64_t peer_id,
                           uint32_t session_id,
                           uint16_t proposal_sequence,
                           bool proposal_from_peer);

/*
 * Classify a PROPOSE before reserving or replacing channel-9 timing. A prior
 * operation retained for this peer is stale, and an older proposal cannot
 * displace a live owner. Fresh replacements remain legal.
 */
enum mesh_event_owner_decision mesh_event_owner_classify_proposal(
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
