#include "discovery_assignment.h"
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
    policy.assignment.operation_budget_ms =
        DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(750u);
    policy.assignment.response_spread_ms = 750u;
    policy.discovery.start_delay_ms =
        OPERATION_POLICY_DISCOVERY_DEFAULT_START_DELAY_MS;
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

static void remove_unique_tlv(struct mesh_outbound *outbound, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    size_t value_offset;
    size_t tlv_offset;
    size_t tlv_len;

    assert(outbound != NULL);
    assert(tlv_find_unique(outbound->payload,
                           outbound->payload_len,
                           type,
                           &value,
                           &value_len) == PROTO_OK);
    value_offset = (size_t)(value - outbound->payload);
    assert(value_offset >= PROTO_TLV_HEADER_LEN);
    tlv_offset = value_offset - PROTO_TLV_HEADER_LEN;
    tlv_len = PROTO_TLV_HEADER_LEN + value_len;
    assert(tlv_offset + tlv_len <= outbound->payload_len);
    memmove(&outbound->payload[tlv_offset],
            &outbound->payload[tlv_offset + tlv_len],
            outbound->payload_len - tlv_offset - tlv_len);
    outbound->payload_len -= (uint16_t)tlv_len;
    outbound->packet.payload_len = outbound->payload_len;
}

static void assert_route_adv_rejected_without_mutation(
    const struct mesh_outbound *advertisement,
    uint64_t local_id)
{
    struct mesh_relay anchor;
    struct mesh_relay_result result;

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, local_id,
                    TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_validate_gateway_route_adv(
               &anchor,
               &advertisement->packet,
               advertisement->payload,
               advertisement->payload_len,
               TEST_GATEWAY_ID) != PROTO_OK);
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
    assert(mesh_relay_validate_gateway_route_adv(
               &anchor,
               &advertisement.packet,
               advertisement.payload,
               advertisement.payload_len,
               TEST_GATEWAY_ID) == PROTO_OK);
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

static void test_header_relevant_adv_must_pass_full_capture_admission(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_outbound valid;
    struct mesh_outbound malformed;
    const uint8_t *value;
    uint8_t value_len;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_build_gateway_route_adv(
               &gateway,
               TEST_ROUTE_SEQUENCE,
               1000u,
               &valid) == PROTO_OK);
    assert(mesh_relay_validate_gateway_route_adv(
               &anchor,
               &valid.packet,
               valid.payload,
               valid.payload_len,
               TEST_GATEWAY_ID) == PROTO_OK);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_GATEWAY_ROUTE_SEQ,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    proto_put_u32_le((uint8_t *)value, TEST_ROUTE_SEQUENCE + 1u);
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 1u);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_GATEWAY_EPOCH,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    proto_put_u16_le((uint8_t *)value, TEST_ROUTE_EPOCH + 1u);
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 2u);

    malformed = valid;
    malformed.packet.ttl--;
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 3u);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_FLOOD_RETRY_COUNT,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint8_t));
    ((uint8_t *)value)[0] = UINT8_MAX;
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 4u);

    malformed = valid;
    malformed.packet.message_age_ms = 1u;
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 5u);

    malformed = valid;
    remove_unique_tlv(&malformed, TLV_FLOOD_PACKET_AGE_MS);
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 9u);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_RELAY_CAPACITY_STATE,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint8_t));
    ((uint8_t *)value)[0] = RELAY_CAP_UNKNOWN;
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 10u);

    malformed = valid;
    malformed.packet.src_id = TEST_ANCHOR_BASE + 50u;
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 6u);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_FLOOD_EPOCH_ID,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    proto_put_u32_le((uint8_t *)value, TEST_ROUTE_SEQUENCE + 1u);
    assert_route_adv_rejected_without_mutation(
        &malformed, TEST_ANCHOR_BASE + 7u);

    {
        struct mesh_relay newer_anchor;
        struct mesh_relay_result result;

        mesh_relay_init(&newer_anchor,
                        MESH_RELAY_ROLE_ANCHOR,
                        TEST_ANCHOR_BASE + 8u,
                        TEST_GATEWAY_ID,
                        TEST_ROUTE_EPOCH + 1u);
        assert(mesh_relay_validate_gateway_route_adv(
                   &newer_anchor,
                   &valid.packet,
                   valid.payload,
                   valid.payload_len,
                   TEST_GATEWAY_ID) == PROTO_ERR_STALE);
        assert(mesh_relay_handle_rx_with_random(
                   &newer_anchor,
                   &valid.packet,
                   valid.payload,
                   valid.payload_len,
                   TEST_GATEWAY_ID,
                   90u,
                   1010u,
                   1u,
                   &result) == PROTO_OK);
        assert(result.status == PROTO_ERR_STALE);
        assert(result_has_action(&result, MESH_RELAY_ACTION_DROP));
        assert(route_selected(&newer_anchor.upstream) == NULL);
    }
}

