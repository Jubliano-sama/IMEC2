#include "mesh_event_owner_registry.h"

#include "mesh.h"

#include <assert.h>
#include <stdint.h>

#define LOCAL_ID UINT64_C(0x1000000000000001)
#define PEER_A_ID UINT64_C(0x2000000000000002)
#define PEER_B_ID UINT64_C(0x3000000000000003)
#define PEER_C_ID UINT64_C(0x4000000000000004)
#define PEER_A_BOOT_NONCE UINT64_C(0xa1a2a3a4a5a6a7a8)
#define PEER_B_BOOT_NONCE UINT64_C(0xb1b2b3b4b5b6b7b8)
#define PEER_C_BOOT_NONCE UINT64_C(0xc1c2c3c4c5c6c7c8)
#define REPLAY_LIFETIME_MS 6000u
#define QUEUE_LIFETIME_MS 6000u

static size_t proposal_payload(uint8_t *payload,
                               size_t payload_capacity,
                               uint64_t boot_nonce)
{
    size_t payload_len = 0u;

    assert(tlv_append_u64(payload, payload_capacity, &payload_len,
                          TLV_MESH_EVENT_BOOT_NONCE,
                          boot_nonce) == PROTO_OK);
    return payload_len;
}

static struct proto_packet proposal(uint64_t peer_id,
                                    uint32_t session_id,
                                    uint16_t sequence,
                                    size_t payload_len)
{
    struct proto_packet packet = {0};

    assert(payload_len <= UINT8_MAX);
    assert(mesh_init_event_control(&packet, MSG_MESH_EVENT_PROPOSE,
                                   peer_id, LOCAL_ID, session_id, sequence,
                                   (uint8_t)payload_len) == PROTO_OK);
    return packet;
}

