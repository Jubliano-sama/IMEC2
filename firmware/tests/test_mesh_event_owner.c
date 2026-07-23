#include "mesh_event_owner.h"
#include "mesh.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define LOCAL_ID UINT64_C(0x1000000000000001)
#define PEER_ID UINT64_C(0x2000000000000002)
#define TEST_PROPOSAL_BOOT_NONCE UINT64_C(0x1020304050607080)
#define TEST_PROPOSAL_PAYLOAD_LEN \
    (PROTO_TLV_U8_ENCODED_LEN + PROTO_TLV_U64_ENCODED_LEN)

static size_t build_proposal_payload(uint8_t *payload, size_t payload_cap,
                                     uint8_t marker, uint64_t boot_nonce)
{
    size_t payload_len = 0u;

    assert(tlv_append_u8(payload, payload_cap, &payload_len,
                         TLV_MESH_EVENT_COUNTER, marker) == PROTO_OK);
    assert(tlv_append_u64(payload, payload_cap, &payload_len,
                          TLV_MESH_EVENT_BOOT_NONCE, boot_nonce) == PROTO_OK);
    return payload_len;
}

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
    uint8_t old_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    uint8_t current_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    uint8_t next_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    size_t old_payload_len;
    size_t current_payload_len;
    size_t next_payload_len;
    struct mesh_event_owner owner = {0};
    struct proto_packet old_proposal;
    struct proto_packet current_proposal;
    struct proto_packet next_proposal;
    struct proto_packet current_end;

    old_payload_len = build_proposal_payload(
        old_payload, sizeof(old_payload), 0x10u, TEST_PROPOSAL_BOOT_NONCE);
    current_payload_len = build_proposal_payload(
        current_payload, sizeof(current_payload), 0x30u,
        TEST_PROPOSAL_BOOT_NONCE);
    next_payload_len = build_proposal_payload(
        next_payload, sizeof(next_payload), 0x50u, TEST_PROPOSAL_BOOT_NONCE);
    old_proposal = proposal_packet(UINT32_C(0x45000001), 100u,
                                   old_payload_len);
    current_proposal = proposal_packet(UINT32_C(0x45000002), 101u,
                                       current_payload_len);
    next_proposal = proposal_packet(UINT32_C(0x45000003), 102u,
                                    next_payload_len);
    current_end = control_packet(MSG_MESH_EVENT_END, UINT32_C(0x45000002),
                                  1u, 0u);

    assert(mesh_event_owner_classify_proposal(
               NULL, LOCAL_ID, PEER_ID, &old_proposal, old_payload,
               old_payload_len) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, old_proposal.session_id, old_proposal.seq,
               true, TEST_PROPOSAL_BOOT_NONCE) == PROTO_OK);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &current_proposal,
               current_payload, current_payload_len) ==
           MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, current_proposal.session_id,
               current_proposal.seq, true, TEST_PROPOSAL_BOOT_NONCE) ==
           PROTO_OK);

    /* Cache clearing after END cannot make operation N fresh again. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &old_proposal, old_payload,
               old_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, old_proposal.session_id, old_proposal.seq,
               true, TEST_PROPOSAL_BOOT_NONCE) == PROTO_ERR_STALE);
    assert(owner.session_id == current_proposal.session_id);
    assert(owner.proposal_sequence == current_proposal.seq);
    assert(owner.active);

    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &current_end,
                                   NULL, 0u) == PROTO_OK);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &old_proposal, old_payload,
               old_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &next_proposal, next_payload,
               next_payload_len) == MESH_EVENT_OWNER_APPLY);
}

static void test_proposal_history_is_bounded_and_rejects_recent_sessions(void)
{
    uint8_t payload[TEST_PROPOSAL_PAYLOAD_LEN];
    size_t payload_len;
    struct mesh_event_owner owner = {0};
    struct proto_packet proposal;
    uint32_t first_session = UINT32_C(0x46000001);

    payload_len = build_proposal_payload(payload, sizeof(payload), 0xa5u,
                                         TEST_PROPOSAL_BOOT_NONCE);

    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, first_session, 1u, true,
               TEST_PROPOSAL_BOOT_NONCE) == PROTO_OK);
    mesh_event_owner_abandon(&owner);
    for (uint16_t i = 1u;
         i <= MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY;
         i++) {
        uint32_t session = first_session + i;

        assert(mesh_event_owner_begin_with_boot_nonce(
                   &owner, PEER_ID, session, (uint16_t)(i + 1u), true,
                   TEST_PROPOSAL_BOOT_NONCE) == PROTO_OK);
        mesh_event_owner_abandon(&owner);
    }
    assert(owner.retired_session_count ==
           MESH_EVENT_OWNER_RETIRED_SESSION_CAPACITY);

    proposal = proposal_packet(first_session + 1u, 2u, payload_len);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &proposal, payload,
               payload_len) == MESH_EVENT_OWNER_STALE);
}

static void test_cross_direction_replacement_keeps_remote_history(void)
{
    uint8_t remote_n_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    uint8_t changed_n_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    uint8_t remote_n2_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    size_t remote_n_payload_len;
    size_t changed_n_payload_len;
    size_t remote_n2_payload_len;
    struct mesh_event_owner owner = {0};
    struct proto_packet remote_n;
    struct proto_packet remote_n2;

    remote_n_payload_len = build_proposal_payload(
        remote_n_payload, sizeof(remote_n_payload), 0x01u,
        TEST_PROPOSAL_BOOT_NONCE);
    changed_n_payload_len = build_proposal_payload(
        changed_n_payload, sizeof(changed_n_payload), 0xfdu,
        TEST_PROPOSAL_BOOT_NONCE);
    remote_n2_payload_len = build_proposal_payload(
        remote_n2_payload, sizeof(remote_n2_payload), 0x03u,
        TEST_PROPOSAL_BOOT_NONCE);
    remote_n = proposal_packet(UINT32_C(0x47000001), 400u,
                               remote_n_payload_len);
    remote_n2 = proposal_packet(UINT32_C(0x47000003), 401u,
                                remote_n2_payload_len);

    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, remote_n.session_id, remote_n.seq, true,
               TEST_PROPOSAL_BOOT_NONCE) == PROTO_OK);
    /* Same wire identity with altered payload must remain inert even after the
     * response cache is unavailable. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &remote_n, changed_n_payload,
               changed_n_payload_len) != MESH_EVENT_OWNER_APPLY);

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
               remote_n_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &remote_n2, remote_n2_payload,
               remote_n2_payload_len) == MESH_EVENT_OWNER_APPLY);
}

static void test_proposal_nonce_required_without_owner_mutation(void)
{
    const uint32_t session_id = UINT32_C(0x47500001);
    const uint64_t boot_nonce = UINT64_C(0xabcdef0123456789);
    uint8_t valid_payload[TEST_PROPOSAL_PAYLOAD_LEN];
    uint8_t zero_payload[PROTO_TLV_U64_ENCODED_LEN] = {
        TLV_MESH_EVENT_BOOT_NONCE,
        sizeof(uint64_t),
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    };
    size_t valid_payload_len;
    struct proto_packet valid_proposal;
    struct proto_packet missing_proposal;
    struct proto_packet zero_proposal;
    struct proto_packet update;
    struct mesh_event_owner owner = {0};
    uint32_t generation;

    valid_payload_len = build_proposal_payload(
        valid_payload, sizeof(valid_payload), 0x42u, boot_nonce);
    valid_proposal = proposal_packet(session_id, 7u, valid_payload_len);
    missing_proposal = proposal_packet(session_id + 1u, 1u, 0u);
    zero_proposal = proposal_packet(session_id + 2u, 1u,
                                    sizeof(zero_payload));

    assert(mesh_event_owner_classify_proposal(
               NULL, LOCAL_ID, PEER_ID, &valid_proposal, valid_payload,
               valid_payload_len) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, valid_proposal.session_id,
               valid_proposal.seq, true, boot_nonce) == PROTO_OK);
    generation = owner.generation;

    /* Missing and zero nonces are rejected while the current owner remains
     * untouched, so neither frame can replace a live timing operation. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &missing_proposal, NULL,
               0u) == MESH_EVENT_OWNER_INVALID);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &zero_proposal, zero_payload,
               sizeof(zero_payload)) == MESH_EVENT_OWNER_INVALID);
    assert(owner.generation == generation);
    assert(owner.session_id == valid_proposal.session_id);
    assert(owner.remote_boot_nonce == boot_nonce);
    assert(owner.active);

    /* UPDATE has its own payload/sequence domain and remains unaffected by
     * the PROPOSE-only incarnation requirement. */
    update = control_packet(MSG_MESH_EVENT_UPDATE, session_id, 8u, 0u);
    assert(mesh_event_owner_classify(&owner, LOCAL_ID, PEER_ID, &update, NULL,
                                     0u) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_commit(&owner, LOCAL_ID, PEER_ID, &update, NULL,
                                   0u) == PROTO_OK);
}

