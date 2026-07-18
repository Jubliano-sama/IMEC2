#include "mesh_relay.h"

#include <assert.h>
#include <string.h>

#define TEST_GATEWAY_ID UINT64_C(0xa001000000000001)
#define TEST_ANCHOR_BASE UINT64_C(0xa002000000001000)
#define TEST_ROUTE_EPOCH 17u
#define TEST_ROUTE_SEQUENCE UINT32_C(0x48494101)

static bool result_has_action(const struct mesh_relay_result *result,
                              enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static struct operation_policy_set complete_policy(void)
{
    struct operation_policy_set policy;

    operation_policy_set_defaults(&policy);
    policy.assignment_present = true;
    policy.discovery_present = true;
    policy.pair_present = true;
    policy.assignment.expected_anchor_count = 12u;
    policy.assignment.operation_budget_ms = 180000u;
    policy.assignment.response_spread_ms = 750u;
    policy.discovery.start_delay_ms = 9000u;
    policy.discovery.slot_ms = 75u;
    policy.discovery.slot_count = 10u;
    policy.discovery.round_count = 2u;
    policy.discovery.report_grace_ms = 1200u;
    policy.discovery.operation_budget_ms = 300000u;
    policy.pair.max_reruns = 1u;
    policy.pair.max_parallel_pairs = 8u;
    return policy;
}

static void assert_policy_equal(const struct operation_policy_set *actual,
                                const struct operation_policy_set *expected)
{
    assert(actual->assignment_present == expected->assignment_present);
    assert(actual->discovery_present == expected->discovery_present);
    assert(actual->pair_present == expected->pair_present);
    assert(actual->assignment.expected_anchor_count ==
           expected->assignment.expected_anchor_count);
    assert(actual->assignment.operation_budget_ms ==
           expected->assignment.operation_budget_ms);
    assert(actual->assignment.response_spread_ms ==
           expected->assignment.response_spread_ms);
    assert(actual->discovery.start_delay_ms ==
           expected->discovery.start_delay_ms);
    assert(actual->discovery.slot_ms == expected->discovery.slot_ms);
    assert(actual->discovery.slot_count == expected->discovery.slot_count);
    assert(actual->discovery.round_count == expected->discovery.round_count);
    assert(actual->discovery.report_grace_ms ==
           expected->discovery.report_grace_ms);
    assert(actual->discovery.operation_budget_ms ==
           expected->discovery.operation_budget_ms);
    assert(actual->pair.max_reruns == expected->pair.max_reruns);
    assert(actual->pair.max_parallel_pairs ==
           expected->pair.max_parallel_pairs);
}

static int copy_policy_tlvs(const uint8_t *payload,
                            size_t payload_len,
                            uint8_t *policy_tlvs,
                            size_t *policy_tlvs_len,
                            size_t *first_policy_offset)
{
    size_t copied = 0u;
    size_t offset = 0u;
    bool found = false;

    if (payload == NULL || policy_tlvs == NULL || policy_tlvs_len == NULL) {
        return PROTO_ERR_ARG;
    }
    while (offset < payload_len) {
        size_t tlv_offset = offset;
        uint8_t type;
        uint8_t len;
        size_t tlv_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (payload_len - offset < len) {
            return PROTO_ERR_MALFORMED;
        }
        tlv_len = PROTO_TLV_HEADER_LEN + len;
        if (type == TLV_OPERATION_POLICY) {
            if (copied + tlv_len > OPERATION_POLICY_ALL_TLVS_LEN) {
                return PROTO_ERR_NO_SPACE;
            }
            if (!found && first_policy_offset != NULL) {
                *first_policy_offset = tlv_offset;
            }
            found = true;
            memcpy(&policy_tlvs[copied], &payload[tlv_offset], tlv_len);
            copied += tlv_len;
        }
        offset += len;
    }
    *policy_tlvs_len = copied;
    return PROTO_OK;
}

static void assert_route_adv_rejected_without_mutation(
    const struct mesh_outbound *advertisement,
    uint64_t local_id)
{
    struct mesh_relay anchor;
    struct mesh_relay_result result;

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, local_id,
                    TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &advertisement->packet,
               advertisement->payload,
               advertisement->payload_len,
               TEST_GATEWAY_ID,
               90u,
               1010u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result_has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!result_has_action(
        &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
    assert(route_selected(&anchor.upstream) == NULL);
}

static void test_gateway_snapshot_freezes_complete_policy(void)
{
    struct mesh_relay gateway;
    struct operation_policy_set policy = complete_policy();
    struct operation_policy_set decoded;
    struct mesh_gateway_route_adv_snapshot snapshot;
    struct mesh_outbound first;
    struct mesh_outbound rebuilt;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &policy,
               &snapshot) == PROTO_OK);
    assert(snapshot.valid);
    assert(snapshot.operation_policy_present);
    assert(snapshot.operation_policy_tlvs_len ==
           OPERATION_POLICY_ALL_TLVS_LEN);
    assert(mesh_relay_build_gateway_route_adv_from_snapshot(
               &gateway, &snapshot, &first) == PROTO_OK);
    assert(first.payload_len == MESH_GATEWAY_ROUTE_ADV_POLICY_PAYLOAD_LEN);
    assert(operation_policy_set_from_tlvs(first.payload,
                                          first.payload_len,
                                          &decoded) == PROTO_OK);
    assert_policy_equal(&decoded, &policy);

    policy.assignment.operation_budget_ms++;
    gateway.upstream.current_epoch++;
    assert(mesh_relay_build_gateway_route_adv_from_snapshot(
               &gateway, &snapshot, &rebuilt) == PROTO_OK);
    assert(memcmp(&first, &rebuilt, sizeof(first)) == 0);
}