static void test_route_request_requires_full_read_only_admission(void)
{
    struct mesh_relay origin;
    struct mesh_relay receiver;
    struct mesh_outbound valid;
    struct mesh_outbound malformed;
    struct mesh_relay_result result;
    const uint8_t *value;
    uint8_t value_len;

    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH);
    mesh_relay_init(&receiver,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 1u,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH);
    assert(mesh_relay_build_route_request(&origin,
                                          TEST_GATEWAY_ID,
                                          &valid,
                                          2000u) == PROTO_OK);
    assert(mesh_relay_validate_route_request(
               &receiver,
               &valid.packet,
               valid.payload,
               valid.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_OK);

    malformed = valid;
    malformed.packet.flags = FLAG_DIAGNOSTIC;
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    malformed = valid;
    malformed.packet.session_id++;
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_SLOT_SEED,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    proto_put_u32_le((uint8_t *)value,
                     proto_get_u32_le(value) ^ UINT32_C(1));
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);
    assert(mesh_relay_handle_rx_with_random(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               90u,
               2010u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result_has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(mesh_relay_find_downlink(&receiver, TEST_ANCHOR_BASE) == NULL);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_FLOOD_PROFILE_VERSION,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    proto_put_u16_le((uint8_t *)value, 2u);
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_QUALITY,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint8_t));
    ((uint8_t *)value)[0] = 101u;
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    malformed = valid;
    remove_unique_tlv(&malformed, TLV_QUEUE_FREE_HINT);
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    malformed = valid;
    assert(tlv_find_unique(malformed.payload,
                           malformed.payload_len,
                           TLV_RELAY_CAPACITY_STATE,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint8_t));
    ((uint8_t *)value)[0] = RELAY_CAP_UNKNOWN;
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    malformed = valid;
    malformed.packet.ttl = 3u;
    assert(mesh_relay_validate_route_request(
               &receiver,
               &malformed.packet,
               malformed.payload,
               malformed.payload_len,
               TEST_ANCHOR_BASE,
               2010u) == PROTO_ERR_MALFORMED);

    assert(mesh_relay_validate_route_request(
               &receiver,
               &valid.packet,
               valid.payload,
               valid.payload_len,
               TEST_ANCHOR_BASE + 2u,
               2010u) == PROTO_ERR_MALFORMED);
}