static void test_boot_nonce_restarts_sequence_and_rejects_old_replay(void)
{
    const uint64_t first_boot_nonce = UINT64_C(0x1122334455667788);
    const uint64_t second_boot_nonce = UINT64_C(0x8877665544332211);
    const uint64_t third_boot_nonce = UINT64_C(0x0123456789abcdef);
    const uint32_t shared_session = UINT32_C(0x48000001);
    const uint32_t lower_sequence_session = UINT32_C(0x48000002);
    uint8_t first_payload[PROTO_TLV_U64_ENCODED_LEN];
    uint8_t second_payload[PROTO_TLV_U64_ENCODED_LEN];
    uint8_t third_payload[PROTO_TLV_U64_ENCODED_LEN];
    size_t first_payload_len = 0u;
    size_t second_payload_len = 0u;
    size_t third_payload_len = 0u;
    struct mesh_event_owner owner = {0};
    struct proto_packet first_proposal;
    struct proto_packet second_proposal;
    struct proto_packet delayed_first_same_session;
    struct proto_packet delayed_first_different_session;
    struct proto_packet second_lower_sequence;
    struct proto_packet third_proposal;
    struct proto_packet delayed_second_same_session;
    struct proto_packet delayed_second_different_session;

    assert(tlv_append_u64(first_payload, sizeof(first_payload),
                          &first_payload_len, TLV_MESH_EVENT_BOOT_NONCE,
                          first_boot_nonce) == PROTO_OK);
    assert(tlv_append_u64(second_payload, sizeof(second_payload),
                          &second_payload_len, TLV_MESH_EVENT_BOOT_NONCE,
                          second_boot_nonce) == PROTO_OK);
    assert(tlv_append_u64(third_payload, sizeof(third_payload),
                          &third_payload_len, TLV_MESH_EVENT_BOOT_NONCE,
                          third_boot_nonce) == PROTO_OK);
    first_proposal = proposal_packet(shared_session, 17u, first_payload_len);
    second_proposal = proposal_packet(shared_session, 17u,
                                      second_payload_len);
    delayed_first_same_session = proposal_packet(shared_session, 17u,
                                                 first_payload_len);
    delayed_first_different_session = proposal_packet(
        UINT32_C(0x48000099), 1u, first_payload_len);
    second_lower_sequence = proposal_packet(lower_sequence_session, 16u,
                                            second_payload_len);
    third_proposal = proposal_packet(shared_session, 17u, third_payload_len);
    delayed_second_same_session = proposal_packet(shared_session, 17u,
                                                  second_payload_len);
    delayed_second_different_session = proposal_packet(
        UINT32_C(0x4800009a), 1u, second_payload_len);

    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, first_proposal.session_id,
               first_proposal.seq, true, first_boot_nonce) == PROTO_OK);
    /* A reset incarnation is a new identity even when it reuses the exact
     * session and sequence from the current owner. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &second_proposal, second_payload,
               second_payload_len) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, second_proposal.session_id,
               second_proposal.seq, true, second_boot_nonce) == PROTO_OK);
    assert(owner.remote_boot_nonce == second_boot_nonce);

    /* Retired incarnations are stale before session identity or sequence
     * checks, whether the delayed frame reuses or changes its session. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &delayed_first_same_session,
               first_payload, first_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, delayed_first_same_session.session_id,
               delayed_first_same_session.seq, true,
               first_boot_nonce) == PROTO_ERR_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &delayed_first_different_session,
               first_payload,
               first_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, delayed_first_different_session.session_id,
               delayed_first_different_session.seq, true,
               first_boot_nonce) == PROTO_ERR_STALE);
    /* The active incarnation keeps the normal sequence ordering rules. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &second_lower_sequence,
               second_payload,
               second_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, second_lower_sequence.session_id,
               second_lower_sequence.seq, true,
               second_boot_nonce) == PROTO_ERR_STALE);

    /* A third incarnation can replace the second one with the same wire
     * session/sequence, and both earlier incarnations remain retired. */
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &third_proposal, third_payload,
               third_payload_len) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, third_proposal.session_id,
               third_proposal.seq, true, third_boot_nonce) == PROTO_OK);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &delayed_first_same_session,
               first_payload, first_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &delayed_second_same_session,
               second_payload, second_payload_len) == MESH_EVENT_OWNER_STALE);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, delayed_second_same_session.session_id,
               delayed_second_same_session.seq, true,
               second_boot_nonce) == PROTO_ERR_STALE);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &delayed_second_different_session,
               second_payload,
               second_payload_len) == MESH_EVENT_OWNER_STALE);
}

