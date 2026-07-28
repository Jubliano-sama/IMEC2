#include "app_mesh_route_reply_ack.h"
#include "app_mesh_route_reply_match.h"
#include "mesh_relay.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#define LOCAL_ID UINT64_C(0x1001)
#define NEXT_HOP_ID UINT64_C(0x1002)

static size_t route_reply_identity_payload(uint8_t *payload,
                                           size_t payload_cap)
{
    size_t payload_len = 0u;

    assert(tlv_append_u64(payload, payload_cap, &payload_len,
                          TLV_INITIATOR_ID, UINT64_C(0x2001)) == PROTO_OK);
    assert(tlv_append_u64(payload, payload_cap, &payload_len,
                          TLV_RESPONDER_ID, UINT64_C(0x2002)) == PROTO_OK);
    assert(tlv_append_u32(payload, payload_cap, &payload_len,
                          TLV_FLOOD_EPOCH_ID, UINT32_C(0x3001)) == PROTO_OK);
    assert(tlv_append_u32(payload, payload_cap, &payload_len,
                          TLV_REPLY_NONCE, UINT32_C(0x3002)) == PROTO_OK);
    assert(tlv_append_u16(payload, payload_cap, &payload_len,
                          TLV_METRIC_CRC, UINT16_C(0x4001)) == PROTO_OK);
    return payload_len;
}

static void test_route_reply_ack_requires_exact_peer_and_identity(void)
{
    struct mesh_outbound route_reply = {
        .packet = {
            .session_id = 17u,
        },
        .next_hop_id = NEXT_HOP_ID,
    };
    struct proto_packet ack = {
        .msg_type = MSG_ROUTE_REPLY_ACK,
        .src_id = NEXT_HOP_ID,
        .dst_id = LOCAL_ID,
        .session_id = 17u,
    };
    uint8_t ack_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t ack_payload_len;

    route_reply.payload_len = (uint16_t)route_reply_identity_payload(
        route_reply.payload, sizeof(route_reply.payload));
    ack_payload_len = route_reply_identity_payload(
        ack_payload, sizeof(ack_payload));
    assert(app_mesh_route_reply_ack_matches(
        &route_reply, &ack, ack_payload, ack_payload_len,
        NEXT_HOP_ID, LOCAL_ID));

    ack.src_id++;
    assert(!app_mesh_route_reply_ack_matches(
        &route_reply, &ack, ack_payload, ack_payload_len,
        NEXT_HOP_ID, LOCAL_ID));
    ack.src_id = NEXT_HOP_ID;
    ack.session_id++;
    assert(!app_mesh_route_reply_ack_matches(
        &route_reply, &ack, ack_payload, ack_payload_len,
        NEXT_HOP_ID, LOCAL_ID));
}

static void test_route_reply_ack_rejects_missing_or_changed_identity_tlv(void)
{
    struct mesh_outbound route_reply = {
        .packet = {
            .session_id = 18u,
        },
        .next_hop_id = NEXT_HOP_ID,
    };
    const struct proto_packet ack = {
        .msg_type = MSG_ROUTE_REPLY_ACK,
        .src_id = NEXT_HOP_ID,
        .dst_id = LOCAL_ID,
        .session_id = 18u,
    };
    uint8_t ack_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t ack_payload_len;

    route_reply.payload_len = (uint16_t)route_reply_identity_payload(
        route_reply.payload, sizeof(route_reply.payload));
    ack_payload_len = route_reply_identity_payload(
        ack_payload, sizeof(ack_payload));
    assert(ack_payload_len > 0u);
    assert(!app_mesh_route_reply_ack_matches(
        &route_reply, &ack, ack_payload, ack_payload_len - 1u,
        NEXT_HOP_ID, LOCAL_ID));

    memcpy(ack_payload, route_reply.payload, route_reply.payload_len);
    ack_payload[ack_payload_len - 1u] ^= 0x01u;
    assert(!app_mesh_route_reply_ack_matches(
        &route_reply, &ack, ack_payload, ack_payload_len,
        NEXT_HOP_ID, LOCAL_ID));
}