static void test_slot_reuse_retains_origin_through_queue_horizon(void)
{
    struct mesh_event_owner owners[1] = {0};
    struct mesh_event_origin_tombstone tombstones[1] = {0};
    const struct mesh_event_owner_registry registry = {
        .owners = owners,
        .owner_capacity = 1u,
        .tombstones = tombstones,
        .tombstone_capacity = 1u,
    };
    uint8_t peer_a_payload[PROTO_TLV_U64_ENCODED_LEN];
    size_t peer_a_payload_len = proposal_payload(
        peer_a_payload, sizeof(peer_a_payload), PEER_A_BOOT_NONCE);
    struct proto_packet delayed_a = proposal(
        PEER_A_ID, UINT32_C(0xa0000001), 1u, peer_a_payload_len);

    assert(mesh_event_owner_registry_begin(
               &registry, PEER_A_ID, delayed_a.session_id, delayed_a.seq,
               true, PEER_A_BOOT_NONCE, 100u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    mesh_event_owner_registry_abandon(&registry, PEER_A_ID);
    assert(!owners[0].active);

    assert(mesh_event_owner_registry_begin(
               &registry, PEER_B_ID, UINT32_C(0xb0000001), 1u, true,
               PEER_B_BOOT_NONCE, 200u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    assert(mesh_event_owner_registry_find(&registry, PEER_A_ID) == NULL);
    assert(mesh_event_owner_registry_find(&registry, PEER_B_ID) == &owners[0]);
    assert(mesh_event_owner_registry_origin_retained(
        &registry, PEER_A_ID, 7000u));

    /*
     * The real registry lookup no longer sees A after B reuses its owner slot.
     * The same classifier used by the receive handler still rejects A because
     * the frame arrived inside A's replay window and was dequeued later.
     */
    assert(mesh_event_owner_registry_classify_proposal(
               &registry, 7000u, 6199u, LOCAL_ID, PEER_A_ID, &delayed_a,
               peer_a_payload, peer_a_payload_len, false) ==
           MESH_EVENT_OWNER_STALE);
    assert(owners[0].peer_id == PEER_B_ID);
    assert(owners[0].session_id == UINT32_C(0xb0000001));
    assert(owners[0].active);
}

static void test_live_tombstone_blocks_admission_instead_of_eviction(void)
{
    struct mesh_event_owner owners[1] = {0};
    struct mesh_event_origin_tombstone tombstones[1] = {0};
    const struct mesh_event_owner_registry registry = {
        .owners = owners,
        .owner_capacity = 1u,
        .tombstones = tombstones,
        .tombstone_capacity = 1u,
    };

    assert(mesh_event_owner_registry_begin(
               &registry, PEER_A_ID, UINT32_C(0xa1000001), 1u, true,
               PEER_A_BOOT_NONCE, 100u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    mesh_event_owner_registry_abandon(&registry, PEER_A_ID);
    assert(mesh_event_owner_registry_begin(
               &registry, PEER_B_ID, UINT32_C(0xb1000001), 1u, true,
               PEER_B_BOOT_NONCE, 200u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    mesh_event_owner_registry_abandon(&registry, PEER_B_ID);

    assert(!mesh_event_owner_registry_can_begin(&registry, PEER_C_ID, 300u));
    assert(mesh_event_owner_registry_begin(
               &registry, PEER_C_ID, UINT32_C(0xc1000001), 1u, true,
               PEER_C_BOOT_NONCE, 300u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_ERR_NO_SPACE);
    assert(owners[0].peer_id == PEER_B_ID);
    assert(tombstones[0].peer_id == PEER_A_ID);
    assert(mesh_event_owner_registry_origin_retained(
        &registry, PEER_A_ID, 300u));

    /* Once retry plus queue lifetime has elapsed, the bounded slot is reusable. */
    assert(mesh_event_owner_registry_can_begin(&registry, PEER_C_ID, 12200u));
    assert(mesh_event_owner_registry_begin(
               &registry, PEER_C_ID, UINT32_C(0xc1000001), 1u, true,
               PEER_C_BOOT_NONCE, 12200u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    assert(owners[0].peer_id == PEER_C_ID);
    assert(tombstones[0].peer_id == PEER_B_ID);
}

static void test_registry_deadlines_are_wrap_safe(void)
{
    struct mesh_event_owner owners[1] = {0};
    struct mesh_event_origin_tombstone tombstones[1] = {0};
    const struct mesh_event_owner_registry registry = {
        .owners = owners,
        .owner_capacity = 1u,
        .tombstones = tombstones,
        .tombstone_capacity = 1u,
    };
    uint32_t rebind_at_ms = UINT32_MAX - 100u;

    assert(mesh_event_owner_registry_begin(
               &registry, PEER_A_ID, UINT32_C(0xa2000001), 1u, true,
               PEER_A_BOOT_NONCE, rebind_at_ms - 1u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    mesh_event_owner_registry_abandon(&registry, PEER_A_ID);
    assert(mesh_event_owner_registry_begin(
               &registry, PEER_B_ID, UINT32_C(0xb2000001), 1u, true,
               PEER_B_BOOT_NONCE, rebind_at_ms, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    assert(mesh_event_owner_registry_origin_retained(
        &registry, PEER_A_ID, 1000u));
    assert(!mesh_event_owner_registry_origin_retained(
        &registry, PEER_A_ID, 11899u));
}

static void test_supervision_retirement_is_the_fresh_incarnation_boundary(void)
{
    struct mesh_event_owner owners[1] = {0};
    struct mesh_event_origin_tombstone tombstones[1] = {0};
    const struct mesh_event_owner_registry registry = {
        .owners = owners,
        .owner_capacity = 1u,
        .tombstones = tombstones,
        .tombstone_capacity = 1u,
    };
    uint8_t stale_payload[PROTO_TLV_U64_ENCODED_LEN];
    uint8_t fresh_payload[PROTO_TLV_U64_ENCODED_LEN];
    size_t stale_payload_len = proposal_payload(
        stale_payload, sizeof(stale_payload), PEER_A_BOOT_NONCE);
    size_t fresh_payload_len = proposal_payload(
        fresh_payload, sizeof(fresh_payload), PEER_B_BOOT_NONCE);
    struct proto_packet stale = proposal(
        PEER_A_ID, UINT32_C(0xa3000001), 1u, stale_payload_len);
    struct proto_packet fresh = proposal(
        PEER_A_ID, UINT32_C(0xa3000002), 1u, fresh_payload_len);

    /* Model the locally proposed owner left behind by Channel-9 timing. */
    assert(mesh_event_owner_registry_begin(
               &registry, PEER_A_ID, stale.session_id, stale.seq, false, 0u,
               100u, REPLAY_LIFETIME_MS, QUEUE_LIFETIME_MS) == PROTO_OK);

    /* A still-usable timing owns the peer and a new incarnation cannot replace it. */
    assert(mesh_event_owner_registry_classify_proposal(
               &registry, 200u, 200u, LOCAL_ID, PEER_A_ID, &fresh,
               fresh_payload, fresh_payload_len, false) ==
           MESH_EVENT_OWNER_STALE);
    assert(owners[0].active);
    assert(owners[0].session_id == stale.session_id);
    assert(owners[0].remote_boot_nonce == 0u);

    /* Supervision expiry retires the owner before receive classification. */
    mesh_event_owner_registry_abandon(&registry, PEER_A_ID);
    assert(mesh_event_owner_registry_classify_proposal(
               &registry, 700u, 699u, LOCAL_ID, PEER_A_ID, &fresh,
               fresh_payload, fresh_payload_len, false) ==
           MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_registry_begin(
               &registry, PEER_A_ID, fresh.session_id, fresh.seq, true,
               PEER_B_BOOT_NONCE, 700u, REPLAY_LIFETIME_MS,
               QUEUE_LIFETIME_MS) == PROTO_OK);
    assert(owners[0].active);
    assert(owners[0].session_id == fresh.session_id);
    assert(owners[0].remote_boot_nonce == PEER_B_BOOT_NONCE);

    /* Work queued before retirement cannot revive the superseded proposal. */
    assert(mesh_event_owner_registry_classify_proposal(
               &registry, 701u, 199u, LOCAL_ID, PEER_A_ID, &stale,
               stale_payload, stale_payload_len, false) ==
           MESH_EVENT_OWNER_STALE);
    assert(owners[0].session_id == fresh.session_id);
    assert(owners[0].remote_boot_nonce == PEER_B_BOOT_NONCE);
}

int main(void)
{
    test_slot_reuse_retains_origin_through_queue_horizon();
    test_live_tombstone_blocks_admission_instead_of_eviction();
    test_registry_deadlines_are_wrap_safe();
    test_supervision_retirement_is_the_fresh_incarnation_boundary();
    return 0;
}
