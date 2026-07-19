#include "mesh_event_owner.h"
#include "mesh.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define LOCAL_ID UINT64_C(0x1000000000000001)
#define PEER_ID UINT64_C(0x2000000000000002)

static struct proto_packet control_packet(uint8_t msg_type,
                                          uint32_t session_id,
                                          uint16_t sequence,
                                          size_t payload_len)
{
    struct proto_packet packet = {0};

    assert(payload_len <= UINT8_MAX);
    assert(mesh_init_event_control(&packet, msg_type, PEER_ID, LOCAL_ID,
                                   session_id, sequence,
                                   (uint8_t)payload_len) == PROTO_OK);
    return packet;
}

static struct proto_packet local_control_packet(uint8_t msg_type,
                                                uint32_t session_id,
                                                uint16_t sequence,
                                                size_t payload_len)
{
    struct proto_packet packet = {0};

    assert(payload_len <= UINT8_MAX);
    assert(mesh_init_event_control(&packet, msg_type, LOCAL_ID, PEER_ID,
                                   session_id, sequence,
                                   (uint8_t)payload_len) == PROTO_OK);
    return packet;
}

static struct proto_packet proposal_packet(uint32_t session_id,
                                           uint16_t sequence,
                                           size_t payload_len)
{
    return control_packet(MSG_MESH_EVENT_PROPOSE, session_id, sequence,
                          payload_len);
}

static void test_update_end_ownership(void)
{
    static const uint8_t update_payload[] = {1u, 2u, 3u, 4u};
    struct mesh_event_owner owner = {0};
    struct proto_packet update = control_packet(MSG_MESH_EVENT_UPDATE,
                                                UINT32_C(0x41000001), 11u,
                                                sizeof(update_payload));
    struct proto_packet conflicting = update;
    struct proto_packet end = control_packet(MSG_MESH_EVENT_END,
                                             UINT32_C(0x41000001), 12u, 0u);
    uint8_t changed_payload[sizeof(update_payload)];
    uint32_t first_generation;

    assert(mesh_event_owner_begin(&owner, PEER_ID, UINT32_C(0x41000001),
                                  60000u, true) == PROTO_OK);
    first_generation = owner.generation;
    assert(mesh_event_owner_generation_matches(&owner, first_generation));
    /* Peer controls have their own sequence domain, independent of PROPOSE. */
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &update,
                                     update_payload,
                                     sizeof(update_payload)) ==
           MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &update,
                                   update_payload,
                                   sizeof(update_payload)) == PROTO_OK);
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &update,
                                     update_payload,
                                     sizeof(update_payload)) ==
           MESH_EVENT_OWNER_DUPLICATE);

    memcpy(changed_payload, update_payload, sizeof(changed_payload));
    changed_payload[0] ^= UINT8_C(0xff);
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &conflicting,
                                     changed_payload,
                                     sizeof(changed_payload)) ==
           MESH_EVENT_OWNER_CONFLICT);

    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &end,
                                     NULL, 0u) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &end, NULL,
                                   0u) == PROTO_OK);
    assert(!owner.active);
    assert(owner.terminal);
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &end, NULL,
                                     0u) == MESH_EVENT_OWNER_DUPLICATE);
}

