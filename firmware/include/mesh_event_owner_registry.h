#ifndef MESH_EVENT_OWNER_REGISTRY_H
#define MESH_EVENT_OWNER_REGISTRY_H

#include "mesh_event_owner.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rebinding an inactive owner slot must not erase the old origin's replay
 * boundary while its protocol retries or already-received queue entries can
 * still surface. reject_until_ms bounds admissible arrival time;
 * retain_until_ms keeps that decision available while the RX queue drains.
 */
struct mesh_event_origin_tombstone {
    uint64_t peer_id;
    uint32_t reject_until_ms;
    uint32_t retain_until_ms;
    bool valid;
};

struct mesh_event_owner_registry {
    struct mesh_event_owner *owners;
    size_t owner_capacity;
    struct mesh_event_origin_tombstone *tombstones;
    size_t tombstone_capacity;
};

bool mesh_event_owner_registry_valid(
    const struct mesh_event_owner_registry *registry);
void mesh_event_owner_registry_reset(
    const struct mesh_event_owner_registry *registry);
struct mesh_event_owner *mesh_event_owner_registry_find(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id);
bool mesh_event_owner_registry_can_begin(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t now_ms);
int mesh_event_owner_registry_begin(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t session_id,
    uint16_t proposal_sequence,
    bool proposal_from_peer,
    uint64_t remote_boot_nonce,
    uint32_t now_ms,
    uint32_t replay_lifetime_ms,
    uint32_t queue_lifetime_ms);
void mesh_event_owner_registry_abandon(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id);
enum mesh_event_owner_decision mesh_event_owner_registry_classify_proposal(
    const struct mesh_event_owner_registry *registry,
    uint32_t now_ms,
    uint32_t received_at_ms,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool reciprocal_local_proposal_proven);
bool mesh_event_owner_registry_origin_retained(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