static void test_successful_listen_completes_attempt(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 0u,
        .max_retries = 4u,
        .send_ret = 0,
        .listen_attempted = true,
        .listen_ret = 0,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_SUCCESS);
    assert(!result.note_retry);
    assert(result.return_ret == 0);
}

static void test_send_failure_retries_until_limit(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 2u,
        .max_retries = 4u,
        .send_ret = -EIO,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY);
    assert(result.note_retry);
    assert(result.return_ret == -EIO);
}

static void test_listen_timeout_retries_until_limit(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 3u,
        .max_retries = 4u,
        .send_ret = 0,
        .listen_attempted = true,
        .listen_ret = -ETIMEDOUT,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY);
    assert(result.note_retry);
    assert(result.return_ret == -ETIMEDOUT);
}

static void test_final_listen_timeout_fails_without_retry_note(void)
{
    const struct app_mesh_route_reply_ack_attempt_state state = {
        .attempt = 4u,
        .max_retries = 4u,
        .send_ret = 0,
        .listen_attempted = true,
        .listen_ret = -ETIMEDOUT,
    };
    struct app_mesh_route_reply_ack_attempt_result result;

    app_mesh_route_reply_ack_decide_attempt(&state, &result);

    assert(result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_FAILED);
    assert(!result.note_retry);
    assert(result.return_ret == -ETIMEDOUT);
}

static void test_primary_failure_uses_valid_backup_hop(void)
{
    const struct app_mesh_route_reply_ack_backup_state state = {
        .primary_ret = -ETIMEDOUT,
        .backup_valid = true,
        .primary_next_hop_id = 0x1002u,
        .backup_next_hop_id = 0x2003u,
    };
    struct app_mesh_route_reply_ack_backup_result result;

    app_mesh_route_reply_ack_decide_backup(&state, &result);

    assert(result.try_backup);
    assert(result.note_retry);
    assert(result.backup_next_hop_id == 0x2003u);
}

static void test_primary_failure_without_distinct_backup_fails(void)
{
    const struct app_mesh_route_reply_ack_backup_state state = {
        .primary_ret = -ETIMEDOUT,
        .backup_valid = true,
        .primary_next_hop_id = 0x1002u,
        .backup_next_hop_id = 0x1002u,
    };
    struct app_mesh_route_reply_ack_backup_result result;

    app_mesh_route_reply_ack_decide_backup(&state, &result);

    assert(!result.try_backup);
    assert(!result.note_retry);
    assert(result.return_ret == -ETIMEDOUT);
    assert(result.clear_reason != 0);
}

static void test_c5_preemption_extends_ack_deadline_by_full_timeout(void)
{
    assert(app_mesh_route_reply_ack_deadline_after_preemption(1000u, 150u, 0u) == 1150u);
    assert(app_mesh_route_reply_ack_deadline_after_preemption(UINT32_MAX, 1u, 0u) == 1u);
    assert(app_mesh_route_reply_ack_deadline_after_preemption(10u, 0u, 0u) == 11u);
}

static void test_c5_preemption_deadline_is_capped_by_attempt_budget(void)
{
    assert(app_mesh_route_reply_ack_deadline_after_preemption(1000u, 150u, 1200u) == 1150u);
    assert(app_mesh_route_reply_ack_deadline_after_preemption(1100u, 150u, 1200u) == 1200u);
    assert(app_mesh_route_reply_ack_deadline_after_preemption(UINT32_MAX, 2u, 1u) == 1u);
}

int main(void)
{
    test_route_reply_ack_requires_exact_peer_and_identity();
    test_route_reply_ack_rejects_missing_or_changed_identity_tlv();
    test_successful_listen_completes_attempt();
    test_send_failure_retries_until_limit();
    test_listen_timeout_retries_until_limit();
    test_final_listen_timeout_fails_without_retry_note();
    test_primary_failure_uses_valid_backup_hop();
    test_primary_failure_without_distinct_backup_fails();
    test_c5_preemption_extends_ack_deadline_by_full_timeout();
    test_c5_preemption_deadline_is_capped_by_attempt_budget();
    return 0;
}