static void test_stale_operation_cannot_mutate_new_owner(void)
{
    uint8_t update_payload[] = {9u, 8u, 7u};
    struct mesh_event_owner owner = {0};
    struct proto_packet stale_update = control_packet(MSG_MESH_EVENT_UPDATE,
        UINT32_C(0x42000001), 21u, sizeof(update_payload));
    struct proto_packet stale_end = control_packet(MSG_MESH_EVENT_END,
        UINT32_C(0x42000001), 22u, 0u);
    struct proto_packet current_end = control_packet(MSG_MESH_EVENT_END,
        UINT32_C(0x42000002), 31u, 0u);
    uint32_t old_generation;

    assert(mesh_event_owner_begin(&owner, PEER_ID, UINT32_C(0x42000001),
                                  20u, true) == PROTO_OK);
    old_generation = owner.generation;
    assert(mesh_event_owner_begin(&owner, PEER_ID, UINT32_C(0x42000002),
                                  30u, true) == PROTO_OK);
    assert(owner.generation != old_generation);
    assert(!mesh_event_owner_generation_matches(&owner, old_generation));
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &stale_update,
                                     update_payload,
                                     sizeof(update_payload)) ==
           MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &stale_end,
                                     NULL, 0u) == MESH_EVENT_OWNER_STALE);
    assert(owner.active);
    assert(owner.session_id == UINT32_C(0x42000002));

    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &current_end,
                                   NULL, 0u) == PROTO_OK);
    assert(owner.terminal);
}

static void test_abandoned_owner_rejects_delayed_control(void)
{
    struct mesh_event_owner owner = {0};
    struct proto_packet end = control_packet(MSG_MESH_EVENT_END,
                                             UINT32_C(0x43000001), 41u, 0u);

    assert(mesh_event_owner_begin(&owner, PEER_ID, UINT32_C(0x43000001),
                                  40u, true) == PROTO_OK);
    mesh_event_owner_abandon(&owner);
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &end, NULL,
                                     0u) == MESH_EVENT_OWNER_STALE);
}

static void test_local_and_remote_sequences_are_independent(void)
{
    static const uint8_t local_payload[] = {0x11u, 0x22u};
    static const uint8_t remote_payload[] = {0x33u, 0x44u};
    struct mesh_event_owner owner = {0};
    struct proto_packet local_update = local_control_packet(
        MSG_MESH_EVENT_UPDATE, UINT32_C(0x44000001), UINT16_C(50000),
        sizeof(local_payload));
    struct proto_packet remote_update = control_packet(
        MSG_MESH_EVENT_UPDATE, UINT32_C(0x44000001), UINT16_C(7),
        sizeof(remote_payload));

    assert(mesh_event_owner_begin(&owner, PEER_ID, UINT32_C(0x44000001),
                                  UINT16_C(31000), true) == PROTO_OK);
    assert(mesh_event_owner_commit_local(&owner, LOCAL_ID, &local_update,
                                         local_payload,
                                         sizeof(local_payload)) == PROTO_OK);
    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &remote_update,
                                   remote_payload,
                                   sizeof(remote_payload)) == PROTO_OK);
    assert(owner.local_sequence == UINT16_C(50000));
    assert(owner.remote_sequence == UINT16_C(7));
    assert(owner.active);
}