static void test_legacy_route_adv_remains_accepted(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_outbound advertisement;
    struct mesh_relay_result result;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_build_gateway_route_adv(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &advertisement) == PROTO_OK);
    assert(advertisement.payload_len == MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &advertisement.packet,
               advertisement.payload,
               advertisement.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1010u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result_has_action(
        &result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(!result_has_action(
        &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
    assert(route_selected(&anchor.upstream) != NULL);
}

static void test_multihop_forward_preserves_exact_policy_bytes(void)
{
    struct mesh_relay gateway;
    struct operation_policy_set policy = complete_policy();
    struct mesh_outbound current;
    uint8_t expected_policy[OPERATION_POLICY_ALL_TLVS_LEN];
    size_t expected_policy_len = 0u;
    uint64_t previous_hop_id = TEST_GATEWAY_ID;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &policy,
               &current) == PROTO_OK);
    assert(copy_policy_tlvs(current.payload,
                            current.payload_len,
                            expected_policy,
                            &expected_policy_len,
                            NULL) == PROTO_OK);
    assert(expected_policy_len == sizeof(expected_policy));

    for (uint8_t hop = 0u; hop < MESH_NETWORK_MAX_HOPS; hop++) {
        struct mesh_relay anchor;
        struct mesh_relay_result result;
        const uint64_t local_id = TEST_ANCHOR_BASE + hop;

        mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                        local_id, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
        assert(mesh_relay_handle_rx_with_random(
                   &anchor,
                   &current.packet,
                   current.payload,
                   current.payload_len,
                   previous_hop_id,
                   (uint8_t)(95u - hop),
                   1010u + hop,
                   hop,
                   &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(result_has_action(
            &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
        assert_policy_equal(&result.operation_policy, &policy);
        assert(route_selected(&anchor.upstream) != NULL);

        if (hop + 1u < MESH_NETWORK_MAX_HOPS) {
            uint8_t forwarded_policy[OPERATION_POLICY_ALL_TLVS_LEN];
            size_t forwarded_policy_len = 0u;

            assert(result_has_action(
                &result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
            assert(copy_policy_tlvs(result.gateway_route_adv.payload,
                                    result.gateway_route_adv.payload_len,
                                    forwarded_policy,
                                    &forwarded_policy_len,
                                    NULL) == PROTO_OK);
            assert(forwarded_policy_len == expected_policy_len);
            assert(memcmp(forwarded_policy,
                          expected_policy,
                          expected_policy_len) == 0);
            current = result.gateway_route_adv;
        } else {
            assert(!result_has_action(
                &result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
        }
        previous_hop_id = local_id;
    }
    assert(current.payload_len == MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN);
}

static void test_malformed_and_duplicate_policy_reject_atomically(void)
{
    struct mesh_relay gateway;
    struct operation_policy_set policy = complete_policy();
    struct mesh_outbound valid;
    struct mesh_outbound partial;
    struct mesh_outbound duplicate;
    struct mesh_outbound malformed;
    uint8_t policy_tlvs[OPERATION_POLICY_ALL_TLVS_LEN];
    size_t policy_tlvs_len = 0u;
    size_t first_policy_offset = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &policy,
               &valid) == PROTO_OK);
    assert(copy_policy_tlvs(valid.payload,
                            valid.payload_len,
                            policy_tlvs,
                            &policy_tlvs_len,
                            &first_policy_offset) == PROTO_OK);
    assert(policy_tlvs_len == sizeof(policy_tlvs));

    partial = valid;
    partial.payload_len -= OPERATION_POLICY_PAIR_TLV_LEN;
    partial.packet.payload_len = partial.payload_len;
    assert_route_adv_rejected_without_mutation(&partial,
                                                TEST_ANCHOR_BASE + 20u);

    duplicate = valid;
    memcpy(&duplicate.payload[duplicate.payload_len],
           &valid.payload[first_policy_offset],
           OPERATION_POLICY_ASSIGNMENT_TLV_LEN);
    duplicate.payload_len += OPERATION_POLICY_ASSIGNMENT_TLV_LEN;
    duplicate.packet.payload_len = duplicate.payload_len;
    assert(duplicate.payload_len <= MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN);
    assert_route_adv_rejected_without_mutation(&duplicate,
                                                TEST_ANCHOR_BASE + 21u);

    malformed = valid;
    malformed.payload[first_policy_offset + PROTO_TLV_HEADER_LEN]++;
    assert_route_adv_rejected_without_mutation(&malformed,
                                                TEST_ANCHOR_BASE + 22u);
}

static void test_invalid_policy_capture_and_snapshot_fail_atomically(void)
{
    struct mesh_relay gateway;
    struct operation_policy_set incomplete = complete_policy();
    struct operation_policy_set complete = complete_policy();
    struct mesh_gateway_route_adv_snapshot snapshot;
    struct mesh_gateway_route_adv_snapshot unchanged;
    struct mesh_outbound outbound;
    struct mesh_outbound unchanged_outbound;
    uint16_t next_seq;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    incomplete.pair_present = false;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    unchanged = snapshot;
    next_seq = gateway.next_seq;
    assert(mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &incomplete,
               &snapshot) == PROTO_ERR_MALFORMED);
    assert(memcmp(&snapshot, &unchanged, sizeof(snapshot)) == 0);
    assert(gateway.next_seq == next_seq);

    assert(mesh_relay_capture_gateway_route_adv_snapshot_with_policy(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &complete,
               &snapshot) == PROTO_OK);
    snapshot.operation_policy_tlvs[PROTO_TLV_HEADER_LEN]++;
    memset(&outbound, 0x5a, sizeof(outbound));
    unchanged_outbound = outbound;
    assert(mesh_relay_build_gateway_route_adv_from_snapshot(
               &gateway, &snapshot, &outbound) == PROTO_ERR_MALFORMED);
    assert(memcmp(&outbound, &unchanged_outbound, sizeof(outbound)) == 0);
}

int main(void)
{
    test_gateway_snapshot_freezes_complete_policy();
    test_legacy_route_adv_remains_accepted();
    test_multihop_forward_preserves_exact_policy_bytes();
    test_malformed_and_duplicate_policy_reject_atomically();
    test_invalid_policy_capture_and_snapshot_fail_atomically();
    return 0;
}
