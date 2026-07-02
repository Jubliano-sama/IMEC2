#include "app_mesh_ch9_ack.h"
#include "mesh.h"
#include "protocol.h"

#include <zephyr/ztest.h>

static struct proto_packet gateway_ack_packet(void)
{
    return (struct proto_packet) {
        .msg_type = MSG_GATEWAY_ACK,
        .flags = FLAG_GATEWAY_ACK,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x1111222233334444ull,
        .session_id = 7001u,
        .seq = 91u,
        .ttl = MESH_DEFAULT_TTL,
    };
}

static void append_ack_lists(uint8_t *payload,
                             size_t payload_cap,
                             size_t *payload_len,
                             const uint32_t *sessions,
                             const uint16_t *seqs,
                             uint8_t count)
{
    uint8_t session_list[16];
    uint8_t seq_list[8];

    zassert_true(count <= 4u);
    for (uint8_t i = 0u; i < count; i++) {
        proto_put_u32_le(&session_list[i * sizeof(uint32_t)], sessions[i]);
        proto_put_u16_le(&seq_list[i * sizeof(uint16_t)], seqs[i]);
    }
    zassert_ok(tlv_append_bytes(payload,
                                payload_cap,
                                payload_len,
                                TLV_MESH_ACK_SESSION_LIST,
                                session_list,
                                (uint8_t)(count * sizeof(uint32_t))));
    zassert_ok(tlv_append_bytes(payload,
                                payload_cap,
                                payload_len,
                                TLV_MESH_ACK_SEQ_LIST,
                                seq_list,
                                (uint8_t)(count * sizeof(uint16_t))));
}

ZTEST(mesh_ch9_ack_handoff, test_partial_ack_marks_only_matching_entry)
{
    const uint32_t sessions[] = { 1001u };
    const uint16_t seqs[] = { 11u };
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entries[] = {
        {
            .session_id = 1001u,
            .seq = 11u,
        },
        {
            .session_id = 1002u,
            .seq = 12u,
        },
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;

    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     sessions,
                     seqs,
                     ARRAY_SIZE(seqs));

    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         entries,
                                         ARRAY_SIZE(entries),
                                         &result));

    zassert_true(result.any_match);
    zassert_false(result.all_acked);
    zassert_equal(result.acked_now, 1u);
    zassert_equal(result.unacked_count, 1u);
    zassert_true(entries[0].acked);
    zassert_false(entries[1].acked);
}

ZTEST(mesh_ch9_ack_handoff, test_complete_ack_uses_session_list_to_disambiguate)
{
    const uint32_t sessions[] = { 1002u };
    const uint16_t seqs[] = { 11u };
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entries[] = {
        {
            .session_id = 1001u,
            .seq = 11u,
        },
        {
            .session_id = 1002u,
            .seq = 11u,
        },
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;

    entries[0].acked = true;
    append_ack_lists(payload,
                     sizeof(payload),
                     &payload_len,
                     sessions,
                     seqs,
                     ARRAY_SIZE(seqs));

    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         entries,
                                         ARRAY_SIZE(entries),
                                         &result));

    zassert_true(result.any_match);
    zassert_true(result.all_acked);
    zassert_equal(result.acked_now, 1u);
    zassert_equal(result.unacked_count, 0u);
    zassert_true(entries[0].acked);
    zassert_true(entries[1].acked);
}

ZTEST(mesh_ch9_ack_handoff, test_legacy_requested_seq_requires_ack_session)
{
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entry = {
        .session_id = ack.session_id,
        .seq = 33u,
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[16];
    size_t payload_len = 0u;

    zassert_ok(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_REQUESTED_MSG_SEQ,
                              entry.seq));
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         &entry,
                                         1u,
                                         &result));
    zassert_true(result.all_acked);
    zassert_true(entry.acked);

    entry.acked = false;
    entry.session_id = ack.session_id + 1u;
    zassert_ok(app_mesh_ch9_tx_ack_apply(&ack,
                                         payload,
                                         payload_len,
                                         &entry,
                                         1u,
                                         &result));
    zassert_false(result.any_match);
    zassert_false(entry.acked);
}

ZTEST(mesh_ch9_ack_handoff, test_malformed_session_list_is_rejected)
{
    const uint8_t bad_session_list[] = { 0x01u, 0x02u };
    const uint16_t seq = 22u;
    struct proto_packet ack = gateway_ack_packet();
    struct app_mesh_ch9_tx_ack_entry entry = {
        .session_id = 1001u,
        .seq = seq,
    };
    struct app_mesh_ch9_tx_ack_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;
    uint8_t seq_list[sizeof(uint16_t)];

    proto_put_u16_le(seq_list, seq);
    zassert_ok(tlv_append_bytes(payload,
                                sizeof(payload),
                                &payload_len,
                                TLV_MESH_ACK_SESSION_LIST,
                                bad_session_list,
                                sizeof(bad_session_list)));
    zassert_ok(tlv_append_bytes(payload,
                                sizeof(payload),
                                &payload_len,
                                TLV_MESH_ACK_SEQ_LIST,
                                seq_list,
                                sizeof(seq_list)));

    zassert_equal(app_mesh_ch9_tx_ack_apply(&ack,
                                            payload,
                                            payload_len,
                                            &entry,
                                            1u,
                                            &result),
                  PROTO_ERR_MALFORMED);
    zassert_false(entry.acked);
}

ZTEST_SUITE(mesh_ch9_ack_handoff, NULL, NULL, NULL, NULL, NULL);