static void test_delayed_proposal_cannot_replace_newer_operation(void)
{
    static const uint8_t old_payload[] = {0x10u, 0x20u};
    static const uint8_t current_payload[] = {0x30u, 0x40u};
    static const uint8_t next_payload[] = {0x50u, 0x60u};
    struct mesh_event_owner owner = {0};
    struct proto_packet old_proposal = proposal_packet(
        UINT32_C(0x45000001), 100u, sizeof(old_payload));
    struct proto_packet current_proposal = proposal_packet(
        UINT32_C(0x45000002), 101u, sizeof(current_payload));
    struct proto_packet next_proposal = proposal_packet(
        UINT32_C(0x45000003), 102u, sizeof(next_payload));
    struct proto_packet current_end = control_packet(
        MSG_MESH_EVENT_END, UINT32_C(0x45000002), 1u, 0u);

    assert(mesh_event_owner_classify_proposal(
               NULL, LOCAL_ID, PEER_ID, &old_proposal, old_payload,
               sizeof(old_payload)) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin(&owner, PEER_ID, old_proposal.session_id,
                                  old_proposal.seq, true) == PROTO_OK);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &current_proposal,
               current_payload, sizeof(current_payload)) ==
           MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin(&owner, PEER_ID,
                                  current_proposal.session_id,
                                  current_proposal.seq, true) == PROTO_OK);

    /* Cache clearing after END cannot make operation N fresh again. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &old_proposal, old_payload,
               sizeof(old_payload)) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_begin(&owner, PEER_ID, old_proposal.session_id,
                                  old_proposal.seq, true) == PROTO_ERR_STALE);
    assert(owner.session_id == current_proposal.session_id);
    assert(owner.proposal_sequence == current_proposal.seq);
    assert(owner.active);

    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &current_end,
                                   NULL, 0u) == PROTO_OK);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &old_proposal, old_payload,
               sizeof(old_payload)) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &next_proposal, next_payload,
               sizeof(next_payload)) == MESH_EVENT_OWNER_APPLY);
}

static void test_proposal_history_is_bounded_and_rejects_recent_sessions(void)
{
    static const uint8_t payload[] = {0xa5u};
    struct mesh_event_owner owner = {0};
    struct proto_packet proposal;
    uint32_t first_session = UINT32_C(0x46000001);

    assert(mesh_event_owner_begin(&owner, PEER_ID, first_session, 1u, true) ==
           PROTO_OK);
    mesh_event_owner_abandon(&owner);
    for (uint16_t i = 1u;
         i <= MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY;
         i++) {
        uint32_t session = first_session + i;

        assert(mesh_event_owner_begin(&owner, PEER_ID, session,
                                      (uint16_t)(i + 1u), true) == PROTO_OK);
        mesh_event_owner_abandon(&owner);
    }
    assert(owner.retired_session_count ==
           MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY);

    proposal = proposal_packet(first_session + 1u, 2u, sizeof(payload));
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &proposal, payload,
               sizeof(payload)) == MESH_EVENT_OWNER_STALE);
}

static void test_cross_direction_replacement_keeps_remote_history(void)
{
    static const uint8_t remote_n_payload[] = {0x01u, 0x02u};
    static const uint8_t changed_n_payload[] = {0x01u, 0xfdu};
    static const uint8_t remote_n2_payload[] = {0x03u, 0x04u};
    struct mesh_event_owner owner = {0};
    struct proto_packet remote_n = proposal_packet(
        UINT32_C(0x47000001), 400u, sizeof(remote_n_payload));
    struct proto_packet remote_n2 = proposal_packet(
        UINT32_C(0x47000003), 401u, sizeof(remote_n2_payload));

    assert(mesh_event_owner_begin(&owner, PEER_ID, remote_n.session_id,
                                  remote_n.seq, true) == PROTO_OK);
    /* Same wire identity with altered payload must remain inert even after the
     * response cache is unavailable. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &remote_n, changed_n_payload,
               sizeof(changed_n_payload)) != MESH_EVENT_OWNER_APPLY);

    /* The local proposal sequence is a different domain and may be lower. */
    assert(mesh_event_owner_begin(&owner, PEER_ID,
                                  UINT32_C(0x47000002), 3u, false) ==
           PROTO_OK);
    assert(owner.active && !owner.proposal_from_peer);
    assert(owner.remote_proposal_seen);
    assert(owner.remote_proposal_sequence == remote_n.seq);

    /* Delayed remote N remains retired, while the peer's true next sequence
     * can replace the locally initiated operation. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &remote_n, remote_n_payload,
               sizeof(remote_n_payload)) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &remote_n2, remote_n2_payload,
               sizeof(remote_n2_payload)) == MESH_EVENT_OWNER_APPLY);
}

int main(void)
{
    test_update_end_ownership();
    test_stale_operation_cannot_mutate_new_owner();
    test_abandoned_owner_rejects_delayed_control();
    test_local_and_remote_sequences_are_independent();
    test_delayed_proposal_cannot_replace_newer_operation();
    test_proposal_history_is_bounded_and_rejects_recent_sessions();
    test_cross_direction_replacement_keeps_remote_history();
    return 0;
}
