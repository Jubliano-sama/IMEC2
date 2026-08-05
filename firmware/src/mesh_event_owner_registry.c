#include "mesh_event_owner_registry.h"

#include <limits.h>
#include <string.h>

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

bool mesh_event_owner_registry_valid(
    const struct mesh_event_owner_registry *registry)
{
    return registry != NULL && registry->owners != NULL &&
           registry->owner_capacity != 0u && registry->tombstones != NULL &&
           registry->tombstone_capacity != 0u;
}

void mesh_event_owner_registry_reset(
    const struct mesh_event_owner_registry *registry)
{
    if (!mesh_event_owner_registry_valid(registry)) {
        return;
    }
    memset(registry->owners, 0,
           registry->owner_capacity * sizeof(registry->owners[0]));
    memset(registry->tombstones, 0,
           registry->tombstone_capacity * sizeof(registry->tombstones[0]));
}

struct mesh_event_owner *mesh_event_owner_registry_find(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id)
{
    if (!mesh_event_owner_registry_valid(registry) || peer_id == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < registry->owner_capacity; i++) {
        if (registry->owners[i].generation != 0u &&
            registry->owners[i].peer_id == peer_id) {
            return &registry->owners[i];
        }
    }
    return NULL;
}

static struct mesh_event_origin_tombstone *tombstone_for_rebind(
    const struct mesh_event_owner_registry *registry,
    uint64_t retired_peer_id,
    uint32_t now_ms)
{
    struct mesh_event_origin_tombstone *available = NULL;

    for (size_t i = 0u; i < registry->tombstone_capacity; i++) {
        struct mesh_event_origin_tombstone *entry = &registry->tombstones[i];

        if (entry->valid && entry->peer_id == retired_peer_id) {
            return entry;
        }
        if (available == NULL &&
            (!entry->valid ||
             deadline_reached(now_ms, entry->retain_until_ms))) {
            available = entry;
        }
    }
    return available;
}

static struct mesh_event_owner *owner_slot_for_begin(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t now_ms,
    struct mesh_event_origin_tombstone **rebind_tombstone)
{
    struct mesh_event_owner *unused = NULL;
    struct mesh_event_owner *rebind = NULL;
    struct mesh_event_origin_tombstone *selected_tombstone = NULL;

    *rebind_tombstone = NULL;
    for (size_t i = 0u; i < registry->owner_capacity; i++) {
        struct mesh_event_owner *owner = &registry->owners[i];

        if (owner->generation != 0u && owner->peer_id == peer_id) {
            return owner;
        }
        if (owner->active) {
            continue;
        }
        if (owner->generation == 0u || owner->peer_id == 0u) {
            if (unused == NULL) {
                unused = owner;
            }
            continue;
        }
        if (rebind == NULL) {
            struct mesh_event_origin_tombstone *candidate =
                tombstone_for_rebind(registry, owner->peer_id, now_ms);

            if (candidate != NULL) {
                rebind = owner;
                selected_tombstone = candidate;
            }
        }
    }
    if (unused != NULL) {
        return unused;
    }
    *rebind_tombstone = selected_tombstone;
    return rebind;
}

bool mesh_event_owner_registry_can_begin(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t now_ms)
{
    struct mesh_event_origin_tombstone *tombstone = NULL;

    if (!mesh_event_owner_registry_valid(registry) || peer_id == 0u) {
        return false;
    }
    return owner_slot_for_begin(registry, peer_id, now_ms, &tombstone) != NULL;
}