static void test_local_owner_between_remote_incarnations(void)
{
    const uint64_t first_boot_nonce = UINT64_C(0xa1a2a3a4a5a6a7a8);
    const uint64_t second_boot_nonce = UINT64_C(0xb1b2b3b4b5b6b7b8);
    const uint32_t session = UINT32_C(0x49000001);
    const uint32_t local_session = UINT32_C(0x49000002);
    uint8_t first_payload[PROTO_TLV_U64_ENCODED_LEN];
    uint8_t second_payload[PROTO_TLV_U64_ENCODED_LEN];
    size_t first_payload_len = 0u;
    size_t second_payload_len = 0u;
    struct mesh_event_owner owner = {0};
    struct proto_packet first_proposal;
    struct proto_packet local_proposal;
    struct proto_packet second_proposal;

    assert(tlv_append_u64(first_payload, sizeof(first_payload),
                          &first_payload_len, TLV_MESH_EVENT_BOOT_NONCE,
                          first_boot_nonce) == PROTO_OK);
    assert(tlv_append_u64(second_payload, sizeof(second_payload),
                          &second_payload_len, TLV_MESH_EVENT_BOOT_NONCE,
                          second_boot_nonce) == PROTO_OK);
    first_proposal = proposal_packet(session, 21u, first_payload_len);
    local_proposal = first_proposal;
    local_proposal.session_id = local_session;
    local_proposal.seq = 3u;
    second_proposal = proposal_packet(session, 1u, second_payload_len);

    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, first_proposal.session_id,
               first_proposal.seq, true, first_boot_nonce) == PROTO_OK);
    assert(mesh_event_owner_begin(&owner, PEER_ID, local_proposal.session_id,
                                  local_proposal.seq, false) == PROTO_OK);
    assert(!owner.proposal_from_peer);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &second_proposal, second_payload,
               second_payload_len) == MESH_EVENT_OWNER_APPLY);
    assert(mesh_event_owner_begin_with_boot_nonce(
               &owner, PEER_ID, second_proposal.session_id,
               second_proposal.seq, true, second_boot_nonce) == PROTO_OK);
    assert(owner.proposal_from_peer);
    assert(owner.remote_boot_nonce == second_boot_nonce);
    assert(mesh_event_owner_classify_proposal(
               &owner, LOCAL_ID, PEER_ID, &first_proposal, first_payload,
               first_payload_len) == MESH_EVENT_OWNER_STALE);
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
    test_proposal_nonce_required_without_owner_mutation();
    test_boot_nonce_restarts_sequence_and_rejects_old_replay();
    test_local_owner_between_remote_incarnations();
    return 0;
}