static void test_route_request_epoch_transition_and_stale_rejection(void)
{
    struct mesh_relay older_origin;
    struct mesh_relay ambiguous_origin;
    struct mesh_relay newer_origin;
    struct mesh_relay receiver;
    struct mesh_relay wrap_origin;
    struct mesh_relay wrap_receiver;
    struct mesh_outbound request;
    struct mesh_relay_result result;
    const struct mesh_downlink_entry *reverse;

    mesh_relay_init(&older_origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 30u,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH);
    mesh_relay_init(&receiver,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 31u,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH + 1u);
    assert(mesh_relay_build_route_request(&older_origin,
                                          TEST_GATEWAY_ID,
                                          &request,
                                          3000u) == PROTO_OK);
    receiver.gateway_route_adv_seq = 77u;
    receiver.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = TEST_ANCHOR_BASE + 32u,
        .next_hop_id = TEST_ANCHOR_BASE + 32u,
        .gateway_id = TEST_GATEWAY_ID,
        .route_epoch = TEST_ROUTE_EPOCH + 1u,
        .hop_count = 1u,
        .quality = 80u,
        .valid = true,
    };
    assert(mesh_relay_validate_route_request(
               &receiver,
               &request.packet,
               request.payload,
               request.payload_len,
               older_origin.local_id,
               3010u) == PROTO_ERR_STALE);
    assert(mesh_relay_handle_rx_with_random(
               &receiver,
               &request.packet,
               request.payload,
               request.payload_len,
               older_origin.local_id,
               90u,
               3010u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(result_has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!result.route_state_changed);
    assert(receiver.upstream.current_epoch == TEST_ROUTE_EPOCH + 1u);
    assert(receiver.gateway_route_adv_seq == 77u);
    assert(receiver.downlinks[0].valid);
    assert(mesh_relay_find_downlink(&receiver,
                                    older_origin.local_id) == NULL);

    mesh_relay_init(&ambiguous_origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 34u,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH + 1u + UINT32_C(0x80000000));
    assert(mesh_relay_build_route_request(&ambiguous_origin,
                                          TEST_GATEWAY_ID,
                                          &request,
                                          3500u) == PROTO_OK);
    assert(mesh_relay_validate_route_request(
               &receiver,
               &request.packet,
               request.payload,
               request.payload_len,
               ambiguous_origin.local_id,
               3510u) == PROTO_ERR_STALE);
    assert(mesh_relay_handle_rx_with_random(
               &receiver,
               &request.packet,
               request.payload,
               request.payload_len,
               ambiguous_origin.local_id,
               90u,
               3510u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!result.route_state_changed);
    assert(receiver.upstream.current_epoch == TEST_ROUTE_EPOCH + 1u);
    assert(receiver.gateway_route_adv_seq == 77u);

    mesh_relay_init(&newer_origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 33u,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH + 2u);
    assert(mesh_relay_build_route_request(&newer_origin,
                                          TEST_GATEWAY_ID,
                                          &request,
                                          4000u) == PROTO_OK);
    receiver.route_discovery.target_id = TEST_GATEWAY_ID;
    receiver.route_discovery.active = true;
    assert(mesh_relay_handle_rx_with_random(
               &receiver,
               &request.packet,
               request.payload,
               request.payload_len,
               newer_origin.local_id,
               90u,
               4010u,
               1u,
               &result) == PROTO_OK);
    assert(result.route_state_changed);
    assert(receiver.upstream.current_epoch == TEST_ROUTE_EPOCH + 2u);
    assert(receiver.gateway_route_adv_seq == 0u);
    assert(!receiver.route_discovery.active);
    assert(mesh_relay_find_downlink(
               &receiver, TEST_ANCHOR_BASE + 32u) == NULL);
    reverse = mesh_relay_find_downlink(&receiver, newer_origin.local_id);
    assert(reverse != NULL);
    assert(reverse->route_epoch == TEST_ROUTE_EPOCH + 2u);

    mesh_relay_init(&wrap_origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 35u,
                    TEST_GATEWAY_ID,
                    1u);
    mesh_relay_init(&wrap_receiver,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 36u,
                    TEST_GATEWAY_ID,
                    UINT32_MAX);
    assert(mesh_relay_build_route_request(&wrap_origin,
                                          TEST_GATEWAY_ID,
                                          &request,
                                          5000u) == PROTO_OK);
    wrap_receiver.gateway_route_adv_seq = UINT32_MAX;
    wrap_receiver.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = TEST_ANCHOR_BASE + 37u,
        .next_hop_id = TEST_ANCHOR_BASE + 37u,
        .gateway_id = TEST_GATEWAY_ID,
        .route_epoch = UINT32_MAX,
        .hop_count = 1u,
        .quality = 80u,
        .valid = true,
    };
    wrap_receiver.route_discovery.target_id = TEST_GATEWAY_ID;
    wrap_receiver.route_discovery.active = true;
    assert(mesh_relay_handle_rx_with_random(
               &wrap_receiver,
               &request.packet,
               request.payload,
               request.payload_len,
               wrap_origin.local_id,
               90u,
               5010u,
               1u,
               &result) == PROTO_OK);
    assert(result.route_state_changed);
    assert(wrap_receiver.upstream.current_epoch == 1u);
    assert(wrap_receiver.gateway_route_adv_seq == 0u);
    assert(!wrap_receiver.route_discovery.active);
    assert(mesh_relay_find_downlink(
               &wrap_receiver, TEST_ANCHOR_BASE + 37u) == NULL);
    reverse = mesh_relay_find_downlink(&wrap_receiver, wrap_origin.local_id);
    assert(reverse != NULL);
    assert(reverse->route_epoch == 1u);
}

static void test_gateway_adv_sequence_freshness_is_commit_late_and_wrap_safe(void)
{
    struct mesh_relay gateway;
    struct mesh_relay next_epoch_gateway;
    struct mesh_relay anchor;
    struct mesh_relay wrap_anchor;
    struct operation_policy_set older_policy = complete_policy();
    struct operation_policy_set newer_policy = complete_policy();
    struct mesh_outbound older;
    struct mesh_outbound newer;
    struct mesh_outbound exact_seq_conflict;
    struct mesh_outbound reordered;
    struct mesh_outbound malformed_next;
    struct mesh_outbound valid_next;
    struct mesh_outbound half_range;
    struct mesh_outbound wrap_max;
    struct mesh_outbound wrap_one;
    struct mesh_outbound next_epoch;
    struct mesh_relay_result result;
    const struct route_candidate *selected;

    older_policy.assignment.operation_budget_ms =
        DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(750u);
    newer_policy.assignment.operation_budget_ms =
        DISCOVERY_ASSIGNMENT_OPERATION_REQUIRED_BUDGET_MS(750u) + 1000u;
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 30u,
                    TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);

    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway, 100u, 1000u, &older_policy, &older) == PROTO_OK);
    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway, 101u, 1001u, &newer_policy, &newer) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &newer.packet,
               newer.payload,
               newer.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1010u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result_has_action(
        &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
    assert_policy_equal(&result.operation_policy, &newer_policy);
    assert(anchor.gateway_route_adv_seq == 101u);
    selected = route_selected(&anchor.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == TEST_GATEWAY_ID);

    assert(mesh_relay_validate_gateway_route_adv(
               &anchor,
               &older.packet,
               older.payload,
               older.payload_len,
               TEST_GATEWAY_ID) == PROTO_ERR_STALE);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &older.packet,
               older.payload,
               older.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1011u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(result_has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!result_has_action(
        &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
    assert(anchor.gateway_route_adv_seq == 101u);

    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &newer.packet,
               newer.payload,
               newer.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1012u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!result_has_action(
        &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
    assert(anchor.gateway_route_adv_seq == 101u);

    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway,
               101u,
               1013u,
               &older_policy,
               &exact_seq_conflict) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &exact_seq_conflict.packet,
               exact_seq_conflict.payload,
               exact_seq_conflict.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1013u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!result_has_action(
        &result, MESH_RELAY_ACTION_INSTALL_OPERATION_POLICY));
    assert(anchor.gateway_route_adv_seq == 101u);

    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway,
               102u,
               1014u,
               &newer_policy,
               &valid_next) == PROTO_OK);
    malformed_next = valid_next;
    malformed_next.packet.message_age_ms++;
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &malformed_next.packet,
               malformed_next.payload,
               malformed_next.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1014u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(anchor.gateway_route_adv_seq == 101u);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &valid_next.packet,
               valid_next.payload,
               valid_next.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1015u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(anchor.gateway_route_adv_seq == 102u);

    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway,
               101u,
               1016u,
               &older_policy,
               &reordered) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &reordered.packet,
               reordered.payload,
               reordered.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1016u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(anchor.gateway_route_adv_seq == 102u);

    assert(mesh_relay_build_gateway_route_adv_with_policy(
               &gateway,
               102u + UINT32_C(0x80000000),
               1017u,
               &newer_policy,
               &half_range) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &half_range.packet,
               half_range.payload,
               half_range.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1017u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(anchor.gateway_route_adv_seq == 102u);

    mesh_relay_init(&wrap_anchor, MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 31u,
                    TEST_GATEWAY_ID, TEST_ROUTE_EPOCH);
    assert(mesh_relay_build_gateway_route_adv(
               &gateway, UINT32_MAX, 1020u, &wrap_max) == PROTO_OK);
    assert(mesh_relay_build_gateway_route_adv(
               &gateway, 1u, 1021u, &wrap_one) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(
               &wrap_anchor,
               &wrap_max.packet,
               wrap_max.payload,
               wrap_max.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1020u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(wrap_anchor.gateway_route_adv_seq == UINT32_MAX);
    assert(mesh_relay_handle_rx_with_random(
               &wrap_anchor,
               &wrap_one.packet,
               wrap_one.payload,
               wrap_one.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1021u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(wrap_anchor.gateway_route_adv_seq == 1u);

    mesh_relay_init(&next_epoch_gateway, MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID, TEST_GATEWAY_ID, TEST_ROUTE_EPOCH + 1u);
    assert(mesh_relay_build_gateway_route_adv(
               &next_epoch_gateway, 1u, 1022u, &next_epoch) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(
               &anchor,
               &next_epoch.packet,
               next_epoch.payload,
               next_epoch.payload_len,
               TEST_GATEWAY_ID,
               90u,
               1022u,
               1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(anchor.upstream.current_epoch == TEST_ROUTE_EPOCH + 1u);
    assert(anchor.gateway_route_adv_seq == 1u);
}

static void test_configured_route_uses_one_complete_epoch_transition(void)
{
    struct mesh_relay anchor;
    struct route_candidate old_route = {
        .next_hop_id = TEST_GATEWAY_ID,
        .gateway_id = TEST_GATEWAY_ID,
        .route_epoch = TEST_ROUTE_EPOCH,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = 80u,
        .valid = true,
    };
    struct route_candidate next_route = old_route;
    bool epoch_changed = false;

    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_BASE + 40u,
                    TEST_GATEWAY_ID,
                    TEST_ROUTE_EPOCH);
    assert(route_upsert_candidate(&anchor.upstream, &old_route) == PROTO_OK);
    anchor.gateway_route_adv_seq = 101u;
    anchor.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = TEST_ANCHOR_BASE + 41u,
        .next_hop_id = TEST_ANCHOR_BASE + 41u,
        .gateway_id = TEST_GATEWAY_ID,
        .route_epoch = TEST_ROUTE_EPOCH,
        .hop_count = 1u,
        .quality = 70u,
        .valid = true,
    };
    anchor.event_timings[0].next_hop_id = TEST_GATEWAY_ID;
    anchor.event_timings[0].valid = true;
    anchor.route_discovery.target_id = TEST_GATEWAY_ID;
    anchor.route_discovery.active = true;

    next_route.route_epoch++;
    next_route.last_seen_ms++;
    assert(mesh_relay_validate_configured_gateway_route(
               &anchor, &next_route, &epoch_changed) == PROTO_OK);
    assert(epoch_changed);
    assert(anchor.upstream.current_epoch == TEST_ROUTE_EPOCH);
    assert(anchor.gateway_route_adv_seq == 101u);
    assert(anchor.downlinks[0].valid);
    assert(anchor.event_timings[0].valid);
    assert(anchor.route_discovery.active);

    assert(mesh_relay_upsert_configured_gateway_route(
               &anchor, &next_route) == PROTO_OK);
    assert(anchor.upstream.current_epoch == TEST_ROUTE_EPOCH + 1u);
    assert(anchor.gateway_route_adv_seq == 0u);
    assert(route_selected(&anchor.upstream) != NULL);
    assert(route_selected(&anchor.upstream)->route_epoch ==
           TEST_ROUTE_EPOCH + 1u);
    assert(!anchor.downlinks[0].valid);
    assert(!anchor.event_timings[0].valid);
    assert(!anchor.route_discovery.active);

    anchor.downlinks[0].valid = true;
    anchor.event_timings[0].valid = true;
    anchor.route_discovery.active = true;
    next_route.last_seen_ms++;
    assert(mesh_relay_validate_configured_gateway_route(
               &anchor, &next_route, &epoch_changed) == PROTO_OK);
    assert(!epoch_changed);
    assert(mesh_relay_upsert_configured_gateway_route(
               &anchor, &next_route) == PROTO_OK);
    assert(anchor.downlinks[0].valid);
    assert(anchor.event_timings[0].valid);
    assert(anchor.route_discovery.active);

    old_route.route_epoch = TEST_ROUTE_EPOCH;
    assert(mesh_relay_validate_configured_gateway_route(
               &anchor, &old_route, &epoch_changed) == PROTO_ERR_STALE);
    old_route.route_epoch =
        next_route.route_epoch + UINT32_C(0x80000000);
    assert(mesh_relay_validate_configured_gateway_route(
               &anchor, &old_route, &epoch_changed) == PROTO_ERR_STALE);
    assert(anchor.upstream.current_epoch == TEST_ROUTE_EPOCH + 1u);
    assert(anchor.downlinks[0].valid);
    assert(anchor.event_timings[0].valid);
    assert(anchor.route_discovery.active);
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
    test_header_relevant_adv_must_pass_full_capture_admission();
    test_route_request_requires_full_read_only_admission();
    test_route_request_epoch_transition_and_stale_rejection();
    test_gateway_adv_sequence_freshness_is_commit_late_and_wrap_safe();
    test_configured_route_uses_one_complete_epoch_transition();
    test_multihop_forward_preserves_exact_policy_bytes();
    test_malformed_and_duplicate_policy_reject_atomically();
    test_invalid_policy_capture_and_snapshot_fail_atomically();
    return 0;
}