int mesh_event_owner_registry_begin(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t session_id,
    uint16_t proposal_sequence,
    bool proposal_from_peer,
    uint64_t remote_boot_nonce,
    uint32_t now_ms,
    uint32_t replay_lifetime_ms,
    uint32_t queue_lifetime_ms)
{
    struct mesh_event_origin_tombstone *tombstone = NULL;
    struct mesh_event_owner *owner;
    uint64_t retired_peer_id = 0u;
    int ret;

    if (!mesh_event_owner_registry_valid(registry) || peer_id == 0u ||
        replay_lifetime_ms == 0u || queue_lifetime_ms == 0u ||
        replay_lifetime_ms > (uint32_t)INT32_MAX ||
        queue_lifetime_ms > (uint32_t)INT32_MAX ||
        replay_lifetime_ms >
            (uint32_t)INT32_MAX - queue_lifetime_ms) {
        return PROTO_ERR_ARG;
    }
    owner = owner_slot_for_begin(registry, peer_id, now_ms, &tombstone);
    if (owner == NULL) {
        return PROTO_ERR_NO_SPACE;
    }
    if (owner->generation != 0u && owner->peer_id != peer_id) {
        if (tombstone == NULL || owner->active) {
            return PROTO_ERR_NO_SPACE;
        }
        retired_peer_id = owner->peer_id;
    }

    ret = mesh_event_owner_begin_with_boot_nonce(
        owner, peer_id, session_id, proposal_sequence, proposal_from_peer,
        remote_boot_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (retired_peer_id != 0u) {
        tombstone->peer_id = retired_peer_id;
        tombstone->reject_until_ms = now_ms + replay_lifetime_ms;
        tombstone->retain_until_ms =
            tombstone->reject_until_ms + queue_lifetime_ms;
        tombstone->valid = true;
    }
    return PROTO_OK;
}

void mesh_event_owner_registry_abandon(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id)
{
    struct mesh_event_owner *owner =
        mesh_event_owner_registry_find(registry, peer_id);

    if (owner != NULL) {
        mesh_event_owner_abandon(owner);
    }
}

bool mesh_event_owner_registry_origin_retained(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t now_ms)
{
    if (!mesh_event_owner_registry_valid(registry) || peer_id == 0u) {
        return false;
    }
    for (size_t i = 0u; i < registry->tombstone_capacity; i++) {
        const struct mesh_event_origin_tombstone *entry =
            &registry->tombstones[i];

        if (entry->valid && entry->peer_id == peer_id &&
            !deadline_reached(now_ms, entry->retain_until_ms)) {
            return true;
        }
    }
    return false;
}

static bool proposal_arrived_in_retained_window(
    const struct mesh_event_owner_registry *registry,
    uint64_t peer_id,
    uint32_t now_ms,
    uint32_t received_at_ms)
{
    for (size_t i = 0u; i < registry->tombstone_capacity; i++) {
        const struct mesh_event_origin_tombstone *entry =
            &registry->tombstones[i];

        if (!entry->valid || entry->peer_id != peer_id ||
            deadline_reached(now_ms, entry->retain_until_ms)) {
            continue;
        }
        return !deadline_reached(received_at_ms, entry->reject_until_ms);
    }
    return false;
}

enum mesh_event_owner_decision mesh_event_owner_registry_classify_proposal(
    const struct mesh_event_owner_registry *registry,
    uint32_t now_ms,
    uint32_t received_at_ms,
    uint64_t local_id,
    uint64_t previous_hop_id,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool reciprocal_local_proposal_proven)
{
    struct mesh_event_owner *owner;

    if (!mesh_event_owner_registry_valid(registry)) {
        return MESH_EVENT_OWNER_INVALID;
    }
    owner = mesh_event_owner_registry_find(registry, previous_hop_id);
    if (owner == NULL &&
        proposal_arrived_in_retained_window(
            registry, previous_hop_id, now_ms, received_at_ms)) {
        return MESH_EVENT_OWNER_STALE;
    }
    if (reciprocal_local_proposal_proven) {
        return mesh_event_owner_classify_reciprocal_proposal(
            owner, local_id, previous_hop_id, packet, payload, payload_len);
    }
    return mesh_event_owner_classify_proposal(
        owner, local_id, previous_hop_id, packet, payload, payload_len);
}
