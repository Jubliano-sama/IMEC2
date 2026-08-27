#include "mesh_relay.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_radio_timing.h"
#include "report.h"
#include "uwb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ANCHOR_A 0x1111222233334444ull
#define ANCHOR_B 0x5555666677778888ull
#define ANCHOR_C 0x2222333344445555ull
#define ANCHOR_D 0x6666777788889999ull
#define GATEWAY  0x9999888877776666ull
#define NETWORK_ID 0x494D4543u

static bool has_action(const struct mesh_relay_result *result, enum mesh_relay_action action)
{
    return (result->actions & action) != 0u;
}

static size_t build_click_report_with_participants(
    struct proto_packet *packet,
    uint64_t anchor_id,
    uint32_t event_seq,
    uint16_t seq,
    int32_t distance_mm,
    const uint64_t *participant_anchor_ids,
    uint8_t participant_anchor_count,
    uint8_t *payload,
    size_t payload_cap)
{
    const int32_t distance_samples_mm[] = {distance_mm};
    const uint8_t range_round_indices[] = {0u};
    const uint64_t sequence_start_timestamps_ms[] = {1000u};
    const struct range_report_fields fields = {
        .clicker_id = anchor_id == ANCHOR_C ? ANCHOR_A : ANCHOR_C,
        .anchor_id = anchor_id,
        .event_seq = event_seq,
        .timestamp_ms = 1000u,
        .distance_mm = distance_mm,
        .quality = 90u,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples_mm,
        .range_round_indices = range_round_indices,
        .sequence_start_timestamps_ms = sequence_start_timestamps_ms,
        .sample_count = 1u,
        .distance_sample_count = 1u,
        .participant_anchor_ids = participant_anchor_ids,
        .participant_anchor_count = participant_anchor_count,
        .burst_id = event_seq,
        .burst_id_present = true,
        .omit_rsl = true,
        .omit_cir = true,
    };
    size_t payload_len = 0u;

    assert(packet != NULL);
    assert(payload != NULL);
    assert(report_append_range_tlvs(payload,
                                    payload_cap,
                                    &payload_len,
                                    &fields) == PROTO_OK);
    assert(report_init_click_packet(packet,
                                    anchor_id,
                                    GATEWAY,
                                    proto_click_report_session_id(
                                        fields.clicker_id, event_seq),
                                    seq,
                                    (uint8_t)payload_len) == PROTO_OK);
    return payload_len;
}

static size_t build_valid_click_report(struct proto_packet *packet,
                                       uint64_t anchor_id,
                                       uint32_t event_seq,
                                       uint16_t seq,
                                       int32_t distance_mm,
                                       uint8_t *payload,
                                       size_t payload_cap)
{
    const uint64_t participant_anchor_ids[] = {
        ANCHOR_A,
        ANCHOR_C,
        ANCHOR_B,
        ANCHOR_D,
    };

    return build_click_report_with_participants(
        packet,
        anchor_id,
        event_seq,
        seq,
        distance_mm,
        participant_anchor_ids,
        (uint8_t)(sizeof(participant_anchor_ids) /
                  sizeof(participant_anchor_ids[0])),
        payload,
        payload_cap);
}





#define TEST_GATEWAY_ACK_STORE_COUNT 16u
#define TEST_ANCHOR_ROUTE_STORE_COUNT 16u

static struct mesh_gateway_ack_store
    test_gateway_ack_stores[TEST_GATEWAY_ACK_STORE_COUNT];
static size_t test_gateway_ack_store_next;
static struct mesh_anchor_downlink_store
    test_anchor_route_stores[TEST_ANCHOR_ROUTE_STORE_COUNT];
static size_t test_anchor_route_store_next;

static void raw_mesh_relay_init(struct mesh_relay *relay,
                                enum mesh_relay_role role,
                                uint64_t local_id,
                                uint64_t gateway_id,
                                uint32_t route_epoch)
{
    mesh_relay_init(relay, role, local_id, gateway_id, route_epoch);
}

static void test_mesh_relay_init(struct mesh_relay *relay,
                                 enum mesh_relay_role role,
                                 uint64_t local_id,
                                 uint64_t gateway_id,
                                 uint32_t route_epoch)
{
    raw_mesh_relay_init(relay, role, local_id, gateway_id, route_epoch);
    if (role == MESH_RELAY_ROLE_GATEWAY) {
        struct mesh_gateway_ack_store *store =
            &test_gateway_ack_stores[test_gateway_ack_store_next];

        test_gateway_ack_store_next =
            (test_gateway_ack_store_next + 1u) % TEST_GATEWAY_ACK_STORE_COUNT;
        assert(mesh_relay_attach_gateway_ack_store(relay, store) == PROTO_OK);
    } else if (role == MESH_RELAY_ROLE_ANCHOR) {
        struct mesh_anchor_downlink_store *store =
            &test_anchor_route_stores[test_anchor_route_store_next];

        test_anchor_route_store_next =
            (test_anchor_route_store_next + 1u) %
            TEST_ANCHOR_ROUTE_STORE_COUNT;
        assert(mesh_relay_attach_anchor_downlink_store(relay, store) == PROTO_OK);
    }
}

#define mesh_relay_init(...) test_mesh_relay_init(__VA_ARGS__)

static void build_gateway_ack_for_packet(
    struct proto_packet *ack,
    uint8_t *ack_payload,
    size_t ack_payload_cap,
    size_t *ack_payload_len,
    uint64_t destination_id,
    uint16_t ack_seq,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len)
{
    *ack_payload_len = 0u;
    assert(mesh_append_requested_seq(ack_payload,
                                     ack_payload_cap,
                                     ack_payload_len,
                                     acknowledged_packet->seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack_payload,
                                             ack_payload_cap,
                                             ack_payload_len,
                                             acknowledged_packet,
                                             acknowledged_payload,
                                             acknowledged_payload_len) ==
           PROTO_OK);
    assert(mesh_init_gateway_ack(ack,
                                 GATEWAY,
                                 destination_id,
                                 acknowledged_packet->session_id,
                                 ack_seq,
                                 (uint8_t)*ack_payload_len) == PROTO_OK);
}

static void build_hop_ack_for_packet(
    struct proto_packet *ack,
    uint64_t source_id,
    uint64_t destination_id,
    uint16_t ack_seq,
    uint8_t *ack_payload,
    size_t ack_payload_cap,
    size_t *ack_payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len)
{
    *ack_payload_len = 0u;
    assert(mesh_append_requested_seq(ack_payload,
                                     ack_payload_cap,
                                     ack_payload_len,
                                     acknowledged_packet->seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack_payload,
                                             ack_payload_cap,
                                             ack_payload_len,
                                             acknowledged_packet,
                                             acknowledged_payload,
                                             acknowledged_payload_len) ==
           PROTO_OK);
    memset(ack, 0, sizeof(*ack));
    ack->msg_type = MSG_MESH_HOP_ACK;
    ack->src_id = source_id;
    ack->dst_id = destination_id;
    ack->session_id = acknowledged_packet->session_id;
    ack->seq = ack_seq;
    ack->ttl = MESH_GATEWAY_ACK_TTL;
    ack->payload_len = (uint16_t)*ack_payload_len;
}

static size_t unique_u32_count(const uint32_t *values, size_t count)
{
    size_t unique_count = 0u;

    for (size_t i = 0u; i < count; i++) {
        bool seen = false;

        for (size_t j = 0u; j < i; j++) {
            if (values[i] == values[j]) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique_count++;
        }
    }
    return unique_count;
}

static struct route_candidate direct_gateway_route(uint64_t next_hop_id,
                                                   uint32_t epoch,
                                                   uint8_t quality)
{
    struct route_candidate candidate = {
        .next_hop_id = next_hop_id,
        .gateway_id = GATEWAY,
        .route_epoch = epoch,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = quality,
        .failure_count = 0u,
        .valid = true,
    };
    return candidate;
}

static struct mesh_event_params channel9_params(uint32_t first_event_time_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = 100u,
        .event_window_ms = 20u,
        .first_event_time_ms = first_event_time_ms,
        .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 2u,
        .supervision_timeout_ms = 500u,
    };
    return params;
}

static struct mesh_channel5_requirements clear_channel5_requirements(void)
{
    const struct mesh_channel5_requirements requirements = {
        .next_required_scan_start_ms = 0u,
        .active_until_ms = 0u,
        .retune_guard_ms = 5u,
        .click_epoch_active = false,
        .discovery_active = false,
        .ranging_active = false,
    };
    return requirements;
}

static const struct mesh_relay_event_timing_entry *find_event_timing(
    const struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &relay->event_timings[i];

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            return entry;
        }
    }
    return NULL;
}

static void assert_event_timing_equal(const struct mesh_event_timing *actual,
                                      const struct mesh_event_timing *expected)
{
    assert(actual != NULL);
    assert(expected != NULL);
    assert(actual->mesh_channel == expected->mesh_channel);
    assert(actual->event_interval_ms == expected->event_interval_ms);
    assert(actual->event_window_ms == expected->event_window_ms);
    assert(actual->next_event_time_ms == expected->next_event_time_ms);
    assert(actual->event_counter == expected->event_counter);
    assert(actual->guard_ms == expected->guard_ms);
    assert(actual->peer_clock_skew_estimate_ppm ==
           expected->peer_clock_skew_estimate_ppm);
    assert(actual->max_missed_events == expected->max_missed_events);
    assert(actual->missed_event_count == expected->missed_event_count);
    assert(actual->supervision_timeout_ms == expected->supervision_timeout_ms);
    assert(actual->last_successful_ch9_event_ms ==
           expected->last_successful_ch9_event_ms);
    assert(actual->local_tx_on_even_events ==
           expected->local_tx_on_even_events);
    assert(actual->route_fresh == expected->route_fresh);
    assert(actual->timing_fresh == expected->timing_fresh);
    assert(actual->fallback_required == expected->fallback_required);
}

static const struct route_candidate *find_route_candidate(
    const struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate = &relay->upstream.candidates[i];

        if (candidate->valid && candidate->next_hop_id == next_hop_id) {
            return candidate;
        }
    }
    return NULL;
}

static void assert_route_candidate_equal(
    const struct route_candidate *actual,
    const struct route_candidate *expected)
{
    assert(actual != NULL);
    assert(expected != NULL);
    assert(actual->next_hop_id == expected->next_hop_id);
    assert(actual->gateway_id == expected->gateway_id);
    assert(actual->route_epoch == expected->route_epoch);
    assert(actual->last_seen_ms == expected->last_seen_ms);
    assert(actual->last_success_ms == expected->last_success_ms);
    assert(actual->hold_down_until_ms == expected->hold_down_until_ms);
    assert(actual->route_cost == expected->route_cost);
    assert(actual->queue_free_hint == expected->queue_free_hint);
    assert(actual->hop_count == expected->hop_count);
    assert(actual->link_quality == expected->link_quality);
    assert(actual->failure_count == expected->failure_count);
    assert(actual->relay_capacity_state == expected->relay_capacity_state);
    assert(actual->channel9_busy_hint == expected->channel9_busy_hint);
    assert(actual->capacity_observed_at_ms == expected->capacity_observed_at_ms);
    assert(actual->capacity_valid_until_ms == expected->capacity_valid_until_ms);
    assert(actual->capacity_hint_valid == expected->capacity_hint_valid);
    assert(actual->hold_down_valid == expected->hold_down_valid);
    assert(actual->channel9_timing_valid == expected->channel9_timing_valid);
    assert(actual->valid == expected->valid);
}

static void assert_upstream_ancestry_equal(
    const struct mesh_upstream_ancestry_entry *actual,
    const struct mesh_upstream_ancestry_entry *expected)
{
    assert(actual != NULL);
    assert(expected != NULL);
    assert(actual->next_hop_id == expected->next_hop_id);
    assert(actual->route_epoch == expected->route_epoch);
    assert(actual->gateway_route_seq == expected->gateway_route_seq);
    assert(actual->valid == expected->valid);
    assert(actual->path.count == expected->path.count);
    for (uint8_t i = 0u; i < actual->path.count; i++) {
        assert(actual->path.node_ids[i] == expected->path.node_ids[i]);
    }
}

static const struct mesh_upstream_ancestry_entry *find_upstream_ancestry(
    const struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    assert(relay != NULL);
    assert(relay->anchor_downlink_store != NULL);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct mesh_upstream_ancestry_entry *entry =
            &relay->anchor_downlink_store->upstream_ancestry[i];

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            return entry;
        }
    }
    return NULL;
}

static void seed_upstream_candidate_with_ancestry(
    struct mesh_relay *relay,
    uint64_t next_hop_id,
    uint32_t route_epoch,
    uint8_t hop_count,
    uint8_t quality,
    uint32_t now_ms,
    const uint64_t *path_ids,
    uint8_t path_count)
{
    struct route_candidate candidate = {
        .next_hop_id = next_hop_id,
        .gateway_id = GATEWAY,
        .route_epoch = route_epoch,
        .last_seen_ms = now_ms,
        .hop_count = hop_count,
        .link_quality = quality,
        .valid = true,
    };

    assert(relay != NULL);
    assert(relay->anchor_downlink_store != NULL);
    assert(path_ids != NULL);
    assert(path_count == (uint8_t)(hop_count + 2u));
    assert(path_count <= MESH_ROUTE_PATH_MAX_NODES);
    assert(route_upsert_candidate(&relay->upstream, &candidate) == PROTO_OK);

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *stored = &relay->upstream.candidates[i];
        struct mesh_upstream_ancestry_entry *ancestry;

        if (!stored->valid || stored->next_hop_id != next_hop_id ||
            stored->route_epoch != route_epoch) {
            continue;
        }
        ancestry = &relay->anchor_downlink_store->upstream_ancestry[i];
        memset(ancestry, 0, sizeof(*ancestry));
        memcpy(ancestry->path.node_ids,
               path_ids,
               (size_t)path_count * sizeof(path_ids[0]));
        ancestry->path.count = path_count;
        ancestry->next_hop_id = next_hop_id;
        ancestry->route_epoch = route_epoch;
        ancestry->valid = true;
        return;
    }
    assert(false);
}

static void mark_upstream_ancestry_gateway_route_seq(
    struct mesh_relay *relay,
    uint64_t next_hop_id,
    uint32_t gateway_route_seq)
{
    assert(relay != NULL);
    assert(relay->anchor_downlink_store != NULL);
    assert(gateway_route_seq != 0u);

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        struct mesh_upstream_ancestry_entry *entry =
            &relay->anchor_downlink_store->upstream_ancestry[i];

        if (entry->valid && entry->next_hop_id == next_hop_id) {
            entry->gateway_route_seq = gateway_route_seq;
            return;
        }
    }
    assert(false);
}

static uint8_t route_candidate_index_for_next_hop(
    const struct mesh_relay *relay,
    uint64_t next_hop_id)
{
    assert(relay != NULL);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (relay->upstream.candidates[i].valid &&
            relay->upstream.candidates[i].next_hop_id == next_hop_id) {
            return i;
        }
    }
    assert(false);
    return ROUTE_NO_SELECTION;
}

static void test_route_reply_nonce_value_and_identity(void)
{
    const uint32_t session_id = UINT32_C(0x12345678);
    const uint32_t flood_epoch_id = UINT32_C(0xa5a55a5a);
    const uint16_t nonce = mesh_route_reply_nonce(ANCHOR_A,
                                                  GATEWAY,
                                                  session_id,
                                                  flood_epoch_id);

    assert(nonce == UINT16_C(0xfff7));
    assert(mesh_route_reply_nonce(ANCHOR_A,
                                  GATEWAY,
                                  session_id,
                                  flood_epoch_id) == nonce);
    assert(mesh_route_reply_nonce(ANCHOR_B,
                                  GATEWAY,
                                  session_id,
                                  flood_epoch_id) != nonce);
    assert(mesh_route_reply_nonce(ANCHOR_A,
                                  ANCHOR_B,
                                  session_id,
                                  flood_epoch_id) != nonce);
    assert(mesh_route_reply_nonce(ANCHOR_A,
                                  GATEWAY,
                                  session_id + 1u,
                                  flood_epoch_id) != nonce);
    assert(mesh_route_reply_nonce(ANCHOR_A,
                                  GATEWAY,
                                  session_id,
                                  flood_epoch_id + 1u) != nonce);
    assert(mesh_route_reply_nonce(0u, 0u, 1u, 1u) == 1u);
}

static void test_mesh_relay_result_preserves_required_simultaneous_outputs(void)
{
    struct mesh_relay_result result = {0};

    assert(sizeof(result) <= (3u * sizeof(struct mesh_outbound)) +
                             sizeof(struct operation_policy_set) + 40u);

    result.route_reply.packet.msg_type = MSG_ROUTE_REPLY;
    result.route_reply.packet.seq = 101u;
    result.route_reply_ack.packet.msg_type = MSG_ROUTE_REPLY_ACK;
    result.route_reply_ack.packet.seq = 102u;
    assert(result.route_reply.packet.msg_type == MSG_ROUTE_REPLY);
    assert(result.route_reply.packet.seq == 101u);
    assert(result.route_reply_ack.packet.msg_type == MSG_ROUTE_REPLY_ACK);
    assert(result.route_reply_ack.packet.seq == 102u);

    memset(&result, 0, sizeof(result));
    result.busy.packet.msg_type = MSG_RELAY_BUSY;
    result.busy.packet.seq = 201u;
    result.route_reply_ack.packet.msg_type = MSG_ROUTE_REPLY_ACK;
    result.route_reply_ack.packet.seq = 202u;
    assert(result.busy.packet.msg_type == MSG_RELAY_BUSY);
    assert(result.busy.packet.seq == 201u);
    assert(result.route_reply_ack.packet.msg_type == MSG_ROUTE_REPLY_ACK);
    assert(result.route_reply_ack.packet.seq == 202u);
}

static int tlv_present(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    return tlv_find(payload, payload_len, type, &value, &value_len);
}

static struct mesh_downlink_entry *test_downlink_at_mutable(
    struct mesh_relay *relay,
    size_t index)
{
    assert(relay != NULL);
    assert(index < mesh_relay_downlink_capacity(relay));
    if (index < MESH_RELAY_DOWNLINK_ROUTES) {
        return &relay->downlinks[index];
    }
    assert(relay->anchor_downlink_store != NULL);
    return &relay->anchor_downlink_store->entries[
        index - MESH_RELAY_DOWNLINK_ROUTES];
}

static void seed_downlink(struct mesh_relay *relay,
                          uint64_t target_id,
                          uint64_t next_hop_id,
                          uint32_t epoch,
                          uint8_t hop_count,
                          uint8_t quality,
                          uint32_t now_ms)
{
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(relay); i++) {
        struct mesh_downlink_entry *entry = test_downlink_at_mutable(relay, i);

        if (entry->valid) {
            continue;
        }
        entry->target_id = target_id;
        entry->next_hop_id = next_hop_id;
        entry->gateway_id = GATEWAY;
        entry->route_epoch = epoch;
        entry->last_seen_ms = now_ms;
        entry->hop_count = hop_count;
        entry->quality = quality;
        entry->valid = true;
        return;
    }

    assert(false);
}

static void assert_rx_rejected_without_relay_mutation(
    struct mesh_relay *relay,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct mesh_relay before = *relay;
    struct mesh_relay_result result;

    assert(mesh_relay_handle_rx(relay,
                                packet,
                                payload,
                                payload_len,
                                previous_hop_id,
                                80u,
                                3000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(memcmp(relay, &before, sizeof(*relay)) == 0);
}

static uint8_t channel9_timing_count(const struct mesh_relay *relay)
{
    uint8_t count = 0u;

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid) {
            count++;
        }
    }
    return count;
}

static bool channel9_timing_present(const struct mesh_relay *relay, uint64_t peer_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (relay->event_timings[i].valid &&
            relay->event_timings[i].next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static uint32_t require_tlv_u32(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    return proto_get_u32_le(value);
}

static uint16_t require_tlv_u16(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    return proto_get_u16_le(value);
}

static const uint8_t *require_tlv_bytes(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint8_t expected_len)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find_unique(payload,
                           payload_len,
                           type,
                           &value,
                           &value_len) == PROTO_OK);
    assert(value_len == expected_len);
    return value;
}

static uint8_t require_tlv_u8(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint8_t));
    return value[0];
}

static uint64_t require_tlv_u64(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    return proto_get_u64_le(value);
}

static void assert_command_result_id_equal(const struct command_result_id *actual,
                                           const struct command_result_id *expected)
{
    assert(actual->gateway_id == expected->gateway_id);
    assert(actual->gateway_epoch == expected->gateway_epoch);
    assert(actual->command_seq == expected->command_seq);
    assert(actual->node_id == expected->node_id);
    assert(actual->node_boot_counter == expected->node_boot_counter);
    assert(actual->result_seq == expected->result_seq);
}

static struct command_result_id deliberate_crc16_collision_result_id(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x22334456u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 25u,
        .result_seq = 26u,
    };

    return result_id;
}

static void build_deliberate_crc16_collision_results(
    uint8_t *payload_a,
    uint8_t *payload_b,
    size_t payload_cap,
    bool include_collection_epoch,
    size_t *payload_len)
{
    static const uint8_t suffix_a[] = {0x00u, 0x00u, 0x01u};
    static const uint8_t suffix_b[] = {0x23u, 0x14u, 0x00u};
    const struct command_result_id result_id =
        deliberate_crc16_collision_result_id();
    uint8_t digest_a[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t digest_b[SEMANTIC_DIGEST_SHA256_LEN];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;

    assert(payload_a != NULL);
    assert(payload_b != NULL);
    assert(payload_len != NULL);
    assert(command_result_id_append_tlvs(payload_a,
                                         payload_cap,
                                         &payload_a_len,
                                         &result_id) == PROTO_OK);
    assert(command_result_id_append_tlvs(payload_b,
                                         payload_cap,
                                         &payload_b_len,
                                         &result_id) == PROTO_OK);
    assert(mesh_append_command_result(payload_a,
                                      payload_cap,
                                      &payload_a_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(mesh_append_command_result(payload_b,
                                      payload_cap,
                                      &payload_b_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    if (include_collection_epoch) {
        assert(tlv_append_u32(payload_a,
                              payload_cap,
                              &payload_a_len,
                              TLV_COLLECTION_EPOCH_ID,
                              UINT32_C(0x10203040)) == PROTO_OK);
        assert(tlv_append_u32(payload_b,
                              payload_cap,
                              &payload_b_len,
                              TLV_COLLECTION_EPOCH_ID,
                              UINT32_C(0x10203040)) == PROTO_OK);
    }
    assert(tlv_append_bytes(payload_a,
                            payload_cap,
                            &payload_a_len,
                            TLV_ERROR_DETAIL,
                            suffix_a,
                            sizeof(suffix_a)) == PROTO_OK);
    assert(tlv_append_bytes(payload_b,
                            payload_cap,
                            &payload_b_len,
                            TLV_ERROR_DETAIL,
                            suffix_b,
                            sizeof(suffix_b)) == PROTO_OK);
    assert(payload_a_len == payload_b_len);
    assert(proto_crc16_ccitt_false(payload_a, payload_a_len) ==
           proto_crc16_ccitt_false(payload_b, payload_b_len));
    assert(memcmp(payload_a, payload_b, payload_a_len) != 0);
    assert(semantic_digest_sha256(payload_a, payload_a_len, digest_a));
    assert(semantic_digest_sha256(payload_b, payload_b_len, digest_b));
    assert(!semantic_digest_equal(digest_a, digest_b, sizeof(digest_a)));
    *payload_len = payload_a_len;
}

static uint32_t expected_outbox_packet_id(const struct proto_packet *packet)
{
    uint8_t identity[sizeof(uint32_t) + sizeof(uint16_t)];
    uint16_t crc;

    proto_put_u32_le(&identity[0], packet->session_id);
    proto_put_u16_le(&identity[sizeof(uint32_t)], packet->seq);
    crc = proto_crc16_ccitt_false(identity, sizeof(identity));
    return ((uint32_t)crc << 16) | packet->seq;
}

static uint32_t expected_mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static uint32_t expected_collection_retry_seed(uint64_t node_id,
                                               uint32_t command_seq,
                                               uint32_t collection_epoch_id,
                                               uint8_t retry_round)
{
    uint32_t seed = command_seq ^ collection_epoch_id;

    seed ^= (uint32_t)node_id;
    seed ^= (uint32_t)(node_id >> 32);
    seed ^= (uint32_t)retry_round << 24;
    return expected_mix32(seed);
}

static void assert_outbox_tracks_packet(const struct mesh_relay *relay,
                                        const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint32_t created_uptime_ms,
                                        enum mesh_relay_delivery_state delivery_state)
{
    const struct mesh_outbox_record *record = &relay->outbox_record;

    assert(record->valid);
    assert(record->delivery_state == delivery_state);
    assert(record->packet_id == expected_outbox_packet_id(packet));
    assert(record->session_id == packet->session_id);
    assert(record->seq == packet->seq);
    assert(record->gateway_id == GATEWAY);
    assert(record->packet_class == packet->msg_type);
    assert(record->created_uptime_ms == created_uptime_ms);
    assert(record->age_ms_saturating == packet->message_age_ms);
    assert(record->retry_round == 0u);
    assert(record->selected_parent_index != UINT8_MAX);
    assert(!record->custody_accepted);
    assert(record->custody_parent == 0u);
    assert(!record->gateway_acked);
    assert(record->expiry_s == COMMAND_RESULT_EXPIRY_DEFAULT_S);
    assert(record->payload_crc == proto_crc16_ccitt_false(payload, payload_len));
    assert(record->payload_len == payload_len);
}

static void build_identity_command_result_payload(uint8_t *payload,
                                                  size_t payload_cap,
                                                  size_t target_len,
                                                  const struct command_result_id *result_id,
                                                  size_t *payload_len)
{
    uint8_t padding[24];

    assert(payload != NULL);
    assert(result_id != NULL);
    assert(payload_len != NULL);
    assert(target_len <= payload_cap);

    *payload_len = 0u;
    assert(command_result_id_append_tlvs(payload,
                                         payload_cap,
                                         payload_len,
                                         result_id) == PROTO_OK);
    assert(mesh_append_command_result(payload,
                                      payload_cap,
                                      payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    memset(padding, 0xA5, sizeof(padding));
    while (*payload_len < target_len) {
        size_t remaining = target_len - *payload_len;
        uint8_t chunk_len = remaining > sizeof(padding) + 2u ?
                            (uint8_t)sizeof(padding) :
                            (uint8_t)(remaining > 2u ? remaining - 2u : 0u);

        assert(tlv_append_bytes(payload,
                                payload_cap,
                                payload_len,
                                TLV_MESH_TEST_PADDING,
                                padding,
                                chunk_len) == PROTO_OK);
    }
    assert(*payload_len == target_len);
}

static void build_collection_command_result_payload(uint8_t *payload,
                                                    size_t payload_cap,
                                                    size_t target_len,
                                                    const struct command_result_id *result_id,
                                                    uint32_t collection_epoch_id,
                                                    size_t *payload_len)
{
    build_identity_command_result_payload(payload,
                                          payload_cap,
                                          target_len,
                                          result_id,
                                          payload_len);
    assert(tlv_append_u32(payload,
                          payload_cap,
                          payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          collection_epoch_id) == PROTO_OK);
}

static void decode_outbound_over_uwb(const struct mesh_outbound *out,
                                     uint64_t sender_id,
                                     uint64_t receiver_id,
                                     struct proto_packet *packet,
                                     uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len,
                                     uint64_t *previous_hop_id)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;

    assert(out != NULL);
    assert(packet != NULL);
    assert(payload != NULL);
    assert(payload_len != NULL);
    assert(previous_hop_id != NULL);
    assert(uwb_mesh_frame_encode(NETWORK_ID,
                                 sender_id,
                                 out->next_hop_id,
                                 &out->packet,
                                 out->payload,
                                 frame,
                                 sizeof(frame),
                                 &frame_len) == PROTO_OK);
    assert(uwb_mesh_frame_decode(frame,
                                 frame_len,
                                 NETWORK_ID,
                                 receiver_id,
                                 previous_hop_id,
                                 packet,
                                 payload,
                                 payload_cap,
                                 payload_len) == PROTO_OK);
    assert(*previous_hop_id == sender_id);
    assert(packet->payload_len == *payload_len);
}

static void test_relay_replays_hop_ack_for_accepted_gateway_report_duplicate(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report = {0};
    struct mesh_relay_result result;
    struct range_report_fields fields = {
        .clicker_id = ANCHOR_C,
        .anchor_id = ANCHOR_A,
        .event_seq = 42u,
        .timestamp_ms = 1000u,
        .distance_mm = 1200,
        .quality = 90u,
        .range_status = RANGE_OK,
        .omit_rsl = true,
        .omit_cir = true,
    };
    uint8_t payload[96];
    uint8_t conflicting_payload[96];
    size_t payload_len = 0u;
    size_t conflicting_payload_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(report_append_range_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &fields) == PROTO_OK);
    fields.distance_mm++;
    assert(report_append_range_tlvs(conflicting_payload,
                                    sizeof(conflicting_payload),
                                    &conflicting_payload_len,
                                    &fields) == PROTO_OK);
    assert(conflicting_payload_len == payload_len);
    assert(report_init_range_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    proto_click_report_session_id(ANCHOR_C,
                                                                  42u),
                                    9u,
                                    FLAG_DIAGNOSTIC,
                                    (uint16_t)payload_len) == PROTO_OK);
    report.message_age_ms = 73u;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                95u,
                                2000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(result.forward.next_hop_id == GATEWAY);
    assert(result.forward.packet.src_id == ANCHOR_A);
    assert(result.forward.packet.dst_id == GATEWAY);
    assert(result.forward.packet.session_id == report.session_id);
    assert(result.forward.packet.seq == report.seq);
    assert(result.forward.packet.ttl == REPORT_DEFAULT_TTL - 1u);
    assert(result.forward.packet.message_age_ms == 73u);
    assert(result.forward.queued_at_ms == 2000u);
    assert(result.forward.queued_at_valid);
    assert(result.forward.payload_len == payload_len);
    assert(memcmp(result.forward.payload, payload, payload_len) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                95u,
                                2001u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(result.hop_ack.next_hop_id == ANCHOR_A);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                conflicting_payload,
                                conflicting_payload_len,
                                ANCHOR_A,
                                95u,
                                2002u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
}

static void test_range_retry_attempts_do_not_conflict_in_relay_dedup(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet attempt_one = {0};
    struct proto_packet attempt_two = {0};
    struct proto_packet aliased_attempt_two;
    struct mesh_relay_result result;
    struct range_report_fields fields = {
        .clicker_id = ANCHOR_C,
        .anchor_id = ANCHOR_A,
        .event_seq = UINT32_C(0x010203),
        .timestamp_ms = 1000u,
        .distance_mm = 1200,
        .quality = 90u,
        .range_status = RANGE_OK,
        .attempt_index = 1u,
        .detection_source = DETECTION_SOURCE_UWB_WAKE_CLAIM,
        .detection_attempt_present = true,
        .burst_id = UINT32_C(0x01020301),
        .burst_id_present = true,
        .omit_rsl = true,
        .omit_cir = true,
    };
    uint8_t payload_one[128];
    uint8_t payload_two[128];
    size_t payload_one_len = 0u;
    size_t payload_two_len = 0u;
    uint16_t attempt_one_seq = 0u;
    uint16_t attempt_two_seq = 0u;

    assert(report_append_range_tlvs(payload_one,
                                    sizeof(payload_one),
                                    &payload_one_len,
                                    &fields) == PROTO_OK);
    fields.attempt_index = 2u;
    fields.burst_id = UINT32_C(0x01020302);
    assert(report_append_range_tlvs(payload_two,
                                    sizeof(payload_two),
                                    &payload_two_len,
                                    &fields) == PROTO_OK);
    assert(report_range_transport_seq(1u, 0u, &attempt_one_seq) == PROTO_OK);
    assert(report_range_transport_seq(2u, 0u, &attempt_two_seq) == PROTO_OK);
    assert(attempt_two_seq ==
           attempt_one_seq + RANGE_REPORT_MAX_TRANSPORT_FRAGMENTS);

    assert(report_init_range_packet(&attempt_one,
                                    ANCHOR_A,
                                    GATEWAY,
                                    proto_click_report_session_id(
                                        ANCHOR_C, UINT32_C(0x010203)),
                                    attempt_one_seq,
                                    FLAG_DIAGNOSTIC,
                                    (uint16_t)payload_one_len) == PROTO_OK);
    assert(report_init_range_packet(&attempt_two,
                                    ANCHOR_A,
                                    GATEWAY,
                                    proto_click_report_session_id(
                                        ANCHOR_C, UINT32_C(0x010203)),
                                    attempt_two_seq,
                                    FLAG_DIAGNOSTIC,
                                    (uint16_t)payload_two_len) == PROTO_OK);
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &attempt_one,
                                payload_one,
                                payload_one_len,
                                ANCHOR_A,
                                95u,
                                2100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    /*
     * This is the old producer behavior: a different retry payload under the
     * first attempt's header sequence is correctly rejected as a conflict.
     */
    aliased_attempt_two = attempt_two;
    aliased_attempt_two.seq = attempt_one.seq;
    assert(mesh_relay_handle_rx(&relay,
                                &aliased_attempt_two,
                                payload_two,
                                payload_two_len,
                                ANCHOR_A,
                                95u,
                                2101u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);

    assert(mesh_relay_handle_rx(&relay,
                                &attempt_two,
                                payload_two,
                                payload_two_len,
                                ANCHOR_A,
                                95u,
                                2102u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.packet.seq == attempt_two_seq);
}

static void test_pending_retry_requires_exact_payload(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report = {0};
    struct mesh_outbound tracked;
    struct mesh_pending_tx pending_before;
    struct mesh_relay_result result;
    struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 43u,
        .battery_mv = 3000u,
    };
    uint8_t payload[64];
    uint8_t mutated_payload[64];
    size_t payload_len = 0u;
    size_t mutated_payload_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    fields.battery_mv++;
    assert(report_append_self_test_tlvs(mutated_payload,
                                        sizeof(mutated_payload),
                                        &mutated_payload_len,
                                        &fields) == PROTO_OK);
    assert(mutated_payload_len == payload_len);
    assert(report_init_self_test_packet(&report,
                                        ANCHOR_A,
                                        GATEWAY,
                                        43u,
                                        43u,
                                        (uint8_t)payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                95u,
                                2200u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_start_tx(&relay,
                               &result.forward.packet,
                               result.forward.payload,
                               result.forward.payload_len,
                               2201u,
                               &tracked) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    pending_before = relay.pending;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                95u,
                                2202u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                mutated_payload,
                                mutated_payload_len,
                                ANCHOR_A,
                                95u,
                                2203u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
}

static void test_duplicate_cache_expires_by_time_window(void)
{
    struct mesh_relay relay;
    struct proto_packet packet;
    struct mesh_relay_result result;
    uint8_t payload[8];
    size_t payload_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&packet,
                             GATEWAY,
                             ANCHOR_B,
                             55u,
                             7u,
                             (uint8_t)payload_len) == PROTO_OK);
    packet.ttl = gateway_command_origin_ttl(CMD_GET_STATUS);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(mesh_relay_commit_anchor_command_delivery(&relay,
                                                     &packet,
                                                     payload,
                                                     payload_len,
                                                     1000u) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                1001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                61001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
}

static void test_channel9_tx_plans_next_local_tx_slot_without_mutating_timing(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_timing before;
    struct mesh_event_timing unobserved_rx_commit;
    struct mesh_event_timing missed_rx_commit;
    struct mesh_event_timing observed_rx_commit;
    struct mesh_event_params params = channel9_params(1000u);
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_plan plan = {0};
    struct mesh_event_plan ordinary_plan = {0};
    const struct mesh_relay_event_timing_entry *entry;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&timing, false);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    before = relay.event_timings[0].timing;

    /* The current RX event is still owned by the receiver, but the TX caller
     * gets an exact wait plan for the following alternating event. */
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1094u,
                                                &plan) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);
    assert(plan.start_ms == 1100u);
    assert_event_timing_equal(&relay.event_timings[0].timing, &before);

    /* Reaching the selected event's guard makes that same shadow event
     * runnable while the authoritative RX timing remains untouched. */
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1095u,
                                                &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 1100u);
    assert_event_timing_equal(&relay.event_timings[0].timing, &before);

    /* A sender can reach the planned TX without a separate RX callback. The
     * commit accounts for the fully elapsed RX and the TX exactly once. */
    mesh_relay_note_channel9_unobserved_turn(&relay, GATEWAY, plan.start_ms);
    entry = find_event_timing(&relay, GATEWAY);
    assert(entry != NULL);
    assert(entry->timing.event_counter == before.event_counter + 2u);
    assert(entry->timing.next_event_time_ms == 1200u);
    assert(mesh_event_timing_local_rx_slot(&entry->timing));
    unobserved_rx_commit = entry->timing;

    /* Once the receiver records a miss, the same planned TX event is
     * immediately runnable and is not skipped to a later parity slot. */
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    mesh_relay_note_channel9_missed(&relay, GATEWAY, NULL);
    entry = find_event_timing(&relay, GATEWAY);
    assert(entry != NULL);
    assert(entry->timing.event_counter == before.event_counter + 1u);
    assert(entry->timing.next_event_time_ms == 1100u);
    assert(mesh_event_timing_local_tx_slot(&entry->timing));
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1095u,
                                                &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 1100u);
    mesh_relay_note_channel9_unobserved_turn(&relay, GATEWAY, plan.start_ms);
    entry = find_event_timing(&relay, GATEWAY);
    assert(entry != NULL);
    missed_rx_commit = entry->timing;
    assert(unobserved_rx_commit.missed_event_count == 0u);
    assert(missed_rx_commit.missed_event_count == 1u);
    unobserved_rx_commit.missed_event_count =
        missed_rx_commit.missed_event_count;
    assert_event_timing_equal(&missed_rx_commit, &unobserved_rx_commit);

    /* An observed RX advances to the identical local-TX event as a miss. */
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    mesh_relay_note_channel9_rx(&relay, GATEWAY, 1000u, 1005u);
    entry = find_event_timing(&relay, GATEWAY);
    assert(entry != NULL);
    assert(entry->timing.event_counter == before.event_counter + 1u);
    assert(entry->timing.next_event_time_ms == 1100u);
    assert(mesh_event_timing_local_tx_slot(&entry->timing));
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1095u,
                                                &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 1100u);
    mesh_relay_note_channel9_unobserved_turn(&relay, GATEWAY, plan.start_ms);
    entry = find_event_timing(&relay, GATEWAY);
    assert(entry != NULL);
    observed_rx_commit = entry->timing;
    assert(observed_rx_commit.event_counter == before.event_counter + 2u);
    assert(observed_rx_commit.next_event_time_ms == 1200u);
    assert(mesh_event_timing_local_rx_slot(&observed_rx_commit));

    /* Observing RX rather than missing it differs only in the intentional
     * missed-event health counter; the cadence and every other field match. */
    assert(observed_rx_commit.missed_event_count == 0u);
    assert(missed_rx_commit.missed_event_count == 1u);
    observed_rx_commit.missed_event_count = missed_rx_commit.missed_event_count;
    assert_event_timing_equal(&observed_rx_commit, &missed_rx_commit);

    /* Channel-5 active-until policy is evaluated at the selected local-TX
     * event. Activity which covered RX but ended before TX must not defer it. */
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    requirements.active_until_valid = true;
    requirements.active_until_ms = 1050u;
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1095u,
                                                &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);

    /* A required scan between the retained RX and selected TX rejects TX,
     * proving that scan overlap also uses the selected event's timestamp. */
    requirements = clear_channel5_requirements();
    requirements.next_required_scan_start_valid = true;
    requirements.next_required_scan_start_ms = 1060u;
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1095u,
                                                &plan) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD);
    assert(plan.start_ms == 1100u);
    assert_event_timing_equal(&relay.event_timings[0].timing, &before);
    requirements = clear_channel5_requirements();

    /* Unsigned uptime wrap preserves the adjacent event and immutable shared
     * cadence just as it does away from wrap. */
    params = channel9_params(UINT32_MAX - 49u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&timing, false);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    before = relay.event_timings[0].timing;
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                UINT32_MAX - 10u,
                                                &plan) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);
    assert(plan.start_ms == 50u);
    assert_event_timing_equal(&relay.event_timings[0].timing, &before);
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                45u,
                                                &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 50u);
    assert_event_timing_equal(&relay.event_timings[0].timing, &before);

    /* A currently runnable local-TX event remains the selected event. */
    params = channel9_params(2000u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&timing, true);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             1995u,
                                             &ordinary_plan) == PROTO_OK);
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1995u,
                                                &plan) == PROTO_OK);
    assert(plan.action == ordinary_plan.action);
    assert(plan.start_ms == ordinary_plan.start_ms);
    assert(plan.end_ms == ordinary_plan.end_ms);
    assert(plan.window_ms == ordinary_plan.window_ms);
}

static void test_status_tlvs_report_selected_route(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 44u, 77u);
    uint8_t payload[128];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 44u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(route_record_failure_at(&relay.upstream,
                                   ROUTE_FAILURE_GATEWAY_ACK,
                                   1000u) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream,
                                   ROUTE_FAILURE_GATEWAY_ACK,
                                   1100u) == ROUTE_DELIVERY_RETRY_CURRENT);
    relay.duplicates[0].valid = true;
    relay.duplicates[1].valid = true;
    relay.result_bundle.active = true;
    relay.result_bundle.record_count = 2u;
    relay.outbox_record.valid = true;
    relay.outbox_record.delivery_state = MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK;
    relay.route_discovery.attempts = 3u;
    relay.diagnostics.flood_suppression_count = 4u;
    mesh_relay_note_route_reply_retry(&relay);
    mesh_relay_note_route_reply_retry(&relay);
    relay.diagnostics.busy_response_count = 5u;
    relay.upstream.candidates[relay.upstream.selected_index].hold_down_until_ms = 12345u;
    relay.upstream.candidates[relay.upstream.selected_index].hold_down_valid = true;

    assert(mesh_relay_append_status_tlvs(&relay, payload, sizeof(payload), &payload_len) == PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_GATEWAY_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    assert(proto_get_u64_le(value) == GATEWAY);

    assert(tlv_find(payload, payload_len, TLV_NEXT_HOP_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    assert(proto_get_u64_le(value) == ANCHOR_B);

    assert(tlv_find(payload, payload_len, TLV_ROUTE_EPOCH, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    assert(proto_get_u32_le(value) == 44u);

    assert(tlv_find(payload, payload_len, TLV_HOP_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 2u);

    assert(tlv_find(payload, payload_len, TLV_QUALITY, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 77u);

    assert(tlv_find(payload, payload_len, TLV_RETRY_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 2u);

    assert(tlv_find(payload, payload_len, TLV_MESH_DUPLICATE_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 2u);

    assert(tlv_find(payload, payload_len, TLV_COLLECTION_PENDING_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 3u);

    assert(tlv_find(payload, payload_len, TLV_PARENT_HOLDDOWN_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 1u);

    assert(tlv_find(payload, payload_len, TLV_ROUTE_DISCOVERY_ATTEMPTS, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 3u);

    assert(tlv_find(payload, payload_len, TLV_OUTBOX_DELIVERY_STATE, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(tlv_find(payload, payload_len, TLV_FLOOD_SUPPRESSION_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 4u);

    assert(tlv_find(payload, payload_len, TLV_ROUTE_REPLY_RETRY_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 2u);

    assert(tlv_find(payload, payload_len, TLV_BUSY_RESPONSE_COUNT, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 5u);
}

static void test_status_tlvs_report_missing_route_reason(void)
{
    struct mesh_relay relay;
    uint8_t payload[48];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 44u);

    assert(mesh_relay_append_status_tlvs(&relay, payload, sizeof(payload), &payload_len) == PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_GATEWAY_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint64_t));
    assert(proto_get_u64_le(value) == GATEWAY);

    assert(tlv_find(payload, payload_len, TLV_REASON, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == (uint8_t)(-PROTO_ERR_NOT_FOUND));
    assert(tlv_find(payload, payload_len, TLV_NEXT_HOP_ID, &value, &value_len) == PROTO_ERR_NOT_FOUND);
}

static void test_legacy_route_beacons_are_dropped(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = 0x33u,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 12u,
        .seq = 3u,
        .ttl = 1u,
        .payload_len = 0u,
    };

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                GATEWAY,
                                80u,
                                3000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(route_selected(&relay.upstream) == NULL);

    packet.msg_type = 0x34u;
    packet.dst_id = relay.local_id;
    packet.seq = 4u;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                GATEWAY,
                                80u,
                                3010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}



static void test_malformed_broadcast_command_fails_closed(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    uint8_t payload[32];
    size_t payload_len = 0u;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0x12345679u,
        .seq = 12u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_PING) == PROTO_OK);
    packet.payload_len = (uint8_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3020u,
                                            3u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_gateway_command_envelope_rejects_before_state_mutation(void)
{
    struct mesh_relay relay;
    struct proto_packet invalid;
    const struct proto_packet broadcast = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0x12345680u,
        .seq = 15u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    const struct proto_packet targeted = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = ANCHOR_C,
        .session_id = 0x12345681u,
        .seq = 16u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t broadcast_payload[48];
    uint8_t targeted_payload[8];
    size_t broadcast_payload_len = 0u;
    size_t targeted_payload_len = 0u;

    assert(mesh_append_command_id(broadcast_payload,
                                  sizeof(broadcast_payload),
                                  &broadcast_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(broadcast_payload,
                         sizeof(broadcast_payload),
                         &broadcast_payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(broadcast_payload,
                         sizeof(broadcast_payload),
                         &broadcast_payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(broadcast_payload,
                          sizeof(broadcast_payload),
                          &broadcast_payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(broadcast_payload,
                          sizeof(broadcast_payload),
                          &broadcast_payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(mesh_append_command_id(targeted_payload,
                                  sizeof(targeted_payload),
                                  &targeted_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    13u);
    invalid = broadcast;
    invalid.payload_len = (uint16_t)broadcast_payload_len;
    invalid.src_id = ANCHOR_B;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        broadcast_payload,
        broadcast_payload_len,
        GATEWAY);

    invalid = broadcast;
    invalid.payload_len = (uint16_t)broadcast_payload_len;
    invalid.flags = FLAG_GATEWAY_ACK_REQUIRED;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        broadcast_payload,
        broadcast_payload_len,
        GATEWAY);

    invalid = broadcast;
    invalid.payload_len = (uint16_t)broadcast_payload_len;
    invalid.ttl = 0u;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        broadcast_payload,
        broadcast_payload_len,
        GATEWAY);

    invalid = broadcast;
    invalid.payload_len = (uint16_t)broadcast_payload_len;
    invalid.ttl = FLOOD_EPOCH_GLOBAL_TTL + 1u;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        broadcast_payload,
        broadcast_payload_len,
        GATEWAY);

    invalid = broadcast;
    invalid.payload_len = (uint16_t)broadcast_payload_len;
    invalid.ttl = FLOOD_EPOCH_GLOBAL_TTL - 1u;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        broadcast_payload,
        broadcast_payload_len,
        GATEWAY);

    invalid = broadcast;
    invalid.payload_len = (uint16_t)broadcast_payload_len;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        broadcast_payload,
        broadcast_payload_len,
        ANCHOR_B);

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    13u);
    seed_downlink(&relay, ANCHOR_C, ANCHOR_B, 13u, 2u, 80u, 2000u);
    invalid = targeted;
    invalid.payload_len = (uint16_t)targeted_payload_len;
    invalid.src_id = ANCHOR_D;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        targeted_payload,
        targeted_payload_len,
        GATEWAY);

    invalid = targeted;
    invalid.payload_len = (uint16_t)targeted_payload_len;
    invalid.flags = FLAG_ERROR;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        targeted_payload,
        targeted_payload_len,
        GATEWAY);

    invalid = targeted;
    invalid.payload_len = (uint16_t)targeted_payload_len;
    invalid.ttl = FLOOD_EPOCH_GLOBAL_TTL - 1u;
    assert_rx_rejected_without_relay_mutation(
        &relay,
        &invalid,
        targeted_payload,
        targeted_payload_len,
        GATEWAY);
}


static void test_group_scope_broadcast_fails_closed_before_local_delivery(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    uint8_t payload[32];
    size_t payload_len = 0u;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 13u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_GROUP) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3020u,
                                            3u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_targeted_gateway_command_commits_only_after_local_admission(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = ANCHOR_A,
        .session_id = 1002u,
        .seq = 14u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[8];
    size_t payload_len = 0u;

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                1000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));

    /*
     * Parsing and local delivery do not prove result custody. If the
     * application cannot reserve a response slot, the exact transport retry
     * must reach local admission again.
     */
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                1001u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));

    assert(mesh_relay_commit_anchor_command_delivery(&relay,
                                                     &packet,
                                                     payload,
                                                     payload_len,
                                                     1001u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                1002u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}

static void test_command_flood_broadcast_deduplicates_logical_sequence(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result admission;
    struct mesh_relay_result result;
    uint8_t payload[96];
    uint8_t conflicting_payload[96];
    uint8_t next_payload[96];
    const uint8_t *command_seq_value = NULL;
    const uint8_t *flood_epoch_value = NULL;
    uint8_t command_seq_len = 0u;
    uint8_t flood_epoch_len = 0u;
    size_t payload_len = 0u;
    struct proto_packet duplicate_packet;
    struct proto_packet next_packet;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 13u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          12u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);
    packet.payload_len = (uint8_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3020u,
                                            3u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.msg_type == MSG_COMMAND);
    assert(result.forward.packet.dst_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL - 1u);
    assert(result.forward.queued_at_ms == 3020u);
    assert(result.forward.queued_at_valid);
    assert(result.forward.earliest_tx_valid);
    assert(result.forward.earliest_tx_ms >= 3020u + 1800u);
    assert(result.forward.earliest_tx_ms <
           3020u + FLOOD_WAVE_MS + FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS);
    assert(result.forward.payload_len > payload_len);
    assert(memcmp(result.forward.payload, payload, payload_len) == 0);
    assert(require_tlv_u32(result.forward.payload,
                           result.forward.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_MAX_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS);
    assert(require_tlv_u16(result.forward.payload,
                           result.forward.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS);
    assert(require_tlv_u8(result.forward.payload,
                          result.forward.payload_len,
                          TLV_FLOOD_RETRY_COUNT) ==
           FLOOD_DEFAULT_RETRY_COUNT);
    admission = result;

    /*
     * Local admission retry is allowed only for a byte-for-byte semantic
     * replay of the current flood attempt. Reusing that attempt identity with
     * a different command must fail closed instead of executing locally while
     * forwarding remains suppressed.
     */
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3021u,
                                            4u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));

    memcpy(conflicting_payload, payload, payload_len);
    assert(tlv_find(conflicting_payload,
                    payload_len,
                    TLV_COMMAND_SEQ,
                    &command_seq_value,
                    &command_seq_len) == PROTO_OK);
    assert(command_seq_len == sizeof(uint32_t));
    proto_put_u32_le(
        &conflicting_payload[command_seq_value - conflicting_payload],
        1002u);
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            conflicting_payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3022u,
                                            5u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    /*
     * If the platform cannot transmit or retain the consequent reflood, it
     * rolls back transport admission. The next exact RF copy must reflood,
     * while the following copy is suppressed again after successful
     * admission.
     */
    assert(mesh_relay_rollback_forward_admission(&relay,
                                                  &packet,
                                                  payload,
                                                  payload_len,
                                                  &admission) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3023u,
                                            4u,
                                            &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3024u,
                                            5u,
                                            &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    memcpy(next_payload, payload, payload_len);
    assert(tlv_find(next_payload,
                    payload_len,
                    TLV_FLOOD_EPOCH_ID,
                    &flood_epoch_value,
                    &flood_epoch_len) == PROTO_OK);
    assert(flood_epoch_len == sizeof(uint32_t));
    proto_put_u32_le(&next_payload[flood_epoch_value - next_payload], 2003u);
    next_packet = packet;
    next_packet.seq = 14u;
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &next_packet,
                                            next_payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            3030u,
                                            5u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(require_tlv_u32(next_payload,
                           payload_len,
                           TLV_COMMAND_SEQ) == 1001u);
    assert(require_tlv_u32(next_payload,
                           payload_len,
                           TLV_FLOOD_EPOCH_ID) == 2003u);

    duplicate_packet = next_packet;
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                next_payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3040u,
                                &result) == PROTO_OK);
    /*
     * Flood suppression and semantic acceptance have different owners. The
     * relay must not re-flood an exact flood attempt, but it must let the anchor
     * retry local admission because an earlier copy may have found the
     * delayed-command slot busy and deliberately remained uncommitted.
    */
    assert(result.status == PROTO_ERR_STALE);
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    duplicate_packet.seq = 99u;
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                next_payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3041u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    /*
     * The relay forwarding window follows unsigned RFC 1982 ordering. Wrap
     * from UINT32_MAX to 1 is newer, the exact half-range is ambiguous and
     * fails closed, and the retained pre-wrap attempt remains stale.
     */
    relay.command_replay = (struct mesh_command_replay_window) {
        .forwarded = UINT64_C(1),
        .newest_command_seq = UINT32_MAX,
        .initialized = true,
    };
    proto_put_u32_le(
        &next_payload[flood_epoch_value - next_payload], 1u);
    next_packet.seq = 100u;
    assert(mesh_relay_handle_rx(&relay,
                                &next_packet,
                                next_payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3050u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(relay.command_replay.newest_command_seq == 1u);

    proto_put_u32_le(
        &next_payload[flood_epoch_value - next_payload],
        UINT32_C(0x80000001));
    next_packet.seq = 101u;
    assert(mesh_relay_handle_rx(&relay,
                                &next_packet,
                                next_payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3060u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(relay.command_replay.newest_command_seq == 1u);

    proto_put_u32_le(
        &next_payload[flood_epoch_value - next_payload], UINT32_MAX);
    next_packet.seq = 102u;
    assert(mesh_relay_handle_rx(&relay,
                                &next_packet,
                                next_payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3070u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(relay.command_replay.newest_command_seq == 1u);
}

static void test_scheduled_survey_controls_reuse_compact_relay_wave(void)
{
    const enum command_id scheduled_ids[] = {
        CMD_SURVEY_START,
        CMD_SURVEY_PLAN,
    };
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[64];

    for (size_t i = 0u;
         i < sizeof(scheduled_ids) / sizeof(scheduled_ids[0]);
         i++) {
        size_t payload_len = 0u;
        uint32_t now_ms = 6000u + (uint32_t)i * 1000u;

        packet.session_id = 7100u + (uint32_t)i;
        packet.seq = (uint16_t)(30u + i);
        assert(mesh_append_command_id(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      scheduled_ids[i]) == PROTO_OK);
        assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                             TLV_COMMAND_SCOPE,
                             CMD_SCOPE_ALL_HEARD) == PROTO_OK);
        assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                             TLV_COMMAND_RESPONSE_MODE,
                             CMD_RESPONSE_NONE) == PROTO_OK);
        assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                              TLV_COMMAND_SEQ,
                              packet.session_id) == PROTO_OK);
        assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                              TLV_FLOOD_EPOCH_ID,
                              8100u + (uint32_t)i) == PROTO_OK);
        packet.payload_len = (uint16_t)payload_len;

        mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                        ANCHOR_A, GATEWAY, 13u);
        assert(mesh_relay_handle_rx_with_random(&relay,
                                                &packet,
                                                payload,
                                                payload_len,
                                                GATEWAY,
                                                80u,
                                                now_ms,
                                                (uint32_t)(i + 5u),
                                                &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
        assert(result.forward.flood_retry_count ==
               MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
        assert(result.forward.earliest_tx_valid);
        assert(result.forward.earliest_tx_ms - now_ms >=
               DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS);
        assert(result.forward.earliest_tx_ms - now_ms <=
               DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS +
                   MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS);
    }
}

static void test_single_assignment_table_relays_three_copies_across_two_hops(void)
{
    struct mesh_relay relay_a;
    struct mesh_relay relay_b;
    struct mesh_relay direct_relays[3];
    struct mesh_relay_result result;
    struct mesh_outbound hop;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 7001u,
        .seq = 17u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_ASSIGN_DISCOVERY_SLOTS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          packet.session_id) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          8001u) == PROTO_OK);
    assert(discovery_assignment_append_control_tlvs(
               payload,
               sizeof(payload),
               &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_TABLE,
               9001u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    /* DDD: the three-copy gateway root burst gives each direct anchor one
     * idempotent three-copy relay owner after that root burst is clear. */
    mesh_relay_init(&direct_relays[0],
                    MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    mesh_relay_init(&direct_relays[1],
                    MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 13u);
    mesh_relay_init(&direct_relays[2],
                    MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 13u);
    for (size_t i = 0u; i < 3u; i++) {
        assert(mesh_relay_handle_rx_with_random(&direct_relays[i],
                                                &packet,
                                                payload,
                                                payload_len,
                                                GATEWAY,
                                                80u,
                                                4000u,
                                                (uint32_t)(i + 1u),
                                                &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
        assert(result.forward.flood_retry_count ==
               MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
        assert(1u + result.forward.flood_retry_count ==
               MESH_ENUMERATION_RELAY_COPY_COUNT);
        assert(result.forward.earliest_tx_valid);
        assert(result.forward.earliest_tx_ms ==
               4000u +
                   DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS +
                   mesh_enumeration_relay_delay_ms(
                           direct_relays[i].local_id, &packet));
        assert(result.forward.earliest_tx_ms - 4000u >=
               DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS);
        assert(result.forward.earliest_tx_ms - 4000u <=
               DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS +
                   MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS);
        {
            enum discovery_assignment_phase forwarded_phase;
            uint32_t forwarded_epoch = 0u;

            assert(discovery_assignment_extract_control_tlvs(
                       result.forward.payload,
                       result.forward.payload_len,
                       &forwarded_phase,
                       &forwarded_epoch) == PROTO_OK);
            assert(forwarded_phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE);
            assert(forwarded_epoch == 9001u);
        }
    }

    /* Forced hop: the exact same TABLE identity advances across two relays,
     * with each relay owning three physical copies. */
    mesh_relay_init(&relay_a, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    mesh_relay_init(&relay_b, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 13u);

    assert(mesh_relay_handle_rx_with_random(&relay_a,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            4000u,
                                            1u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    hop = result.forward;
    assert(hop.flood_retry_count ==
           MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
    assert(1u + hop.flood_retry_count ==
           MESH_ENUMERATION_RELAY_COPY_COUNT);
    assert(hop.earliest_tx_valid);
    assert(hop.earliest_tx_ms - 4000u >=
           DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS);
    assert(hop.earliest_tx_ms - 4000u <=
           DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS +
               MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS);
    assert(mesh_relay_handle_rx_with_random(&relay_b,
                                            &hop.packet,
                                            hop.payload,
                                            hop.payload_len,
                                            ANCHOR_A,
                                            70u,
                                            4010u,
                                            2u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.flood_retry_count ==
           MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
    assert(1u + result.forward.flood_retry_count ==
           MESH_ENUMERATION_RELAY_COPY_COUNT);
    assert(result.forward.earliest_tx_valid);
    assert(result.forward.earliest_tx_ms - 4010u >=
           DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS);
    assert(result.forward.earliest_tx_ms - 4010u <=
           DISCOVERY_ASSIGNMENT_UPSTREAM_COPY_BURST_REMAINDER_MS +
               MESH_ENUMERATION_RELAY_MAX_INITIAL_DELAY_MS);

    /* An exact duplicate of the one gateway origin is local-idempotent and
     * cannot create a second relay owner. */
    assert(mesh_relay_handle_rx(&relay_a,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                4020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    /* The separate CLAIM-bound ABORT has a fresh outer identity, but keeps
     * the same one-origin/three-copy relay contract. */
    payload_len = 0u;
    packet.session_id = 7002u;
    packet.seq = 18u;
    packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_ASSIGN_DISCOVERY_SLOTS) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_SEQ, packet.session_id) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_FLOOD_EPOCH_ID, 8002u) == PROTO_OK);
    assert(discovery_assignment_append_control_tlvs(
               payload, sizeof(payload), &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_ABORT, 9001u) == PROTO_OK);
    {
        const struct discovery_assignment_abort_identity abort_identity = {
            .epoch = 9001u,
            .claim_session_id = 7001u,
            .claim_command_seq = 7001u,
        };

        assert(discovery_assignment_append_abort_identity(
                   payload, sizeof(payload), &payload_len,
                   &abort_identity) == PROTO_OK);
    }
    packet.payload_len = (uint16_t)payload_len;
    mesh_relay_init(&relay_a, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(mesh_relay_handle_rx_with_random(&relay_a,
                                            &packet,
                                            payload,
                                            payload_len,
                                            GATEWAY,
                                            80u,
                                            5000u,
                                            4u,
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.flood_retry_count ==
           MESH_ENUMERATION_RELAY_COPY_COUNT - 1u);
    assert(1u + result.forward.flood_retry_count ==
           MESH_ENUMERATION_RELAY_COPY_COUNT);
    assert(result.forward.earliest_tx_valid);
}

static void test_collection_eack_broadcast_deduplicates_exact_round_only(void)
{
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .membership_epoch = 3u,
        .expected_count = 6u,
        .received_count = 5u,
        .packet_sequence = 2u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 1u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_0_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet duplicate_packet;
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 2u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[96];
    size_t fixed_payload_len;
    size_t payload_len = 0u;

    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    fixed_payload_len = payload_len;
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    duplicate_packet = packet;
    duplicate_packet.ttl = FLOOD_EPOCH_GLOBAL_TTL - 1u;
    assert_rx_rejected_without_relay_mutation(&relay,
                                              &duplicate_packet,
                                              payload,
                                              payload_len,
                                              GATEWAY);
    duplicate_packet = packet;
    duplicate_packet.payload_len = (uint16_t)fixed_payload_len;
    assert_rx_rejected_without_relay_mutation(&relay,
                                              &duplicate_packet,
                                              payload,
                                              fixed_payload_len,
                                              GATEWAY);
    duplicate_packet = packet;
    duplicate_packet.session_id++;
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3038u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));

    duplicate_packet = packet;
    duplicate_packet.seq++;
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3039u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3040u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.msg_type == MSG_GATEWAY_COLLECTION_EACK);
    assert(result.forward.packet.src_id == GATEWAY);
    assert(result.forward.packet.dst_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL - 1u);
    assert(result.forward.queued_at_ms == 3040u);
    assert(result.forward.queued_at_valid);
    assert(result.forward.earliest_tx_valid);
    assert(result.forward.earliest_tx_ms >= 3040u);
    assert(result.forward.earliest_tx_ms < 3040u + FLOOD_WAVE_MS);
    assert(result.forward.payload_len == payload_len);
    assert(memcmp(result.forward.payload, payload, payload_len) == 0);

    duplicate_packet = packet;
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3050u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    {
        struct gateway_collection_eack next_eack = eack;

        next_eack.retry_round = 2u;
        next_eack.packet_sequence = 3u;
        next_eack.next_retry_spread_ms = COLLECTION_RETRY_ROUND_1_MS;
        payload_len = 0u;
        assert(gateway_collection_eack_append_tlvs(payload,
                                                   sizeof(payload),
                                                   &payload_len,
                                                   &next_eack) == PROTO_OK);
        assert(tlv_append_u64(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_NODE_ID,
                              ANCHOR_B) == PROTO_OK);
        duplicate_packet.seq = 3u;
        duplicate_packet.payload_len = (uint16_t)payload_len;
    }
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3060u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
}

static void test_collection_eack_received_list_confirms_pending_result(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1001u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 61u,
        .result_seq = 62u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 1u,
        .packet_sequence = 23u,
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
        .retry_round = 1u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_0_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 23u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3003u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(eack_payload,
                          sizeof(eack_payload),
                          &eack_payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert_outbox_tracks_packet(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED);
    assert(relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static size_t build_recovery_eack_for_packet(
    uint8_t *payload,
    size_t payload_cap,
    struct proto_packet *eack_packet,
    const struct proto_packet *confirmed_packet,
    const uint8_t *confirmed_payload,
    size_t confirmed_payload_len,
    uint16_t gateway_epoch,
    uint32_t command_seq,
    uint32_t collection_epoch_id,
    uint32_t recovery_attempt_id,
    uint64_t claimed_packet_src_id)
{
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = gateway_epoch,
        .command_seq = command_seq,
        .collection_epoch_id = collection_epoch_id,
        .membership_epoch = gateway_epoch,
        .expected_count = 1u,
        .received_count = 1u,
        .packet_sequence = (uint16_t)recovery_attempt_id,
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
        .retry_round = 0u,
        .next_retry_spread_ms = 0u,
        .collection_open = false,
    };
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t payload_len = 0u;

    assert(confirmed_packet != NULL);
    assert(confirmed_payload != NULL);
    assert(semantic_digest_sha256(confirmed_payload,
                                  confirmed_payload_len,
                                  digest));
    assert(gateway_collection_eack_append_tlvs(payload,
                                               payload_cap,
                                               &payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u32(payload, payload_cap, &payload_len,
                          TLV_COLLECTION_RECOVERY_ATTEMPT_ID,
                          recovery_attempt_id) == PROTO_OK);
    assert(tlv_append_u64(payload, payload_cap, &payload_len,
                          TLV_NODE_ID, claimed_packet_src_id) == PROTO_OK);
    assert(tlv_append_u16(payload, payload_cap, &payload_len,
                          TLV_RESULT_SEQ, confirmed_packet->seq) == PROTO_OK);
    assert(tlv_append_u16(payload, payload_cap, &payload_len,
                          TLV_PAYLOAD_LEN,
                          (uint16_t)confirmed_payload_len) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            payload_cap,
                            &payload_len,
                            TLV_RESULT_SHA256_COMMITMENT,
                            digest,
                            sizeof(digest)) == PROTO_OK);
    *eack_packet = (struct proto_packet) {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = command_seq,
        .seq = eack.packet_sequence,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = (uint16_t)payload_len,
    };
    return payload_len;
}

static void test_recovery_eack_releases_only_exact_packet_and_bundle(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1001u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 61u,
        .result_seq = 62u,
    };
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct mesh_outbound tx;
    struct proto_packet result_packet;
    struct proto_packet eack_packet;
    uint8_t result_payload[128];
    uint8_t eack_payload[160];
    size_t result_payload_len = 0u;
    size_t eack_payload_len;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3003u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);

    eack_payload_len = build_recovery_eack_for_packet(
        eack_payload, sizeof(eack_payload), &eack_packet, &result_packet,
        result_payload, result_payload_len, 13u, result_id.command_seq, 3003u,
        9001u, ANCHOR_B);
    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));

    {
        struct proto_packet wrong_sequence = result_packet;

        wrong_sequence.seq++;
        eack_payload_len = build_recovery_eack_for_packet(
            eack_payload, sizeof(eack_payload), &eack_packet, &wrong_sequence,
            result_payload, result_payload_len, 13u, result_id.command_seq,
            3003u, 9002u, ANCHOR_A);
        assert(mesh_relay_handle_rx(&relay,
                                    &eack_packet,
                                    eack_payload,
                                    eack_payload_len,
                                    GATEWAY,
                                    80u,
                                    5101u,
                                    &result) == PROTO_OK);
        assert(mesh_relay_tx_active(&relay));
        assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    }
    eack_payload_len = build_recovery_eack_for_packet(
        eack_payload, sizeof(eack_payload), &eack_packet, &result_packet,
        result_payload, result_payload_len - 1u, 13u, result_id.command_seq,
        3003u, 9003u, ANCHOR_A);
    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5102u,
                                &result) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    {
        uint8_t wrong_payload[sizeof(result_payload)];

        memcpy(wrong_payload, result_payload, result_payload_len);
        wrong_payload[result_payload_len - 1u] ^= 0x01u;
        eack_payload_len = build_recovery_eack_for_packet(
            eack_payload, sizeof(eack_payload), &eack_packet, &result_packet,
            wrong_payload, result_payload_len, 13u, result_id.command_seq,
            3003u, 9004u, ANCHOR_A);
        assert(mesh_relay_handle_rx(&relay,
                                    &eack_packet,
                                    eack_payload,
                                    eack_payload_len,
                                    GATEWAY,
                                    80u,
                                    5103u,
                                    &result) == PROTO_OK);
        assert(mesh_relay_tx_active(&relay));
    }

    eack_payload_len = build_recovery_eack_for_packet(
        eack_payload, sizeof(eack_payload), &eack_packet, &result_packet,
        result_payload, result_payload_len, 13u, result_id.command_seq, 3003u,
        9005u, ANCHOR_A);
    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5110u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_CLOSED));
    assert(!mesh_relay_tx_active(&relay));

    {
        const struct command_result_id bundle_result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 13u,
            .command_seq = 1001u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 1u,
            .result_seq = 91u,
        };
        struct result_bundle_header bundle = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 13u,
            .command_seq = 1001u,
            .collection_epoch_id = 3003u,
            .bundle_id = 7u,
            .record_count = 1u,
        };
        struct result_bundle_record bundle_record = {0};
        struct proto_packet bundle_packet = {
            .msg_type = MSG_RESULT_BUNDLE,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = ANCHOR_A,
            .dst_id = GATEWAY,
            .session_id = 1001u,
            .seq = 91u,
            .ttl = MESH_DEFAULT_TTL,
        };
        uint8_t bundle_record_payload[96];
        uint8_t bundle_records[160];
        uint8_t bundle_payload[256];
        size_t bundle_record_payload_len = 0u;
        size_t bundle_records_len = 0u;
        size_t bundle_payload_len = 0u;

        build_collection_command_result_payload(
            bundle_record_payload,
            sizeof(bundle_record_payload),
            64u,
            &bundle_result_id,
            bundle.collection_epoch_id,
            &bundle_record_payload_len);
        bundle_record.result_id = bundle_result_id;
        bundle_record.payload_len = (uint16_t)bundle_record_payload_len;
        bundle_record.payload_crc = proto_crc16_ccitt_false(
            bundle_record_payload, bundle_record_payload_len);
        bundle_record.payload = bundle_record_payload;
        assert(result_bundle_record_append_tlv(bundle_records,
                                               sizeof(bundle_records),
                                               &bundle_records_len,
                                               &bundle_record) == PROTO_OK);
        bundle.bundle_crc =
            proto_crc16_ccitt_false(bundle_records, bundle_records_len);
        assert(result_bundle_header_append_tlvs(bundle_payload,
                                                sizeof(bundle_payload),
                                                &bundle_payload_len,
                                                &bundle) == PROTO_OK);
        assert(sizeof(bundle_payload) - bundle_payload_len >=
               bundle_records_len);
        memcpy(&bundle_payload[bundle_payload_len],
               bundle_records,
               bundle_records_len);
        bundle_payload_len += bundle_records_len;
        bundle_packet.payload_len = (uint16_t)bundle_payload_len;
        mesh_relay_init(&relay,
                        MESH_RELAY_ROLE_ANCHOR,
                        ANCHOR_A,
                        GATEWAY,
                        13u);
        assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
        assert(mesh_relay_start_tx(&relay,
                                   &bundle_packet,
                                   bundle_payload,
                                   bundle_payload_len,
                                   5200u,
                                   &tx) == PROTO_OK);
        eack_payload_len = build_recovery_eack_for_packet(
            eack_payload, sizeof(eack_payload), &eack_packet, &bundle_packet,
            bundle_payload, bundle_payload_len, 13u, bundle.command_seq,
            bundle.collection_epoch_id, 9006u, ANCHOR_A);
        assert(mesh_relay_handle_rx(&relay,
                                    &eack_packet,
                                    eack_payload,
                                    eack_payload_len,
                                    GATEWAY,
                                    80u,
                                    5210u,
                                    &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
        assert(!mesh_relay_tx_active(&relay));
    }
}

static void test_four_eack_broadcasts_cover_fifty_anchor_rosters(void)
{
    static const size_t anchor_counts[] = {2u, 6u, 16u, 32u, 50u};
    const uint64_t anchor_id_base = UINT64_C(0x1111222233300000);

    for (size_t count_index = 0u;
         count_index < sizeof(anchor_counts) / sizeof(anchor_counts[0]);
         count_index++) {
        const size_t anchor_count = anchor_counts[count_index];
        const uint32_t command_seq = (uint32_t)(2000u + anchor_count);
        const uint32_t collection_epoch_id = (uint32_t)(4000u + anchor_count);
        struct gateway_collection_eack eack = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 13u,
            .command_seq = command_seq,
            .collection_epoch_id = collection_epoch_id,
            .membership_epoch = 3u,
            .expected_count = (uint16_t)anchor_count,
            .received_count = (uint16_t)anchor_count,
            .packet_sequence = 1u,
            .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
            .retry_round = 0u,
            .next_retry_spread_ms = COLLECTION_RETRY_ROUND_0_MS,
            .collection_open = false,
        };
        struct proto_packet eack_packet = {
            .msg_type = MSG_GATEWAY_COLLECTION_EACK,
            .src_id = GATEWAY,
            .dst_id = MESH_BROADCAST_ID,
            .session_id = command_seq,
            .seq = 1u,
            .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        };
        uint8_t eack_payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t eack_payload_len = 0u;

        assert(gateway_collection_eack_append_tlvs(eack_payload,
                                                   sizeof(eack_payload),
                                                   &eack_payload_len,
                                                   &eack) == PROTO_OK);
        for (size_t node_index = 0u; node_index < anchor_count; node_index++) {
            assert(tlv_append_u64(eack_payload,
                                  sizeof(eack_payload),
                                  &eack_payload_len,
                                  TLV_NODE_ID,
                                  anchor_id_base + node_index) == PROTO_OK);
        }
        eack_packet.payload_len = (uint16_t)eack_payload_len;

        for (size_t node_index = 0u; node_index < anchor_count; node_index++) {
            const uint64_t node_id = anchor_id_base + node_index;
            const uint8_t missed_broadcasts = (uint8_t)(node_index % 4u);
            const struct command_result_id result_id = {
                .gateway_id = GATEWAY,
                .gateway_epoch = 13u,
                .command_seq = command_seq,
                .node_id = node_id,
                .node_boot_counter = 61u,
                .result_seq = (uint16_t)(node_index + 1u),
            };
            struct mesh_relay relay;
            struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
            struct proto_packet result_packet;
            struct mesh_outbound tx;
            struct mesh_relay_result result;
            uint8_t result_payload[128];
            size_t result_payload_len = 0u;
            bool heard = false;

            build_collection_command_result_payload(result_payload,
                                                    sizeof(result_payload),
                                                    64u,
                                                    &result_id,
                                                    collection_epoch_id,
                                                    &result_payload_len);
            assert(mesh_init_command_result(&result_packet,
                                            node_id,
                                            GATEWAY,
                                            command_seq,
                                            (uint16_t)(node_index + 1u),
                                            (uint8_t)result_payload_len,
                                            false) == PROTO_OK);
            mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, node_id, GATEWAY, 13u);
            assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
            assert(mesh_relay_start_tx(&relay,
                                       &result_packet,
                                       result_payload,
                                       result_payload_len,
                                       5000u,
                                       &tx) == PROTO_OK);
            assert(relay.outbox_record.delivery_state ==
                   MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

            for (uint8_t broadcast = 0u; broadcast < 4u; broadcast++) {
                if (broadcast < missed_broadcasts) {
                    continue;
                }
                assert(mesh_relay_handle_rx(&relay,
                                            &eack_packet,
                                            eack_payload,
                                            eack_payload_len,
                                            GATEWAY,
                                            80u,
                                            5100u + broadcast,
                                            &result) == PROTO_OK);
                heard = true;
                break;
            }

            assert(heard);
            assert(result.status == PROTO_OK);
            assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
            assert(!mesh_relay_tx_active(&relay));
            assert(!relay.outbox_record.valid);
            assert(relay.outbox_record.delivery_state ==
                   MESH_RELAY_DELIVERY_GATEWAY_ACKED);
        }
    }
}

static void test_collection_eack_received_list_schedules_retry_when_not_received(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1002u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 63u,
        .result_seq = 64u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1002u,
        .collection_epoch_id = 3004u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 1u,
        .packet_sequence = 24u,
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
        .retry_round = 1u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_0_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1002u,
        .seq = 24u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3004u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(eack_payload,
                          sizeof(eack_payload),
                          &eack_payload_len,
                          TLV_NODE_ID,
                          ANCHOR_B) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert_outbox_tracks_packet(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms ==
           5100u + mesh_relay_collection_retry_delay_ms(
                       COLLECTION_RETRY_ROUND_0_MS,
                       expected_collection_retry_seed(result_id.node_id,
                                                      result_id.command_seq,
                                                      3004u,
                                                      eack.retry_round)));
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.retry_round == eack.retry_round);
    assert(relay.outbox_record.created_uptime_ms == 5000u);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static void test_collection_eack_missing_list_schedules_patient_retry(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1003u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 65u,
        .result_seq = 66u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1003u,
        .collection_epoch_id = 3005u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 11u,
        .packet_sequence = 25u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 2u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_1_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1003u,
        .seq = 25u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3005u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(eack_payload,
                          sizeof(eack_payload),
                          &eack_payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert_outbox_tracks_packet(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms ==
           5100u + mesh_relay_collection_retry_delay_ms(
                       COLLECTION_RETRY_ROUND_1_MS,
                       expected_collection_retry_seed(result_id.node_id,
                                                      result_id.command_seq,
                                                      3005u,
                                                      eack.retry_round)));
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.retry_round == eack.retry_round);
    assert(relay.outbox_record.created_uptime_ms == 5000u);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static void test_collection_eack_missing_list_absent_node_confirms_delivery(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1003u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 65u,
        .result_seq = 66u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1003u,
        .collection_epoch_id = 3005u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 11u,
        .packet_sequence = 25u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 2u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_1_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay before_malformed;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1003u,
        .seq = 25u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3005u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(eack_payload,
                          sizeof(eack_payload),
                          &eack_payload_len,
                          TLV_NODE_ID,
                          ANCHOR_B) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert_outbox_tracks_packet(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    before_malformed = relay;
    eack_packet.payload_len =
        (uint16_t)(eack_payload_len - PROTO_TLV_U64_ENCODED_LEN);
    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_packet.payload_len,
                                GATEWAY,
                                80u,
                                5090u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(memcmp(&relay, &before_malformed, sizeof(relay)) == 0);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_IDLE);
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED);
    assert(relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static void test_collection_eack_closed_stops_pending_without_success(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1004u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 67u,
        .result_seq = 68u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1004u,
        .collection_epoch_id = 3006u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 11u,
        .packet_sequence = 26u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 3u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_2_MS,
        .collection_open = false,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1004u,
        .seq = 26u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3006u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(eack_payload,
                          sizeof(eack_payload),
                          &eack_payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert_outbox_tracks_packet(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_CLOSED));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_COLLECTION_CLOSED);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.retry_round == eack.retry_round);
    assert(relay.outbox_record.created_uptime_ms == 5000u);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static void test_collection_eack_closed_roster_bitmap_stops_pending_without_list(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1014u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 91u,
        .result_seq = 92u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1014u,
        .collection_epoch_id = 3014u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 11u,
        .packet_sequence = 31u,
        .eack_format = EACK_FORMAT_ROSTER_BITMAP,
        .retry_round = 4u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_3_MS,
        .collection_open = false,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1014u,
        .seq = 31u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3014u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert_outbox_tracks_packet(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_CLOSED));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_COLLECTION_CLOSED);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.retry_round == eack.retry_round);
    assert(relay.outbox_record.created_uptime_ms == 5000u);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static void test_collection_result_survives_route_loss_until_eack(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1005u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 69u,
        .result_seq = 70u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1005u,
        .collection_epoch_id = 3007u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 1u,
        .packet_sequence = 26u,
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
        .retry_round = 2u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_1_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 70u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1005u,
        .seq = 26u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t result_payload[128];
    uint8_t eack_payload[128];
    size_t result_payload_len = 0u;
    size_t eack_payload_len = 0u;
    uint32_t now_ms = 7000u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3007u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    result_packet.message_age_ms = 41u;
    assert(gateway_collection_eack_append_tlvs(eack_payload,
                                               sizeof(eack_payload),
                                               &eack_payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(eack_payload,
                          sizeof(eack_payload),
                          &eack_payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    eack_packet.payload_len = (uint16_t)eack_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    route.last_seen_ms = now_ms;
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               now_ms,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, now_ms);

    for (uint8_t i = 1u; i <= 3u; i++) {
        uint32_t expected_retry_ms;

        now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
        expected_retry_ms = now_ms + mesh_relay_collection_retry_delay_ms(
                                         i == 1u ? COLLECTION_RETRY_ROUND_0_MS :
                                         i == 2u ? COLLECTION_RETRY_ROUND_1_MS :
                                                   COLLECTION_RETRY_ROUND_2_MS,
                                         expected_collection_retry_seed(result_id.node_id,
                                                                        result_id.command_seq,
                                                                        3007u,
                                                                        i));
        assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        assert(relay.pending.retry_after_ms == expected_retry_ms);
        assert(relay.outbox_record.retry_round == i);
        assert(mesh_relay_tx_active(&relay));
        assert(route_selected(&relay.upstream) != NULL);

        now_ms = relay.pending.retry_after_ms;
        assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
        assert(result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
        assert(result.retransmit.next_hop_id == GATEWAY);
        assert(mesh_relay_tx_active(&relay));
        mesh_relay_note_tx_sent(&relay, &result.retransmit, now_ms);
    }

    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.payload_len == result_payload_len);
    assert(memcmp(relay.pending.payload, result_payload, result_payload_len) == 0);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.packet_id == expected_outbox_packet_id(&result_packet));
    assert(relay.outbox_record.age_ms_saturating > result_packet.message_age_ms);
    assert(route_selected(&relay.upstream) != NULL);

    assert(mesh_relay_handle_rx(&relay,
                                &eack_packet,
                                eack_payload,
                                eack_payload_len,
                                GATEWAY,
                                80u,
                                now_ms + 100u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED);
    assert(relay.outbox_record.gateway_acked);
}

static void test_click_preemption_preserves_pending_collection_result(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1006u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 71u,
        .result_seq = 72u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3008u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active_local_collection_result(&relay));
    assert(mesh_relay_defer_tx(&relay, 5100u, UINT32_C(0x12345678)));
    assert(mesh_relay_tx_active(&relay));
    assert(mesh_relay_tx_active_local_collection_result(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms - 5100u >= RELAY_BUSY_RETRY_MIN_MS);
    assert(relay.pending.retry_after_ms - 5100u <=
           RELAY_BUSY_RETRY_MIN_MS + (RELAY_BUSY_RETRY_MIN_MS / 2u));
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.packet.message_age_ms == 100u);
    assert(relay.pending.queued_at_ms == 5100u);
    assert(relay.pending.payload_len == result_payload_len);
    assert(memcmp(relay.pending.payload, result_payload, result_payload_len) == 0);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(relay.outbox_record.age_ms_saturating == 100u);

    assert(mesh_relay_tick(&relay, relay.pending.retry_after_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
    assert(result.retransmit.payload_len == result_payload_len);
    assert(memcmp(result.retransmit.payload, result_payload, result_payload_len) == 0);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
}

static void test_collection_result_timeout_uses_collection_retry_round(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1007u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 73u,
        .result_seq = 74u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    const struct route_candidate *selected;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;
    uint32_t timeout_ms;
    uint32_t expected_retry_ms;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3009u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.retry_round == 0u);

    timeout_ms = relay.pending.gateway_ack_deadline_ms + 1u;
    expected_retry_ms = timeout_ms + mesh_relay_collection_retry_delay_ms(
                                         COLLECTION_RETRY_ROUND_0_MS,
                                         expected_collection_retry_seed(result_id.node_id,
                                                                        result_id.command_seq,
                                                                        3009u,
                                                                        1u));

    assert(mesh_relay_tick(&relay, timeout_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == expected_retry_ms);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(relay.outbox_record.retry_round == 1u);
    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->failure_count == 0u);
}

static void test_collection_result_expiry_requires_exact_terminal_commit(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1008u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 75u,
        .result_seq = 76u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct proto_packet mismatched_terminal;
    struct mesh_outbound tx;
    struct mesh_outbound terminal;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;
    uint32_t expiry_ms;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3010u,
                                            &result_payload_len);
    assert(tlv_append_u32(result_payload,
                          sizeof(result_payload),
                          &result_payload_len,
                          TLV_COMMAND_EXPIRY_S,
                          1u) == PROTO_OK);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.expiry_s == 1u);

    expiry_ms = relay.outbox_record.created_uptime_ms + 1000u;
    assert(mesh_relay_tick(&relay, expiry_ms, &result) == PROTO_OK);
    assert(result.status == MESH_RELAY_ERR_OUTBOX_EXPIRED);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_EXPIRED);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.age_ms_saturating == 1000u);
    terminal = result.terminal;
    assert(terminal.packet.msg_type == result_packet.msg_type);
    assert(terminal.packet.session_id == result_packet.session_id);
    assert(terminal.packet.seq == result_packet.seq);
    assert(terminal.payload_len == result_payload_len);
    assert(memcmp(terminal.payload,
                  result_payload,
                  result_payload_len) == 0);

    assert(mesh_relay_tick(&relay, expiry_ms + 1u, &result) == PROTO_OK);
    assert(result.status == MESH_RELAY_ERR_OUTBOX_EXPIRED);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(result.terminal.packet.msg_type == terminal.packet.msg_type);
    assert(result.terminal.packet.session_id == terminal.packet.session_id);
    assert(result.terminal.packet.seq == terminal.packet.seq);
    assert(result.terminal.payload_len == terminal.payload_len);
    assert(memcmp(result.terminal.payload,
                  terminal.payload,
                  terminal.payload_len) == 0);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.outbox_record.valid);

    mismatched_terminal = terminal.packet;
    mismatched_terminal.seq++;
    assert(mesh_relay_commit_terminal_release(&relay,
                                              &mismatched_terminal,
                                              terminal.payload,
                                              terminal.payload_len) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.outbox_record.valid);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);

    assert(mesh_relay_commit_terminal_release(&relay,
                                              &terminal.packet,
                                              terminal.payload,
                                              terminal.payload_len) ==
           PROTO_OK);
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_EXPIRED);
    assert(mesh_relay_tick(&relay, expiry_ms + 2u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
}

static void test_collection_outbox_snapshot_restores_after_reinit(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1008u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 75u,
        .result_seq = 76u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;
    uint32_t restore_ms = 100u;
    uint32_t retry_ms = restore_ms + RELAY_BUSY_RETRY_MIN_MS;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3010u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_export_outbox_snapshot(&relay, 5100u, &snapshot) == PROTO_OK);
    assert(snapshot.valid);
    assert(snapshot.record.age_ms_saturating == 100u);
    assert(snapshot.pending.packet.message_age_ms == 100u);
    assert(snapshot.pending.payload_len == result_payload_len);
    assert(memcmp(snapshot.pending.payload, result_payload, result_payload_len) == 0);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              restore_ms) ==
           PROTO_ERR_MALFORMED);
    assert(!mesh_relay_tx_active(&restored));
    assert(mesh_relay_seed_route_freshness(
               &restored, 13u, 0u) == PROTO_OK);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              restore_ms) == PROTO_OK);
    assert(mesh_relay_tx_active(&restored));
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored.pending.retry_after_ms == retry_ms);
    assert(restored.pending.next_hop_id == 0u);
    assert(restored.pending.packet.message_age_ms == 100u);
    assert(restored.outbox_record.valid);
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    assert(mesh_relay_tick(&restored, retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(result.retransmit.payload_len == result_payload_len);
    assert(memcmp(result.retransmit.payload, result_payload, result_payload_len) == 0);
    assert(result.retransmit.packet.message_age_ms == 100u + RELAY_BUSY_RETRY_MIN_MS);
}

static void test_collection_outbox_snapshot_preserves_retry_round_delay(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1009u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 77u,
        .result_seq = 78u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;
    uint32_t timeout_ms;
    uint32_t expected_retry_ms;
    uint32_t snapshot_ms;
    uint32_t restore_ms = 250u;
    uint32_t restored_retry_ms;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3011u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);

    timeout_ms = relay.pending.gateway_ack_deadline_ms + 1u;
    expected_retry_ms = timeout_ms + mesh_relay_collection_retry_delay_ms(
                                         COLLECTION_RETRY_ROUND_0_MS,
                                         expected_collection_retry_seed(result_id.node_id,
                                                                        result_id.command_seq,
                                                                        3011u,
                                                                        1u));
    assert(mesh_relay_tick(&relay, timeout_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == expected_retry_ms);
    assert(relay.outbox_record.retry_round == 1u);

    snapshot_ms = timeout_ms + 100u;
    assert(mesh_relay_export_outbox_snapshot(&relay, snapshot_ms, &snapshot) == PROTO_OK);
    assert(snapshot.snapshot_at_ms == snapshot_ms);
    assert(snapshot.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(snapshot.pending.retry_after_ms == expected_retry_ms);
    assert(snapshot.record.retry_round == 1u);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              restore_ms) == PROTO_OK);
    restored_retry_ms = restore_ms + (expected_retry_ms - snapshot_ms);
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored.pending.retry_after_ms == restored_retry_ms);
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(restored.outbox_record.retry_round == 1u);

    assert(mesh_relay_tick(&restored, restored_retry_ms - 1u, &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(mesh_relay_tick(&restored, restored_retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(result.retransmit.payload_len == result_payload_len);
    assert(memcmp(result.retransmit.payload, result_payload, result_payload_len) == 0);
}

static void test_collection_outbox_snapshot_rejects_corrupt_payload(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1009u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 77u,
        .result_seq = 78u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3011u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_export_outbox_snapshot(&relay, 5100u, &snapshot) == PROTO_OK);

    snapshot.pending.payload[0] ^= 0x01u;
    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(mesh_relay_restore_outbox_snapshot(&restored, &snapshot, 100u) ==
           PROTO_ERR_MALFORMED);
    assert(!mesh_relay_tx_active(&restored));
}

static void test_crc16_collision_cannot_alias_outbox_snapshot_payload(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    uint8_t payload_a[64];
    uint8_t payload_b[64];
    size_t payload_len = 0u;

    build_deliberate_crc16_collision_results(payload_a,
                                              payload_b,
                                              sizeof(payload_a),
                                              false,
                                              &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    UINT32_C(0xb1b2b3b4),
                                    41u,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               payload_a,
                               payload_len,
                               5200u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             5201u,
                                             &snapshot) == PROTO_OK);
    assert(snapshot.record.payload_crc ==
           proto_crc16_ccitt_false(payload_b, payload_len));

    memcpy(snapshot.pending.payload, payload_b, payload_len);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    3u);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              5300u) ==
           PROTO_ERR_MALFORMED);
    assert(!mesh_relay_tx_active(&restored));
}

static void test_collection_outbox_snapshot_rejects_completed_record(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1010u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 79u,
        .result_seq = 80u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet = {0};
    struct mesh_outbound tx;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            3012u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_export_outbox_snapshot(&relay, 5100u, &snapshot) == PROTO_OK);

    snapshot.record.gateway_acked = true;
    snapshot.record.delivery_state = MESH_RELAY_DELIVERY_GATEWAY_ACKED;
    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(mesh_relay_restore_outbox_snapshot(&restored, &snapshot, 100u) ==
           PROTO_ERR_MALFORMED);
    assert(!mesh_relay_tx_active(&restored));
}

static void test_collection_eack_broadcast_rejects_wrong_gateway_epoch(void)
{
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 12u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .membership_epoch = 3u,
        .expected_count = 6u,
        .received_count = 5u,
        .packet_sequence = 23u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 1u,
        .next_retry_spread_ms = COLLECTION_RETRY_ROUND_0_MS,
        .collection_open = true,
    };
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 23u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3060u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_collection_recovery_eack_refloods_fresh_attempt_across_epoch(void)
{
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 12u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .membership_epoch = 1u,
        .expected_count = 1u,
        .received_count = 1u,
        .packet_sequence = 23u,
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
        .retry_round = 0u,
        .next_retry_spread_ms = 0u,
        .collection_open = false,
    };
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 23u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    uint8_t payload[128];
    const uint8_t packet_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0x7du};
    size_t payload_len = 0u;
    size_t attempt_offset;

    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    attempt_offset = payload_len;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_RECOVERY_ATTEMPT_ID,
                          9001u) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          ANCHOR_A) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_RESULT_SEQ,
                          77u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_PAYLOAD_LEN,
                          1u) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_RESULT_SHA256_COMMITMENT,
                            packet_digest,
                            sizeof(packet_digest)) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3060u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3070u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));

    proto_put_u32_le(&payload[attempt_offset + PROTO_TLV_HEADER_LEN], 9002u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3080u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    {
        struct gateway_collection_eack open = eack;

        open.collection_open = true;
        payload_len = 0u;
        assert(gateway_collection_eack_append_tlvs(
                   payload, sizeof(payload), &payload_len, &open) == PROTO_OK);
        assert(tlv_append_u32(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_COLLECTION_RECOVERY_ATTEMPT_ID,
                              9003u) == PROTO_OK);
        assert(tlv_append_u64(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_NODE_ID,
                              ANCHOR_A) == PROTO_OK);
        assert(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_RESULT_SEQ,
                              77u) == PROTO_OK);
        assert(tlv_append_u16(payload,
                              sizeof(payload),
                              &payload_len,
                              TLV_PAYLOAD_LEN,
                              1u) == PROTO_OK);
        assert(tlv_append_bytes(payload,
                                sizeof(payload),
                                &payload_len,
                                TLV_RESULT_SHA256_COMMITMENT,
                                packet_digest,
                                sizeof(packet_digest)) == PROTO_OK);
        packet.payload_len = (uint16_t)payload_len;
        assert(gateway_collection_eack_packet_validate(
                   &packet, payload, payload_len, NULL) ==
               PROTO_ERR_MALFORMED);
    }
}



static void test_downlink_route_selection_uses_weighted_quality(void)
{
    struct mesh_relay gateway;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 31u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_A, 31u, 1u, 0u, 1000u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 31u, 2u, 100u, 1100u);

    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
}

static void test_start_tx_accepts_aged_upstream_route_until_failures(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 32u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    const struct route_candidate *failed;
    uint8_t payload[1] = {0x5Au};
    const uint32_t fourth_failure_ms = 83400u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 32u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 91u, 1u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               83001u,
                               &tx) == PROTO_OK);
    assert(route_selected(&relay.upstream) != NULL);

    /* Once the unrelated core owner is gone, the relay no longer has proof
     * that the app queue or gateway still owns this report. Re-forward the
     * exact retry so a missed end-to-end ACK can be replaced. */
    mesh_relay_cancel_tx(&relay);
    mesh_relay_note_delivery_failure_at(&relay, GATEWAY, 83100u);
    mesh_relay_note_delivery_failure_at(&relay, GATEWAY, 83200u);
    assert(route_selected(&relay.upstream) != NULL);
    mesh_relay_note_delivery_failure_at(&relay, GATEWAY, 83300u);
    assert(route_selected(&relay.upstream) != NULL);
    mesh_relay_note_delivery_failure_at(&relay, GATEWAY, fourth_failure_ms);
    assert(route_selected(&relay.upstream) == NULL);
    failed = &relay.upstream.candidates[0];
    assert(failed->hold_down_until_ms ==
           fourth_failure_ms + ROUTE_PARENT_HOLDDOWN_MS);
    assert(failed->hold_down_valid);
    assert(mesh_relay_expire_routes(
               &relay,
               fourth_failure_ms + ROUTE_PARENT_HOLDDOWN_MS - 1u) == 0u);
    assert(route_selected(&relay.upstream) == NULL);
    assert(mesh_relay_expire_routes(
               &relay,
               fourth_failure_ms + ROUTE_PARENT_HOLDDOWN_MS) == 0u);
    assert(route_selected(&relay.upstream) == failed);
    assert(!failed->hold_down_valid);
}

static void test_route_normalization_prevents_direct_fastpath_misclassification(void)
{
    struct mesh_relay relay;
    struct route_candidate direct = direct_gateway_route(GATEWAY, 33u, 95u);
    struct route_candidate alternate = direct_gateway_route(ANCHOR_B, 33u, 80u);
    struct route_candidate *direct_stored = NULL;
    struct proto_packet report;
    struct mesh_outbound tx;
    uint64_t next_hop_id = 0u;
    const uint8_t payload[] = {0x5Au};

    alternate.hop_count = 1u;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 33u);
    assert(route_upsert_candidate(&relay.upstream, &direct) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &alternate) == PROTO_OK);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == GATEWAY);

    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (relay.upstream.candidates[i].valid &&
            relay.upstream.candidates[i].next_hop_id == GATEWAY) {
            direct_stored = &relay.upstream.candidates[i];
            break;
        }
    }
    assert(direct_stored != NULL);
    direct_stored->hold_down_valid = true;
    direct_stored->hold_down_until_ms = 5000u;

    /* A read-only lookup still observes the old selected index.  The app's
     * direct-path classifier must normalize at the send time just as
     * mesh_relay_start_tx() does, or these two selections disagree. */
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == GATEWAY);
    assert(mesh_relay_expire_routes(&relay, 2000u) == 0u);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    94u,
                                    1u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               2000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    assert(mesh_relay_cancel_tx_if_matches(&relay, &tx) == PROTO_OK);
}

static void test_downlink_next_hop_send_completes_immediately(void)
{
    struct mesh_relay gateway;
    struct proto_packet command;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t command_payload[16];
    size_t command_payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 33u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 33u, 2u, 100u, 1000u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_A, 33u, 1u, 0u, 1010u);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             400u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    command.ttl = gateway_command_origin_ttl(CMD_GET_STATUS);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               2000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    assert(!mesh_relay_tx_active(&gateway));

    assert(mesh_relay_tick(&gateway, 10000u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
}

static void test_downlink_send_refreshes_route_age(void)
{
    struct mesh_relay gateway;
    struct proto_packet command;
    struct mesh_outbound tx;
    const struct mesh_downlink_entry *downlink;
    uint8_t command_payload[16];
    size_t command_payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 34u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 34u, 2u, 80u, 1000u);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             401u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    command.ttl = gateway_command_origin_ttl(CMD_GET_STATUS);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               7400u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    assert(!mesh_relay_tx_active(&gateway));

    mesh_relay_note_tx_sent(&gateway, &tx, 7450u);

    assert(mesh_relay_expire_routes(&gateway, 8001u) == 0u);
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->last_seen_ms == 7450u);
}

static void test_ttl_zero_packet_is_dropped_without_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_relay_result result;
    const struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 77u,
        .battery_mv = 3000u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&report,
                                        ANCHOR_A,
                                        GATEWAY,
                                        77u,
                                        77u,
                                        (uint8_t)payload_len) == PROTO_OK);
    report.ttl = 0u;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4000u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
}

static void test_zero_session_or_sequence_mesh_packets_are_rejected(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x56u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 78u, 1u, sizeof(payload)) ==
           PROTO_OK);

    report.session_id = 0u;
    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                80u,
                                4010u,
                                &result) == PROTO_ERR_ARG);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               4010u,
                               &tx) == PROTO_ERR_ARG);

    report.session_id = 78u;
    report.seq = 0u;
    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                80u,
                                4020u,
                                &result) == PROTO_ERR_ARG);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               4020u,
                               &tx) == PROTO_ERR_ARG);
}

static void test_busy_relay_transfers_gateway_report_to_bounded_queue(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet local_report;
    struct proto_packet incoming_report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_pending_tx pending_before;
    uint8_t local_payload[128];
    uint8_t incoming_payload[128];
    size_t local_payload_len;
    size_t incoming_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    local_payload_len = build_valid_click_report(&local_report,
                                                 ANCHOR_B,
                                                 78u,
                                                 2u,
                                                 1200,
                                                 local_payload,
                                                 sizeof(local_payload));
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               local_payload_len,
                               4100u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    pending_before = relay.pending;

    incoming_payload_len = build_valid_click_report(&incoming_report,
                                                    ANCHOR_A,
                                                    79u,
                                                    3u,
                                                    1300,
                                                    incoming_payload,
                                                    sizeof(incoming_payload));

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4110u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(result.forward.next_hop_id == GATEWAY);
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);

    /* Once app admission commits the duplicate record, a lost child hop ACK
     * is repaired without adding the same immutable report twice. */
    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4111u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);

    mesh_relay_cancel_tx(&relay);
    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4112u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(result.hop_ack.next_hop_id == ANCHOR_A);
}

static void test_live_duplicate_child_retry_replays_hop_ack_without_reforward(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report;
    struct mesh_outbound upstream;
    struct mesh_relay_result result;
    struct mesh_pending_tx pending_before;
    struct mesh_outbox_record outbox_before;
    uint8_t payload[128];
    size_t payload_len;
    bool contains = false;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           80u,
                                           4u,
                                           1200,
                                           payload,
                                           sizeof(payload));

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                4200u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(result.forward.next_hop_id == GATEWAY);
    assert(result.hop_ack.next_hop_id == ANCHOR_A);
    assert(mesh_ack_payload_contains_packet(&result.hop_ack.packet,
                                            result.hop_ack.payload,
                                            result.hop_ack.payload_len,
                                            &report,
                                            payload,
                                            payload_len,
                                            &contains) == PROTO_OK);
    assert(contains);

    upstream = result.forward;
    assert(mesh_relay_start_tx(&relay,
                               &upstream.packet,
                               upstream.payload,
                               upstream.payload_len,
                               4210u,
                               &upstream) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(
               &relay, &upstream, ANCHOR_A) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &upstream, 4220u);
    pending_before = relay.pending;
    outbox_before = relay.outbox_record;

    /* The first HOP_ACK was lost. The relay already owns the exact packet,
     * so the child's identical retry repairs only that custody edge. */
    contains = false;
    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                4230u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(result.hop_ack.next_hop_id == ANCHOR_A);
    assert(mesh_ack_payload_contains_packet(&result.hop_ack.packet,
                                            result.hop_ack.payload,
                                            result.hop_ack.payload_len,
                                            &report,
                                            payload,
                                            payload_len,
                                            &contains) == PROTO_OK);
    assert(contains);
    assert(memcmp(&relay.pending,
                  &pending_before,
                  sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record,
                  &outbox_before,
                  sizeof(outbox_before)) == 0);
}

static void test_busy_relay_still_delivers_direct_local_command(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet local_report;
    struct proto_packet command;
    struct proto_packet transit_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    struct mesh_pending_tx pending_before;
    struct mesh_outbox_record outbox_before;
    uint8_t local_payload[128];
    uint8_t command_payload[8];
    uint8_t transit_payload[128];
    size_t local_payload_len;
    size_t command_payload_len = 0u;
    size_t transit_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    local_payload_len = build_valid_click_report(&local_report,
                                                 ANCHOR_A,
                                                 0x8101u,
                                                 81u,
                                                 1400,
                                                 local_payload,
                                                 sizeof(local_payload));
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               local_payload_len,
                               5000u,
                               &tracked_report) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    pending_before = relay.pending;
    outbox_before = relay.outbox_record;

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             UINT32_C(0x81028102),
                             82u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    command.ttl = gateway_command_origin_ttl(CMD_GET_STATUS);
    assert(mesh_relay_handle_rx(&relay,
                                &command,
                                command_payload,
                                command_payload_len,
                                GATEWAY,
                                95u,
                                5010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record, &outbox_before,
                  sizeof(outbox_before)) == 0);

    /* Reliable reports transfer to the app queue without borrowing or
     * replacing the active core owner's slot. */
    transit_payload_len = build_valid_click_report(&transit_report,
                                                   ANCHOR_C,
                                                   0x8103u,
                                                   83u,
                                                   1500,
                                                   transit_payload,
                                                   sizeof(transit_payload));
    assert(mesh_relay_handle_rx(&relay,
                                &transit_report,
                                transit_payload,
                                transit_payload_len,
                                ANCHOR_C,
                                90u,
                                5020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record, &outbox_before,
                  sizeof(outbox_before)) == 0);
}

static void test_busy_gateway_still_accepts_direct_route_probe(void)
{
    struct mesh_relay gateway;
    struct proto_packet route_probe = {
        .msg_type = MSG_GATEWAY_ROUTE_REQ,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x82018201),
        .seq = 81u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_relay_result result;
    struct mesh_pending_tx pending_before;
    struct mesh_outbox_record outbox_before;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    3u);
    /* Exercise the receive admission predicate against an unrelated retained
     * core owner directly. Gateway-originated command transmission uses a
     * separate lane and therefore cannot be used to manufacture this state. */
    gateway.pending.state = MESH_RELAY_TX_WAIT_RETRY_BACKOFF;
    gateway.pending.packet.msg_type = MSG_COMMAND_RESULT;
    gateway.pending.packet.src_id = ANCHOR_C;
    gateway.pending.packet.dst_id = GATEWAY;
    gateway.pending.packet.session_id = UINT32_C(0x82028202);
    gateway.pending.packet.seq = 82u;
    gateway.pending.packet.ttl = MESH_DEFAULT_TTL;
    gateway.pending.next_hop_id = ANCHOR_C;
    gateway.pending.retry_after_ms = 7000u;
    gateway.pending.queued_at_ms = 6010u;
    assert(mesh_relay_tx_active(&gateway));
    pending_before = gateway.pending;
    outbox_before = gateway.outbox_record;

    assert(mesh_relay_handle_rx(&gateway,
                                &route_probe,
                                NULL,
                                0u,
                                ANCHOR_A,
                                95u,
                                6020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(result.gateway_ack.packet.dst_id == ANCHOR_A);
    assert(result.gateway_ack.next_hop_id == ANCHOR_A);
    assert(memcmp(&gateway.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&gateway.outbox_record, &outbox_before,
                  sizeof(outbox_before)) == 0);
}

static void test_busy_relay_repairs_queued_report_hop_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_report;
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    uint8_t incoming_payload[128];
    uint8_t local_payload[128];
    size_t incoming_payload_len;
    size_t local_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    incoming_payload_len = build_valid_click_report(&incoming_report,
                                                    ANCHOR_A,
                                                    80u,
                                                    4u,
                                                    1300,
                                                    incoming_payload,
                                                    sizeof(incoming_payload));

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4200u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    local_payload_len = build_valid_click_report(&local_report,
                                                 ANCHOR_B,
                                                 81u,
                                                 5u,
                                                 1400,
                                                 local_payload,
                                                 sizeof(local_payload));
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               local_payload_len,
                               4201u,
                               &tracked_report) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4202u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_busy_relay_sends_result_busy_for_command_result(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 90u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 5u,
        .result_seq = 6u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_result;
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    uint8_t incoming_payload[96];
    uint8_t local_payload[128];
    size_t incoming_payload_len = 0u;
    size_t local_payload_len;

    build_collection_command_result_payload(incoming_payload,
                                            sizeof(incoming_payload),
                                            64u,
                                            &result_id,
                                            3001u,
                                            &incoming_payload_len);
    assert(mesh_init_command_result(&incoming_result,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)incoming_payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    local_payload_len = build_valid_click_report(&local_report,
                                                 ANCHOR_B,
                                                 81u,
                                                 5u,
                                                 1400,
                                                 local_payload,
                                                 sizeof(local_payload));
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               local_payload_len,
                               4201u,
                               &tracked_report) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_result,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4202u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    assert(result.busy.packet.msg_type == MSG_RESULT_BUSY);
    assert(result.busy.packet.dst_id == ANCHOR_A);
    assert(result.busy.next_hop_id == ANCHOR_A);
}

static void test_result_busy_preserves_command_result_identity(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x12345678u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 11u,
        .result_seq = 12u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_result;
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    struct result_busy decoded_busy = {0};
    uint8_t incoming_payload[96];
    uint8_t local_payload[128];
    size_t incoming_payload_len = 0u;
    size_t local_payload_len;

    build_collection_command_result_payload(incoming_payload,
                                            sizeof(incoming_payload),
                                            64u,
                                            &result_id,
                                            3001u,
                                            &incoming_payload_len);
    assert(mesh_init_command_result(&incoming_result,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)incoming_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    local_payload_len = build_valid_click_report(&local_report,
                                                 ANCHOR_B,
                                                 81u,
                                                 5u,
                                                 1400,
                                                 local_payload,
                                                 sizeof(local_payload));
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               local_payload_len,
                               4201u,
                               &tracked_report) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_result,
                                incoming_payload,
                                incoming_payload_len,
                                ANCHOR_A,
                                80u,
                                4202u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    assert(result.busy.packet.msg_type == MSG_RESULT_BUSY);
    assert(result_busy_from_tlvs(result.busy.payload,
                                 result.busy.payload_len,
                                 &decoded_busy) == PROTO_OK);
    assert_command_result_id_equal(&decoded_busy.result_id, &result_id);
    assert(decoded_busy.retry_after_ms == RELAY_BUSY_RETRY_MAX_MS);
    assert(decoded_busy.capacity_state == RELAY_CAP_RED);
    assert(decoded_busy.capacity_validity_interval_ms == RELAY_BUSY_RETRY_MAX_MS);
}

static void test_result_offer_gets_result_grant_when_parent_has_capacity(void)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x22334455u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 21u,
            .result_seq = 22u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x789au,
        .priority = 4u,
    };
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x22334455),
        .seq = 22u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    struct result_grant decoded_grant = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4300u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    assert(result.result_grant.packet.msg_type == MSG_RESULT_GRANT);
    assert(result.result_grant.packet.src_id == ANCHOR_B);
    assert(result.result_grant.packet.dst_id == ANCHOR_A);
    assert(result.result_grant.packet.session_id == packet.session_id);
    assert(result.result_grant.packet.ttl == 1u);
    assert(result.result_grant.next_hop_id == ANCHOR_A);
    assert(result.result_grant.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(result_grant_from_tlvs(result.result_grant.payload,
                                  result.result_grant.payload_len,
                                  &decoded_grant) == PROTO_OK);
    assert_command_result_id_equal(&decoded_grant.result_id, &offer.result_id);
    assert(decoded_grant.granted_channel == UWB_CHANNEL_MESH_PAYLOAD);
    assert(decoded_grant.max_bytes == offer.result_len);
    assert(decoded_grant.event_offset_hint == 0u);
    assert(relay.result_offer_reservation.valid);
    assert_command_result_id_equal(&relay.result_offer_reservation.result_id, &offer.result_id);
    assert(relay.result_offer_reservation.child_id == ANCHOR_A);
    assert(relay.result_offer_reservation.result_len == offer.result_len);
    assert(semantic_digest_equal(relay.result_offer_reservation.result_digest,
                                 offer.result_digest,
                                 sizeof(offer.result_digest)));
    assert(relay.result_offer_reservation_deadline_ms ==
           4300u + MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u);
}

static void test_result_offer_reservation_expires_without_sliding(void)
{
    struct result_offer offer_a = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x22334455u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 21u,
            .result_seq = 22u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x789au,
        .priority = 4u,
    };
    struct result_offer offer_c = offer_a;
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x22334455),
        .seq = 22u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t payload_a[96];
    uint8_t payload_c[96];
    size_t payload_a_len = 0u;
    size_t payload_c_len = 0u;
    const uint32_t accepted_ms = 4300u;
    const uint32_t deadline_ms =
        accepted_ms + MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u;

    offer_c.result_id.node_id = ANCHOR_C;
    offer_c.result_id.node_boot_counter++;
    offer_c.result_id.result_seq++;
    offer_c.result_crc++;
    assert(result_offer_append_tlvs(payload_a,
                                    sizeof(payload_a),
                                    &payload_a_len,
                                    &offer_a) == PROTO_OK);
    assert(result_offer_append_tlvs(payload_c,
                                    sizeof(payload_c),
                                    &payload_c_len,
                                    &offer_c) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    packet.payload_len = (uint16_t)payload_a_len;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload_a,
                                payload_a_len,
                                ANCHOR_A,
                                80u,
                                accepted_ms,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));

    /* A matching grant retry does not extend the immutable lease. */
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload_a,
                                payload_a_len,
                                ANCHOR_A,
                                80u,
                                accepted_ms + 1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(relay.result_offer_reservation_deadline_ms == deadline_ms);

    packet.src_id = ANCHOR_C;
    packet.seq = offer_c.result_id.result_seq;
    packet.payload_len = (uint16_t)payload_c_len;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload_c,
                                payload_c_len,
                                ANCHOR_C,
                                80u,
                                deadline_ms - 1u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));

    /* The closed deadline releases A and grants C in the same receive turn. */
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload_c,
                                payload_c_len,
                                ANCHOR_C,
                                80u,
                                deadline_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(relay.result_offer_reservation.child_id == ANCHOR_C);

    assert(mesh_relay_tick(&relay,
                           deadline_ms +
                               MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u,
                           &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED));
    assert(!relay.result_offer_reservation.valid);
    assert(relay.result_offer_reservation_deadline_ms == 0u);
}

static void test_result_offer_reservation_deadline_zero_wraps_safely(void)
{
    struct result_offer accepted_offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x66778899u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 33u,
            .result_seq = 34u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x4567u,
        .priority = 4u,
    };
    struct result_offer conflicting_offer = accepted_offer;
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x66778899),
        .seq = 34u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t accepted_payload[96];
    uint8_t conflicting_payload[96];
    size_t accepted_payload_len = 0u;
    size_t conflicting_payload_len = 0u;
    const uint32_t expiry_ms =
        MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u;
    const uint32_t accepted_ms = UINT32_MAX - expiry_ms + 1u;

    conflicting_offer.result_crc++;
    conflicting_offer.result_digest[0] ^= 0x01u;
    assert(result_offer_append_tlvs(accepted_payload,
                                    sizeof(accepted_payload),
                                    &accepted_payload_len,
                                    &accepted_offer) == PROTO_OK);
    assert(result_offer_append_tlvs(conflicting_payload,
                                    sizeof(conflicting_payload),
                                    &conflicting_payload_len,
                                    &conflicting_offer) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    packet.payload_len = (uint16_t)accepted_payload_len;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                accepted_payload,
                                accepted_payload_len,
                                ANCHOR_A,
                                80u,
                                accepted_ms,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(relay.result_offer_reservation.valid);
    assert(relay.result_offer_reservation_deadline_ms == 0u);

    packet.payload_len = (uint16_t)conflicting_payload_len;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                conflicting_payload,
                                conflicting_payload_len,
                                ANCHOR_A,
                                80u,
                                UINT32_MAX,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    assert(relay.result_offer_reservation.valid);
    assert(semantic_digest_equal(
        relay.result_offer_reservation.result_digest,
        accepted_offer.result_digest,
        sizeof(accepted_offer.result_digest)));
    assert_command_result_id_equal(&relay.result_offer_reservation.result_id,
                                   &accepted_offer.result_id);

    /* The closed wrapped deadline releases the exact old binding. */
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                conflicting_payload,
                                conflicting_payload_len,
                                ANCHOR_A,
                                80u,
                                0u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(relay.result_offer_reservation.valid);
    assert(semantic_digest_equal(
        relay.result_offer_reservation.result_digest,
        conflicting_offer.result_digest,
        sizeof(conflicting_offer.result_digest)));
}

static void test_result_transfer_requires_matching_offer_len_digest(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x22334456u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 25u,
        .result_seq = 26u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct result_offer offer = {
        .result_id = result_id,
        .priority = 4u,
    };
    struct proto_packet offer_packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x22334456),
        .seq = 26u,
        .ttl = 1u,
    };
    struct proto_packet result_packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x22334456),
        .seq = 26u,
        .ttl = REPORT_DEFAULT_TTL,
    };
    struct mesh_relay_result offer_result;
    struct mesh_relay_result result;
    uint8_t offer_payload[96];
    uint8_t result_payload[COLLECTION_BUNDLE_TARGET_BYTES + 32u];
    size_t offer_payload_len = 0u;
    size_t result_payload_len = 0u;

    build_collection_command_result_payload(
        result_payload,
        sizeof(result_payload),
        COLLECTION_BUNDLE_TARGET_BYTES + 16u,
        &result_id,
        3001u,
        &result_payload_len);
    offer.result_len = (uint16_t)result_payload_len;
    offer.result_crc = proto_crc16_ccitt_false(result_payload, result_payload_len);
    assert(semantic_digest_sha256(result_payload,
                                  result_payload_len,
                                  offer.result_digest));
    assert(result_offer_append_tlvs(offer_payload,
                                    sizeof(offer_payload),
                                    &offer_payload_len,
                                    &offer) == PROTO_OK);
    offer_packet.payload_len = (uint16_t)offer_payload_len;
    result_packet.payload_len = (uint16_t)result_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &offer_packet,
                                offer_payload,
                                offer_payload_len,
                                ANCHOR_A,
                                80u,
                                4300u,
                                &offer_result) == PROTO_OK);
    assert(has_action(&offer_result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(relay.result_offer_reservation.valid);

    result_payload[result_payload_len - 1u] ^= 0x5au;
    assert(mesh_relay_handle_rx(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                ANCHOR_A,
                                80u,
                                4310u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(relay.result_offer_reservation.valid);
    assert_command_result_id_equal(&relay.result_offer_reservation.result_id, &result_id);

    result_payload[result_payload_len - 1u] ^= 0x5au;
    assert(mesh_relay_handle_rx(&relay,
                                &result_packet,
                                result_payload,
                                result_payload_len,
                                ANCHOR_A,
                                80u,
                                4320u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!relay.result_offer_reservation.valid);
}

static void test_crc16_collision_cannot_cross_result_offer_reservation_snapshot(void)
{
    const struct command_result_id result_id =
        deliberate_crc16_collision_result_id();
    struct route_candidate child_route =
        direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct route_candidate parent_route =
        direct_gateway_route(GATEWAY, 3u, 90u);
    struct mesh_relay child;
    struct mesh_relay parent;
    struct mesh_relay restored_parent;
    struct mesh_relay_child_custody_snapshot snapshot;
    struct proto_packet result_packet;
    struct mesh_outbound offer_tx;
    struct mesh_relay_result result;
    struct result_offer decoded_offer;
    uint8_t payload_a[64];
    uint8_t payload_b[64];
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t payload_len = 0u;

    build_deliberate_crc16_collision_results(payload_a,
                                              payload_b,
                                              sizeof(payload_a),
                                              true,
                                              &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&child,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&child.upstream, &child_route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&child,
                                         &result_packet,
                                         payload_a,
                                         payload_len,
                                         6000u,
                                         &offer_tx) == PROTO_OK);
    assert(result_offer_from_tlvs(offer_tx.payload,
                                  offer_tx.payload_len,
                                  &decoded_offer) == PROTO_OK);
    assert_command_result_id_equal(&decoded_offer.result_id, &result_id);
    assert(decoded_offer.result_len == payload_len);
    assert(decoded_offer.result_crc ==
           proto_crc16_ccitt_false(payload_a, payload_len));
    assert(decoded_offer.result_crc ==
           proto_crc16_ccitt_false(payload_b, payload_len));
    assert(semantic_digest_sha256(payload_a, payload_len, payload_digest));
    assert(semantic_digest_equal(decoded_offer.result_digest,
                                 payload_digest,
                                 sizeof(payload_digest)));

    mesh_relay_init(&parent,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&parent.upstream, &parent_route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&parent,
                                &offer_tx.packet,
                                offer_tx.payload,
                                offer_tx.payload_len,
                                ANCHOR_A,
                                90u,
                                6010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(parent.result_offer_reservation.valid);
    assert(mesh_relay_export_child_custody_snapshot(&parent,
                                                    6011u,
                                                    &snapshot) == PROTO_OK);

    mesh_relay_init(&restored_parent,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&restored_parent.upstream,
                                  &parent_route) == PROTO_OK);
    assert(mesh_relay_restore_child_custody_snapshot(&restored_parent,
                                                     &snapshot,
                                                     7000u) == PROTO_OK);

    assert(mesh_relay_handle_rx(&restored_parent,
                                &result_packet,
                                payload_b,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                7010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(restored_parent.result_offer_reservation.valid);

    assert(mesh_relay_handle_rx(&restored_parent,
                                &result_packet,
                                payload_a,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                7020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(mesh_relay_result_bundle_pending(&restored_parent));
    assert(!restored_parent.result_offer_reservation.valid);
}

static void test_result_offer_rejects_wrong_gateway_epoch(void)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 2u,
            .command_seq = 0x55667788u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 31u,
            .result_seq = 32u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x1234u,
        .priority = 4u,
    };
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x55667788),
        .seq = 32u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4301u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
}

static void test_result_offer_rejects_cross_child_and_zero_identity(void)
{
    struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x55667788u,
            .node_id = ANCHOR_C,
            .node_boot_counter = 31u,
            .result_seq = 32u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x1234u,
        .result_digest = {0xa5u},
        .priority = 4u,
    };
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x55667788),
        .seq = 32u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4302u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(!relay.result_offer_reservation.valid);

    offer.result_id.node_id = ANCHOR_A;
    offer.result_id.node_boot_counter = 0u;
    payload_len = 0u;
    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4303u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(!relay.result_offer_reservation.valid);
}

static void test_result_offer_gets_result_busy_when_parent_busy(void)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x66778899u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 23u,
            .result_seq = 24u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x4567u,
        .priority = 6u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x66778899),
        .seq = 24u,
        .ttl = 1u,
    };
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    struct result_busy decoded_busy = {0};
    uint8_t payload[96];
    uint8_t local_payload[1] = {0x47u};
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    82u,
                                    6u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
                               4301u,
                               &tracked_report) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4302u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(result.busy.packet.msg_type == MSG_RESULT_BUSY);
    assert(result.busy.packet.dst_id == ANCHOR_A);
    assert(result.busy.next_hop_id == ANCHOR_A);
    assert(result_busy_from_tlvs(result.busy.payload,
                                 result.busy.payload_len,
                                 &decoded_busy) == PROTO_OK);
    assert_command_result_id_equal(&decoded_busy.result_id, &offer.result_id);
    assert(decoded_busy.retry_after_ms == RELAY_BUSY_RETRY_MAX_MS);
    assert(decoded_busy.capacity_state == RELAY_CAP_RED);
    assert(decoded_busy.capacity_validity_interval_ms == RELAY_BUSY_RETRY_MAX_MS);
}

static void test_result_offer_gets_result_busy_when_parent_capacity_red(void)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x6677889au,
            .node_id = ANCHOR_A,
            .node_boot_counter = 24u,
            .result_seq = 25u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x4568u,
        .priority = 6u,
    };
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x6677889a),
        .seq = 25u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    struct result_busy decoded_busy = {0};
    struct mesh_outbound route_request;
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    relay.result_bundle.active = true;
    relay.result_bundle.gateway_id = GATEWAY;
    relay.result_bundle.gateway_epoch = 3u;
    relay.result_bundle.command_seq = offer.result_id.command_seq;
    relay.result_bundle.collection_epoch_id = 10u;
    relay.result_bundle.record_count = MESH_RELAY_RESULT_BUNDLE_RECORDS;

    assert(mesh_relay_build_route_request(&relay,
                                          GATEWAY,
                                          &route_request,
                                          4303u) == PROTO_OK);
    assert(require_tlv_u8(route_request.payload,
                          route_request.payload_len,
                          TLV_RELAY_CAPACITY_STATE) == RELAY_CAP_RED);
    assert(require_tlv_u16(route_request.payload,
                           route_request.payload_len,
                           TLV_QUEUE_FREE_HINT) == 0u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4304u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_GRANT));
    assert(result_busy_from_tlvs(result.busy.payload,
                                 result.busy.payload_len,
                                 &decoded_busy) == PROTO_OK);
    assert_command_result_id_equal(&decoded_busy.result_id, &offer.result_id);
    assert(decoded_busy.retry_after_ms == RELAY_BUSY_RETRY_MAX_MS);
    assert(decoded_busy.capacity_state == RELAY_CAP_RED);
    assert(decoded_busy.capacity_validity_interval_ms == RELAY_BUSY_RETRY_MAX_MS);
}

static void test_large_command_result_starts_result_offer(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x10111213u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 41u,
        .result_seq = 42u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound offer_tx;
    struct result_offer decoded_offer = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&relay,
                                         &result_packet,
                                         payload,
                                         payload_len,
                                         4400u,
                                         &offer_tx) == PROTO_OK);

    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT);
    assert(relay.pending.result_offer_active);
    assert(relay.pending.packet.msg_type == MSG_COMMAND_RESULT);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
    assert(offer_tx.packet.msg_type == MSG_RESULT_OFFER);
    assert(offer_tx.packet.src_id == ANCHOR_A);
    assert(offer_tx.packet.dst_id == ANCHOR_B);
    assert(offer_tx.packet.session_id == result_packet.session_id);
    assert(offer_tx.packet.seq == result_packet.seq);
    assert(offer_tx.next_hop_id == ANCHOR_B);
    assert(offer_tx.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(result_offer_from_tlvs(offer_tx.payload,
                                  offer_tx.payload_len,
                                  &decoded_offer) == PROTO_OK);
    assert_command_result_id_equal(&decoded_offer.result_id, &result_id);
    assert(decoded_offer.result_len == payload_len);
    assert(decoded_offer.result_crc == proto_crc16_ccitt_false(payload, payload_len));
    {
        uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];

        assert(semantic_digest_sha256(payload, payload_len, expected_digest));
        assert(semantic_digest_equal(decoded_offer.result_digest,
                                     expected_digest,
                                     sizeof(expected_digest)));
    }
}

struct result_offer_retry_fixture {
    struct mesh_relay relay;
    struct command_result_id result_id;
    struct proto_packet result_packet;
    struct mesh_outbound offer;
    uint8_t payload[96];
    size_t payload_len;
};

static void result_offer_retry_fixture_init(
    struct result_offer_retry_fixture *fixture,
    uint64_t node_id,
    uint32_t command_seq,
    uint16_t seq,
    uint32_t now_ms)
{
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);

    memset(fixture, 0, sizeof(*fixture));
    fixture->result_id.gateway_id = GATEWAY;
    fixture->result_id.gateway_epoch = 3u;
    fixture->result_id.command_seq = command_seq;
    fixture->result_id.node_id = node_id;
    fixture->result_id.node_boot_counter = command_seq + 1u;
    fixture->result_id.result_seq = seq;
    build_identity_command_result_payload(fixture->payload,
                                          sizeof(fixture->payload),
                                          64u,
                                          &fixture->result_id,
                                          &fixture->payload_len);
    assert(mesh_init_command_result(&fixture->result_packet,
                                    node_id,
                                    GATEWAY,
                                    command_seq,
                                    seq,
                                    (uint8_t)fixture->payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&fixture->relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    node_id,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&fixture->relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&fixture->relay,
                                         &fixture->result_packet,
                                         fixture->payload,
                                         fixture->payload_len,
                                         now_ms,
                                         &fixture->offer) == PROTO_OK);
    assert(fixture->relay.outbox_record.expiry_s ==
           MESH_RELAY_RESULT_OFFER_EXPIRY_S);
}

static uint32_t first_missing_result_grant_retry_delay_ms(uint64_t node_id,
                                                          uint32_t random_value)
{
    struct result_offer_retry_fixture fixture;
    struct mesh_relay_result result;
    const uint32_t sent_ms = 1001u;
    const uint32_t timeout_ms = sent_ms + RREP_ACK_TIMEOUT_MS;

    result_offer_retry_fixture_init(&fixture,
                                    node_id,
                                    (uint32_t)node_id | 1u,
                                    (uint16_t)node_id | 1u,
                                    1000u);
    mesh_relay_note_tx_sent(&fixture.relay, &fixture.offer, sent_ms);
    assert(fixture.relay.outbox_record.retry_round == 1u);
    assert(mesh_relay_tick_with_random(&fixture.relay,
                                       timeout_ms,
                                       random_value,
                                       &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(fixture.relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    return fixture.relay.pending.retry_after_ms - timeout_ms;
}

static void test_missing_result_grant_uses_identity_scoped_fresh_random_backoff(void)
{
    const uint32_t same_identity_delay_a =
        first_missing_result_grant_retry_delay_ms(ANCHOR_A, 0x10203040u);
    const uint32_t same_identity_delay_b =
        first_missing_result_grant_retry_delay_ms(ANCHOR_A, 0x50607080u);
    const uint32_t other_identity_delay =
        first_missing_result_grant_retry_delay_ms(ANCHOR_C, 0x10203040u);

    assert(same_identity_delay_a >= MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS);
    assert(same_identity_delay_a <=
           MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS +
               (MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS / 2u));
    assert(same_identity_delay_b != same_identity_delay_a);
    assert(other_identity_delay != same_identity_delay_a);
}

static void test_missing_result_grant_retries_exact_identity_then_exhausts(void)
{
    struct result_offer_retry_fixture fixture;
    struct mesh_relay_result result;
    struct mesh_outbound terminal;
    struct result_offer decoded_offer;
    uint32_t sent_ms = 2001u;

    result_offer_retry_fixture_init(&fixture,
                                    ANCHOR_A,
                                    0x41424344u,
                                    55u,
                                    2000u);
    mesh_relay_note_tx_sent(&fixture.relay, &fixture.offer, sent_ms);

    for (uint8_t attempt = 1u;
         attempt < MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS;
         attempt++) {
        uint32_t timeout_ms = sent_ms + RREP_ACK_TIMEOUT_MS;
        uint32_t expected_base_ms = MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS;
        uint32_t retry_ms;

        for (uint8_t exponent = 1u; exponent < attempt; exponent++) {
            expected_base_ms *= 2u;
            if (expected_base_ms >= MESH_RELAY_RESULT_OFFER_RETRY_MAX_MS) {
                expected_base_ms = MESH_RELAY_RESULT_OFFER_RETRY_MAX_MS;
                break;
            }
        }
        assert(mesh_relay_tick_with_random(&fixture.relay,
                                           timeout_ms,
                                           0xABC00000u + attempt,
                                           &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(fixture.relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        retry_ms = fixture.relay.pending.retry_after_ms;
        assert(retry_ms - timeout_ms >= expected_base_ms);
        assert(retry_ms - timeout_ms <= expected_base_ms + (expected_base_ms / 2u));

        assert(mesh_relay_tick_with_random(&fixture.relay,
                                           retry_ms,
                                           0xDEF00000u + attempt,
                                           &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
        assert(result.retransmit.packet.msg_type == MSG_RESULT_OFFER);
        assert(result.retransmit.packet.session_id == fixture.result_packet.session_id);
        assert(result.retransmit.packet.seq == fixture.result_packet.seq);
        assert(result_offer_from_tlvs(result.retransmit.payload,
                                      result.retransmit.payload_len,
                                      &decoded_offer) == PROTO_OK);
        assert_command_result_id_equal(&decoded_offer.result_id, &fixture.result_id);
        sent_ms = retry_ms + 1u;
        mesh_relay_note_tx_sent(&fixture.relay, &result.retransmit, sent_ms);
        assert(fixture.relay.outbox_record.retry_round == attempt + 1u);
    }

    assert(mesh_relay_tick_with_random(&fixture.relay,
                                       sent_ms + RREP_ACK_TIMEOUT_MS,
                                       0x13579BDFu,
                                       &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(result.status == MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED);
    assert(mesh_relay_tx_active(&fixture.relay));
    assert(fixture.relay.pending.state ==
           MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(fixture.relay.outbox_record.valid);
    assert(fixture.relay.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED);
    assert(result.terminal.packet.msg_type == MSG_COMMAND_RESULT);
    assert(result.terminal.packet.src_id == fixture.result_packet.src_id);
    assert(result.terminal.packet.dst_id == fixture.result_packet.dst_id);
    assert(result.terminal.packet.session_id == fixture.result_packet.session_id);
    assert(result.terminal.packet.seq == fixture.result_packet.seq);
    assert(result.terminal.payload_len == fixture.payload_len);
    assert(memcmp(result.terminal.payload,
                  fixture.payload,
                  fixture.payload_len) == 0);
    terminal = result.terminal;

    assert(mesh_relay_tick_with_random(&fixture.relay,
                                       sent_ms + RREP_ACK_TIMEOUT_MS + 1u,
                                       0u,
                                       &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(result.status == MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED);
    assert(result.terminal.packet.session_id == terminal.packet.session_id);
    assert(result.terminal.packet.seq == terminal.packet.seq);
    assert(result.terminal.payload_len == terminal.payload_len);
    assert(memcmp(result.terminal.payload,
                  terminal.payload,
                  terminal.payload_len) == 0);
    assert(mesh_relay_commit_terminal_release(&fixture.relay,
                                              &terminal.packet,
                                              terminal.payload,
                                              terminal.payload_len) ==
           PROTO_OK);
    assert(!mesh_relay_tx_active(&fixture.relay));
    assert(!fixture.relay.outbox_record.valid);
    assert(mesh_relay_tick_with_random(&fixture.relay,
                                       sent_ms + RREP_ACK_TIMEOUT_MS + 2u,
                                       0u,
                                       &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
}

static void test_result_offer_deadline_is_terminal(void)
{
    struct result_offer_retry_fixture fixture;
    struct mesh_relay_result result;
    struct mesh_outbound terminal;

    result_offer_retry_fixture_init(&fixture,
                                    ANCHOR_A,
                                    0x61626364u,
                                    75u,
                                    3000u);
    mesh_relay_note_tx_sent(&fixture.relay, &fixture.offer, 3001u);
    assert(mesh_relay_tick_with_random(
               &fixture.relay,
               3000u + (MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u),
               0x2468ACE0u,
               &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(result.status == MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED);
    assert(mesh_relay_tx_active(&fixture.relay));
    assert(fixture.relay.pending.state ==
           MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(fixture.relay.outbox_record.valid);
    assert(fixture.relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_EXPIRED);
    terminal = result.terminal;
    assert(mesh_relay_commit_terminal_release(&fixture.relay,
                                              &terminal.packet,
                                              terminal.payload,
                                              terminal.payload_len) ==
           PROTO_OK);
    assert(!mesh_relay_tx_active(&fixture.relay));
    assert(!fixture.relay.outbox_record.valid);
}

static void test_result_grant_releases_pending_command_result(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x20212223u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 43u,
        .result_seq = 44u,
    };
    const struct result_grant grant = {
        .result_id = result_id,
        .granted_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .max_bytes = 64u,
        .event_offset_hint = 0u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct proto_packet result_packet;
    struct proto_packet grant_packet = {
        .msg_type = MSG_RESULT_GRANT,
        .flags = 0u,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = UINT32_C(0x20212223),
        .seq = 11u,
        .ttl = 1u,
    };
    struct mesh_outbound offer_tx;
    struct mesh_relay_result grant_result;
    uint8_t payload[96];
    uint8_t grant_payload[96];
    size_t payload_len = 0u;
    size_t grant_payload_len = 0u;

    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);
    assert(result_grant_append_tlvs(grant_payload,
                                    sizeof(grant_payload),
                                    &grant_payload_len,
                                    &grant) == PROTO_OK);
    grant_packet.payload_len = (uint16_t)grant_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&relay,
                                         &result_packet,
                                         payload,
                                         payload_len,
                                         4400u,
                                         &offer_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &grant_packet,
                                grant_payload,
                                grant_payload_len,
                                ANCHOR_B,
                                80u,
                                4410u,
                                &grant_result) == PROTO_OK);

    assert(grant_result.status == PROTO_OK);
    assert(has_action(&grant_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(!relay.pending.result_offer_active);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(grant_result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
    assert(grant_result.retransmit.packet.src_id == ANCHOR_A);
    assert(grant_result.retransmit.packet.dst_id == GATEWAY);
    assert(grant_result.retransmit.next_hop_id == ANCHOR_B);
    assert(grant_result.retransmit.radio_channel == UWB_CHANNEL_MESH_PAYLOAD);
    assert(grant_result.retransmit.payload_len == payload_len);
    assert(memcmp(grant_result.retransmit.payload, payload, payload_len) == 0);
}

static void test_forwarded_child_result_offer_snapshot_restores_after_reinit(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x20212224u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 43u,
        .result_seq = 45u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound offer_tx;
    struct mesh_relay_result tick_result;
    struct result_offer decoded_offer = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&relay,
                                         &result_packet,
                                         payload,
                                         payload_len,
                                         4400u,
                                         &offer_tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &offer_tx, 4401u);
    assert(relay.pending.result_offer_active);
    assert(relay.pending.queued_at_ms == 4400u);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.retry_round == 1u);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             4410u,
                                             &snapshot) == PROTO_OK);
    assert(snapshot.pending.queued_at_ms == 4410u);
    assert(snapshot.record.age_ms_saturating == 10u);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              5000u) == PROTO_OK);
    assert(restored.pending.result_offer_active);
    assert(restored.pending.packet.src_id == ANCHOR_A);
    assert(restored.outbox_record.valid);
    assert(restored.outbox_record.retry_round == 1u);
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK);

    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT);
    assert(restored.pending.gateway_ack_deadline_ms == 5001u);
    assert(mesh_relay_tick_with_random(&restored,
                                       5001u,
                                       0x12345678u,
                                       &tick_result) == PROTO_OK);
    assert(tick_result.actions == MESH_RELAY_ACTION_NONE);
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored.pending.retry_after_ms - 5001u >=
           MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS);
    assert(restored.pending.retry_after_ms - 5001u <=
           MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS +
               (MESH_RELAY_RESULT_OFFER_RETRY_BASE_MS / 2u));
    assert(mesh_relay_tick(&restored,
                           restored.pending.retry_after_ms,
                           &tick_result) == PROTO_OK);
    assert(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(tick_result.retransmit.packet.msg_type == MSG_RESULT_OFFER);
    assert(tick_result.retransmit.packet.src_id == ANCHOR_B);
    assert(tick_result.retransmit.packet.dst_id == GATEWAY);
    assert(result_offer_from_tlvs(tick_result.retransmit.payload,
                                  tick_result.retransmit.payload_len,
                                  &decoded_offer) == PROTO_OK);
    assert_command_result_id_equal(&decoded_offer.result_id, &result_id);
}

static void test_forwarded_child_result_payload_snapshot_restores_after_grant(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x20212225u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 43u,
        .result_seq = 46u,
    };
    const struct result_grant grant = {
        .result_id = result_id,
        .granted_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .max_bytes = 64u,
        .event_offset_hint = 0u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet result_packet;
    struct proto_packet grant_packet = {
        .msg_type = MSG_RESULT_GRANT,
        .flags = 0u,
        .src_id = GATEWAY,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x20212225),
        .seq = 15u,
        .ttl = 1u,
    };
    struct mesh_outbound offer_tx;
    struct mesh_relay_result grant_result;
    struct mesh_relay_result tick_result;
    uint8_t payload[96];
    uint8_t grant_payload[96];
    size_t payload_len = 0u;
    size_t grant_payload_len = 0u;

    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);
    assert(result_grant_append_tlvs(grant_payload,
                                    sizeof(grant_payload),
                                    &grant_payload_len,
                                    &grant) == PROTO_OK);
    grant_packet.payload_len = (uint16_t)grant_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&relay,
                                         &result_packet,
                                         payload,
                                         payload_len,
                                         4400u,
                                         &offer_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &grant_packet,
                                grant_payload,
                                grant_payload_len,
                                GATEWAY,
                                80u,
                                4410u,
                                &grant_result) == PROTO_OK);
    assert(has_action(&grant_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(!relay.pending.result_offer_active);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             4420u,
                                             &snapshot) == PROTO_OK);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              5000u) == PROTO_OK);
    assert(!restored.pending.result_offer_active);
    assert(restored.pending.packet.src_id == ANCHOR_A);
    assert(restored.outbox_record.valid);
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);

    assert(mesh_relay_tick(&restored,
                           5000u + RELAY_BUSY_RETRY_MIN_MS,
                           &tick_result) == PROTO_OK);
    assert(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(tick_result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
    assert(tick_result.retransmit.packet.src_id == ANCHOR_A);
    assert(tick_result.retransmit.packet.dst_id == GATEWAY);
    assert(tick_result.retransmit.next_hop_id == GATEWAY);
    assert(tick_result.retransmit.payload_len == payload_len);
    assert(memcmp(tick_result.retransmit.payload, payload, payload_len) == 0);
}

static void test_result_busy_retries_result_offer_not_payload(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x30313233u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 45u,
        .result_seq = 46u,
    };
    const struct result_busy busy = {
        .result_id = result_id,
        .retry_after_ms = RELAY_BUSY_RETRY_MIN_MS,
        .capacity_state = RELAY_CAP_YELLOW,
        .capacity_validity_interval_ms = RELAY_BUSY_RETRY_MIN_MS,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct proto_packet result_packet;
    struct proto_packet busy_packet = {
        .msg_type = MSG_RESULT_BUSY,
        .flags = 0u,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = UINT32_C(0x30313233),
        .seq = 12u,
        .ttl = 1u,
    };
    struct mesh_outbound offer_tx;
    struct mesh_relay_result busy_result;
    struct mesh_relay_result tick_result;
    uint8_t payload[96];
    uint8_t busy_payload[128];
    size_t payload_len = 0u;
    size_t busy_payload_len = 0u;
    uint32_t retry_delay_ms;

    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);
    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          result_packet.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     result_packet.seq) == PROTO_OK);
    assert(result_busy_append_tlvs(busy_payload,
                                   sizeof(busy_payload),
                                   &busy_payload_len,
                                   &busy) == PROTO_OK);
    busy_packet.payload_len = (uint16_t)busy_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&relay,
                                         &result_packet,
                                         payload,
                                         payload_len,
                                         4400u,
                                         &offer_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &busy_packet,
                                            busy_payload,
                                            busy_payload_len,
                                            ANCHOR_B,
                                            80u,
                                            4410u,
                                            UINT32_C(0x4f3a2b1c),
                                            &busy_result) == PROTO_OK);
    assert(has_action(&busy_result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.result_offer_active);
    retry_delay_ms = relay.pending.retry_after_ms - 4410u;
    assert(retry_delay_ms >= RELAY_BUSY_RETRY_MIN_MS);
    assert(retry_delay_ms <=
           RELAY_BUSY_RETRY_MIN_MS + (RELAY_BUSY_RETRY_MIN_MS / 2u));
    assert(retry_delay_ms != RELAY_BUSY_RETRY_MIN_MS);

    assert(mesh_relay_tick(&relay,
                           relay.pending.retry_after_ms,
                           &tick_result) == PROTO_OK);
    assert(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(tick_result.retransmit.packet.msg_type == MSG_RESULT_OFFER);
    assert(tick_result.retransmit.packet.session_id == result_packet.session_id);
    assert(tick_result.retransmit.packet.seq == result_packet.seq);
    assert(tick_result.retransmit.next_hop_id == ANCHOR_B);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT);
}

static void test_result_busy_preserves_proven_route_across_deadline_wrap(void)
{
    const uint32_t busy_received_ms =
        UINT32_MAX - RELAY_BUSY_RETRY_MIN_MS + 1u;
    struct result_offer_retry_fixture fixture;
    struct result_busy busy = {0};
    struct proto_packet busy_packet = {
        .msg_type = MSG_RESULT_BUSY,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = UINT32_C(0x95a5b5c5),
        .seq = 13u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    struct route_candidate route_before;
    const struct route_candidate *selected;
    uint8_t busy_payload[128];
    size_t busy_payload_len = 0u;

    result_offer_retry_fixture_init(&fixture,
                                    ANCHOR_A,
                                    UINT32_C(0x30313235),
                                    49u,
                                    busy_received_ms - 10u);
    selected = route_selected(&fixture.relay.upstream);
    assert(selected != NULL);
    route_before = *selected;
    busy_packet.session_id = fixture.result_packet.session_id;
    busy.result_id = fixture.result_id;
    busy.retry_after_ms = RELAY_BUSY_RETRY_MIN_MS;
    busy.capacity_state = RELAY_CAP_YELLOW;
    busy.capacity_validity_interval_ms = RELAY_BUSY_RETRY_MIN_MS;
    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          fixture.result_packet.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     fixture.result_packet.seq) == PROTO_OK);
    assert(result_busy_append_tlvs(busy_payload,
                                   sizeof(busy_payload),
                                   &busy_payload_len,
                                   &busy) == PROTO_OK);
    busy_packet.payload_len = (uint16_t)busy_payload_len;

    assert(mesh_relay_handle_rx_with_random(&fixture.relay,
                                            &busy_packet,
                                            busy_payload,
                                            busy_payload_len,
                                            ANCHOR_B,
                                            80u,
                                            busy_received_ms,
                                            UINT32_C(0x4f3a2b1c),
                                            &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
    selected = route_selected(&fixture.relay.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(memcmp(selected, &route_before, sizeof(route_before)) == 0);
}

static void test_result_busy_ignores_mismatched_command_result_identity(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 3u,
        .command_seq = 0x30313234u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 47u,
        .result_seq = 48u,
    };
    struct command_result_id wrong_id = result_id;
    struct result_busy busy = {
        .retry_after_ms = RELAY_BUSY_RETRY_MIN_MS,
        .capacity_state = RELAY_CAP_YELLOW,
        .capacity_validity_interval_ms = RELAY_BUSY_RETRY_MIN_MS,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct proto_packet result_packet;
    struct proto_packet busy_packet = {
        .msg_type = MSG_RESULT_BUSY,
        .flags = 0u,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = UINT32_C(0x30313234),
        .seq = 13u,
        .ttl = 1u,
    };
    struct mesh_outbound offer_tx;
    struct mesh_relay_result busy_result;
    uint8_t payload[96];
    uint8_t busy_payload[128];
    size_t payload_len = 0u;
    size_t busy_payload_len = 0u;

    wrong_id.result_seq++;
    busy.result_id = wrong_id;
    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);
    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          result_packet.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     result_packet.seq) == PROTO_OK);
    assert(result_busy_append_tlvs(busy_payload,
                                   sizeof(busy_payload),
                                   &busy_payload_len,
                                   &busy) == PROTO_OK);
    busy_packet.payload_len = (uint16_t)busy_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_result_offer(&relay,
                                         &result_packet,
                                         payload,
                                         payload_len,
                                         4400u,
                                         &offer_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &busy_packet,
                                busy_payload,
                                busy_payload_len,
                                ANCHOR_B,
                                80u,
                                4410u,
                                &busy_result) == PROTO_OK);

    assert(busy_result.status == PROTO_OK);
    assert(!has_action(&busy_result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT);
    assert(relay.pending.result_offer_active);
    assert(relay.pending.retry_after_ms == 0u);
}

static void test_relay_busy_defers_matching_pending_tx(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct proto_packet report;
    struct proto_packet busy;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_event_timing timing = {0};
    struct mesh_relay_event_timing_entry timing_before;
    struct mesh_event_params params = channel9_params(5000u);
    struct route_candidate route_before;
    uint8_t payload[1] = {0x33u};
    uint8_t busy_payload[32];
    size_t busy_payload_len = 0u;
    uint32_t retry_delay_ms;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_B, &timing) == PROTO_OK);
    route_before = *route_selected(&relay.upstream);
    timing_before = *find_event_timing(&relay, ANCHOR_B);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    82u,
                                    7u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               5000u,
                               &tx) == PROTO_OK);

    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          tx.packet.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     tx.packet.seq) == PROTO_OK);
    assert(tlv_append_u16(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_RETRY_AFTER_MS,
                          RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    assert(tlv_append_u8(busy_payload,
                         sizeof(busy_payload),
                         &busy_payload_len,
                         TLV_RELAY_CAPACITY_STATE,
                         RELAY_CAP_RED) == PROTO_OK);
    assert(tlv_append_u16(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                          RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    busy.msg_type = MSG_RELAY_BUSY;
    busy.flags = 0u;
    busy.src_id = ANCHOR_B;
    busy.dst_id = ANCHOR_A;
    busy.session_id = tx.packet.session_id;
    busy.seq = 9u;
    busy.ttl = 1u;
    busy.payload_len = (uint16_t)busy_payload_len;

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &busy,
                                            busy_payload,
                                            busy_payload_len,
                                            ANCHOR_B,
                                            80u,
                                            5010u,
                                            UINT32_C(0x76543210),
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_delay_ms = relay.pending.retry_after_ms - 5010u;
    assert(retry_delay_ms >= RELAY_BUSY_RETRY_MIN_MS);
    assert(retry_delay_ms <=
           RELAY_BUSY_RETRY_MIN_MS + (RELAY_BUSY_RETRY_MIN_MS / 2u));
    assert(route_selected(&relay.upstream) != NULL);
    assert(memcmp(route_selected(&relay.upstream),
                  &route_before,
                  sizeof(route_before)) == 0);
    assert(find_event_timing(&relay, ANCHOR_B) != NULL);
    assert(memcmp(find_event_timing(&relay, ANCHOR_B),
                  &timing_before,
                  sizeof(timing_before)) == 0);
}

static void test_consecutive_relay_busy_retries_preserve_packet_and_escalate(void)
{
    static const uint32_t expected_base_ms[] = {
        500u, 1000u, 2000u, 4000u, 5000u, 5000u,
    };
    static const uint32_t random_values[] = {
        UINT32_C(0x10203040), UINT32_C(0x50607080),
        UINT32_C(0x90a0b0c0), UINT32_C(0xd0e0f001),
        UINT32_C(0x23456789), UINT32_C(0xabcdef01),
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 3u, 90u);
    struct proto_packet report;
    struct proto_packet busy = {
        .msg_type = MSG_RELAY_BUSY,
        .flags = 0u,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = 0x77889900u,
        .seq = 20u,
        .ttl = 1u,
    };
    struct mesh_outbound tx;
    struct mesh_relay_result busy_result;
    struct mesh_relay_result tick_result;
    uint8_t payload[4] = {0x31u, 0x41u, 0x59u, 0x26u};
    uint8_t busy_payload[32];
    size_t busy_payload_len = 0u;
    uint32_t now_ms = 5000u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    busy.session_id,
                                    7u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               now_ms,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, now_ms);

    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          report.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     report.seq) == PROTO_OK);
    assert(tlv_append_u16(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_RETRY_AFTER_MS,
                          RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    assert(tlv_append_u8(busy_payload,
                         sizeof(busy_payload),
                         &busy_payload_len,
                         TLV_RELAY_CAPACITY_STATE,
                         RELAY_CAP_YELLOW) == PROTO_OK);
    assert(tlv_append_u16(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                          RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    busy.payload_len = (uint16_t)busy_payload_len;

    for (size_t round = 0u;
         round < sizeof(expected_base_ms) / sizeof(expected_base_ms[0]);
         round++) {
        uint32_t retry_delay_ms;
        uint32_t retry_at_ms;

        now_ms += 10u;
        busy.seq = (uint16_t)(20u + round);
        assert(mesh_relay_handle_rx_with_random(&relay,
                                                &busy,
                                                busy_payload,
                                                busy_payload_len,
                                                ANCHOR_B,
                                                80u,
                                                now_ms,
                                                random_values[round],
                                                &busy_result) == PROTO_OK);
        assert(busy_result.status == PROTO_OK);
        assert(has_action(&busy_result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        assert(relay.pending.busy_retry_round == round + 1u);
        retry_at_ms = relay.pending.retry_after_ms;
        retry_delay_ms = retry_at_ms - now_ms;
        assert(retry_delay_ms >= expected_base_ms[round]);
        assert(retry_delay_ms <=
               expected_base_ms[round] + (expected_base_ms[round] / 2u));
        assert(relay.pending.packet.msg_type == report.msg_type);
        assert(relay.pending.packet.src_id == report.src_id);
        assert(relay.pending.packet.dst_id == report.dst_id);
        assert(relay.pending.packet.session_id == report.session_id);
        assert(relay.pending.packet.seq == report.seq);
        assert(relay.pending.payload_len == sizeof(payload));
        assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);

        assert(mesh_relay_tick_with_random(&relay,
                                           retry_at_ms,
                                           random_values[round] ^ UINT32_MAX,
                                           &tick_result) == PROTO_OK);
        assert(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
        assert(tick_result.retransmit.packet.msg_type == report.msg_type);
        assert(tick_result.retransmit.packet.src_id == report.src_id);
        assert(tick_result.retransmit.packet.dst_id == report.dst_id);
        assert(tick_result.retransmit.packet.session_id == report.session_id);
        assert(tick_result.retransmit.packet.seq == report.seq);
        assert(tick_result.retransmit.payload_len == sizeof(payload));
        assert(memcmp(tick_result.retransmit.payload,
                      payload,
                      sizeof(payload)) == 0);
        mesh_relay_note_tx_sent(&relay, &tick_result.retransmit, retry_at_ms);
        now_ms = retry_at_ms;
    }
}

static void test_result_busy_and_small_grant_use_fresh_random_backoff(void)
{
    enum { SAMPLE_COUNT = 16 };
    struct result_offer_retry_fixture baseline;
    struct result_offer_retry_fixture trial;
    struct result_busy busy;
    struct result_grant grant;
    struct proto_packet busy_packet = {
        .msg_type = MSG_RESULT_BUSY,
        .flags = 0u,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = 0x23242526u,
        .seq = 81u,
        .ttl = 1u,
    };
    struct proto_packet grant_packet = {
        .msg_type = MSG_RESULT_GRANT,
        .flags = 0u,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = 0x2728292au,
        .seq = 82u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t busy_payload[128];
    uint8_t grant_payload[128];
    size_t busy_payload_len = 0u;
    size_t grant_payload_len = 0u;
    uint32_t busy_delays[SAMPLE_COUNT];
    uint32_t grant_delays[SAMPLE_COUNT];
    const uint32_t now_ms = 4010u;

    result_offer_retry_fixture_init(&baseline,
                                    ANCHOR_A,
                                    0x30313240u,
                                    61u,
                                    4000u);
    busy_packet.session_id = baseline.result_packet.session_id;
    grant_packet.session_id = baseline.result_packet.session_id;
    mesh_relay_note_tx_sent(&baseline.relay, &baseline.offer, 4001u);

    memset(&busy, 0, sizeof(busy));
    busy.result_id = baseline.result_id;
    busy.retry_after_ms = RELAY_BUSY_RETRY_MIN_MS;
    busy.capacity_state = RELAY_CAP_YELLOW;
    busy.capacity_validity_interval_ms = RELAY_BUSY_RETRY_MIN_MS;
    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          baseline.result_packet.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     baseline.result_packet.seq) == PROTO_OK);
    assert(result_busy_append_tlvs(busy_payload,
                                   sizeof(busy_payload),
                                   &busy_payload_len,
                                   &busy) == PROTO_OK);
    busy_packet.payload_len = (uint16_t)busy_payload_len;

    memset(&grant, 0, sizeof(grant));
    grant.result_id = baseline.result_id;
    grant.granted_channel = UWB_CHANNEL_MESH_PAYLOAD;
    grant.max_bytes = (uint16_t)(baseline.payload_len - 1u);
    assert(result_grant_append_tlvs(grant_payload,
                                    sizeof(grant_payload),
                                    &grant_payload_len,
                                    &grant) == PROTO_OK);
    grant_packet.payload_len = (uint16_t)grant_payload_len;

    for (size_t i = 0u; i < SAMPLE_COUNT; i++) {
        uint32_t random_value =
            UINT32_C(0x9e3779b9) * (uint32_t)(i + 1u);

        trial = baseline;
        assert(mesh_relay_handle_rx_with_random(&trial.relay,
                                                &busy_packet,
                                                busy_payload,
                                                busy_payload_len,
                                                ANCHOR_B,
                                                80u,
                                                now_ms,
                                                random_value,
                                                &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
        busy_delays[i] = trial.relay.pending.retry_after_ms - now_ms;
        assert(busy_delays[i] >= RELAY_BUSY_RETRY_MIN_MS);
        assert(busy_delays[i] <=
               RELAY_BUSY_RETRY_MIN_MS + (RELAY_BUSY_RETRY_MIN_MS / 2u));
        assert(trial.relay.pending.packet.session_id ==
               baseline.result_packet.session_id);
        assert(trial.relay.pending.packet.seq == baseline.result_packet.seq);
        assert(trial.relay.pending.payload_len == baseline.payload_len);
        assert(memcmp(trial.relay.pending.payload,
                      baseline.payload,
                      baseline.payload_len) == 0);

        trial = baseline;
        assert(mesh_relay_handle_rx_with_random(&trial.relay,
                                                &grant_packet,
                                                grant_payload,
                                                grant_payload_len,
                                                ANCHOR_B,
                                                80u,
                                                now_ms,
                                                random_value,
                                                &result) == PROTO_OK);
        assert(result.status == PROTO_ERR_NO_SPACE);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        grant_delays[i] = trial.relay.pending.retry_after_ms - now_ms;
        assert(grant_delays[i] >= RELAY_BUSY_RETRY_MIN_MS);
        assert(grant_delays[i] <=
               RELAY_BUSY_RETRY_MIN_MS + (RELAY_BUSY_RETRY_MIN_MS / 2u));
        assert(trial.relay.pending.packet.session_id ==
               baseline.result_packet.session_id);
        assert(trial.relay.pending.packet.seq == baseline.result_packet.seq);
        assert(trial.relay.pending.payload_len == baseline.payload_len);
        assert(memcmp(trial.relay.pending.payload,
                      baseline.payload,
                      baseline.payload_len) == 0);
    }

    assert(unique_u32_count(busy_delays, SAMPLE_COUNT) >= 8u);
    assert(unique_u32_count(grant_delays, SAMPLE_COUNT) >= 8u);
    assert(busy_delays[0] != RELAY_BUSY_RETRY_MIN_MS ||
           busy_delays[1] != RELAY_BUSY_RETRY_MIN_MS);
    assert(grant_delays[0] != RELAY_BUSY_RETRY_MIN_MS ||
           grant_delays[1] != RELAY_BUSY_RETRY_MIN_MS);
}

static void test_collection_retry_uses_fresh_random_for_same_identity_round(void)
{
    enum { SAMPLE_COUNT = 16 };
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 0x51525354u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 91u,
        .result_seq = 92u,
    };
    struct mesh_relay baseline;
    struct mesh_relay trial;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 90u);
    struct proto_packet result_packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    size_t result_payload_len = 0u;
    uint32_t delays[SAMPLE_COUNT];
    uint32_t legacy_delay_a;
    uint32_t legacy_delay_b;
    uint32_t explicit_zero_delay;
    uint32_t timeout_ms;
    const uint32_t jitter_ms =
        (COLLECTION_RETRY_ROUND_0_MS * COLLECTION_RETRY_JITTER_PERCENT) / 100u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            64u,
                                            &result_id,
                                            0x61626364u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&baseline,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    13u);
    assert(route_upsert_candidate(&baseline.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&baseline,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               6000u,
                               &tx) == PROTO_OK);
    timeout_ms = baseline.pending.gateway_ack_deadline_ms + 1u;

    trial = baseline;
    assert(mesh_relay_tick(&trial, timeout_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    legacy_delay_a = trial.pending.retry_after_ms - timeout_ms;
    trial = baseline;
    assert(mesh_relay_tick(&trial, timeout_ms, &result) == PROTO_OK);
    legacy_delay_b = trial.pending.retry_after_ms - timeout_ms;
    trial = baseline;
    assert(mesh_relay_tick_with_random(&trial,
                                       timeout_ms,
                                       0u,
                                       &result) == PROTO_OK);
    explicit_zero_delay = trial.pending.retry_after_ms - timeout_ms;
    assert(legacy_delay_a == legacy_delay_b);
    assert(legacy_delay_a == explicit_zero_delay);

    for (size_t i = 0u; i < SAMPLE_COUNT; i++) {
        uint32_t random_value =
            UINT32_C(0x7f4a7c15) * (uint32_t)(i + 1u);

        trial = baseline;
        assert(mesh_relay_tick_with_random(&trial,
                                           timeout_ms,
                                           random_value,
                                           &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
        assert(trial.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        assert(trial.outbox_record.retry_round == 1u);
        delays[i] = trial.pending.retry_after_ms - timeout_ms;
        assert(delays[i] >= COLLECTION_RETRY_ROUND_0_MS - jitter_ms);
        assert(delays[i] <= COLLECTION_RETRY_ROUND_0_MS + jitter_ms);
        assert(trial.pending.packet.session_id == result_packet.session_id);
        assert(trial.pending.packet.seq == result_packet.seq);
        assert(trial.pending.payload_len == result_payload_len);
        assert(memcmp(trial.pending.payload,
                      result_payload,
                      result_payload_len) == 0);
    }
    assert(unique_u32_count(delays, SAMPLE_COUNT) >= 8u);
}

static void test_alternate_parent_selection_preserves_immediate_retry(void)
{
    struct mesh_relay relay;
    struct route_candidate primary = direct_gateway_route(GATEWAY, 17u, 90u);
    struct route_candidate alternate = direct_gateway_route(ANCHOR_B, 17u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[2] = {0xA5u, 0x5Au};
    uint32_t now_ms = 7000u;

    alternate.hop_count = 1u;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 17u);
    assert(route_upsert_candidate(&relay.upstream, &primary) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &alternate) == PROTO_OK);
    assert(route_selected(&relay.upstream) != NULL);
    assert(route_selected(&relay.upstream)->next_hop_id == GATEWAY);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    0x71727374u,
                                    93u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               now_ms,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, now_ms);

    for (uint8_t failure = 1u; failure <= ROUTE_RETRIES_PER_CANDIDATE; failure++) {
        uint32_t random_value = UINT32_C(0x11111111) * failure;
        uint32_t retry_at_ms;

        now_ms += 100u;
        assert(mesh_relay_note_pending_parent_failure(&relay,
                                                      now_ms,
                                                      random_value,
                                                      &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(relay.pending.next_hop_id == GATEWAY);
        assert(relay.pending.retry_after_ms ==
               now_ms + mesh_relay_retry_backoff_ms(failure, random_value));
        retry_at_ms = relay.pending.retry_after_ms;
        assert(mesh_relay_tick(&relay, retry_at_ms, &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
        assert(result.retransmit.next_hop_id == GATEWAY);
        mesh_relay_note_tx_sent(&relay, &result.retransmit, retry_at_ms);
        now_ms = retry_at_ms;
    }

    now_ms += 100u;
    assert(mesh_relay_note_pending_parent_failure(&relay,
                                                  now_ms,
                                                  UINT32_C(0x89abcdef),
                                                  &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(route_selected(&relay.upstream) != NULL);
    assert(route_selected(&relay.upstream)->next_hop_id == ANCHOR_B);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.next_hop_id == ANCHOR_B);
    assert(relay.pending.retry_after_ms == now_ms);
    assert(relay.pending.packet.session_id == report.session_id);
    assert(relay.pending.packet.seq == report.seq);
    assert(relay.pending.payload_len == sizeof(payload));
    assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);
}

static void test_local_gateway_bound_tx_waits_for_gateway_ack(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored_confirm;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_outbound confirm;
    struct mesh_relay_outbox_snapshot confirm_snapshot;
    struct mesh_relay_result result;
    struct proto_packet ack;
    struct proto_packet stale_ack;
    struct proto_packet stale_report;
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t stale_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;
    size_t stale_ack_payload_len = 0u;
    uint8_t payload[128];
    uint8_t stale_payload[128];
    size_t payload_len;
    size_t stale_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           88u,
                                           7u,
                                           1200,
                                           payload,
                                           sizeof(payload));
    stale_payload_len = build_valid_click_report(&stale_report,
                                                 ANCHOR_A,
                                                 88u,
                                                 7u,
                                                 1300,
                                                 stale_payload,
                                                 sizeof(stale_payload));
    assert(stale_payload_len == payload_len);

    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(tx.next_hop_id == ANCHOR_B);
    assert(tx.packet.seq == report.seq);
    assert_outbox_tracks_packet(&relay,
                                &report,
                                payload,
                                payload_len,
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);

    build_gateway_ack_for_packet(&ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 2u,
                                 &report,
                                 payload,
                                 payload_len);
    build_gateway_ack_for_packet(&stale_ack,
                                 stale_ack_payload,
                                 sizeof(stale_ack_payload),
                                 &stale_ack_payload_len,
                                 ANCHOR_A,
                                 3u,
                                 &report,
                                 stale_payload,
                                 stale_payload_len);

    assert(mesh_relay_handle_rx(&relay,
                                &stale_ack,
                                stale_ack_payload,
                                stale_ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5090u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.outbox_record.valid);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.packet.msg_type == report.msg_type);
    assert(relay.pending.gateway_ack_confirm_pending);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
    assert(relay.pending.radio_channel == UWB_CHANNEL_MESH_PAYLOAD);
    assert(relay.outbox_record.valid);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             5100u,
                                             &confirm_snapshot) == PROTO_OK);
    mesh_relay_init(&restored_confirm,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    5u);
    assert(mesh_relay_restore_outbox_snapshot(&restored_confirm,
                                              &confirm_snapshot,
                                              6100u) == PROTO_OK);
    assert(restored_confirm.pending.packet.msg_type == report.msg_type);
    assert(!restored_confirm.pending.gateway_ack_confirm_pending);
    assert(restored_confirm.pending.gateway_ack_recovery_flags != 0u);
    assert(restored_confirm.pending.state ==
           MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored_confirm.pending.radio_channel ==
           UWB_CHANNEL_MESH_PAYLOAD);
    assert(route_upsert_candidate(&restored_confirm.upstream, &route) ==
           PROTO_OK);
    assert(mesh_relay_tick(&restored_confirm, 6101u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tick(
               &restored_confirm,
               6100u + MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS,
               &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    assert(result.retransmit.packet.msg_type == report.msg_type);
    assert(memcmp(result.retransmit.payload, payload, payload_len) == 0);
    assert(result.retransmit.radio_channel == UWB_CHANNEL_MESH_PAYLOAD);
    assert(mesh_relay_tick(&relay, 5101u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    assert(result.retransmit.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
    assert(result.retransmit.radio_channel == UWB_CHANNEL_MESH_PAYLOAD);
    confirm = result.retransmit;
    mesh_relay_note_tx_sent(&relay, &confirm, 5101u);
    build_gateway_ack_for_packet(&ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 4u,
                                 &confirm.packet,
                                 confirm.payload,
                                 confirm.payload_len);
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5110u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(mesh_relay_tx_active(&relay));
    assert(mesh_relay_commit_gateway_ack_confirm_terminal(
               &relay,
               &confirm.packet,
               confirm.payload,
               confirm.payload_len,
               5111u) == PROTO_OK);
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_GATEWAY_ACKED);
    assert(relay.outbox_record.gateway_acked);
}

static void test_gateway_reset_replays_immutable_original_after_new_epoch(void)
{
    struct mesh_relay source;
    struct mesh_relay new_gateway;
    struct mesh_gateway_ack_store gateway_ack_store;
    struct route_candidate old_route = direct_gateway_route(GATEWAY, 5u, 90u);
    struct proto_packet report;
    struct proto_packet old_ack;
    struct mesh_outbound original_tx;
    struct mesh_outbound first_confirm;
    struct mesh_outbound first_replay;
    struct mesh_outbound second_replay;
    struct mesh_outbound final_confirm;
    struct mesh_outbound newer_adv;
    struct proto_packet expected_same_boot_confirm;
    struct mesh_relay_result source_result;
    struct mesh_relay_result gateway_result;
    struct mesh_gateway_ack_confirm_identity confirm_identity;
    uint8_t report_payload[128];
    uint8_t old_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t expected_same_boot_confirm_payload[
        MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t report_payload_len;
    size_t old_ack_payload_len = 0u;
    size_t expected_same_boot_confirm_payload_len = 0u;
    uint32_t first_replay_ms;
    uint32_t delayed_old_ack_ms;
    uint32_t second_replay_ms;

    mesh_relay_init(&source,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    5u);
    assert(route_upsert_candidate(&source.upstream, &old_route) == PROTO_OK);
    report_payload_len = build_valid_click_report(&report,
                                                  ANCHOR_A,
                                                  900u,
                                                  9u,
                                                  1200,
                                                  report_payload,
                                                  sizeof(report_payload));
    assert(mesh_relay_start_tx(&source,
                               &report,
                               report_payload,
                               report_payload_len,
                               1000u,
                               &original_tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&source, &original_tx, 1000u);
    build_gateway_ack_for_packet(&old_ack,
                                 old_ack_payload,
                                 sizeof(old_ack_payload),
                                 &old_ack_payload_len,
                                 ANCHOR_A,
                                 2u,
                                 &report,
                                 report_payload,
                                 report_payload_len);

    assert(mesh_relay_handle_rx(&source,
                                &old_ack,
                                old_ack_payload,
                                old_ack_payload_len,
                                GATEWAY,
                                90u,
                                1010u,
                                &source_result) == PROTO_OK);
    assert(source_result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(source.pending.gateway_ack_confirm_pending);
    assert(source.pending.gateway_ack_confirm_route_epoch == 5u);
    assert(source.pending.packet.msg_type == report.msg_type);
    assert(source.pending.payload_len == report_payload_len);
    assert(memcmp(source.pending.payload,
                  report_payload,
                  report_payload_len) == 0);

    /* Same-boot retries materialize the exact compact proof without changing
     * the original owner bytes. */
    assert(mesh_relay_pending_gateway_ack_confirm_wire(
               &source,
               1011u,
               &expected_same_boot_confirm,
               expected_same_boot_confirm_payload,
               sizeof(expected_same_boot_confirm_payload),
               &expected_same_boot_confirm_payload_len) == PROTO_OK);
    assert(mesh_relay_tick(&source, 1011u, &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    assert(source_result.retransmit.packet.msg_type ==
           MSG_GATEWAY_ACK_CONFIRM);
    first_confirm = source_result.retransmit;
    assert(first_confirm.packet.flags == expected_same_boot_confirm.flags);
    assert(first_confirm.packet.src_id == expected_same_boot_confirm.src_id);
    assert(first_confirm.packet.dst_id == expected_same_boot_confirm.dst_id);
    assert(first_confirm.packet.session_id ==
           expected_same_boot_confirm.session_id);
    assert(first_confirm.packet.seq == expected_same_boot_confirm.seq);
    assert(first_confirm.packet.ttl == expected_same_boot_confirm.ttl);
    assert(first_confirm.packet.message_age_ms ==
           expected_same_boot_confirm.message_age_ms);
    assert(first_confirm.payload_len ==
           expected_same_boot_confirm_payload_len);
    assert(memcmp(first_confirm.payload,
                  expected_same_boot_confirm_payload,
                  expected_same_boot_confirm_payload_len) == 0);
    mesh_relay_note_tx_sent(&source, &first_confirm, 1011u);
    assert(source.pending.packet.msg_type == report.msg_type);
    assert(memcmp(source.pending.payload,
                  report_payload,
                  report_payload_len) == 0);

    /* A reset gateway has empty ACK history, but a strictly newer validated
     * route epoch is same-wire proof that replay may target a new incarnation. */
    mesh_relay_init(&new_gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    6u);
    assert(mesh_relay_attach_gateway_ack_store(&new_gateway,
                                               &gateway_ack_store) == PROTO_OK);
    assert(mesh_relay_build_gateway_route_adv(&new_gateway,
                                              1u,
                                              1100u,
                                              &newer_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&source,
                                &newer_adv.packet,
                                newer_adv.payload,
                                newer_adv.payload_len,
                                GATEWAY,
                                95u,
                                1110u,
                                &source_result) == PROTO_OK);
    assert(has_action(&source_result,
                      MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING));
    assert(!source.pending.gateway_ack_confirm_pending);
    assert(source.pending.gateway_ack_recovery_flags != 0u);

    /* An indistinguishable old ACK arriving before physical replay is ignored. */
    assert(mesh_relay_handle_rx(&source,
                                &old_ack,
                                old_ack_payload,
                                old_ack_payload_len,
                                GATEWAY,
                                90u,
                                1111u,
                                &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_NONE);
    assert(!source.pending.gateway_ack_confirm_pending);

    first_replay_ms =
        1110u + MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS;
    assert(mesh_relay_tick(&source,
                           first_replay_ms,
                           &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    first_replay = source_result.retransmit;
    assert(first_replay.packet.msg_type == report.msg_type);
    assert(first_replay.payload_len == report_payload_len);
    assert(memcmp(first_replay.payload,
                  report_payload,
                  report_payload_len) == 0);
    mesh_relay_note_tx_sent(&source, &first_replay, first_replay_ms);

    /* The ACK wire has no incarnation. Even an old ACK that arrives after the
     * replay can arm another confirm, so newer-epoch authorization survives
     * and turns its missing confirmation into another exact original replay. */
    delayed_old_ack_ms = first_replay_ms + 100u;
    assert(mesh_relay_handle_rx(&source,
                                &old_ack,
                                old_ack_payload,
                                old_ack_payload_len,
                                GATEWAY,
                                90u,
                                delayed_old_ack_ms,
                                &source_result) == PROTO_OK);
    assert(source_result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(source.pending.gateway_ack_confirm_pending);
    assert(source.pending.gateway_ack_recovery_flags != 0u);
    assert(mesh_relay_tick(
               &source,
               delayed_old_ack_ms +
                   MESH_RELAY_GATEWAY_ACK_CONFIRM_REPLAY_MS,
               &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_NONE);
    assert(!source.pending.gateway_ack_confirm_pending);

    second_replay_ms =
        delayed_old_ack_ms + MESH_RELAY_GATEWAY_ACK_CONFIRM_REPLAY_MS +
        MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS;
    assert(mesh_relay_tick(&source,
                           second_replay_ms,
                           &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    second_replay = source_result.retransmit;
    assert(second_replay.packet.msg_type == report.msg_type);
    assert(second_replay.payload_len == report_payload_len);
    assert(memcmp(second_replay.payload,
                  report_payload,
                  report_payload_len) == 0);
    mesh_relay_note_tx_sent(&source, &second_replay, second_replay_ms);

    /* The replay re-enters the ordinary gateway host-acceptance boundary. */
    assert(mesh_relay_handle_rx(&new_gateway,
                                &second_replay.packet,
                                second_replay.payload,
                                second_replay.payload_len,
                                ANCHOR_A,
                                95u,
                                second_replay_ms + 1u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_commit_gateway_delivery(
               &new_gateway,
               &second_replay.packet,
               second_replay.payload,
               second_replay.payload_len,
               ANCHOR_A,
               second_replay_ms + 2u,
               &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_handle_rx(&source,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                95u,
                                second_replay_ms + 3u,
                                &source_result) == PROTO_OK);
    assert(source_result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(mesh_relay_tick(&source,
                           second_replay_ms + 4u,
                           &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    final_confirm = source_result.retransmit;
    assert(final_confirm.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
    mesh_relay_note_tx_sent(&source,
                            &final_confirm,
                            second_replay_ms + 4u);

    assert(mesh_relay_gateway_ack_confirm_history_match(
               &new_gateway,
               &final_confirm.packet,
               final_confirm.payload,
               final_confirm.payload_len,
               &confirm_identity) == PROTO_OK);
    assert(mesh_relay_commit_gateway_delivery(
               &new_gateway,
               &final_confirm.packet,
               final_confirm.payload,
               final_confirm.payload_len,
               ANCHOR_A,
               second_replay_ms + 5u,
               &gateway_result) == PROTO_OK);
    assert(mesh_relay_handle_rx(&source,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                95u,
                                second_replay_ms + 6u,
                                &source_result) == PROTO_OK);
    assert(source_result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(mesh_relay_commit_gateway_ack_confirm_terminal(
               &source,
               &final_confirm.packet,
               final_confirm.payload,
               final_confirm.payload_len,
               second_replay_ms + 7u) == PROTO_OK);
    assert(!mesh_relay_tx_active(&source));
}

static void test_transit_ack_confirm_without_original_expires_explicitly(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 6u, 90u);
    struct mesh_downlink_entry *source_downlink;
    struct proto_packet original;
    struct proto_packet confirm;
    struct proto_packet delayed_ack;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t original_payload[128];
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    uint8_t delayed_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t original_payload_len;
    size_t confirm_payload_len = 0u;
    size_t delayed_ack_payload_len = 0u;
    const uint32_t started_ms = 1000u;
    const uint32_t expiry_ms =
        started_ms + COMMAND_RESULT_EXPIRY_DEFAULT_S * 1000u;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    6u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    source_downlink = test_downlink_at_mutable(&relay, 0u);
    *source_downlink = (struct mesh_downlink_entry) {
        .target_id = ANCHOR_A,
        .next_hop_id = ANCHOR_A,
        .gateway_id = GATEWAY,
        .route_epoch = 6u,
        .last_seen_ms = started_ms,
        .hop_count = 1u,
        .quality = 90u,
        .valid = true,
    };

    original_payload_len = build_valid_click_report(&original,
                                                     ANCHOR_A,
                                                     901u,
                                                     10u,
                                                     1300,
                                                     original_payload,
                                                     sizeof(original_payload));
    assert(mesh_gateway_ack_confirm_payload_build(
               &original,
               original_payload,
               original_payload_len,
               confirm_payload,
               sizeof(confirm_payload),
               &confirm_payload_len) == PROTO_OK);
    assert(mesh_init_gateway_ack_confirm(&confirm,
                                         ANCHOR_A,
                                         GATEWAY,
                                         original.session_id,
                                         original.seq) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &confirm,
                               confirm_payload,
                               confirm_payload_len,
                               started_ms,
                               &tx) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(&relay,
                                                &tx,
                                                ANCHOR_A) == PROTO_OK);
    assert(relay.pending.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
    assert(!relay.pending.gateway_ack_confirm_pending);
    assert(relay.pending.gateway_ack_recovery_flags == 0u);
    assert(relay.outbox_record.expiry_s == COMMAND_RESULT_EXPIRY_DEFAULT_S);
    mesh_relay_note_tx_sent(&relay, &tx, started_ms);

    build_gateway_ack_for_packet(&delayed_ack,
                                 delayed_ack_payload,
                                 sizeof(delayed_ack_payload),
                                 &delayed_ack_payload_len,
                                 ANCHOR_A,
                                 11u,
                                 &confirm,
                                 confirm_payload,
                                 confirm_payload_len);
    assert(mesh_relay_handle_rx(&relay,
                                &delayed_ack,
                                delayed_ack_payload,
                                delayed_ack_payload_len,
                                GATEWAY,
                                90u,
                                expiry_ms - 1u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(
        &result,
        MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD);

    /* There is no source original behind this transit-only confirm. Even a
     * gateway ACK retained until the final legal millisecond cannot create
     * recovery bytes; terminal expiry is explicit instead of infinite debt. */
    assert(mesh_relay_tick(&relay, expiry_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(result.status == MESH_RELAY_ERR_OUTBOX_EXPIRED);
    assert(result.terminal.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(mesh_relay_commit_terminal_release(&relay,
                                              &result.terminal.packet,
                                              result.terminal.payload,
                                              result.terminal.payload_len) ==
           PROTO_OK);
    assert(!mesh_relay_tx_active(&relay));
}

static void test_delayed_old_ack_at_retention_boundary_expires_original(void)
{
    struct mesh_relay source;
    struct mesh_relay gateway;
    struct route_candidate old_route = direct_gateway_route(GATEWAY, 5u, 90u);
    struct proto_packet report;
    struct proto_packet old_ack;
    struct mesh_outbound original_tx;
    struct mesh_outbound newer_adv;
    struct mesh_relay_result result;
    uint8_t report_payload[128];
    uint8_t old_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t report_payload_len;
    size_t old_ack_payload_len = 0u;
    uint32_t replay_ms;
    uint32_t expiry_ms;

    mesh_relay_init(&source,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    5u);
    assert(route_upsert_candidate(&source.upstream, &old_route) == PROTO_OK);
    report_payload_len = build_valid_click_report(&report,
                                                  ANCHOR_A,
                                                  902u,
                                                  12u,
                                                  1400,
                                                  report_payload,
                                                  sizeof(report_payload));
    assert(mesh_relay_start_tx(&source,
                               &report,
                               report_payload,
                               report_payload_len,
                               1000u,
                               &original_tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&source, &original_tx, 1000u);
    build_gateway_ack_for_packet(&old_ack,
                                 old_ack_payload,
                                 sizeof(old_ack_payload),
                                 &old_ack_payload_len,
                                 ANCHOR_A,
                                 13u,
                                 &report,
                                 report_payload,
                                 report_payload_len);
    assert(mesh_relay_handle_rx(&source,
                                &old_ack,
                                old_ack_payload,
                                old_ack_payload_len,
                                GATEWAY,
                                90u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    6u);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              1u,
                                              1100u,
                                              &newer_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&source,
                                &newer_adv.packet,
                                newer_adv.payload,
                                newer_adv.payload_len,
                                GATEWAY,
                                95u,
                                1110u,
                                &result) == PROTO_OK);
    assert(has_action(&result,
                      MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING));
    replay_ms = 1110u + MESH_RELAY_GATEWAY_ACK_RECOVERY_QUARANTINE_MS;
    assert(mesh_relay_tick(&source, replay_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    assert(result.retransmit.packet.msg_type == report.msg_type);
    mesh_relay_note_tx_sent(&source, &result.retransmit, replay_ms);

    expiry_ms = source.outbox_record.created_uptime_ms +
        source.outbox_record.expiry_s * 1000u;
    assert(mesh_relay_handle_rx(&source,
                                &old_ack,
                                old_ack_payload,
                                old_ack_payload_len,
                                GATEWAY,
                                90u,
                                expiry_ms - 1u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(source.pending.gateway_ack_confirm_pending);

    /* Transit may legally retain the indistinguishable pre-reset ACK for the
     * entire one-day horizon. At the source horizon there is no time left for
     * another confirm/replay cycle, so fail explicitly with the immutable
     * original instead of promoting the late ACK_CONFIRM to unbounded debt. */
    assert(mesh_relay_tick(&source, expiry_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
    assert(result.status == MESH_RELAY_ERR_OUTBOX_EXPIRED);
    assert(result.terminal.packet.msg_type == report.msg_type);
    assert(result.terminal.payload_len == report_payload_len);
    assert(memcmp(result.terminal.payload,
                  report_payload,
                  report_payload_len) == 0);
    assert(mesh_relay_commit_terminal_release(&source,
                                              &result.terminal.packet,
                                              result.terminal.payload,
                                              result.terminal.payload_len) ==
           PROTO_OK);
    assert(!mesh_relay_tx_active(&source));
}

static void test_cancel_tx_if_matches_never_releases_another_exact_owner(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_outbound conflict;
    uint8_t payload[128];
    size_t payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           189u,
                                           8u,
                                           1200,
                                           payload,
                                           sizeof(payload));
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.outbox_record.valid);

    conflict = tx;
    conflict.packet.seq++;
    assert(mesh_relay_cancel_tx_if_matches(&relay, &conflict) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.outbox_record.valid);

    conflict = tx;
    conflict.payload[0] ^= 0x5au;
    assert(mesh_relay_cancel_tx_if_matches(&relay, &conflict) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.outbox_record.valid);

    assert(mesh_relay_cancel_tx_if_matches(&relay, &tx) == PROTO_OK);
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(mesh_relay_cancel_tx_if_matches(&relay, &tx) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_local_gateway_bound_tx_accepts_batched_gateway_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_outbound confirm;
    struct mesh_relay_result result;
    struct proto_packet ack;
    uint8_t ack_payload[160];
    uint8_t ambiguous_ack_payload[168];
    uint8_t session_list[2u * sizeof(uint32_t)];
    uint8_t seq_list[4];
    size_t ack_payload_len = 0u;
    size_t ambiguous_ack_payload_len;
    uint8_t payload[128];
    size_t payload_len;
    struct proto_packet first_report;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           188u,
                                           7u,
                                           1200,
                                           payload,
                                           sizeof(payload));

    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    first_report = report;
    first_report.seq = (uint16_t)(report.seq + 2u);
    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     first_report.seq) == PROTO_OK);
    proto_put_u32_le(&session_list[0], first_report.session_id);
    proto_put_u32_le(&session_list[sizeof(uint32_t)], report.session_id);
    assert(tlv_append_bytes(ack_payload,
                            sizeof(ack_payload),
                            &ack_payload_len,
                            TLV_MESH_ACK_SESSION_LIST,
                            session_list,
                            sizeof(session_list)) == PROTO_OK);
    proto_put_u16_le(&seq_list[0], first_report.seq);
    proto_put_u16_le(&seq_list[2], report.seq);
    assert(tlv_append_bytes(ack_payload,
                            sizeof(ack_payload),
                            &ack_payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seq_list,
                            sizeof(seq_list)) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack_payload,
                                             sizeof(ack_payload),
                                             &ack_payload_len,
                                             &first_report,
                                             payload,
                                             payload_len) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack_payload,
                                             sizeof(ack_payload),
                                             &ack_payload_len,
                                             &report,
                                             payload,
                                             payload_len) == PROTO_OK);
    assert(mesh_init_gateway_ack(&ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 first_report.session_id,
                                 2u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);

    memcpy(ambiguous_ack_payload, ack_payload, ack_payload_len);
    ambiguous_ack_payload_len = ack_payload_len;
    assert(mesh_append_requested_seq(ambiguous_ack_payload,
                                     sizeof(ambiguous_ack_payload),
                                     &ambiguous_ack_payload_len,
                                     report.seq) == PROTO_OK);
    ack.payload_len = (uint16_t)ambiguous_ack_payload_len;
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ambiguous_ack_payload,
                                ambiguous_ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5090u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(mesh_relay_tx_active(&relay));

    ack.payload_len = (uint16_t)ack_payload_len;
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5100u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(mesh_relay_tx_active(&relay));
    assert(mesh_relay_tick(&relay, 5101u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    confirm = result.retransmit;
    mesh_relay_note_tx_sent(&relay, &confirm, 5101u);
    build_gateway_ack_for_packet(&ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 3u,
                                 &confirm.packet,
                                 confirm.payload,
                                 confirm.payload_len);
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5110u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(mesh_relay_commit_gateway_ack_confirm_terminal(
               &relay,
               &confirm.packet,
               confirm.payload,
               confirm.payload_len,
               5111u) == PROTO_OK);
    assert(!mesh_relay_tx_active(&relay));
}

static void test_start_tx_initializes_earliest_tx_time(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    uint8_t payload[1] = {0x42u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    188u,
                                    7u,
                                    sizeof(payload)) == PROTO_OK);

    memset(&tx, 0xa5, sizeof(tx));
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               5000u,
                               &tx) == PROTO_OK);
    assert(tx.queued_at_ms == 5000u);
    assert(tx.earliest_tx_ms == 5000u);
    assert(tx.queued_at_valid);
    assert(tx.earliest_tx_valid);

    mesh_relay_cancel_tx(&relay);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               0u,
                               &tx) == PROTO_OK);
    assert(tx.queued_at_ms == 0u);
    assert(tx.earliest_tx_ms == 0u);
    assert(tx.queued_at_valid);
    assert(tx.earliest_tx_valid);
}

static void test_relayed_tx_waits_for_gateway_ack_after_next_hop_send(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay rejected;
    struct route_candidate route = direct_gateway_route(GATEWAY, 5u, 80u);
    struct proto_packet report;
    struct proto_packet ack;
    struct mesh_outbound tx;
    struct mesh_outbound confirm;
    struct mesh_relay_outbox_snapshot snapshot;
    struct mesh_relay_outbox_snapshot malformed_snapshot;
    struct mesh_pending_tx pending_before;
    struct mesh_outbox_record outbox_before;
    struct mesh_relay_result result;
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t wrong_seq_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;
    size_t wrong_seq_payload_len = 0u;
    uint8_t payload[128];
    size_t payload_len;
    struct proto_packet wrong_report;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 5u, 1u, 85u, 5900u);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           89u,
                                           8u,
                                           1200,
                                           payload,
                                           sizeof(payload));
    assert(report.ttl > 0u);
    report.ttl--;

    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               payload_len,
                               6000u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(&relay,
                                                &tx,
                                                ANCHOR_A) == PROTO_OK);
    assert(tx.next_hop_id == GATEWAY);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.packet_class == MSG_CLICK_REPORT);

    mesh_relay_note_tx_sent(&relay, &tx, 6050u);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    build_gateway_ack_for_packet(&ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 2u,
                                 &report,
                                 payload,
                                 payload_len);
    wrong_report = report;
    wrong_report.seq++;
    assert(mesh_append_requested_seq(wrong_seq_payload,
                                     sizeof(wrong_seq_payload),
                                     &wrong_seq_payload_len,
                                     wrong_report.seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(wrong_seq_payload,
                                             sizeof(wrong_seq_payload),
                                             &wrong_seq_payload_len,
                                             &wrong_report,
                                             payload,
                                             payload_len) == PROTO_OK);

    /* Generic transit state is persistable without weakening its exact
     * packet and payload identity. */
    assert(mesh_relay_export_outbox_snapshot(&relay, 6075u, &snapshot) ==
           PROTO_OK);
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    5u);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              7000u) == PROTO_OK);
    assert(restored.pending.packet.msg_type == report.msg_type);
    assert(restored.pending.packet.src_id == report.src_id);
    assert(restored.pending.packet.dst_id == report.dst_id);
    assert(restored.pending.packet.session_id == report.session_id);
    assert(restored.pending.packet.seq == report.seq);
    assert(restored.pending.payload_len == payload_len);
    assert(memcmp(restored.pending.payload, payload, payload_len) == 0);
    assert(restored.outbox_record.packet_id == relay.outbox_record.packet_id);
    assert(restored.outbox_record.payload_crc ==
           relay.outbox_record.payload_crc);

    malformed_snapshot = snapshot;
    malformed_snapshot.pending.payload[0] ^= 0xffu;
    mesh_relay_init(&rejected,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    5u);
    assert(mesh_relay_restore_outbox_snapshot(&rejected,
                                              &malformed_snapshot,
                                              7000u) == PROTO_ERR_MALFORMED);
    malformed_snapshot = snapshot;
    malformed_snapshot.local_id = ANCHOR_C;
    mesh_relay_init(&rejected,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    5u);
    assert(mesh_relay_restore_outbox_snapshot(&rejected,
                                              &malformed_snapshot,
                                              7000u) == PROTO_ERR_MALFORMED);

    pending_before = relay.pending;
    outbox_before = relay.outbox_record;
    ack.src_id = ANCHOR_C;
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                GATEWAY,
                                90u,
                                6080u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record,
                  &outbox_before,
                  sizeof(outbox_before)) == 0);

    ack.src_id = GATEWAY;
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_C,
                                90u,
                                6081u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record,
                  &outbox_before,
                  sizeof(outbox_before)) == 0);

    ack.session_id++;
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                GATEWAY,
                                90u,
                                6082u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record,
                  &outbox_before,
                  sizeof(outbox_before)) == 0);

    ack.session_id = report.session_id;
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                wrong_seq_payload,
                                wrong_seq_payload_len,
                                GATEWAY,
                                90u,
                                6083u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(memcmp(&relay.outbox_record,
                  &outbox_before,
                  sizeof(outbox_before)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                GATEWAY,
                                90u,
                                6100u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(
        &result, MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING));
    assert(relay.pending.gateway_ack_confirm_pending);
    assert(relay.outbox_record.valid);
    assert(!relay.outbox_record.gateway_acked);
    assert(mesh_relay_tick(&relay, 6101u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    assert(result.retransmit.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
    assert(result.retransmit.packet.src_id == ANCHOR_A);
    assert(result.retransmit.next_hop_id == GATEWAY);
    confirm = result.retransmit;
    mesh_relay_note_tx_sent(&relay, &confirm, 6101u);
    build_gateway_ack_for_packet(&ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 3u,
                                 &confirm.packet,
                                 confirm.payload,
                                 confirm.payload_len);
    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                GATEWAY,
                                90u,
                                6110u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(mesh_relay_commit_gateway_ack_confirm_terminal(
               &relay,
               &confirm.packet,
               confirm.payload,
               confirm.payload_len,
               6111u) == PROTO_OK);
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);

    assert(mesh_relay_tick(&relay, 7000u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
}

static void test_gateway_ack_timeout_retries_then_requests_route_discovery(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 9u, 70u);
    struct route_candidate unrelated = direct_gateway_route(ANCHOR_C, 9u, 60u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(7000u);
    const struct route_candidate *unrelated_after_failure;
    const struct mesh_downlink_entry *downlink_after_failure;
    uint8_t payload[1] = {0x44u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 9u);
    route.last_seen_ms = 7000u;
    unrelated.hop_count = 1u;
    unrelated.last_seen_ms = 7000u;
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &unrelated) == PROTO_OK);
    for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        if (relay.upstream.candidates[i].valid &&
            relay.upstream.candidates[i].next_hop_id == ANCHOR_C) {
            relay.upstream.candidates[i].hold_down_until_ms =
                UINT32_C(1000000000);
            relay.upstream.candidates[i].hold_down_valid = true;
        }
    }
    seed_downlink(&relay, ANCHOR_B, ANCHOR_B, 9u, 1u, 80u, 7000u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    params = channel9_params(7100u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_B, &timing) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 90u, 9u, sizeof(payload)) == PROTO_OK);
    report.message_age_ms = 123u;

    uint32_t now_ms = 7000u;

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), now_ms, &tx) == PROTO_OK);
    assert(tx.packet.message_age_ms == 123u);
    mesh_relay_note_tx_sent(&relay, &tx, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == now_ms + route_retry_backoff_ms(1u));
    assert(mesh_relay_tx_active(&relay));

    now_ms = relay.pending.retry_after_ms;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(result.retransmit.packet.message_age_ms ==
           123u + ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u + route_retry_backoff_ms(1u));
    assert(mesh_relay_tx_active(&relay));
    mesh_relay_note_tx_sent(&relay, &result.retransmit, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == now_ms + route_retry_backoff_ms(2u));
    assert(mesh_relay_tx_active(&relay));

    now_ms = relay.pending.retry_after_ms;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.message_age_ms ==
           123u + ((ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u) * 2u) +
           route_retry_backoff_ms(1u) + route_retry_backoff_ms(2u));
    assert(mesh_relay_tx_active(&relay));
    mesh_relay_note_tx_sent(&relay, &result.retransmit, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == now_ms + route_retry_backoff_ms(3u));
    assert(mesh_relay_tx_active(&relay));

    now_ms = relay.pending.retry_after_ms;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.message_age_ms ==
           123u + ((ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u) * 3u) +
           route_retry_backoff_ms(1u) + route_retry_backoff_ms(2u) +
           route_retry_backoff_ms(3u));
    assert(mesh_relay_tx_active(&relay));
    mesh_relay_note_tx_sent(&relay, &result.retransmit, now_ms);

    now_ms += ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    assert(mesh_relay_tick(&relay, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.next_hop_id == 0u);
    assert(relay.pending.retry_after_ms - now_ms >= RELAY_BUSY_RETRY_MIN_MS);
    assert(relay.pending.retry_after_ms - now_ms <=
           RELAY_BUSY_RETRY_MIN_MS + (RELAY_BUSY_RETRY_MIN_MS / 2u));
    assert(relay.pending.gateway_ack_deadline_ms == 0u);
    assert(relay.pending.packet.msg_type == MSG_CLICK_REPORT);
    assert(relay.pending.packet.src_id == ANCHOR_A);
    assert(relay.pending.payload_len == sizeof(payload));
    assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);
    assert(relay.outbox_record.valid);
    assert(route_selected(&relay.upstream) == NULL);
    assert(relay.upstream.candidates[0].valid);
    assert(relay.upstream.candidates[0].hold_down_until_ms != 0u);
    assert(relay.upstream.candidates[0].hold_down_valid);
    assert(!relay.upstream.candidates[0].channel9_timing_valid);
    unrelated_after_failure = find_route_candidate(&relay, ANCHOR_C);
    assert(unrelated_after_failure != NULL);
    assert(unrelated_after_failure->valid);
    assert(unrelated_after_failure->hold_down_until_ms ==
           UINT32_C(1000000000));
    assert(unrelated_after_failure->hold_down_valid);
    downlink_after_failure = mesh_relay_find_downlink(&relay, ANCHOR_B);
    assert(downlink_after_failure != NULL);
    assert(downlink_after_failure->next_hop_id == ANCHOR_B);
    assert(downlink_after_failure->route_epoch == 9u);
    assert(find_event_timing(&relay, GATEWAY) == NULL);
    assert(find_event_timing(&relay, ANCHOR_B) == NULL);
}

static void test_child_retry_is_acked_while_transit_custody_has_no_parent(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 9u, 80u);
    struct proto_packet report;
    struct proto_packet altered_report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_pending_tx pending_before;
    struct mesh_outbox_record outbox_before;
    uint8_t payload[128];
    uint8_t altered_payload[128];
    size_t payload_len;
    size_t altered_payload_len;
    uint32_t now_ms = 8000u;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    9u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           91u,
                                           10u,
                                           1200,
                                           payload,
                                           sizeof(payload));
    altered_payload_len = build_valid_click_report(&altered_report,
                                                   ANCHOR_A,
                                                   91u,
                                                   10u,
                                                   1300,
                                                   altered_payload,
                                                   sizeof(altered_payload));
    assert(altered_payload_len == payload_len);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                now_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.next_hop_id == GATEWAY);
    assert(mesh_relay_start_tx(&relay,
                               &result.forward.packet,
                               result.forward.payload,
                               result.forward.payload_len,
                               now_ms,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, now_ms);

    for (uint8_t failure = 1u;
         failure <= ROUTE_RETRIES_PER_CANDIDATE;
         failure++) {
        uint32_t retry_at_ms;

        now_ms += 100u;
        assert(mesh_relay_note_pending_parent_failure(
                   &relay,
                   now_ms,
                   UINT32_C(0x10203040) + failure,
                   &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        assert(relay.pending.next_hop_id == GATEWAY);
        retry_at_ms = relay.pending.retry_after_ms;
        assert(mesh_relay_tick(&relay, retry_at_ms, &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
        assert(result.retransmit.next_hop_id == GATEWAY);
        mesh_relay_note_tx_sent(&relay, &result.retransmit, retry_at_ms);
        now_ms = retry_at_ms;
    }

    now_ms += 100u;
    assert(mesh_relay_note_pending_parent_failure(
               &relay,
               now_ms,
               UINT32_C(0x50607080),
               &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.next_hop_id == 0u);
    assert(relay.pending.packet.src_id == ANCHOR_A);
    assert(relay.pending.packet.dst_id == GATEWAY);
    assert(relay.pending.payload_len == payload_len);
    assert(memcmp(relay.pending.payload, payload, payload_len) == 0);
    assert(relay.outbox_record.valid);
    assert(route_selected(&relay.upstream) == NULL);
    pending_before = relay.pending;
    outbox_before = relay.outbox_record;

    /*
     * The child still needs hop-level custody progress while this relay is
     * finding a replacement parent. There is no valid upstream outbound yet,
     * so an exact retry must be ACKed without forwarding to ID zero.
     */
    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                now_ms + 1u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.hop_ack.next_hop_id == ANCHOR_A);
    assert(result.hop_ack.next_hop_id != 0u);
    assert(relay.pending.state == pending_before.state);
    assert(relay.pending.next_hop_id == pending_before.next_hop_id);
    assert(relay.pending.retry_after_ms == pending_before.retry_after_ms);
    assert(relay.pending.gateway_ack_deadline_ms ==
           pending_before.gateway_ack_deadline_ms);
    assert(relay.pending.queued_at_ms == pending_before.queued_at_ms);
    assert(relay.pending.packet.message_age_ms ==
           pending_before.packet.message_age_ms);
    assert(relay.pending.payload_len == pending_before.payload_len);
    assert(memcmp(relay.pending.payload,
                  pending_before.payload,
                  pending_before.payload_len) == 0);
    assert(relay.outbox_record.created_uptime_ms ==
           outbox_before.created_uptime_ms);
    assert(relay.outbox_record.age_ms_saturating ==
           outbox_before.age_ms_saturating);
    assert(relay.outbox_record.expiry_s == outbox_before.expiry_s);
    assert(memcmp(relay.outbox_record.semantic_digest,
                  outbox_before.semantic_digest,
                  sizeof(outbox_before.semantic_digest)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                altered_payload,
                                altered_payload_len,
                                ANCHOR_A,
                                90u,
                                now_ms + 2u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(relay.pending.state == pending_before.state);
    assert(relay.pending.next_hop_id == pending_before.next_hop_id);
    assert(relay.pending.retry_after_ms == pending_before.retry_after_ms);
    assert(relay.pending.gateway_ack_deadline_ms ==
           pending_before.gateway_ack_deadline_ms);
    assert(relay.pending.queued_at_ms == pending_before.queued_at_ms);
    assert(relay.pending.packet.message_age_ms ==
           pending_before.packet.message_age_ms);
    assert(relay.pending.payload_len == pending_before.payload_len);
    assert(memcmp(relay.pending.payload,
                  pending_before.payload,
                  pending_before.payload_len) == 0);
    assert(relay.outbox_record.age_ms_saturating ==
           outbox_before.age_ms_saturating);
    assert(memcmp(relay.outbox_record.semantic_digest,
                  outbox_before.semantic_digest,
                  sizeof(outbox_before.semantic_digest)) == 0);
}

static void test_gateway_ack_timeout_handles_ms_wrap(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 10u, 70u);
    struct proto_packet report = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x45u};
    const uint32_t start_ms = 0xffffff00u;
    const uint32_t before_wrap_ms = start_ms + 10u;
    const uint32_t timed_out_ms = start_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u;
    uint32_t retry_ms;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 10u);
    route.last_seen_ms = start_ms;
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 91u, 10u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), start_ms, &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, start_ms);
    assert(relay.pending.gateway_ack_deadline_ms < start_ms);

    assert(mesh_relay_tick(&relay, before_wrap_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&relay, timed_out_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_ms = relay.pending.retry_after_ms;
    assert(retry_ms == timed_out_ms + route_retry_backoff_ms(1u));
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&relay, retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(result.retransmit.packet.message_age_ms ==
           ROUTE_GATEWAY_ACK_TIMEOUT_MS + 1u + route_retry_backoff_ms(1u));
    assert(mesh_relay_tx_active(&relay));
}

static void test_deferred_retransmit_waits_for_actual_radio_send(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 10u, 70u);
    struct proto_packet report = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x46u};
    uint32_t timeout_ms;
    uint32_t retry_ms;
    uint32_t deferred_retry_ms;
    uint32_t control_retry_ms;
    uint32_t actual_send_ms;
    uint8_t failure_count;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 10u);
    route.last_seen_ms = 1000u;
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    92u,
                                    10u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               1000u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, 1050u);
    assert(relay.pending.gateway_ack_deadline_ms ==
           1050u + ROUTE_GATEWAY_ACK_TIMEOUT_MS);

    timeout_ms = relay.pending.gateway_ack_deadline_ms + 1u;
    assert(mesh_relay_tick(&relay, timeout_ms, &result) == PROTO_OK);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_ms = relay.pending.retry_after_ms;
    failure_count = route_selected(&relay.upstream)->failure_count;
    assert(failure_count == 1u);

    assert(mesh_relay_tick(&relay, retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    deferred_retry_ms = retry_ms + 400u;
    assert(mesh_relay_note_retransmit_deferred(&relay,
                                               &result.retransmit,
                                               deferred_retry_ms) == PROTO_OK);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == deferred_retry_ms);
    assert(relay.pending.gateway_ack_deadline_ms == 0u);
    assert(route_selected(&relay.upstream)->failure_count == failure_count);

    assert(mesh_relay_tick(&relay, deferred_retry_ms - 1u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tick(&relay, deferred_retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    control_retry_ms = deferred_retry_ms + 250u;
    assert(mesh_relay_defer_pending_retry(&relay,
                                          control_retry_ms) == PROTO_OK);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == control_retry_ms);
    assert(relay.pending.gateway_ack_deadline_ms == 0u);
    assert(route_selected(&relay.upstream)->failure_count == failure_count);

    assert(mesh_relay_tick(&relay, control_retry_ms - 1u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tick(&relay, control_retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    actual_send_ms = control_retry_ms + 50u;
    mesh_relay_note_tx_sent(&relay, &result.retransmit, actual_send_ms);
    assert(relay.pending.gateway_ack_deadline_ms ==
           actual_send_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS);
    assert(route_selected(&relay.upstream)->failure_count == failure_count);
}

static void test_route_discovery_reaches_ttl_eight_with_backoff(void)
{
    static const uint8_t expected_ttls[] = {
        1u, 2u, 4u, 6u, 8u, 8u, 8u, 8u,
    };
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    uint32_t now_ms = 1000u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);

    for (uint8_t attempt = 1u; attempt <= 8u; attempt++) {
        assert(mesh_relay_prepare_route_request(&relay,
                                                GATEWAY,
                                                now_ms,
                                                0u,
                                                &route_req) == PROTO_OK);
        assert(relay.route_discovery.active);
        assert(relay.route_discovery.target_id == GATEWAY);
        assert(relay.route_discovery.attempts == attempt);
        assert(route_req.packet.ttl == expected_ttls[attempt - 1u]);
        assert(route_req.queued_at_ms == now_ms);
        assert(route_req.earliest_tx_ms == now_ms);
        assert(route_req.queued_at_valid);
        assert(route_req.earliest_tx_valid);
        assert(route_req.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
        assert(relay.route_discovery.next_request_ms ==
               now_ms + mesh_relay_route_discovery_backoff_ms(attempt, 0u));
        assert(mesh_relay_prepare_route_request(&relay,
                                                GATEWAY,
                                                now_ms + 1u,
                                                0u,
                                                &route_req) == PROTO_ERR_BUSY);
        now_ms = relay.route_discovery.next_request_ms;
    }
    assert(relay.route_discovery.attempts == 8u);
    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            now_ms,
                                            0u,
                                            &route_req) == PROTO_OK);
    assert(route_req.packet.ttl == 8u);
}

static void test_route_discovery_preprobe_gate_is_same_target_and_due_aware(void)
{
    struct mesh_relay relay;
    uint32_t remaining_ms = UINT32_MAX;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);

    /* A first attempt has no retained backoff and may perform its probe. */
    assert(!mesh_relay_route_discovery_backoff_pending(
        &relay, GATEWAY, 1000u, &remaining_ms));
    assert(remaining_ms == 0u);

    relay.route_discovery.active = true;
    relay.route_discovery.target_id = GATEWAY;
    relay.route_discovery.attempts = 1u;
    relay.route_discovery.next_request_ms = 4500u;

    /* Repeated service for the same target must preserve the exact remainder. */
    remaining_ms = UINT32_MAX;
    assert(mesh_relay_route_discovery_backoff_pending(
        &relay, GATEWAY, 1200u, &remaining_ms));
    assert(remaining_ms == 3300u);

    /* Another target is independent, while the retained target becomes due
     * exactly at its deadline. */
    remaining_ms = UINT32_MAX;
    assert(!mesh_relay_route_discovery_backoff_pending(
        &relay, ANCHOR_B, 1200u, &remaining_ms));
    assert(remaining_ms == 0u);
    assert(!mesh_relay_route_discovery_backoff_pending(
        &relay, GATEWAY, 4500u, &remaining_ms));
    assert(remaining_ms == 0u);

    /* Wrapped deadlines retain modular remaining time. */
    relay.route_discovery.next_request_ms = 2u;
    remaining_ms = UINT32_MAX;
    assert(mesh_relay_route_discovery_backoff_pending(
        &relay, GATEWAY, UINT32_MAX - 2u, &remaining_ms));
    assert(remaining_ms == 5u);
}

static void test_route_discovery_retry_sequence_handles_wrap_and_jitter(void)
{
    static const uint32_t base_ms[] = {
        1000u, 2000u, 4000u, 8000u, 16000u, 32000u, 60000u, 60000u,
    };
    static const uint32_t random_values[] = {
        UINT32_C(0x13579bdf), UINT32_C(0x2468ace0),
        UINT32_C(0xabcdef01), UINT32_C(0x10203040),
        UINT32_C(0xfedcba98), UINT32_C(0x55aa55aa),
        UINT32_C(0xdeadbeef), UINT32_C(0x01010101),
    };
    static const uint8_t expected_ttls[] = {
        1u, 2u, 4u, 6u, 8u, 8u, 8u, 8u,
    };
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    const uint8_t attempt_count =
        (uint8_t)(sizeof(base_ms) / sizeof(base_ms[0]));
    uint32_t now_ms = UINT32_MAX - 500u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);

    for (uint8_t attempt = 1u; attempt <= attempt_count; attempt++) {
        uint32_t random_value = random_values[attempt - 1u];
        uint32_t delay_ms = mesh_relay_route_discovery_backoff_ms(attempt,
                                                                  random_value);

        assert(delay_ms >= base_ms[attempt - 1u]);
        assert(delay_ms <= base_ms[attempt - 1u] * 2u);
        assert(mesh_relay_prepare_route_request(&relay,
                                                GATEWAY,
                                                now_ms,
                                                random_value,
                                                &route_req) == PROTO_OK);
        assert(route_req.packet.ttl == expected_ttls[attempt - 1u]);
        assert(relay.route_discovery.attempts == attempt);
        assert(relay.route_discovery.next_request_ms == now_ms + delay_ms);
        assert(mesh_relay_prepare_route_request(&relay,
                                                GATEWAY,
                                                now_ms + 1u,
                                                random_value ^ UINT32_MAX,
                                                &route_req) == PROTO_ERR_BUSY);
        now_ms = relay.route_discovery.next_request_ms;
    }

    assert(relay.route_discovery.attempts == attempt_count);
}

static void test_idle_route_solicit_without_upstream_is_forwarded_once(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    uint32_t flood_epoch_id;
    uint32_t slot_seed;
    uint16_t flood_profile_version;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);

    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    route_req.packet.ttl = 2u;
    flood_epoch_id = require_tlv_u32(route_req.payload,
                                     route_req.payload_len,
                                     TLV_FLOOD_EPOCH_ID);
    flood_profile_version = require_tlv_u16(route_req.payload,
                                            route_req.payload_len,
                                            TLV_FLOOD_PROFILE_VERSION);
    slot_seed = require_tlv_u32(route_req.payload,
                                route_req.payload_len,
                                TLV_SLOT_SEED);
    assert(flood_epoch_id == route_req.packet.session_id);
    assert(flood_profile_version != 0u);
    assert(slot_seed != 0u);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(result.route_request.packet.msg_type == MSG_ROUTE_REQ);
    assert(result.route_request.packet.ttl == route_req.packet.ttl - 1u);
    assert(result.route_request.next_hop_id == MESH_BROADCAST_ID);
    assert(result.route_request.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(require_tlv_u8(result.route_request.payload,
                          result.route_request.payload_len,
                          TLV_HOP_COUNT) == 1u);
    assert(relay.flood_seen[0].valid);
    assert(relay.flood_seen[0].forward_count == 1u);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
}

static void test_route_request_capacity_failure_does_not_poison_retry(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(&relay); i++) {
        *test_downlink_at_mutable(&relay, i) = (struct mesh_downlink_entry) {
            .target_id = UINT64_C(0xd000000000000100) + i,
            .next_hop_id = UINT64_C(0xd000000000000200) + i,
            .gateway_id = GATEWAY,
            .route_epoch = 13u,
            .last_seen_ms = 1000u,
            .hop_count = 1u,
            .quality = 100u,
            .valid = true,
        };
    }

    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    route_req.packet.ttl = 2u;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NO_SPACE);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    for (uint8_t i = 0u; i < MESH_RELAY_FLOOD_SEEN_SIZE; i++) {
        assert(!relay.flood_seen[i].valid);
    }

    test_downlink_at_mutable(&relay, 0u)->valid = false;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(relay.flood_seen[0].valid);
    assert(relay.flood_seen[0].forward_count == 1u);
}

static void test_better_route_request_copy_updates_pending_forward(void)
{
    struct mesh_relay origin;
    struct mesh_relay first_hop;
    struct mesh_relay better_hop;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result first_hop_forward;
    struct mesh_relay_result better_hop_forward;
    struct mesh_relay_result first;
    struct mesh_relay_result better;
    struct mesh_relay_result late;
    uint32_t first_due_ms;
    uint32_t forward_delay_ms;
    uint32_t wrap_first_ms;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);
    mesh_relay_init(&first_hop, MESH_RELAY_ROLE_ANCHOR, ANCHOR_D, GATEWAY, 12u);
    mesh_relay_init(&better_hop, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 12u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);
    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    route_req.packet.ttl = 4u;

    assert(mesh_relay_handle_rx_with_random(&first_hop,
                                            &route_req.packet,
                                            route_req.payload,
                                            route_req.payload_len,
                                            ANCHOR_A,
                                            20u,
                                            1005u,
                                            2u,
                                            &first_hop_forward) == PROTO_OK);
    assert(has_action(&first_hop_forward, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(mesh_relay_handle_rx_with_random(&better_hop,
                                            &route_req.packet,
                                            route_req.payload,
                                            route_req.payload_len,
                                            ANCHOR_A,
                                            90u,
                                            1005u,
                                            3u,
                                            &better_hop_forward) == PROTO_OK);
    assert(has_action(&better_hop_forward, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &first_hop_forward.route_request.packet,
                                            first_hop_forward.route_request.payload,
                                            first_hop_forward.route_request.payload_len,
                                            ANCHOR_D,
                                            100u,
                                            1010u,
                                            1u,
                                            &first) == PROTO_OK);
    assert(first.status == PROTO_OK);
    assert(has_action(&first, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!has_action(&first, MESH_RELAY_ACTION_UPDATE_ROUTE_REQ));
    first_due_ms = first.route_request.earliest_tx_ms;
    assert(first_due_ms > 1020u);
    assert(require_tlv_u8(first.route_request.payload,
                          first.route_request.payload_len,
                          TLV_QUALITY) == 20u);

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &better_hop_forward.route_request.packet,
                                            better_hop_forward.route_request.payload,
                                            better_hop_forward.route_request.payload_len,
                                            ANCHOR_C,
                                            100u,
                                            1020u,
                                            7u,
                                            &better) == PROTO_OK);
    assert(better.status == PROTO_OK);
    assert(has_action(&better, MESH_RELAY_ACTION_UPDATE_ROUTE_REQ));
    assert(!has_action(&better, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(better.route_request.earliest_tx_ms == first_due_ms);
    assert(require_tlv_u8(better.route_request.payload,
                          better.route_request.payload_len,
                          TLV_QUALITY) == 90u);
    assert(relay.flood_seen[0].forward_count == 1u);
    assert(relay.flood_seen[0].best_previous_hop == ANCHOR_C);

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &better_hop_forward.route_request.packet,
                                            better_hop_forward.route_request.payload,
                                            better_hop_forward.route_request.payload_len,
                                            ANCHOR_C,
                                            100u,
                                            first_due_ms,
                                            9u,
                                            &late) == PROTO_OK);
    assert(late.status == PROTO_ERR_STALE);
    assert(has_action(&late, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&late, MESH_RELAY_ACTION_UPDATE_ROUTE_REQ));

    forward_delay_ms = first_due_ms - 1010u;
    assert(forward_delay_ms > 1u);
    wrap_first_ms = 0u - forward_delay_ms;
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);
    assert(mesh_relay_handle_rx_with_random(
               &relay,
               &first_hop_forward.route_request.packet,
               first_hop_forward.route_request.payload,
               first_hop_forward.route_request.payload_len,
               ANCHOR_D,
               100u,
               wrap_first_ms,
               1u,
               &first) == PROTO_OK);
    assert(has_action(&first, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(first.route_request.earliest_tx_valid);
    assert(first.route_request.earliest_tx_ms == 0u);
    assert(relay.flood_seen[0].forward_count == 1u);
    assert(relay.flood_seen[0].forward_due_ms == 0u);

    assert(mesh_relay_handle_rx_with_random(
               &relay,
               &better_hop_forward.route_request.packet,
               better_hop_forward.route_request.payload,
               better_hop_forward.route_request.payload_len,
               ANCHOR_C,
               100u,
               wrap_first_ms + 1u,
               7u,
               &better) == PROTO_OK);
    assert(has_action(&better, MESH_RELAY_ACTION_UPDATE_ROUTE_REQ));
    assert(better.route_request.earliest_tx_valid);
    assert(better.route_request.earliest_tx_ms == 0u);
    assert(require_tlv_u8(better.route_request.payload,
                          better.route_request.payload_len,
                          TLV_QUALITY) == 90u);
}

static void test_route_request_retry_uses_new_flood_identity(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    uint32_t first_session_id;
    uint32_t first_flood_id;
    uint32_t retry_session_id;
    uint32_t retry_flood_id;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);

    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    first_session_id = route_req.packet.session_id;
    first_flood_id = require_tlv_u32(route_req.payload,
                                     route_req.payload_len,
                                     TLV_FLOOD_EPOCH_ID);
    assert(route_req.packet.ttl == 1u);
    assert(first_flood_id == first_session_id);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NOT_FOUND);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            origin.route_discovery.next_request_ms,
                                            0u,
                                            &route_req) == PROTO_OK);
    retry_session_id = route_req.packet.session_id;
    retry_flood_id = require_tlv_u32(route_req.payload,
                                     route_req.payload_len,
                                     TLV_FLOOD_EPOCH_ID);
    assert(route_req.packet.ttl == 2u);
    assert(retry_flood_id == retry_session_id);
    assert(retry_flood_id != first_flood_id);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(result.route_request.packet.ttl == route_req.packet.ttl - 1u);
}

static void test_unanswered_timed_route_request_does_not_reserve_channel9(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct mesh_event_params params = channel9_params(5000u);
    struct mesh_event_timing proposed_timing;
    uint32_t first_flood_id;
    uint32_t retry_flood_id;
    uint32_t retry_ms;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);

    assert(mesh_event_timing_negotiate(&proposed_timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);
    assert(mesh_relay_prepare_route_request_with_timing(&origin,
                                                        GATEWAY,
                                                        &proposed_timing,
                                                        1000u,
                                                        1000u,
                                                        0u,
                                                        &route_req) == PROTO_OK);
    first_flood_id = require_tlv_u32(route_req.payload,
                                     route_req.payload_len,
                                     TLV_FLOOD_EPOCH_ID);
    assert(route_req.packet.ttl == 1u);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NOT_FOUND);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(channel9_timing_count(&relay) == 0u);
    assert(find_event_timing(&relay, ANCHOR_A) == NULL);

    retry_ms = origin.route_discovery.next_request_ms;
    assert(mesh_relay_prepare_route_request_with_timing(&origin,
                                                        GATEWAY,
                                                        &proposed_timing,
                                                        1000u,
                                                        retry_ms,
                                                        0u,
                                                        &route_req) == PROTO_OK);
    retry_flood_id = require_tlv_u32(route_req.payload,
                                     route_req.payload_len,
                                     TLV_FLOOD_EPOCH_ID);
    assert(route_req.packet.ttl == 2u);
    assert(retry_flood_id != first_flood_id);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                retry_ms + 10u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(result.route_request.packet.ttl == route_req.packet.ttl - 1u);
    assert(channel9_timing_count(&relay) == 0u);
    assert(find_event_timing(&relay, ANCHOR_A) == NULL);
}

static void test_parent_relay_replies_without_child_route_discovery(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate parent = direct_gateway_route(GATEWAY, 21u, 88u);
    uint32_t flood_epoch_id;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 21u);
    assert(route_upsert_candidate(&relay.upstream, &parent) == PROTO_OK);
    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            2000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    flood_epoch_id = require_tlv_u32(route_req.payload,
                                     route_req.payload_len,
                                     TLV_FLOOD_EPOCH_ID);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                2010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(result.route_reply.packet.src_id == GATEWAY);
    assert(result.route_reply.packet.dst_id == ANCHOR_A);
    assert(result.route_reply.next_hop_id == ANCHOR_A);
    assert(require_tlv_u32(result.route_reply.payload,
                           result.route_reply.payload_len,
                           TLV_FLOOD_EPOCH_ID) == flood_epoch_id);
    assert(!relay.route_discovery.active);
}

static void test_parent_relay_rejects_existing_child_route_request_while_connected(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate parent = direct_gateway_route(GATEWAY, 21u, 88u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(2500u);

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 21u);
    assert(route_upsert_candidate(&relay.upstream, &parent) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_A, &timing) == PROTO_OK);
    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            2000u,
                                            0u,
                                            &route_req) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                2010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
}

static void test_route_request_expires_stale_channel9_before_capacity_check(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate parent = direct_gateway_route(GATEWAY, 21u, 88u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(2500u);

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 21u);
    assert(route_upsert_candidate(&relay.upstream, &parent) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_A, &timing) == PROTO_OK);
    mesh_relay_note_channel9_missed(&relay, ANCHOR_A, NULL);
    mesh_relay_note_channel9_missed(&relay, ANCHOR_A, NULL);
    assert(find_event_timing(&relay, ANCHOR_A) != NULL);

    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            2600u,
                                            0u,
                                            &route_req) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_C,
                                80u,
                                2610u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(find_event_timing(&relay, ANCHOR_A) == NULL);
}

static void test_parent_relay_rejects_unrelated_child_route_request_while_connected(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate parent = direct_gateway_route(GATEWAY, 21u, 88u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(2500u);

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 21u);
    assert(route_upsert_candidate(&relay.upstream, &parent) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_A, &timing) == PROTO_OK);
    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            2000u,
                                            0u,
                                            &route_req) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_C,
                                80u,
                                2010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
}

static void test_gateway_route_advertisement_seeds_and_floods_parent_candidates(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor_a;
    struct mesh_relay anchor_b;
    struct mesh_outbound adv;
    struct mesh_relay_result result_a;
    struct mesh_relay_result result_b;
    struct mesh_relay_result duplicate_result;
    struct mesh_relay_result next_adv_result;
    struct mesh_relay_result next_epoch_result;
    struct mesh_outbound equivalent_adv;
    struct mesh_outbound next_adv;
    struct mesh_outbound next_epoch_adv;
    const struct route_candidate *selected;
    uint32_t flood_epoch_id;
    uint32_t slot_seed;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 9u);
    mesh_relay_init(&anchor_a, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 9u);
    mesh_relay_init(&anchor_b, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 9u);

    assert(mesh_relay_build_gateway_route_adv(&gateway, 77u, 1000u, &adv) == PROTO_OK);
    assert(adv.packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    assert(adv.payload_len == MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN);
    assert(adv.packet.payload_len == MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN);
    assert(adv.packet.src_id == GATEWAY);
    assert(adv.packet.dst_id == MESH_BROADCAST_ID);
    assert(adv.packet.session_id == 77u);
    assert(adv.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(adv.next_hop_id == MESH_BROADCAST_ID);
    assert(adv.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(adv.queued_at_ms == 1000u);
    assert(adv.earliest_tx_ms == 1000u);
    assert(require_tlv_u64(adv.payload, adv.payload_len, TLV_GATEWAY_ID) == GATEWAY);
    assert(require_tlv_u16(adv.payload, adv.payload_len, TLV_GATEWAY_EPOCH) == 9u);
    assert(require_tlv_u32(adv.payload, adv.payload_len, TLV_GATEWAY_ROUTE_SEQ) == 77u);
    assert(require_tlv_u8(adv.payload, adv.payload_len, TLV_HOP_COUNT) == 0u);
    assert(require_tlv_u8(adv.payload, adv.payload_len, TLV_PATH_QUALITY_MIN) == 100u);
    assert(require_tlv_u16(adv.payload, adv.payload_len, TLV_ACCUMULATED_COST) == 0u);
    assert(require_tlv_u8(adv.payload, adv.payload_len, TLV_RELAY_CAPACITY_STATE) ==
           RELAY_CAP_GREEN);
    assert(require_tlv_u16(adv.payload,
                           adv.payload_len,
                           TLV_CAPACITY_VALIDITY_INTERVAL_MS) ==
           RELAY_CAPACITY_HINT_VALIDITY_MS);
    assert(require_tlv_u16(adv.payload, adv.payload_len, TLV_FLOOD_PROFILE_VERSION) != 0u);
    flood_epoch_id = require_tlv_u32(adv.payload, adv.payload_len, TLV_FLOOD_EPOCH_ID);
    slot_seed = require_tlv_u32(adv.payload, adv.payload_len, TLV_SLOT_SEED);
    assert(flood_epoch_id == 77u);
    assert(slot_seed != 0u);
    assert(require_tlv_u32(adv.payload,
                           adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_MAX_MS) ==
           MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_MAX_MS);
    assert(require_tlv_u16(adv.payload,
                           adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS) ==
           MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_SLOT_MS);
    assert(require_tlv_u8(adv.payload, adv.payload_len, TLV_FLOOD_RETRY_COUNT) ==
           FLOOD_DEFAULT_RETRY_COUNT);

    assert(mesh_relay_handle_rx(&anchor_a,
                                &adv.packet,
                                adv.payload,
                                adv.payload_len,
                                GATEWAY,
                                82u,
                                1010u,
                                &result_a) == PROTO_OK);
    assert(result_a.status == PROTO_OK);
    selected = route_selected(&anchor_a.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == GATEWAY);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->route_epoch == 9u);
    assert(selected->hop_count == 0u);
    assert(selected->link_quality == 82u);
    assert(has_action(&result_a, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(result_a.gateway_route_adv.packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    assert(result_a.gateway_route_adv.packet.src_id == GATEWAY);
    assert(result_a.gateway_route_adv.packet.seq == adv.packet.seq);
    assert(result_a.gateway_route_adv.packet.session_id == adv.packet.session_id);
    assert(result_a.gateway_route_adv.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL - 1u);
    assert(result_a.gateway_route_adv.next_hop_id == MESH_BROADCAST_ID);
    assert(result_a.gateway_route_adv.queued_at_ms == 1010u);
    assert(result_a.gateway_route_adv.earliest_tx_ms >= 1010u);
    assert(result_a.gateway_route_adv.earliest_tx_ms <=
           1010u + MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_MAX_MS);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_EPOCH_ID) == flood_epoch_id);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_SLOT_SEED) == slot_seed);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_MAX_MS) ==
           MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_MAX_MS);
    assert(require_tlv_u16(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS) ==
           MESH_GATEWAY_ROUTE_ADV_FORWARD_JITTER_SLOT_MS);
    assert(require_tlv_u8(result_a.gateway_route_adv.payload,
                          result_a.gateway_route_adv.payload_len,
                          TLV_FLOOD_RETRY_COUNT) ==
           FLOOD_DEFAULT_RETRY_COUNT);
    result_a.gateway_route_adv.packet.message_age_ms = 1234u;
    assert(require_tlv_u8(result_a.gateway_route_adv.payload,
                          result_a.gateway_route_adv.payload_len,
                          TLV_HOP_COUNT) == 1u);
    assert(require_tlv_u8(result_a.gateway_route_adv.payload,
                          result_a.gateway_route_adv.payload_len,
                          TLV_PATH_QUALITY_MIN) == 82u);
    assert(require_tlv_u16(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_ACCUMULATED_COST) == 118u);

    assert(mesh_relay_rollback_forward_admission(&anchor_a,
                                                  &adv.packet,
                                                  adv.payload,
                                                  adv.payload_len,
                                                  &result_a) == PROTO_OK);
    /* Transport rollback releases only duplicate admission. Route ordering is
     * an honest RAM commit, so the exact advertisement remains stale. */
    assert(anchor_a.gateway_route_adv_seq == 77u);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &adv.packet,
                                adv.payload,
                                adv.payload_len,
                                GATEWAY,
                                82u,
                                1019u,
                                &duplicate_result) == PROTO_OK);
    assert(duplicate_result.status == PROTO_ERR_STALE);
    assert(has_action(&duplicate_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&duplicate_result,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    assert(mesh_relay_handle_rx(&anchor_a,
                                &adv.packet,
                                adv.payload,
                                adv.payload_len,
                                GATEWAY,
                                82u,
                                1020u,
                                &duplicate_result) == PROTO_OK);
    assert(duplicate_result.status == PROTO_ERR_STALE);
    assert(has_action(&duplicate_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&duplicate_result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    equivalent_adv = adv;
    equivalent_adv.packet.seq++;
    assert(mesh_relay_handle_rx(&anchor_a,
                                &equivalent_adv.packet,
                                equivalent_adv.payload,
                                equivalent_adv.payload_len,
                                GATEWAY,
                                82u,
                                1025u,
                                &duplicate_result) == PROTO_OK);
    assert(duplicate_result.status == PROTO_ERR_STALE);
    assert(has_action(&duplicate_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&duplicate_result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    assert(mesh_relay_build_gateway_route_adv(&gateway, 78u, 1026u, &next_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &next_adv.packet,
                                next_adv.payload,
                                next_adv.payload_len,
                                GATEWAY,
                                82u,
                                1027u,
                                &next_adv_result) == PROTO_OK);
    assert(next_adv_result.status == PROTO_OK);
    assert(has_action(&next_adv_result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(mesh_relay_rollback_forward_admission(&anchor_a,
                                                  &next_adv.packet,
                                                  next_adv.payload,
                                                  next_adv.payload_len,
                                                  &next_adv_result) ==
           PROTO_OK);
    assert(anchor_a.gateway_route_adv_seq == 78u);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &adv.packet,
                                adv.payload,
                                adv.payload_len,
                                GATEWAY,
                                82u,
                                1028u,
                                &duplicate_result) == PROTO_OK);
    assert(duplicate_result.status == PROTO_ERR_STALE);
    assert(!has_action(&duplicate_result,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(mesh_relay_handle_rx(&anchor_a,
                                &next_adv.packet,
                                next_adv.payload,
                                next_adv.payload_len,
                                GATEWAY,
                                82u,
                                1029u,
                                &next_adv_result) == PROTO_OK);
    assert(next_adv_result.status == PROTO_ERR_STALE);
    assert(has_action(&next_adv_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&next_adv_result,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(anchor_a.gateway_route_adv_seq == 78u);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &adv.packet,
                                adv.payload,
                                adv.payload_len,
                                GATEWAY,
                                82u,
                                1029u,
                                &duplicate_result) == PROTO_OK);
    assert(duplicate_result.status == PROTO_ERR_STALE);
    assert(anchor_a.gateway_route_adv_seq == 78u);

    gateway.upstream.current_epoch = 10u;
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              79u,
                                              1029u,
                                              &next_epoch_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &next_epoch_adv.packet,
                                next_epoch_adv.payload,
                                next_epoch_adv.payload_len,
                                GATEWAY,
                                82u,
                                1030u,
                                &next_epoch_result) == PROTO_OK);
    assert(next_epoch_result.status == PROTO_OK);
    assert(next_epoch_result.route_state_changed);
    assert(mesh_relay_rollback_forward_admission(&anchor_a,
                                                  &next_epoch_adv.packet,
                                                  next_epoch_adv.payload,
                                                  next_epoch_adv.payload_len,
                                                  &next_epoch_result) ==
           PROTO_OK);
    assert(anchor_a.upstream.current_epoch == 10u);
    assert(anchor_a.gateway_route_adv_seq == 79u);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &next_epoch_adv.packet,
                                next_epoch_adv.payload,
                                next_epoch_adv.payload_len,
                                GATEWAY,
                                82u,
                                1031u,
                                &next_epoch_result) == PROTO_OK);
    assert(next_epoch_result.status == PROTO_ERR_STALE);
    assert(has_action(&next_epoch_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&next_epoch_result,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    assert(mesh_relay_handle_rx(&anchor_b,
                                &result_a.gateway_route_adv.packet,
                                result_a.gateway_route_adv.payload,
                                result_a.gateway_route_adv.payload_len,
                                ANCHOR_A,
                                70u,
                                1030u,
                                &result_b) == PROTO_OK);
    assert(result_b.status == PROTO_OK);
    selected = route_selected(&anchor_b.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_A);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->route_epoch == 9u);
    assert(selected->hop_count == 1u);
    assert(selected->link_quality == 70u);
    assert(has_action(&result_b, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(result_b.gateway_route_adv.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL - 2u);
    assert(require_tlv_u8(result_b.gateway_route_adv.payload,
                          result_b.gateway_route_adv.payload_len,
                          TLV_HOP_COUNT) == 2u);
    assert(require_tlv_u8(result_b.gateway_route_adv.payload,
                          result_b.gateway_route_adv.payload_len,
                          TLV_PATH_QUALITY_MIN) == 70u);
    assert(require_tlv_u16(result_b.gateway_route_adv.payload,
                           result_b.gateway_route_adv.payload_len,
                           TLV_ACCUMULATED_COST) == 230u);

    (void)mesh_relay_expire_routes(&anchor_b, 60000u);
    selected = route_selected(&anchor_b.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_A);
}

static void test_gateway_route_advertisement_retains_distinct_parent_copy(void)
{
    struct mesh_relay gateway;
    struct mesh_relay parent_a;
    struct mesh_relay parent_b;
    struct mesh_relay anchor;
    struct mesh_outbound advertisement;
    struct mesh_relay_result at_parent_a;
    struct mesh_relay_result at_parent_b;
    struct mesh_relay_result first_path;
    struct mesh_relay_result alternate_path;
    struct mesh_relay_result exact_replay;
    const struct route_candidate *selected;
    const struct route_candidate *candidate_a;
    const struct route_candidate *candidate_b;
    const struct mesh_upstream_ancestry_entry *ancestry_a;
    const struct mesh_upstream_ancestry_entry *ancestry_b;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 9u);
    mesh_relay_init(&parent_a, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 9u);
    mesh_relay_init(&parent_b, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 9u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 9u);

    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              77u,
                                              1000u,
                                              &advertisement) == PROTO_OK);
    assert(mesh_relay_handle_rx(&parent_a,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                95u,
                                1010u,
                                &at_parent_a) == PROTO_OK);
    assert(mesh_relay_handle_rx(&parent_b,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                80u,
                                1011u,
                                &at_parent_b) == PROTO_OK);
    assert(has_action(&at_parent_a,
                      MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(has_action(&at_parent_b,
                      MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    assert(mesh_relay_handle_rx(&anchor,
                                &at_parent_a.gateway_route_adv.packet,
                                at_parent_a.gateway_route_adv.payload,
                                at_parent_a.gateway_route_adv.payload_len,
                                ANCHOR_A,
                                90u,
                                1020u,
                                &first_path) == PROTO_OK);
    assert(first_path.status == PROTO_OK);
    assert(has_action(&first_path,
                      MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    assert(mesh_relay_handle_rx(&anchor,
                                &at_parent_b.gateway_route_adv.packet,
                                at_parent_b.gateway_route_adv.payload,
                                at_parent_b.gateway_route_adv.payload_len,
                                ANCHOR_B,
                                60u,
                                1021u,
                                &alternate_path) == PROTO_OK);
    assert(alternate_path.status == PROTO_OK);
    assert(!has_action(&alternate_path,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));

    candidate_a = find_route_candidate(&anchor, ANCHOR_A);
    candidate_b = find_route_candidate(&anchor, ANCHOR_B);
    assert(candidate_a != NULL);
    assert(candidate_b != NULL);
    assert(candidate_a->route_epoch == 9u);
    assert(candidate_b->route_epoch == 9u);
    assert(candidate_a->hop_count == 1u);
    assert(candidate_b->hop_count == 1u);
    assert(candidate_a->link_quality == 90u);
    assert(candidate_b->link_quality == 60u);
    selected = route_selected(&anchor.upstream);
    assert(selected == candidate_a);

    ancestry_a = find_upstream_ancestry(&anchor, ANCHOR_A);
    ancestry_b = find_upstream_ancestry(&anchor, ANCHOR_B);
    assert(ancestry_a != NULL);
    assert(ancestry_b != NULL);
    assert(ancestry_a->path.count == 3u);
    assert(ancestry_a->path.node_ids[0] == GATEWAY);
    assert(ancestry_a->path.node_ids[1] == ANCHOR_A);
    assert(ancestry_a->path.node_ids[2] == ANCHOR_C);
    assert(ancestry_b->path.count == 3u);
    assert(ancestry_b->path.node_ids[0] == GATEWAY);
    assert(ancestry_b->path.node_ids[1] == ANCHOR_B);
    assert(ancestry_b->path.node_ids[2] == ANCHOR_C);

    assert(mesh_relay_handle_rx(&anchor,
                                &at_parent_b.gateway_route_adv.packet,
                                at_parent_b.gateway_route_adv.payload,
                                at_parent_b.gateway_route_adv.payload_len,
                                ANCHOR_B,
                                60u,
                                1022u,
                                &exact_replay) == PROTO_OK);
    assert(exact_replay.status == PROTO_ERR_STALE);
    assert(has_action(&exact_replay, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&exact_replay,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(route_selected(&anchor.upstream) == candidate_a);
}

static void test_gateway_route_epoch_serial_wrap_and_ambiguity(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_relay_result result;
    struct mesh_outbound advertisement;
    const struct route_candidate *selected;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    UINT16_MAX);
    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    UINT16_MAX);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              10u,
                                              1000u,
                                              &advertisement) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                90u,
                                1001u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);

    anchor.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = ANCHOR_B,
        .next_hop_id = ANCHOR_B,
        .gateway_id = GATEWAY,
        .route_epoch = UINT16_MAX,
        .last_seen_ms = 1002u,
        .hop_count = 1u,
        .quality = 90u,
        .valid = true,
    };
    anchor.event_timings[0].next_hop_id = ANCHOR_B;
    anchor.event_timings[0].valid = true;

    gateway.upstream.current_epoch = UINT32_C(0x00010001);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              1u,
                                              1010u,
                                              &advertisement) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                90u,
                                1011u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(anchor.upstream.current_epoch == UINT32_C(0x00010001));
    assert(mesh_relay_find_downlink(&anchor, ANCHOR_B) == NULL);
    assert(!anchor.event_timings[0].valid);
    selected = route_selected(&anchor.upstream);
    assert(selected != NULL);
    assert(selected->route_epoch == UINT32_C(0x00010001));

    gateway.upstream.current_epoch = UINT16_MAX;
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              2u,
                                              1020u,
                                              &advertisement) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                90u,
                                1021u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(anchor.upstream.current_epoch == UINT32_C(0x00010001));

    gateway.upstream.current_epoch = UINT32_C(0x00018001);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              3u,
                                              1030u,
                                              &advertisement) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                90u,
                                1031u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(anchor.upstream.current_epoch == UINT32_C(0x00010001));

    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    UINT32_MAX);
    gateway.upstream.current_epoch = 1u;
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              4u,
                                              1040u,
                                              &advertisement) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &advertisement.packet,
                                advertisement.payload,
                                advertisement.payload_len,
                                GATEWAY,
                                90u,
                                1041u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(anchor.upstream.current_epoch == 1u);

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    UINT32_C(0x00010000));
    assert(gateway.upstream.current_epoch == UINT32_C(0x00010001));
    gateway.upstream.current_epoch = UINT16_MAX;
    mesh_relay_invalidate_routes(&gateway);
    assert(gateway.upstream.current_epoch == UINT32_C(0x00010001));
}

static void test_route_ancestry_blocks_three_node_cycle_and_accepts_churn(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor_a;
    struct mesh_relay anchor_b;
    struct mesh_relay anchor_c;
    struct mesh_outbound gateway_adv;
    struct mesh_outbound next_gateway_adv;
    struct mesh_outbound route_request;
    struct mesh_outbound route_reply;
    struct mesh_relay_result at_a;
    struct mesh_relay_result at_b;
    struct mesh_relay_result at_c;
    struct mesh_relay_result direct_at_c;
    struct mesh_route_path reply_path = {0};
    const struct route_candidate *selected;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 9u);
    mesh_relay_init(&anchor_a, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 9u);
    mesh_relay_init(&anchor_b, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 9u);
    mesh_relay_init(&anchor_c, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 9u);

    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                               77u,
                                               1000u,
                                               &gateway_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &gateway_adv.packet,
                                gateway_adv.payload,
                                gateway_adv.payload_len,
                                GATEWAY,
                                90u,
                                1010u,
                                &at_a) == PROTO_OK);
    assert(has_action(&at_a, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(mesh_relay_handle_rx(&anchor_b,
                                &at_a.gateway_route_adv.packet,
                                at_a.gateway_route_adv.payload,
                                at_a.gateway_route_adv.payload_len,
                                ANCHOR_A,
                                90u,
                                1020u,
                                &at_b) == PROTO_OK);
    assert(has_action(&at_b, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(mesh_relay_handle_rx(&anchor_c,
                                &at_b.gateway_route_adv.packet,
                                at_b.gateway_route_adv.payload,
                                at_b.gateway_route_adv.payload_len,
                                ANCHOR_B,
                                90u,
                                1030u,
                                &at_c) == PROTO_OK);
    selected = route_selected(&anchor_c.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);

    assert(mesh_relay_build_route_request(&anchor_a,
                                          GATEWAY,
                                          &route_request,
                                          1100u) == PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(&anchor_c,
                                                    &route_request.packet,
                                                    route_request.payload,
                                                    route_request.payload_len,
                                                    ANCHOR_A,
                                                    1110u,
                                                    0u,
                                                    &route_reply) ==
           PROTO_ERR_NOT_FOUND);

    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                               78u,
                                               1200u,
                                               &next_gateway_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor_c,
                                &next_gateway_adv.packet,
                                next_gateway_adv.payload,
                                next_gateway_adv.payload_len,
                                GATEWAY,
                                95u,
                                1210u,
                                &direct_at_c) == PROTO_OK);
    selected = route_selected(&anchor_c.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == GATEWAY);

    assert(mesh_relay_build_route_reply_for_request(&anchor_c,
                                                    &route_request.packet,
                                                    route_request.payload,
                                                    route_request.payload_len,
                                                    ANCHOR_A,
                                                    1220u,
                                                    0u,
                                                    &route_reply) == PROTO_OK);
    assert(mesh_route_path_from_tlvs(route_reply.payload,
                                     route_reply.payload_len,
                                     &reply_path) == PROTO_OK);
    assert(reply_path.count == 2u);
    assert(reply_path.node_ids[0] == GATEWAY);
    assert(reply_path.node_ids[1] == ANCHOR_C);
}

static void test_gateway_route_advertisement_snapshot_rebuild_is_exact(void)
{
    struct mesh_relay gateway;
    struct mesh_gateway_route_adv_snapshot snapshot;
    struct mesh_outbound first;
    struct mesh_outbound rebuilt;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY, GATEWAY, 9u);
    assert(mesh_relay_capture_gateway_route_adv_snapshot(
               &gateway, 0x12345678u, 998u, &snapshot) == PROTO_OK);
    assert(mesh_relay_build_gateway_route_adv_from_snapshot(
               &gateway, &snapshot, &first) == PROTO_OK);

    gateway.upstream.current_epoch++;
    assert(mesh_relay_build_gateway_route_adv_from_snapshot(
               &gateway, &snapshot, &rebuilt) == PROTO_OK);
    assert(memcmp(&first, &rebuilt, sizeof(first)) == 0);
    assert(first.payload_len == MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN);
}

static void test_gateway_route_advertisement_reports_busy_capacity(void)
{
    struct mesh_relay gateway;
    struct mesh_outbound adv;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 9u);
    gateway.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;

    assert(mesh_relay_build_gateway_route_adv(&gateway, 78u, 2000u, &adv) == PROTO_OK);
    assert(adv.packet.msg_type == MSG_GATEWAY_ROUTE_ADV);
    assert(adv.packet.src_id == GATEWAY);
    assert(adv.packet.dst_id == MESH_BROADCAST_ID);
    assert(require_tlv_u8(adv.payload, adv.payload_len, TLV_RELAY_CAPACITY_STATE) ==
           RELAY_CAP_RED);
    assert(require_tlv_u16(adv.payload,
                           adv.payload_len,
                           TLV_CAPACITY_VALIDITY_INTERVAL_MS) ==
           RELAY_CAPACITY_HINT_VALIDITY_MS);
}

static void test_route_discovery_ready_resets_attempt_budget(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_req;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);

    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    assert(relay.route_discovery.attempts == 1u);

    mesh_relay_note_route_discovery_ready(&relay, GATEWAY);
    assert(!relay.route_discovery.active);

    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            1001u,
                                            0u,
                                            &route_req) == PROTO_OK);
    assert(relay.route_discovery.attempts == 1u);
}

static void test_retry_and_route_discovery_backoff_apply_jitter(void)
{
    assert(mesh_relay_retry_backoff_ms(1u, 0u) == 1500u);
    assert(mesh_relay_retry_backoff_ms(1u, 5u) == 1505u);
    assert(mesh_relay_retry_backoff_ms(1u, 750u) == 2250u);
    assert(mesh_relay_retry_backoff_ms(2u, 7u) == 3007u);
    assert(mesh_relay_retry_backoff_ms(3u, 3000u) == 9000u);
    assert(mesh_relay_route_discovery_backoff_ms(1u, 0u) == 1000u);
    assert(mesh_relay_route_discovery_backoff_ms(3u, 7u) == 4007u);
    assert(mesh_relay_route_discovery_backoff_ms(5u, 11u) == 16011u);
    assert(mesh_relay_route_discovery_backoff_ms(8u, 0u) == 60000u);
    assert(mesh_relay_route_discovery_backoff_ms(8u, 60000u) == 120000u);
}

static void test_collection_retry_delay_uses_symmetric_jitter(void)
{
    const uint32_t base_ms = COLLECTION_RETRY_ROUND_1_MS;
    const uint32_t jitter_ms =
        (base_ms * COLLECTION_RETRY_JITTER_PERCENT) / 100u;
    uint32_t retry_a;
    uint32_t retry_b;

    assert(mesh_relay_collection_retry_delay_ms(0u, 123u) == 0u);
    assert(mesh_relay_collection_retry_delay_ms(base_ms, 0u) ==
           base_ms - jitter_ms);
    assert(mesh_relay_collection_retry_delay_ms(base_ms, jitter_ms) ==
           base_ms);
    assert(mesh_relay_collection_retry_delay_ms(base_ms, jitter_ms * 2u) ==
           base_ms + jitter_ms);

    retry_a = mesh_relay_collection_retry_delay_ms(base_ms, 0x12345678u);
    retry_b = mesh_relay_collection_retry_delay_ms(base_ms, 0x12345678u);
    assert(retry_a == retry_b);
    assert(retry_a >= base_ms - jitter_ms);
    assert(retry_a <= base_ms + jitter_ms);
}

static void test_held_down_candidate_can_return_after_hold_down(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 13u, 70u);
    const struct route_candidate *selected;
    const uint32_t hold_down_deadline_ms =
        2300u + ROUTE_PARENT_HOLDDOWN_MS;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2000u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2100u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2200u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(route_selected(&relay.upstream) == NULL);

    assert(mesh_relay_expire_routes(&relay, hold_down_deadline_ms - 1u) == 0u);
    assert(route_selected(&relay.upstream) == NULL);
    assert(mesh_relay_expire_routes(&relay, hold_down_deadline_ms) == 0u);
    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == GATEWAY);
    assert(selected->failure_count == 0u);
}

static void test_forced_route_invalidation_clears_routes_and_discovery_state(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 14u, 80u);
    struct mesh_outbound route_req;
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(5000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 14u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_B, ANCHOR_B, 14u, 1u, 80u, 1000u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    relay.result_offer_reservation.valid = true;
    relay.result_offer_reservation.child_id = ANCHOR_B;
    relay.result_offer_reservation.result_id.gateway_id = GATEWAY;
    relay.result_offer_reservation.result_id.gateway_epoch = 14u;
    relay.result_offer_reservation_deadline_ms = 61000u;

    mesh_relay_invalidate_routes(&relay);

    assert(relay.upstream.current_epoch == 15u);
    assert(route_selected(&relay.upstream) == NULL);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) == NULL);
    assert(!relay.event_timings[0].valid);
    assert(!relay.route_discovery.active);
    assert(!relay.result_offer_reservation.valid);
    assert(relay.result_offer_reservation_deadline_ms == 0u);
}

static void test_configured_route_epoch_transition_is_atomic_and_runs_once(void)
{
    struct mesh_relay relay;
    struct route_candidate old_route =
        direct_gateway_route(GATEWAY, 14u, 80u);
    struct route_candidate new_route =
        direct_gateway_route(GATEWAY, 15u, 90u);
    struct route_candidate invalid;
    struct mesh_outbound route_req;
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(5000u);
    bool epoch_changed = false;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 14u);
    assert(route_upsert_candidate(&relay.upstream, &old_route) == PROTO_OK);
    relay.gateway_route_adv_seq = 88u;
    seed_downlink(&relay, ANCHOR_B, ANCHOR_B, 14u, 1u, 80u, 1000u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) ==
           PROTO_OK);
    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);

    assert(mesh_relay_validate_configured_gateway_route(
               &relay, &new_route, &epoch_changed) == PROTO_OK);
    assert(epoch_changed);
    assert(relay.upstream.current_epoch == 14u);
    assert(relay.gateway_route_adv_seq == 88u);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) != NULL);
    assert(relay.event_timings[0].valid);
    assert(relay.route_discovery.active);

    assert(mesh_relay_upsert_configured_gateway_route(
               &relay, &new_route) == PROTO_OK);
    assert(relay.upstream.current_epoch == 15u);
    assert(relay.gateway_route_adv_seq == 0u);
    assert(route_selected(&relay.upstream) != NULL);
    assert(route_selected(&relay.upstream)->route_epoch == 15u);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) == NULL);
    assert(!relay.event_timings[0].valid);
    assert(!relay.route_discovery.active);

    /* Same-epoch route refreshes must not repeat epoch cleanup. */
    seed_downlink(&relay, ANCHOR_B, ANCHOR_B, 15u, 1u, 85u, 1100u);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) ==
           PROTO_OK);
    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            1100u,
                                            0u,
                                            &route_req) == PROTO_OK);
    new_route.last_seen_ms++;
    assert(mesh_relay_validate_configured_gateway_route(
               &relay, &new_route, &epoch_changed) == PROTO_OK);
    assert(!epoch_changed);
    assert(mesh_relay_upsert_configured_gateway_route(
               &relay, &new_route) == PROTO_OK);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) != NULL);
    assert(relay.event_timings[0].valid);
    assert(relay.route_discovery.active);

    invalid = new_route;
    invalid.route_epoch = 14u;
    assert(mesh_relay_validate_configured_gateway_route(
               &relay, &invalid, &epoch_changed) == PROTO_ERR_STALE);
    invalid.route_epoch = 15u + UINT32_C(0x80000000);
    assert(mesh_relay_validate_configured_gateway_route(
               &relay, &invalid, &epoch_changed) == PROTO_ERR_STALE);
    invalid.route_epoch = UINT32_C(0x00010000);
    assert(mesh_relay_validate_configured_gateway_route(
               &relay, &invalid, &epoch_changed) == PROTO_ERR_ARG);
    assert(relay.upstream.current_epoch == 15u);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) != NULL);
    assert(relay.event_timings[0].valid);
    assert(relay.route_discovery.active);
}

static void test_local_route_clear_preserves_gateway_epoch(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 14u, 70u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(5000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 14u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_B, ANCHOR_B, 14u, 1u, 80u, 1000u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);

    mesh_relay_clear_routes_preserve_epoch(&relay);

    assert(relay.upstream.current_epoch == 14u);
    assert(route_selected(&relay.upstream) == NULL);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) == NULL);
    assert(!relay.event_timings[0].valid);
}

static void test_forwarded_gateway_bound_packet_sends_hop_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 15u, 70u);
    struct proto_packet report;
    struct mesh_relay_result result;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;
    bool contains = false;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 15u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                          ANCHOR_A,
                                          150u,
                                          3u,
                                          1200,
                                          payload,
                                          sizeof(payload));

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                2000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(result.hop_ack.packet.msg_type == MSG_MESH_HOP_ACK);
    assert(result.hop_ack.packet.src_id == ANCHOR_B);
    assert(result.hop_ack.packet.dst_id == ANCHOR_A);
    assert(result.hop_ack.packet.session_id == report.session_id);
    assert(result.hop_ack.next_hop_id == ANCHOR_A);
    assert(tlv_find(result.hop_ack.payload,
                    result.hop_ack.payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    assert(proto_get_u16_le(value) == report.seq);
    assert(mesh_ack_payload_contains_packet(&result.hop_ack.packet,
                                            result.hop_ack.payload,
                                            result.hop_ack.payload_len,
                                            &report,
                                            payload,
                                            payload_len,
                                            &contains) == PROTO_OK);
    assert(contains);
}

static void test_hop_ack_transfers_terminal_custody_without_timeout_extension(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 16u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t report_payload[1] = {0x43u};
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t report_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t ack_payload_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 16u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    160u,
                                    4u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&origin,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               1000u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&origin, &tx, 1000u);

    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             77u,
                             ack_payload,
                             sizeof(ack_payload),
                             &ack_payload_len,
                             &report,
                             report_payload,
                             sizeof(report_payload));

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                1100u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(mesh_packet_semantic_digest(&report,
                                       report_payload,
                                       sizeof(report_payload),
                                       report_digest));
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &origin, &report, report_digest) == PROTO_OK);
    assert(!mesh_relay_tx_active(&origin));
}

static void test_wrong_hop_ack_cannot_transfer_terminal_custody(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 16u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t report_payload[1] = {0x43u};
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 16u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    161u,
                                    4u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&origin,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               1000u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&origin, &tx, 1000u);

    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             77u,
                             ack_payload,
                             sizeof(ack_payload),
                             &ack_payload_len,
                             &report,
                             report_payload,
                             sizeof(report_payload));

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_C,
                                80u,
                                1100u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
}

static void test_assignment_result_transfers_terminal_custody_to_parent(void)
{
    struct route_candidate origin_route =
        direct_gateway_route(ANCHOR_B, 16u, 90u);
    struct route_candidate parent_route =
        direct_gateway_route(ANCHOR_C, 16u, 90u);
    struct route_candidate upper_route =
        direct_gateway_route(GATEWAY, 16u, 90u);
    struct mesh_relay origin;
    struct mesh_relay parent;
    struct mesh_relay upper;
    struct mesh_relay gateway;
    struct proto_packet result_packet;
    struct mesh_outbound origin_result;
    struct mesh_outbound parent_result;
    struct mesh_outbound upper_result;
    struct mesh_outbound hop_ack;
    struct mesh_outbound parent_hop_ack;
    struct mesh_outbound wrong_hop_ack;
    struct mesh_outbound gateway_ack;
    struct mesh_relay_result result;
    struct discovery_assignment_table_commitment table_commitment = {
        .bytes = {0xa5u},
    };
    uint8_t result_payload[128];
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t wrong_result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t result_payload_len = 0u;

    assert(mesh_append_command_result(result_payload,
                                      sizeof(result_payload),
                                      &result_payload_len,
                                      CMD_ASSIGN_DISCOVERY_SLOTS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(discovery_assignment_append_control_tlvs(
               result_payload,
               sizeof(result_payload),
               &result_payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_ACK,
               16u) == PROTO_OK);
    assert(discovery_assignment_append_claim_hash(
               result_payload,
               sizeof(result_payload),
               &result_payload_len,
               discovery_assignment_hash(ANCHOR_A)) == PROTO_OK);
    assert(discovery_assignment_append_table_commitment(
               result_payload,
               sizeof(result_payload),
               &result_payload_len,
               &table_commitment) == PROTO_OK);
    assert(tlv_append_u8(result_payload,
                         sizeof(result_payload),
                         &result_payload_len,
                         TLV_HOP_COUNT,
                         2u) == PROTO_OK);
    assert(result_payload_len > COLLECTION_RESULT_INLINE_C5_MAX_BYTES);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    UINT32_C(0x16253443),
                                    18u,
                                    (uint8_t)result_payload_len,
                                    true) == PROTO_OK);

    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    16u);
    mesh_relay_init(&parent,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    16u);
    mesh_relay_init(&upper,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_C,
                    GATEWAY,
                    16u);
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    16u);
    origin_route.hop_count = 2u;
    parent_route.hop_count = 1u;
    assert(route_upsert_candidate(&origin.upstream, &origin_route) ==
           PROTO_OK);
    assert(route_upsert_candidate(&parent.upstream, &parent_route) ==
           PROTO_OK);
    assert(route_upsert_candidate(&upper.upstream, &upper_route) ==
           PROTO_OK);
    seed_downlink(&parent, ANCHOR_A, ANCHOR_A, 16u, 1u, 90u, 1000u);

    /* Assignment results intentionally have no collection identity. Even
     * above the C5 inline threshold they use the ordinary reliable packet
     * lane, which is the exact path exercised by the three-anchor HIL. */
    assert(command_result_id_from_tlvs(result_payload,
                                       result_payload_len,
                                       &(struct command_result_id) {0}) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_packet_rx_semantics_validate(&result_packet,
                                             result_payload,
                                             result_payload_len,
                                             ANCHOR_A,
                                             ANCHOR_B,
                                             GATEWAY) == PROTO_OK);
    assert(mesh_packet_semantic_digest(&result_packet,
                                       result_payload,
                                       result_payload_len,
                                       result_digest));
    assert(mesh_relay_start_tx(&origin,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               1000u,
                               &origin_result) == PROTO_OK);
    mesh_relay_note_tx_sent(&origin, &origin_result, 1010u);

    assert(mesh_relay_handle_rx(&parent,
                                &origin_result.packet,
                                origin_result.payload,
                                origin_result.payload_len,
                                ANCHOR_A,
                                90u,
                                1020u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    parent_result = result.forward;
    hop_ack = result.hop_ack;

    /* The parent may acknowledge custody only after retaining the exact
     * immutable response for upstream retry. */
    assert(mesh_relay_start_tx(&parent,
                               &parent_result.packet,
                               parent_result.payload,
                               parent_result.payload_len,
                               1021u,
                               &parent_result) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(&parent,
                                                &parent_result,
                                                ANCHOR_A) == PROTO_OK);
    assert(parent.pending.packet.src_id == ANCHOR_A);
    assert(parent.pending.packet.session_id == result_packet.session_id);
    assert(parent.pending.packet.seq == result_packet.seq);
    assert(parent.pending.payload_len == result_payload_len);
    assert(memcmp(parent.pending.payload,
                  result_payload,
                  result_payload_len) == 0);

    /* Matching the outer ACK fields is insufficient: a changed semantic
     * commitment cannot transfer custody or retire the child's response. */
    wrong_hop_ack = hop_ack;
    assert(wrong_hop_ack.payload_len > 0u);
    wrong_hop_ack.payload[wrong_hop_ack.payload_len - 1u] ^= UINT8_C(0x01);
    assert(mesh_relay_handle_rx(&origin,
                                &wrong_hop_ack.packet,
                                wrong_hop_ack.payload,
                                wrong_hop_ack.payload_len,
                                ANCHOR_B,
                                90u,
                                1029u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&origin));

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack.packet,
                                hop_ack.payload,
                                hop_ack.payload_len,
                                ANCHOR_B,
                                90u,
                                1030u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(mesh_relay_tx_active(&origin));
    memcpy(wrong_result_digest, result_digest, sizeof(wrong_result_digest));
    wrong_result_digest[sizeof(wrong_result_digest) - 1u] ^= UINT8_C(0x01);
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &origin,
               &origin_result.packet,
               wrong_result_digest) == PROTO_ERR_MALFORMED);
    assert(mesh_relay_tx_active(&origin));
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &origin,
               &origin_result.packet,
               result_digest) == PROTO_OK);
    assert(!mesh_relay_tx_active(&origin));

    /* The same transfer repeats at arbitrary depth. Once the next parent has
     * retained the bytes, this relay also retires its copy instead of waiting
     * for a gateway ACK addressed to the original child. */
    mesh_relay_note_tx_sent(&parent, &parent_result, 1050u);
    assert(mesh_relay_handle_rx(&upper,
                                &parent_result.packet,
                                parent_result.payload,
                                parent_result.payload_len,
                                ANCHOR_B,
                                90u,
                                1055u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    upper_result = result.forward;
    parent_hop_ack = result.hop_ack;
    assert(mesh_relay_start_tx(&upper,
                               &upper_result.packet,
                               upper_result.payload,
                               upper_result.payload_len,
                               1056u,
                               &upper_result) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(&upper,
                                                &upper_result,
                                                ANCHOR_B) == PROTO_OK);
    assert(mesh_relay_handle_rx(&parent,
                                &parent_hop_ack.packet,
                                parent_hop_ack.payload,
                                parent_hop_ack.payload_len,
                                ANCHOR_C,
                                90u,
                                1057u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(!mesh_relay_tx_active(&parent));
    assert(mesh_relay_tx_active(&upper));

    mesh_relay_note_tx_sent(&upper, &upper_result, 1060u);
    assert(mesh_relay_handle_rx(&gateway,
                                &upper_result.packet,
                                upper_result.payload,
                                upper_result.payload_len,
                                ANCHOR_C,
                                90u,
                                1065u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &upper_result.packet,
                                              upper_result.payload,
                                              upper_result.payload_len,
                                              ANCHOR_C,
                                              1070u,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    gateway_ack = result.gateway_ack;
    assert(mesh_relay_handle_rx(&upper,
                                &gateway_ack.packet,
                                gateway_ack.payload,
                                gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                1080u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(
        &result, MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING));
    assert(!has_action(
        &result, MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING));
    assert(!mesh_relay_tx_active(&upper));
}

static void test_assignment_claim_transfers_terminal_custody_to_parent(void)
{
    struct mesh_relay origin;
    struct mesh_relay parent;
    struct route_candidate origin_route =
        direct_gateway_route(ANCHOR_B, 16u, 90u);
    struct route_candidate parent_route =
        direct_gateway_route(GATEWAY, 16u, 90u);
    struct proto_packet claim_packet;
    struct mesh_outbound origin_result;
    struct mesh_outbound parent_result;
    struct mesh_outbound hop_ack;
    struct mesh_relay_result result;
    uint8_t payload[96];
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t payload_len = 0u;

    origin_route.hop_count = 1u;
    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    16u);
    mesh_relay_init(&parent,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    16u);
    assert(route_upsert_candidate(&origin.upstream, &origin_route) ==
           PROTO_OK);
    assert(route_upsert_candidate(&parent.upstream, &parent_route) ==
           PROTO_OK);

    assert(mesh_append_command_result(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      CMD_ASSIGN_DISCOVERY_SLOTS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(discovery_assignment_append_control_tlvs(
               payload,
               sizeof(payload),
               &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
               16u) == PROTO_OK);
    assert(discovery_assignment_append_claim_hash(
               payload,
               sizeof(payload),
               &payload_len,
               discovery_assignment_hash(ANCHOR_A)) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_HOP_COUNT,
                         2u) == PROTO_OK);
    assert(mesh_init_command_result(&claim_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    UINT32_C(0x16253444),
                                    19u,
                                    (uint8_t)payload_len,
                                    true) == PROTO_OK);
    assert(mesh_packet_semantic_digest(&claim_packet,
                                       payload,
                                       payload_len,
                                       digest));
    assert(mesh_relay_start_tx(&origin,
                               &claim_packet,
                               payload,
                               payload_len,
                               2000u,
                               &origin_result) == PROTO_OK);
    mesh_relay_note_tx_sent(&origin, &origin_result, 2001u);

    assert(mesh_relay_handle_rx(&parent,
                                &origin_result.packet,
                                origin_result.payload,
                                origin_result.payload_len,
                                ANCHOR_A,
                                90u,
                                2002u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    parent_result = result.forward;
    hop_ack = result.hop_ack;
    assert(mesh_relay_start_tx(&parent,
                               &parent_result.packet,
                               parent_result.payload,
                               parent_result.payload_len,
                               2003u,
                               &parent_result) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(&parent,
                                                &parent_result,
                                                ANCHOR_A) == PROTO_OK);

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack.packet,
                                hop_ack.payload,
                                hop_ack.payload_len,
                                ANCHOR_B,
                                90u,
                                2004u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &origin, &origin_result.packet, digest) == PROTO_OK);
    assert(!mesh_relay_tx_active(&origin));
    assert(mesh_relay_tx_active(&parent));
}

static void test_assignment_immediate_ack_miss_enters_prompt_retry_backoff(void)
{
    static const uint8_t phases[] = {
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        DISCOVERY_ASSIGNMENT_PHASE_ACK,
    };
    static const uint32_t retry_delays_ms[] = {37u, 83u};

    for (size_t i = 0u; i < sizeof(phases) / sizeof(phases[0]); i++) {
        struct mesh_relay relay;
        struct route_candidate route =
            direct_gateway_route(GATEWAY, 16u, 90u);
        struct proto_packet packet;
        struct mesh_outbound tx;
        struct mesh_relay_result result;
        struct discovery_assignment_table_commitment commitment = {
            .bytes = {0x11u, 0x22u, 0x33u, 0x44u},
        };
        uint8_t payload[96];
        size_t payload_len = 0u;
        uint32_t sent_ms = 1010u + ((uint32_t)i * 1000u);
        uint32_t ack_miss_ms = sent_ms + 250u;
        uint32_t retry_at_ms = ack_miss_ms + retry_delays_ms[i];

        mesh_relay_init(&relay,
                        MESH_RELAY_ROLE_ANCHOR,
                        ANCHOR_A,
                        GATEWAY,
                        16u);
        assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
        assert(mesh_append_command_result(payload,
                                          sizeof(payload),
                                          &payload_len,
                                          CMD_ASSIGN_DISCOVERY_SLOTS,
                                          COMMAND_OK,
                                          0u) == PROTO_OK);
        assert(discovery_assignment_append_control_tlvs(
                   payload,
                   sizeof(payload),
                   &payload_len,
                   phases[i],
                   16u) == PROTO_OK);
        assert(discovery_assignment_append_claim_hash(
                   payload,
                   sizeof(payload),
                   &payload_len,
                   discovery_assignment_hash(ANCHOR_A)) == PROTO_OK);
        if (phases[i] == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
            assert(discovery_assignment_append_table_commitment(
                       payload,
                       sizeof(payload),
                       &payload_len,
                       &commitment) == PROTO_OK);
        }
        assert(tlv_append_u8(payload,
                             sizeof(payload),
                             &payload_len,
                             TLV_HOP_COUNT,
                             1u) == PROTO_OK);
        assert(mesh_init_command_result(&packet,
                                        ANCHOR_A,
                                        GATEWAY,
                                        UINT32_C(0x16253450) + (uint32_t)i,
                                        (uint16_t)(30u + i),
                                        (uint8_t)payload_len,
                                        true) == PROTO_OK);
        assert(mesh_relay_start_tx(&relay,
                                   &packet,
                                   payload,
                                   payload_len,
                                   sent_ms - 10u,
                                   &tx) == PROTO_OK);
        mesh_relay_note_tx_sent(&relay, &tx, sent_ms);
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        assert(relay.pending.gateway_ack_deadline_ms ==
               sent_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS);

        /* The synchronous 250 ms ACK window has already completed. Keep the
         * exact core-owned bytes, but do not spend the remaining generic
         * gateway-ACK timeout or return to unrelated Channel-5 work. */
        assert(mesh_relay_note_retransmit_deferred(&relay,
                                                   &tx,
                                                   retry_at_ms) == PROTO_OK);
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        assert(relay.pending.gateway_ack_deadline_ms == 0u);
        assert(relay.pending.retry_after_ms == retry_at_ms);
        assert(relay.outbox_record.valid);
        assert(relay.pending.packet.session_id == packet.session_id);
        assert(relay.pending.packet.seq == packet.seq);
        assert(relay.pending.payload_len == payload_len);
        assert(memcmp(relay.pending.payload, payload, payload_len) == 0);

        assert(mesh_relay_tick_with_random(&relay,
                                           retry_at_ms - 1u,
                                           UINT32_C(0xa5a50000) + (uint32_t)i,
                                           &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
        assert(mesh_relay_tick_with_random(&relay,
                                           retry_at_ms,
                                           UINT32_C(0x5a5a0000) + (uint32_t)i,
                                           &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
        assert(result.retransmit.packet.session_id == packet.session_id);
        assert(result.retransmit.packet.seq == packet.seq);
        assert(result.retransmit.payload_len == payload_len);
        assert(memcmp(result.retransmit.payload, payload, payload_len) == 0);
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        assert(relay.pending.gateway_ack_deadline_ms ==
               retry_at_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS);
    }

    {
        struct mesh_relay relay;
        struct route_candidate route =
            direct_gateway_route(GATEWAY, 16u, 90u);
        struct proto_packet packet = {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            .src_id = ANCHOR_A,
            .dst_id = GATEWAY,
            .session_id = UINT32_C(0x16253460),
            .seq = 40u,
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = 1u,
        };
        struct mesh_outbound tx;
        struct mesh_relay_result result;
        const uint8_t payload[] = {0x5au};
        const uint32_t sent_ms = 5010u;

        mesh_relay_init(&relay,
                        MESH_RELAY_ROLE_ANCHOR,
                        ANCHOR_A,
                        GATEWAY,
                        16u);
        assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
        assert(mesh_relay_start_tx(&relay,
                                   &packet,
                                   payload,
                                   sizeof(payload),
                                   sent_ms - 10u,
                                   &tx) == PROTO_OK);
        mesh_relay_note_tx_sent(&relay, &tx, sent_ms);
        assert(mesh_relay_tick_with_random(&relay,
                                           sent_ms + 250u,
                                           UINT32_C(0x12345678),
                                           &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
        assert(relay.pending.gateway_ack_deadline_ms ==
               sent_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS);
    }
}


static void test_local_acks_require_expected_physical_hop(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 16u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct proto_packet gateway_ack = {0};
    struct mesh_outbound tx;
    struct mesh_pending_tx pending_before;
    struct mesh_relay_result result;
    uint8_t report_payload[1] = {0x44u};
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 16u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    162u,
                                    5u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&origin,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               1000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&origin, &tx, 1000u);

    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             79u,
                             ack_payload,
                             sizeof(ack_payload),
                             &ack_payload_len,
                             &report,
                             report_payload,
                             sizeof(report_payload));

    pending_before = origin.pending;
    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_C,
                                80u,
                                1100u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&origin.pending, &pending_before, sizeof(pending_before)) == 0);

    hop_ack.src_id = ANCHOR_C;
    pending_before = origin.pending;
    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                1150u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&origin.pending, &pending_before, sizeof(pending_before)) == 0);
    hop_ack.src_id = ANCHOR_B;

    assert(mesh_init_gateway_ack(&gateway_ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 report.session_id,
                                 80u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);
    pending_before = origin.pending;
    assert(mesh_relay_handle_rx(&origin,
                                &gateway_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_C,
                                80u,
                                1200u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(&origin.pending, &pending_before, sizeof(pending_before)) == 0);
    assert(origin.outbox_record.valid);

    assert(mesh_relay_handle_rx(&origin,
                                &gateway_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                1210u,
                                &result) == PROTO_OK);
    /* A gateway ACK arms the compact confirmation without replacing the
     * original terminal bytes. */
    assert(has_action(&result,
                      MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING));
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(origin.pending.packet.msg_type == report.msg_type);
    assert(origin.pending.gateway_ack_confirm_pending);
}

static void test_hop_ack_outbox_survives_reset_until_gateway_ack(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 16u,
        .command_seq = 163u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 6u,
        .result_seq = 7u,
    };
    struct mesh_relay origin;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 16u, 75u);
    struct proto_packet result_packet = {0};
    struct proto_packet hop_ack = {0};
    struct proto_packet gateway_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[96];
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t result_payload_len = 0u;
    size_t ack_payload_len = 0u;
    uint32_t restore_ms = 100u;
    uint32_t retry_ms = restore_ms + RELAY_BUSY_RETRY_MIN_MS;

    route.hop_count = 1u;
    build_identity_command_result_payload(result_payload,
                                          sizeof(result_payload),
                                          64u,
                                          &result_id,
                                          &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 16u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&origin,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               1000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&origin, &tx, 1000u);
    assert(origin.outbox_record.valid);
    assert(!origin.outbox_record.gateway_acked);

    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             81u,
                             ack_payload,
                             sizeof(ack_payload),
                             &ack_payload_len,
                             &result_packet,
                             result_payload,
                             result_payload_len);
    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                1100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_PROGRESS));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(origin.outbox_record.valid);
    assert(origin.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(!origin.outbox_record.gateway_acked);

    assert(mesh_relay_export_outbox_snapshot(&origin,
                                             1110u,
                                             &snapshot) == PROTO_OK);
    assert(snapshot.valid);
    assert(snapshot.record.valid);
    assert(snapshot.record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(!snapshot.record.gateway_acked);
    assert(origin.outbox_record.valid);
    assert(!origin.outbox_record.gateway_acked);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 16u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              restore_ms) == PROTO_OK);
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored.pending.next_hop_id == 0u);
    assert(restored.pending.retry_after_ms == retry_ms);
    assert(restored.outbox_record.valid);
    assert(restored.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(!restored.outbox_record.gateway_acked);

    assert(mesh_relay_tick(&restored, retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == ANCHOR_B);
    assert(result.retransmit.packet.session_id == result_packet.session_id);
    assert(result.retransmit.packet.seq == result_packet.seq);
    assert(restored.outbox_record.valid);
    assert(!restored.outbox_record.gateway_acked);
    mesh_relay_note_tx_sent(&restored, &result.retransmit, retry_ms);

    assert(mesh_init_gateway_ack(&gateway_ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 result_packet.session_id,
                                 82u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);
    assert(mesh_relay_handle_rx(&restored,
                                &gateway_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                retry_ms + 10u,
                                &result) == PROTO_OK);
    /* The gateway ACK arms a compact confirmation while retaining the exact
     * original result for same-boot retry. */
    assert(has_action(&result,
                      MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING));
    assert(mesh_relay_tx_active(&restored));
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored.pending.packet.msg_type == result_packet.msg_type);
    assert(restored.pending.gateway_ack_confirm_pending);
    assert(restored.outbox_record.valid);
    assert(restored.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(!restored.outbox_record.gateway_acked);
}

static void test_gateway_reaches_anchor_behind_relay_and_receives_result(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound gateway_command_tx;
    struct mesh_outbound anchor_result_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result anchor_result;
    struct mesh_relay_result gateway_result;
    struct route_candidate relay_route = direct_gateway_route(GATEWAY, 21u, 85u);
    struct route_candidate anchor_route = direct_gateway_route(ANCHOR_B, 21u, 70u);
    struct proto_packet command = {0};
    struct proto_packet command_result = {0};
    uint8_t command_payload[16];
    uint8_t result_payload[32];
    size_t command_payload_len = 0u;
    size_t result_payload_len = 0u;
    const struct mesh_downlink_entry *downlink;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 21u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    anchor_route.hop_count = 1u;
    assert(route_upsert_candidate(&relay.upstream, &relay_route) == PROTO_OK);
    assert(route_upsert_candidate(&anchor.upstream, &anchor_route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 21u, 2u, 70u, 1040u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 21u, 2u, 70u, 1050u);

    downlink = mesh_relay_find_downlink(&relay, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_A);
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_B);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             300u,
                             1u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    command.ttl = gateway_command_origin_ttl(CMD_GET_STATUS);
    assert(mesh_relay_start_tx(&gateway,
                               &command,
                               command_payload,
                               command_payload_len,
                               2000u,
                               &gateway_command_tx) == PROTO_OK);
    assert(gateway_command_tx.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_command_tx.packet,
                                gateway_command_tx.payload,
                                gateway_command_tx.payload_len,
                                GATEWAY,
                                85u,
                                2010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == ANCHOR_A);
    assert(!mesh_relay_tx_active(&gateway));

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                70u,
                                2020u,
                                &anchor_result) == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&anchor_result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_append_command_result(result_payload,
                                      sizeof(result_payload),
                                      &result_payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(mesh_init_command_result(&command_result,
                                    ANCHOR_A,
                                    GATEWAY,
                                    300u,
                                    2u,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    assert(mesh_relay_start_tx(&anchor,
                               &command_result,
                               result_payload,
                               result_payload_len,
                               3000u,
                               &anchor_result_tx) == PROTO_OK);
    assert(anchor_result_tx.next_hop_id == ANCHOR_B);

    assert(mesh_relay_handle_rx(&relay,
                                &anchor_result_tx.packet,
                                anchor_result_tx.payload,
                                anchor_result_tx.payload_len,
                                ANCHOR_A,
                                70u,
                                3010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == GATEWAY);

    assert(mesh_relay_handle_rx(&gateway,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                85u,
                                3020u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &relay_result.forward.packet,
                                              relay_result.forward.payload,
                                              relay_result.forward.payload_len,
                                              ANCHOR_B,
                                              3021u,
                                              &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_B);
    assert(gateway_result.gateway_ack.packet.dst_id == ANCHOR_A);
}


static void test_crc16_collision_cannot_alias_duplicate_or_gateway_ack_history(void)
{
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x22334456),
        .seq = 26u,
        .ttl = 2u,
    };
    uint8_t payload_a[64];
    uint8_t payload_b[64];
    size_t payload_len = 0u;
    const uint32_t accepted_ms = 5000u;
    const uint32_t after_duplicate_window_ms =
        accepted_ms + ROUTE_DEDUP_WINDOW_MS + 1u;

    build_deliberate_crc16_collision_results(payload_a,
                                              payload_b,
                                              sizeof(payload_a),
                                              true,
                                              &payload_len);
    packet.payload_len = (uint16_t)payload_len;
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    3u);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload_a,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                accepted_ms,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload_a,
                                              payload_len,
                                              ANCHOR_B,
                                              accepted_ms + 1u,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);

    /* The hot duplicate cache must reject a same-key CRC16 collision. */
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload_b,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                accepted_ms + 2u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);

    /*
     * Once the short duplicate cache expires, retained gateway ACK history is
     * still authoritative and must reject the colliding mutation.
     */
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload_b,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                after_duplicate_window_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload_a,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                after_duplicate_window_ms + 1u,
                                &result) == PROTO_OK);
    /* Collection results are deliberately redelivered after the retained
     * ACK history lookup; the GUI receipt/commit path owns the next ACK. */
    assert(result.status == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
}

static void build_gateway_ack_confirm_for_record(
    const struct proto_packet *record,
    const uint8_t *record_payload,
    size_t record_payload_len,
    struct proto_packet *confirm,
    uint8_t *confirm_payload,
    size_t confirm_payload_cap,
    size_t *confirm_payload_len)
{
    assert(record != NULL);
    assert(confirm != NULL);
    assert(confirm_payload != NULL);
    assert(confirm_payload_len != NULL);
    assert(mesh_gateway_ack_confirm_payload_build(record,
                                                  record_payload,
                                                  record_payload_len,
                                                  confirm_payload,
                                                  confirm_payload_cap,
                                                  confirm_payload_len) ==
           PROTO_OK);
    assert(mesh_init_gateway_ack_confirm(confirm,
                                         record->src_id,
                                         record->dst_id,
                                         record->session_id,
                                         record->seq) == PROTO_OK);
    assert(confirm->payload_len == *confirm_payload_len);
}

static void confirm_gateway_history_record(
    struct mesh_relay *gateway,
    const struct proto_packet *record,
    const uint8_t *record_payload,
    size_t record_payload_len)
{
    struct mesh_gateway_ack_confirm_identity identity;
    struct proto_packet confirm;
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t confirm_payload_len = 0u;

    build_gateway_ack_confirm_for_record(record,
                                         record_payload,
                                         record_payload_len,
                                         &confirm,
                                         confirm_payload,
                                         sizeof(confirm_payload),
                                         &confirm_payload_len);
    assert(mesh_relay_gateway_ack_confirm_history_match(
               gateway,
               &confirm,
               confirm_payload,
               confirm_payload_len,
               &identity) == PROTO_OK);
}

static void test_gateway_ack_confirm_barrier_requires_exact_accepted_history(void)
{
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct mesh_gateway_ack_confirm_identity identity;
    struct proto_packet accepted;
    struct proto_packet wrong_operation;
    struct proto_packet wrong_type;
    struct proto_packet confirm;
    struct proto_packet alternate_confirm;
    uint8_t accepted_payload[32];
    uint8_t wrong_operation_payload[32];
    uint8_t wrong_type_payload[32];
    uint8_t confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    uint8_t alternate_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
    size_t accepted_payload_len = 0u;
    size_t wrong_operation_payload_len = 0u;
    size_t wrong_type_payload_len = 0u;
    size_t confirm_payload_len = 0u;
    size_t alternate_payload_len = 0u;

    assert(mesh_append_command_result(accepted_payload,
                                      sizeof(accepted_payload),
                                      &accepted_payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(mesh_init_command_result(&accepted,
                                    ANCHOR_A,
                                    GATEWAY,
                                    UINT32_C(0x12345678),
                                    71u,
                                    (uint8_t)accepted_payload_len,
                                    true) == PROTO_OK);
    build_gateway_ack_confirm_for_record(&accepted,
                                         accepted_payload,
                                         accepted_payload_len,
                                         &confirm,
                                         confirm_payload,
                                         sizeof(confirm_payload),
                                         &confirm_payload_len);

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    17u);

    /* Structurally valid proof is not authority before semantic acceptance. */
    memset(&identity, 0xa5, sizeof(identity));
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &confirm,
               confirm_payload,
               confirm_payload_len,
               &identity) == PROTO_ERR_NOT_FOUND);

    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &accepted,
                                              accepted_payload,
                                              accepted_payload_len,
                                              ANCHOR_B,
                                              4000u,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &confirm,
               confirm_payload,
               confirm_payload_len,
               &identity) == PROTO_OK);
    assert(identity.msg_type == accepted.msg_type);
    assert(identity.flags == accepted.flags);
    assert(identity.session_id == accepted.session_id);
    assert(identity.seq == accepted.seq);
    assert(identity.payload_len == accepted.payload_len);

    /* An exact duplicate is idempotent and remains valid barrier evidence. */
    memset(&identity, 0, sizeof(identity));
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &confirm,
               confirm_payload,
               confirm_payload_len,
               &identity) == PROTO_OK);

    memcpy(wrong_operation_payload,
           accepted_payload,
           accepted_payload_len);
    wrong_operation_payload_len = accepted_payload_len;
    wrong_operation = accepted;
    wrong_operation.session_id++;
    build_gateway_ack_confirm_for_record(&wrong_operation,
                                         wrong_operation_payload,
                                         wrong_operation_payload_len,
                                         &alternate_confirm,
                                         alternate_payload,
                                         sizeof(alternate_payload),
                                         &alternate_payload_len);
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &alternate_confirm,
               alternate_payload,
               alternate_payload_len,
               &identity) == PROTO_ERR_NOT_FOUND);

    assert(mesh_append_command_result(wrong_type_payload,
                                      sizeof(wrong_type_payload),
                                      &wrong_type_payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    wrong_type = accepted;
    wrong_type.msg_type = MSG_MESH_DATA;
    wrong_type.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    wrong_type.payload_len = (uint16_t)wrong_type_payload_len;
    build_gateway_ack_confirm_for_record(&wrong_type,
                                         wrong_type_payload,
                                         wrong_type_payload_len,
                                         &alternate_confirm,
                                         alternate_payload,
                                         sizeof(alternate_payload),
                                         &alternate_payload_len);
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &alternate_confirm,
               alternate_payload,
               alternate_payload_len,
               &identity) == PROTO_ERR_NOT_FOUND);

    /* Same header and type with different bytes cannot satisfy the barrier. */
    memcpy(wrong_operation_payload,
           accepted_payload,
           accepted_payload_len);
    wrong_operation_payload[accepted_payload_len - 1u] ^= 0x5au;
    wrong_operation = accepted;
    build_gateway_ack_confirm_for_record(&wrong_operation,
                                         wrong_operation_payload,
                                         accepted_payload_len,
                                         &alternate_confirm,
                                         alternate_payload,
                                         sizeof(alternate_payload),
                                         &alternate_payload_len);
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &alternate_confirm,
               alternate_payload,
               alternate_payload_len,
               &identity) == PROTO_ERR_NOT_FOUND);

    /* Another source cannot borrow this origin's accepted identity. */
    alternate_confirm = confirm;
    alternate_confirm.src_id = ANCHOR_B;
    assert(mesh_relay_gateway_ack_confirm_history_match(
               &gateway,
               &alternate_confirm,
               confirm_payload,
               confirm_payload_len,
               &identity) == PROTO_ERR_NOT_FOUND);
}




static void test_gateway_ordinary_command_result_duplicate_remains_sticky(void)
{
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 0xd4d5d6d7u,
        .seq = 114u,
        .ttl = 2u,
    };
    uint8_t payload[32];
    size_t payload_len = 0u;

    assert(mesh_append_command_result(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      9u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    76u);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                7100u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload,
                                              payload_len,
                                              ANCHOR_B,
                                              7101u,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                7110u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(
               &gateway,
               &packet,
               payload,
               payload_len,
               ANCHOR_B,
               90u,
               7101u + ROUTE_DEDUP_WINDOW_MS + 1u,
               &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}





static void test_gateway_ack_history_isolates_noisy_origin(void)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    uint8_t payload[8];
    uint32_t now_ms = 9000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    79u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        size_t payload_len = 0u;
        uint32_t delivery_ms = now_ms++;

        assert(tlv_append_u8(payload,
                             sizeof(payload),
                             &payload_len,
                             TLV_REASON,
                             (uint8_t)i) == PROTO_OK);
        assert(report_init_self_test_packet(&packet,
                                            ANCHOR_A,
                                            GATEWAY,
                                            UINT32_C(0xd0000000) + i,
                                            (uint16_t)(i + 1u),
                                            (uint8_t)payload_len) == PROTO_OK);
        packet.msg_type = MSG_MESH_DATA;
        packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
        assert(mesh_relay_handle_rx(&gateway,
                                    &packet,
                                    payload,
                                    payload_len,
                                    ANCHOR_A,
                                    90u,
                                    delivery_ms,
                                    &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &packet,
                                                  payload,
                                                  payload_len,
                                                  ANCHOR_A,
                                                  delivery_ms + 1u,
                                                  &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        confirm_gateway_history_record(&gateway,
                                       &packet,
                                       payload,
                                       payload_len);
    }

    {
        size_t payload_len = 0u;

        assert(tlv_append_u8(payload,
                             sizeof(payload),
                             &payload_len,
                             TLV_REASON,
                             0x5au) == PROTO_OK);
        assert(report_init_self_test_packet(&packet,
                                            ANCHOR_B,
                                            GATEWAY,
                                            UINT32_C(0xe0000001),
                                            1u,
                                            (uint8_t)payload_len) == PROTO_OK);
        packet.msg_type = MSG_MESH_DATA;
        packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
        packet.ttl = MESH_DEFAULT_TTL;
        uint32_t delivery_ms = now_ms;
        assert(mesh_relay_handle_rx(&gateway,
                                    &packet,
                                    payload,
                                    payload_len,
                                    ANCHOR_B,
                                    90u,
                                    delivery_ms,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &packet,
                                                  payload,
                                                  payload_len,
                                                  ANCHOR_B,
                                                  delivery_ms + 1u,
                                                  &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    }
}


static void build_gateway_ack_history_self_test(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint64_t origin_id,
    uint16_t ordinal)
{
    *payload_len = 0u;
    assert(tlv_append_u8(payload,
                         payload_cap,
                         payload_len,
                         TLV_REASON,
                         (uint8_t)ordinal) == PROTO_OK);
    assert(report_init_self_test_packet(packet,
                                        origin_id,
                                        GATEWAY,
                                        UINT32_C(0xc0000000) + ordinal,
                                        (uint16_t)(ordinal + 1u),
                                        (uint8_t)*payload_len) == PROTO_OK);
    packet->msg_type = MSG_MESH_DATA;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
}

static void build_gateway_ack_history_assignment_result(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    uint64_t origin_id,
    uint16_t ordinal)
{
    *payload_len = 0u;
    assert(mesh_append_command_result(payload,
                                      payload_cap,
                                      payload_len,
                                      CMD_ASSIGN_DISCOVERY_SLOTS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    *packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = origin_id,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0xd0000000) + ordinal,
        .seq = (uint16_t)(ordinal + 1u),
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = (uint16_t)*payload_len,
    };
}


static void fill_unconfirmed_gateway_ack_partition(
    struct mesh_relay *gateway,
    uint64_t origin_id,
    uint16_t ordinal_base,
    uint32_t *now_ms)
{
    struct mesh_relay_result result;
    struct proto_packet packet;
    uint8_t payload[8];
    size_t payload_len;

    assert(gateway != NULL);
    assert(now_ms != NULL);
    for (uint16_t i = 0u;
         i < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         i++) {
        build_gateway_ack_history_self_test(&packet,
                                            payload,
                                            sizeof(payload),
                                            &payload_len,
                                            origin_id,
                                            (uint16_t)(ordinal_base + i));
        assert(mesh_relay_handle_rx(gateway,
                                    &packet,
                                    payload,
                                    payload_len,
                                    origin_id,
                                    90u,
                                    *now_ms,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
        assert(mesh_relay_commit_gateway_delivery(gateway,
                                                  &packet,
                                                  payload,
                                                  payload_len,
                                                  origin_id,
                                                  *now_ms + 1u,
                                                  &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
        *now_ms += 2u;
    }
}

static void test_gateway_ack_history_holds_full_source_backlog_before_confirm(void)
{
    enum {
        backlog_depth = MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN,
    };
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet records[backlog_depth + 1u];
    uint8_t payloads[backlog_depth + 1u][8];
    size_t payload_lens[backlog_depth + 1u] = {0u};
    uint32_t now_ms = 17000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    920u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    for (uint16_t i = 0u; i <= backlog_depth; i++) {
        build_gateway_ack_history_self_test(&records[i],
                                            payloads[i],
                                            sizeof(payloads[i]),
                                            &payload_lens[i],
                                            ANCHOR_A,
                                            (uint16_t)(80u + i));
    }

    /* Accumulate the source's complete queue before receiving even one
     * ACK_CONFIRM. This crosses the old four-identity limit while preserving
     * every report as exact replay authority. */
    for (uint16_t i = 0u; i < backlog_depth; i++) {
        assert(mesh_relay_handle_rx(&gateway,
                                    &records[i],
                                    payloads[i],
                                    payload_lens[i],
                                    ANCHOR_A,
                                    90u,
                                    now_ms++,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &records[i],
                                                  payloads[i],
                                                  payload_lens[i],
                                                  ANCHOR_A,
                                                  now_ms++,
                                                  &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
        assert(mesh_relay_gateway_identity_confirmation_pending(
            &gateway,
            records[i].src_id,
            records[i].msg_type,
            records[i].session_id,
            records[i].seq,
            now_ms));
    }

    /* Lost gateway ACKs replay from history without sending any report to the
     * host twice. Then exact ACK_CONFIRM proofs release every live debt. */
    for (uint16_t i = 0u; i < backlog_depth; i++) {
        assert(mesh_relay_handle_rx(&gateway,
                                    &records[i],
                                    payloads[i],
                                    payload_lens[i],
                                    ANCHOR_A,
                                    90u,
                                    now_ms++,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_ERR_STALE);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(has_action(&result, MESH_RELAY_ACTION_DROP));
        assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

        confirm_gateway_history_record(&gateway,
                                       &records[i],
                                       payloads[i],
                                       payload_lens[i]);
        assert(!mesh_relay_gateway_identity_confirmation_pending(
            &gateway,
            records[i].src_id,
            records[i].msg_type,
            records[i].session_id,
            records[i].seq,
            now_ms));
    }

    /* A new head-of-line report can replace one confirmed history entry, so
     * the source progresses instead of looping on a full cache. */
    assert(mesh_relay_handle_rx(&gateway,
                                &records[backlog_depth],
                                payloads[backlog_depth],
                                payload_lens[backlog_depth],
                                ANCHOR_A,
                                90u,
                                now_ms++,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &records[backlog_depth],
                                              payloads[backlog_depth],
                                              payload_lens[backlog_depth],
                                              ANCHOR_A,
                                              now_ms,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
}

static void test_gateway_ack_overflow_preserves_all_origin_guarantees(void)
{
    const uint64_t origin_base = UINT64_C(0xad00000000001000);
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    uint8_t payload[8];
    size_t payload_len;
    uint32_t now_ms = 18000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    922u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    /* The first source consumes all 12 shared overflow identities. Every
     * other supported origin must still retain its four guaranteed slots. */
    for (uint16_t owner = 0u;
         owner < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX;
         owner++) {
        uint16_t owner_depth =
            owner == 0u ?
                MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN :
                MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN;

        for (uint16_t ordinal = 0u; ordinal < owner_depth; ordinal++) {
            build_gateway_ack_history_self_test(&packet,
                                                payload,
                                                sizeof(payload),
                                                &payload_len,
                                                origin_base + owner,
                                                ordinal);
            assert(mesh_relay_handle_rx(&gateway,
                                        &packet,
                                        payload,
                                        payload_len,
                                        packet.src_id,
                                        90u,
                                        now_ms++,
                                        &result) == PROTO_OK);
            assert(result.status == PROTO_OK);
            assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
            assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                      &packet,
                                                      payload,
                                                      payload_len,
                                                      packet.src_id,
                                                      now_ms++,
                                                      &result) == PROTO_OK);
            assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
        }
    }

    assert(MESH_RELAY_GATEWAY_ACK_CAPACITY ==
           MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN +
               (MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX - 1u) *
                   MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN);

    /* With the shared overflow exhausted, a second source cannot take a fifth
     * slot until one of its own four identities is confirmed. */
    build_gateway_ack_history_self_test(
        &packet,
        payload,
        sizeof(payload),
        &payload_len,
        origin_base + 1u,
        MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN);
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                packet.src_id,
                                90u,
                                now_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NO_SPACE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}






static void test_gateway_origin_reboot_releases_unconfirmable_ack_debt(void)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet records[
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN + 1u];
    uint8_t payloads[
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN + 1u][8];
    size_t payload_lens[
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN + 1u] = {0u};
    const uint32_t now_ms = 19000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    921u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);
    for (uint16_t i = 0u;
         i <= MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         i++) {
        build_gateway_ack_history_self_test(&records[i],
                                            payloads[i],
                                            sizeof(payloads[i]),
                                            &payload_lens[i],
                                            ANCHOR_A,
                                            (uint16_t)(120u + i));
    }
    for (uint16_t i = 0u;
         i < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         i++) {
        assert(mesh_relay_handle_rx(&gateway,
                                    &records[i],
                                    payloads[i],
                                    payload_lens[i],
                                    ANCHOR_A,
                                    90u,
                                    now_ms + i,
                                    &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &records[i],
                                                  payloads[i],
                                                  payload_lens[i],
                                                  ANCHOR_A,
                                                  now_ms + i,
                                                  &result) == PROTO_OK);
    }

    assert(mesh_relay_handle_rx(&gateway,
                                &records[
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                payloads[
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                payload_lens[
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                ANCHOR_A,
                                90u,
                                now_ms +
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NO_SPACE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_note_gateway_origin_reboot(&gateway, ANCHOR_A) ==
           PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &records[
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                payloads[
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                payload_lens[
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                ANCHOR_A,
                                90u,
                                now_ms +
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN +
                                    1u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &records[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              payloads[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              payload_lens[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              ANCHOR_A,
                                              now_ms +
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN +
                                                  1u,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    /* The surviving pre-reboot commitments remain duplicate authority. */
    assert(mesh_relay_handle_rx(&gateway,
                                &records[1],
                                payloads[1],
                                payload_lens[1],
                                ANCHOR_A,
                                90u,
                                now_ms +
                                    MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN +
                                    2u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
}

static void test_gateway_ack_batch_and_roster_transitions_preserve_live_debt(void)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet records[
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN + 1u];
    uint8_t payloads[
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN + 1u][16];
    size_t payload_lens[
        MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN + 1u] = {0u};

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    93u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);
    for (uint16_t i = 0u;
         i <= MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         i++) {
        uint32_t batch_id =
            i < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN ?
                UINT32_C(0x11110000) :
                UINT32_C(0x22220000);

        assert(tlv_append_u32(payloads[i],
                              sizeof(payloads[i]),
                              &payload_lens[i],
                              TLV_MESH_CH9_BATCH_ID,
                              batch_id) == PROTO_OK);
        assert(tlv_append_u8(payloads[i],
                             sizeof(payloads[i]),
                             &payload_lens[i],
                             TLV_MESH_CH9_BATCH_FLAGS,
                             i + 1u ==
                                     MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN ||
                                 i == MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN ?
                                 MESH_CH9_BATCH_FLAG_FINAL : 0u) == PROTO_OK);
        assert(report_init_self_test_packet(&records[i],
                                            ANCHOR_A,
                                            GATEWAY,
                                            UINT32_C(0x71720000) + i,
                                            (uint16_t)(60u + i),
                                            (uint8_t)payload_lens[i]) ==
               PROTO_OK);
        records[i].msg_type = MSG_MESH_DATA;
        records[i].flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    }
    for (uint16_t i = 0u;
         i < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         i++) {
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &records[i],
                                                  payloads[i],
                                                  payload_lens[i],
                                                  ANCHOR_B,
                                                  20000u + i,
                                                  &result) == PROTO_OK);
    }
    assert(mesh_relay_reconcile_gateway_ack_membership(
               &gateway, NULL, 0u) == PROTO_OK);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &records[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              payloads[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              payload_lens[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              ANCHOR_B,
                                              20000u +
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN,
                                              &result) == PROTO_ERR_NO_SPACE);
    confirm_gateway_history_record(&gateway,
                                   &records[0],
                                   payloads[0],
                                   payload_lens[0]);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &records[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              payloads[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              payload_lens[
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN],
                                              ANCHOR_B,
                                              20001u +
                                                  MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN,
                                              &result) == PROTO_OK);
    for (uint16_t i = 1u;
         i < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         i++) {
        confirm_gateway_history_record(&gateway,
                                       &records[i],
                                       payloads[i],
                                       payload_lens[i]);
    }
}

static void assert_gateway_ack_batch_metadata_rejected(
    struct mesh_relay *gateway,
    struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms)
{
    struct mesh_gateway_ack_store before = *gateway->gateway_ack_store;
    struct mesh_relay_result result;
    uint16_t next_seq_before = gateway->next_seq;

    packet->payload_len = (uint16_t)payload_len;
    assert(mesh_relay_handle_rx(gateway,
                                packet,
                                payload,
                                payload_len,
                                packet->src_id,
                                90u,
                                now_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(memcmp(gateway->gateway_ack_store,
                  &before,
                  sizeof(before)) == 0);
    assert(gateway->next_seq == next_seq_before);

    /*
     * The commit API repeats strict admission so a future caller cannot
     * bypass transport preflight and create ACK history for malformed data.
     */
    assert(mesh_relay_commit_gateway_delivery(gateway,
                                              packet,
                                              payload,
                                              payload_len,
                                              packet->src_id,
                                              now_ms,
                                              &result) ==
           PROTO_ERR_MALFORMED);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(memcmp(gateway->gateway_ack_store,
                  &before,
                  sizeof(before)) == 0);
    assert(gateway->next_seq == next_seq_before);
}

static void test_gateway_ack_history_rejects_malformed_batch_metadata(void)
{
    const uint64_t origin_id = UINT64_C(0xaaf0000000001000);
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = origin_id,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0xef000001),
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    uint8_t payload[32];
    size_t payload_len;
    uint32_t now_ms = 15000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    83u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

#define REJECT_BATCH_METADATA() do {                                         \
        assert_gateway_ack_batch_metadata_rejected(                          \
            &gateway, &packet, payload, payload_len, now_ms++);              \
        packet.session_id++;                                                 \
        packet.seq++;                                                        \
    } while (false)

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          1u) == PROTO_OK);
    REJECT_BATCH_METADATA();

    payload_len = 0u;
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         0u) == PROTO_OK);
    REJECT_BATCH_METADATA();

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          0u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         0u) == PROTO_OK);
    REJECT_BATCH_METADATA();

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          2u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          3u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         0u) == PROTO_OK);
    REJECT_BATCH_METADATA();

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          4u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         0u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         MESH_CH9_BATCH_FLAG_FINAL) == PROTO_OK);
    REJECT_BATCH_METADATA();

    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          5u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         0x80u) == PROTO_OK);
    REJECT_BATCH_METADATA();

    payload[0] = TLV_REASON;
    payload[1] = 2u;
    payload[2] = 1u;
    payload_len = 3u;
    REJECT_BATCH_METADATA();

#undef REJECT_BATCH_METADATA

    /*
     * The largest wire batch ID is valid. Candidate state lives in a separate
     * bitmap, so UINT32_MAX can never alias an enumeration marker.
     */
    payload_len = 0u;
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MESH_CH9_BATCH_ID,
                          UINT32_MAX) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_FLAGS,
                         MESH_CH9_BATCH_FLAG_FINAL) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                origin_id,
                                90u,
                                now_ms++,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload,
                                              payload_len,
                                              origin_id,
                                              now_ms++,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
}


static void test_gateway_ack_history_reserves_maximum_append_only_roster(void)
{
    const uint64_t roster_base = UINT64_C(0xab00000000002000);
    const uint64_t excess_id =
        roster_base + MESH_CONNECTED_MAX_ANCHORS;
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    uint64_t roster[MESH_CONNECTED_MAX_ANCHORS];
    uint8_t payload[32];
    size_t payload_len;
    uint32_t now_ms = 30000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    81u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    /* A cold P=0 roster admits exactly the supported 50 physical origins. */
    for (uint16_t candidate = 0u;
         candidate < MESH_CONNECTED_MAX_ANCHORS;
         candidate++) {
        roster[candidate] = roster_base + candidate;
        build_gateway_ack_history_assignment_result(
            &packet,
            payload,
            sizeof(payload),
            &payload_len,
            roster[candidate],
            candidate);
        assert(mesh_relay_handle_rx(&gateway,
                                    &packet,
                                    payload,
                                    payload_len,
                                    packet.src_id,
                                    90u,
                                    now_ms++,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(mesh_relay_reserve_gateway_ack_candidate(
                   &gateway, packet.src_id, now_ms++) == PROTO_OK);
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &packet,
                                                  payload,
                                                  payload_len,
                                                  packet.src_id,
                                                  now_ms++,
                                                  &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    }
    assert(mesh_relay_reconcile_gateway_ack_membership(
               &gateway, roster, MESH_CONNECTED_MAX_ANCHORS) == PROTO_OK);
    /* A restart with the same durable P=50 roster preserves ACK recovery. */
    assert(mesh_relay_reconcile_gateway_ack_membership(
               &gateway, roster, MESH_CONNECTED_MAX_ANCHORS) == PROTO_OK);

    /*
     * P=50 rejects a competing new candidate before semantic mutation. Its
     * transport preflight remains non-mutating and an existing member's exact
     * ACK recovery remains intact.
     */
    build_gateway_ack_history_assignment_result(
        &packet,
        payload,
        sizeof(payload),
        &payload_len,
        excess_id,
        MESH_CONNECTED_MAX_ANCHORS);
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                packet.src_id,
                                90u,
                                now_ms++,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_reserve_gateway_ack_candidate(
               &gateway, excess_id, now_ms++) == PROTO_ERR_NO_SPACE);

    build_gateway_ack_history_assignment_result(
        &packet,
        payload,
        sizeof(payload),
        &payload_len,
        roster[0],
        0u);
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                packet.src_id,
                                90u,
                                now_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
}

static void test_gateway_ack_candidate_replaces_only_source_oldest_identity(void)
{
    const uint64_t candidate_id = UINT64_C(0xac00000000001000);
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet prior;
    struct proto_packet assignment;
    uint64_t promoted_roster[] = {candidate_id};
    uint8_t prior_payload[8];
    uint8_t assignment_payload[32];
    size_t prior_payload_len;
    size_t assignment_payload_len;
    uint32_t now_ms = 40000u;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    82u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    /* Fill this source's complete queue-depth partition, oldest first. */
    for (uint16_t ordinal = 0u;
         ordinal < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         ordinal++) {
        uint32_t delivery_ms = now_ms++;

        build_gateway_ack_history_self_test(
            &prior,
            prior_payload,
            sizeof(prior_payload),
            &prior_payload_len,
            candidate_id,
            ordinal);
        assert(mesh_relay_handle_rx(&gateway,
                                    &prior,
                                    prior_payload,
                                    prior_payload_len,
                                    candidate_id,
                                    90u,
                                    delivery_ms,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &prior,
                                                  prior_payload,
                                                  prior_payload_len,
                                                  candidate_id,
                                                  delivery_ms + 1u,
                                                  &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    }

    /* Treat the prefilled source as a prior durable roster member. Its next
     * assignment identity must still reserve and commit before semantic state
     * such as the assignment ACK mask is allowed to advance. */
    assert(mesh_relay_reconcile_gateway_ack_membership(
               &gateway, promoted_roster, 1u) == PROTO_OK);

    /* Capacity reuse is legal only after the source's exact terminal proof. */
    build_gateway_ack_history_self_test(
        &prior,
        prior_payload,
        sizeof(prior_payload),
        &prior_payload_len,
        candidate_id,
        0u);
    confirm_gateway_history_record(&gateway,
                                   &prior,
                                   prior_payload,
                                   prior_payload_len);

    build_gateway_ack_history_assignment_result(
        &assignment,
        assignment_payload,
        sizeof(assignment_payload),
        &assignment_payload_len,
        candidate_id,
        3u);
    assert(mesh_relay_handle_rx(&gateway,
                                &assignment,
                                assignment_payload,
                                assignment_payload_len,
                                candidate_id,
                                90u,
                                now_ms++,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    /*
     * A fully validated CLAIM cannot wait a day for capacity. Reservation uses
     * the normal next-packet rule: replace only this source's shortest-lived
     * identity, then commit the assignment result immediately.
     */
    assert(mesh_relay_reserve_gateway_ack_candidate(
               &gateway, candidate_id, now_ms++) == PROTO_OK);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &assignment,
                                              assignment_payload,
                                              assignment_payload_len,
                                              candidate_id,
                                              now_ms++,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    /* Durable promotion keeps accepted identities and clears transition bits. */
    assert(mesh_relay_reconcile_gateway_ack_membership(
               &gateway, promoted_roster, 1u) == PROTO_OK);
    for (size_t byte = 0u;
         byte < MESH_RELAY_GATEWAY_ACK_CANDIDATE_BITMAP_BYTES;
         byte++) {
        assert(ack_store.candidate_identity_bits[byte] == 0u);
    }
    now_ms += ROUTE_DEDUP_WINDOW_MS + 1u;

    /* The newer source-local identities remain exact and ACK-sticky. */
    for (uint16_t ordinal = 1u;
         ordinal < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         ordinal++) {
        build_gateway_ack_history_self_test(
            &prior,
            prior_payload,
            sizeof(prior_payload),
            &prior_payload_len,
            candidate_id,
            ordinal);
        assert(mesh_relay_handle_rx(&gateway,
                                    &prior,
                                    prior_payload,
                                    prior_payload_len,
                                    candidate_id,
                                    90u,
                                    now_ms,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_ERR_STALE);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    }
    assert(mesh_relay_handle_rx(&gateway,
                                &assignment,
                                assignment_payload,
                                assignment_payload_len,
                                candidate_id,
                                90u,
                                now_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    /* Only the confirmed oldest identity was displaced. Reaccepting it would
     * create another live debt, so the full unconfirmed partition fails
     * closed until another exact ACK_CONFIRM permits source-local reuse. */
    build_gateway_ack_history_self_test(
        &prior,
        prior_payload,
        sizeof(prior_payload),
        &prior_payload_len,
        candidate_id,
        0u);
    assert(mesh_relay_handle_rx(&gateway,
                                &prior,
                                prior_payload,
                                prior_payload_len,
                                candidate_id,
                                90u,
                                now_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NO_SPACE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    build_gateway_ack_history_self_test(
        &prior,
        prior_payload,
        sizeof(prior_payload),
        &prior_payload_len,
        candidate_id,
        1u);
    confirm_gateway_history_record(&gateway,
                                   &prior,
                                   prior_payload,
                                   prior_payload_len);
    build_gateway_ack_history_self_test(
        &prior,
        prior_payload,
        sizeof(prior_payload),
        &prior_payload_len,
        candidate_id,
        0u);
    assert(mesh_relay_handle_rx(&gateway,
                                &prior,
                                prior_payload,
                                prior_payload_len,
                                candidate_id,
                                90u,
                                now_ms + 1u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &prior,
                                              prior_payload,
                                              prior_payload_len,
                                              candidate_id,
                                              now_ms + 2u,
                                              &result) == PROTO_OK);

    /* The newly accepted ordinal zero displaced only confirmed ordinal one;
     * the remaining priors and the assignment remain exact. */
    build_gateway_ack_history_self_test(
        &prior,
        prior_payload,
        sizeof(prior_payload),
        &prior_payload_len,
        candidate_id,
        1u);
    {
        struct mesh_gateway_ack_confirm_identity identity;
        struct proto_packet prior_confirm;
        uint8_t prior_confirm_payload[MESH_GATEWAY_ACK_CONFIRM_PAYLOAD_LEN];
        size_t prior_confirm_payload_len = 0u;

        build_gateway_ack_confirm_for_record(&prior,
                                             prior_payload,
                                             prior_payload_len,
                                             &prior_confirm,
                                             prior_confirm_payload,
                                             sizeof(prior_confirm_payload),
                                             &prior_confirm_payload_len);
        assert(mesh_relay_gateway_ack_confirm_history_match(
                   &gateway,
                   &prior_confirm,
                   prior_confirm_payload,
                   prior_confirm_payload_len,
                   &identity) == PROTO_ERR_NOT_FOUND);
    }
    for (uint16_t ordinal = 2u;
         ordinal < MESH_RELAY_GATEWAY_ACK_IDENTITIES_PER_ORIGIN;
         ordinal++) {
        build_gateway_ack_history_self_test(
            &prior,
            prior_payload,
            sizeof(prior_payload),
            &prior_payload_len,
            candidate_id,
            ordinal);
        confirm_gateway_history_record(&gateway,
                                       &prior,
                                       prior_payload,
                                       prior_payload_len);
    }
    confirm_gateway_history_record(&gateway,
                                   &assignment,
                                   assignment_payload,
                                   assignment_payload_len);
}

static void test_gateway_route_probe_churn_cannot_exhaust_ack_history(void)
{
    static struct mesh_gateway_ack_store ack_store;
    static struct mesh_gateway_ack_store ack_store_before;
    struct mesh_relay gateway;
    struct mesh_relay origin;
    struct mesh_relay_result result;
    struct mesh_relay_result origin_result;
    struct proto_packet route_probe = {
        .msg_type = MSG_GATEWAY_ROUTE_REQ,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0xb1b2b3b4),
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet report = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0xc1c2c3c4),
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    const uint8_t report_payload[] = {TLV_REASON, 1u, 0x42u};

    memset(&ack_store, 0, sizeof(ack_store));
    report.payload_len = (uint16_t)sizeof(report_payload);
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    77u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) == PROTO_OK);
    ack_store_before = ack_store;
    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    77u);

    for (uint16_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY + 20u; i++) {
        route_probe.seq = (uint16_t)(i + 1u);
        assert(mesh_relay_handle_rx(&gateway,
                                    &route_probe,
                                    NULL,
                                    0u,
                                    ANCHOR_A,
                                    90u,
                                    9000u + i,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    }
    assert(memcmp(&ack_store, &ack_store_before, sizeof(ack_store)) == 0);
    assert(mesh_relay_handle_rx(&origin,
                                &result.gateway_ack.packet,
                                result.gateway_ack.payload,
                                result.gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                9999u,
                                &origin_result) == PROTO_OK);
    assert(origin_result.actions == MESH_RELAY_ACTION_NONE);
    assert(!mesh_relay_tx_active(&origin));

    assert(mesh_relay_handle_rx(&gateway,
                                &report,
                                report_payload,
                                sizeof(report_payload),
                                ANCHOR_B,
                                90u,
                                10000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &report,
                                              report_payload,
                                              sizeof(report_payload),
                                              ANCHOR_B,
                                              10001u,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
}

static void test_gateway_delivery_commit_rejects_invalid_contract(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_relay_result result;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 0x91929394u,
        .seq = 91u,
        .ttl = 2u,
    };
    struct proto_packet invalid;
    uint8_t payload[16];
    size_t payload_len = 0u;

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          0xa1a2a3a4u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    74u);
    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    74u);

    assert(mesh_relay_commit_gateway_delivery(&anchor,
                                              &packet,
                                              payload,
                                              payload_len,
                                              ANCHOR_A,
                                              5000u,
                                              &result) == PROTO_ERR_ARG);

    invalid = packet;
    invalid.dst_id = ANCHOR_B;
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &invalid,
                                              payload,
                                              payload_len,
                                              ANCHOR_A,
                                              5000u,
                                              &result) == PROTO_ERR_ARG);

    invalid = packet;
    invalid.flags = 0u;
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &invalid,
                                              payload,
                                              payload_len,
                                              ANCHOR_A,
                                              5000u,
                                              &result) == PROTO_ERR_ARG);

    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload,
                                              payload_len,
                                              MESH_BROADCAST_ID,
                                              5000u,
                                              &result) == PROTO_ERR_ARG);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload,
                                              payload_len,
                                              GATEWAY,
                                              5000u,
                                              &result) == PROTO_ERR_ARG);

    invalid = packet;
    invalid.payload_len = 0u;
    /* Payload semantics are owned by the gateway application before commit. */
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &invalid,
                                              NULL,
                                              0u,
                                              ANCHOR_A,
                                              5000u,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
}

static void test_generic_gateway_ack_cannot_complete_collection_eack_custody(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 75u,
        .command_seq = 0xb1b2b3b4u,
        .node_id = ANCHOR_A,
        .node_boot_counter = 101u,
        .result_seq = 102u,
    };
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 75u, 90u);
    struct proto_packet result_packet;
    struct proto_packet gateway_ack;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t result_payload[128];
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t result_payload_len = 0u;
    size_t ack_payload_len = 0u;

    build_collection_command_result_payload(result_payload,
                                            sizeof(result_payload),
                                            96u,
                                            &result_id,
                                            0xc1c2c3c4u,
                                            &result_payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)result_payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    75u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &result_packet,
                               result_payload,
                               result_payload_len,
                               6000u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, 6000u);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    build_gateway_ack_for_packet(&gateway_ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 103u,
                                 &result_packet,
                                 result_payload,
                                 result_payload_len);
    assert(mesh_relay_handle_rx(&relay,
                                &gateway_ack,
                                ack_payload,
                                ack_payload_len,
                                GATEWAY,
                                90u,
                                6010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(!relay.outbox_record.gateway_acked);
}

static void test_gateway_delivers_direct_clicker_self_test_report_and_acks(void)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 710u,
        .failure_code = 5u,
        .battery_mv = 0u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 71u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        GATEWAY,
                                        fields.event_seq,
                                        (uint16_t)fields.event_seq,
                                        (uint8_t)payload_len) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                fields.clicker_id,
                                95u,
                                3000u,
                                &result) == PROTO_OK);

    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload,
                                              payload_len,
                                              fields.clicker_id,
                                              3001u,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(result.gateway_ack.next_hop_id == fields.clicker_id);
    assert(result.gateway_ack.packet.msg_type == MSG_GATEWAY_ACK);
    assert(result.gateway_ack.packet.src_id == GATEWAY);
    assert(result.gateway_ack.packet.dst_id == fields.clicker_id);
    assert(result.gateway_ack.packet.session_id == fields.event_seq);
    assert((result.gateway_ack.packet.flags & FLAG_GATEWAY_ACK) != 0u);
    assert(tlv_find(result.gateway_ack.payload,
                    result.gateway_ack.payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == packet.seq);
}

static void test_gateway_ack_uses_exact_ingress_peer_across_route_churn(void)
{
    static struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    struct mesh_downlink_entry *stale_downlink;
    struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 711u,
        .failure_code = 0u,
        .battery_mv = 0u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 72u);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &ack_store) ==
           PROTO_OK);

    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        GATEWAY,
                                        fields.event_seq,
                                        (uint16_t)fields.event_seq,
                                        (uint8_t)payload_len) == PROTO_OK);
    packet.ttl--;

    /* The powered gateway still believes A is directly reachable, but this
     * exact report proves that relay B owns the live reverse transfer. */
    seed_downlink(&gateway,
                  fields.clicker_id,
                  fields.clicker_id,
                  72u,
                  0u,
                  95u,
                  3000u);
    stale_downlink = test_downlink_at_mutable(&gateway, 0u);
    assert(stale_downlink->target_id == fields.clicker_id);
    assert(stale_downlink->next_hop_id == fields.clicker_id);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                95u,
                                3100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));

    /* Host admission may delay the commit while route maintenance changes
     * the cache again.  Neither the old direct route nor this newer route may
     * replace the ingress peer captured with the accepted delivery. */
    stale_downlink->next_hop_id = ANCHOR_C;
    stale_downlink->last_seen_ms = 3101u;
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &packet,
                                              payload,
                                              payload_len,
                                              ANCHOR_B,
                                              3200u,
                                              &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(result.gateway_ack.packet.dst_id == fields.clicker_id);
    assert(result.gateway_ack.next_hop_id == ANCHOR_B);

    /* A later exact source retry proves a new live reverse edge.  Duplicate
     * history decides whether to redeliver, but the ACK always follows the
     * peer that carried this particular retry. */
    stale_downlink->next_hop_id = fields.clicker_id;
    stale_downlink->last_seen_ms = 3201u;
    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_C,
                                90u,
                                3300u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(result.gateway_ack.packet.dst_id == fields.clicker_id);
    assert(result.gateway_ack.next_hop_id == ANCHOR_C);
}


static void test_late_gateway_ack_bypasses_only_unrelated_core_owner(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 53u, 95u);
    struct proto_packet acknowledged = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x5301),
        .seq = UINT16_C(0x53),
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 1u,
    };
    struct proto_packet unrelated = acknowledged;
    struct proto_packet gateway_ack;
    struct proto_packet invalid_ack;
    struct mesh_outbound unrelated_tx;
    struct mesh_pending_tx pending_before;
    struct mesh_relay_result result;
    struct mesh_downlink_entry *downlink;
    uint8_t acknowledged_payload[1] = {UINT8_C(0xa5)};
    uint8_t unrelated_payload[1] = {UINT8_C(0x5a)};
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 53u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 53u, 2u, 80u, 1000u);

    unrelated.src_id = ANCHOR_B;
    unrelated.session_id = UINT32_C(0x5302);
    unrelated.seq = UINT16_C(0x54);
    assert(mesh_relay_start_tx(&relay,
                               &unrelated,
                               unrelated_payload,
                               sizeof(unrelated_payload),
                               2000u,
                               &unrelated_tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    pending_before = relay.pending;

    build_gateway_ack_for_packet(&gateway_ack,
                                 ack_payload,
                                 sizeof(ack_payload),
                                 &ack_payload_len,
                                 ANCHOR_A,
                                 UINT16_C(0x55),
                                 &acknowledged,
                                 acknowledged_payload,
                                 sizeof(acknowledged_payload));

    /* A fresh late ACK and its exact duplicate both bypass unrelated core
     * custody, while neither may claim or mutate that core owner. */
    for (uint8_t copy = 0u; copy < 2u; copy++) {
        assert(mesh_relay_handle_rx(&relay,
                                    &gateway_ack,
                                    ack_payload,
                                    ack_payload_len,
                                    GATEWAY,
                                    90u,
                                    2010u + copy,
                                    &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
        assert(!has_action(
            &result,
            MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING));
        assert(result.forward.next_hop_id == ANCHOR_A);
        assert(memcmp(&relay.pending,
                      &pending_before,
                      sizeof(pending_before)) == 0);
    }

    invalid_ack = gateway_ack;
    invalid_ack.src_id = ANCHOR_C;
    assert(mesh_relay_handle_rx(&relay, &invalid_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2020u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    invalid_ack = gateway_ack;
    invalid_ack.msg_type = MSG_MESH_HOP_ACK;
    assert(mesh_relay_handle_rx(&relay, &invalid_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2021u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    invalid_ack = gateway_ack;
    invalid_ack.dst_id = ANCHOR_B;
    assert(mesh_relay_handle_rx(&relay, &invalid_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2022u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    invalid_ack = gateway_ack;
    invalid_ack.ttl = 1u;
    assert(mesh_relay_handle_rx(&relay, &invalid_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2023u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    downlink = test_downlink_at_mutable(&relay, 0u);
    assert(downlink->valid && downlink->target_id == ANCHOR_A);
    downlink->valid = false;
    assert(mesh_relay_handle_rx(&relay, &gateway_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2024u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    downlink->valid = true;
    downlink->next_hop_id = GATEWAY;
    assert(mesh_relay_handle_rx(&relay, &gateway_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2025u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));

    downlink->next_hop_id = ANCHOR_B;
    assert(mesh_relay_handle_rx(&relay, &gateway_ack, ack_payload,
                                ack_payload_len, GATEWAY, 90u, 2026u,
                                &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(memcmp(&relay.pending, &pending_before, sizeof(pending_before)) == 0);
}

static void test_reactive_gateway_route_request_and_reply(void)
{
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound route_req;
    struct mesh_outbound report_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result anchor_result;
    struct mesh_relay_result ack_result;
    struct mesh_event_timing proposed_timing;
    struct proto_packet report;
    struct route_candidate route = direct_gateway_route(GATEWAY, 1u, 90u);
    const struct mesh_relay_event_timing_entry *timing_entry;
    struct mesh_event_params params = channel9_params(1500u);
    uint8_t payload[1] = {0x66u};
    const struct route_candidate *selected;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_C, 1u, 4u, 50u, 1005u);

    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 500u, 1u, sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1000u, &report_tx) ==
           PROTO_ERR_NOT_FOUND);

    assert(mesh_event_timing_negotiate(&proposed_timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);
    assert(mesh_relay_build_route_request_with_timing(&anchor,
                                                      GATEWAY,
                                                      &proposed_timing,
                                                      1000u,
                                                      &route_req,
                                                      1000u) == PROTO_OK);
    assert(route_req.packet.msg_type == MSG_ROUTE_REQ);
    assert(route_req.packet.dst_id == MESH_BROADCAST_ID);
    assert(route_req.next_hop_id == MESH_BROADCAST_ID);
    assert(route_req.payload_len <= UWB_MESH_MAX_PAYLOAD_LEN);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_EVENT_INTERVAL_MS) == PROTO_OK);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_EVENT_WINDOW_MS) == PROTO_OK);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_NEXT_EVENT_TIME_MS) == PROTO_OK);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_EVENT_GUARD_MS) == PROTO_OK);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_CHANNEL) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_EVENT_COUNTER) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_CLOCK_SKEW_PPM) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_MAX_MISSED_EVENTS) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_MESH_SUPERVISION_TIMEOUT_MS) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_REPLY_NONCE) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(route_req.payload,
                       route_req.payload_len,
                       TLV_METRIC_CRC) == PROTO_ERR_NOT_FOUND);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1010u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert(!has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    timing_entry = find_event_timing(&relay, ANCHOR_A);
    assert(timing_entry != NULL);
    assert(mesh_event_timing_local_rx_slot(&timing_entry->timing));
    assert(relay_result.route_reply.next_hop_id == ANCHOR_A);
    assert(relay_result.route_reply.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(!relay_result.route_reply_backup_valid);
    assert(tlv_present(relay_result.route_reply.payload,
                       relay_result.route_reply.payload_len,
                       TLV_MESH_EVENT_INTERVAL_MS) == PROTO_OK);
    assert(tlv_present(relay_result.route_reply.payload,
                       relay_result.route_reply.payload_len,
                       TLV_MESH_CHANNEL) == PROTO_ERR_NOT_FOUND);
    (void)require_tlv_u32(relay_result.route_reply.payload,
                          relay_result.route_reply.payload_len,
                          TLV_FLOOD_EPOCH_ID);
    (void)require_tlv_u16(relay_result.route_reply.payload,
                          relay_result.route_reply.payload_len,
                          TLV_REPLY_NONCE);
    (void)require_tlv_u16(relay_result.route_reply.payload,
                          relay_result.route_reply.payload_len,
                          TLV_METRIC_CRC);
    (void)require_tlv_bytes(
        relay_result.route_reply.payload,
        relay_result.route_reply.payload_len,
        TLV_ROUTE_REPLY_SHA256_COMMITMENT,
        SEMANTIC_DIGEST_SHA256_LEN);

    assert(mesh_relay_handle_rx(&anchor,
                                &relay_result.route_reply.packet,
                                relay_result.route_reply.payload,
                                relay_result.route_reply.payload_len,
                                ANCHOR_B,
                                70u,
                                1040u,
                                &anchor_result) == PROTO_OK);
    assert(anchor_result.status == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(anchor_result.route_reply_ack.packet.msg_type == MSG_ROUTE_REPLY_ACK);
    assert(anchor_result.route_reply_ack.packet.src_id == ANCHOR_A);
    assert(anchor_result.route_reply_ack.packet.dst_id == ANCHOR_B);
    assert(anchor_result.route_reply_ack.next_hop_id == ANCHOR_B);
    assert(anchor_result.route_reply_ack.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(anchor_result.route_reply_ack.payload_len ==
           MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN);
    assert(tlv_present(anchor_result.route_reply_ack.payload,
                       anchor_result.route_reply_ack.payload_len,
                       TLV_FLOOD_EPOCH_ID) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(anchor_result.route_reply_ack.payload,
                       anchor_result.route_reply_ack.payload_len,
                       TLV_REPLY_NONCE) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(anchor_result.route_reply_ack.payload,
                       anchor_result.route_reply_ack.payload_len,
                       TLV_METRIC_CRC) == PROTO_ERR_NOT_FOUND);
    assert(semantic_digest_equal(
        require_tlv_bytes(
            anchor_result.route_reply_ack.payload,
            anchor_result.route_reply_ack.payload_len,
            TLV_ROUTE_REPLY_SHA256_COMMITMENT,
            SEMANTIC_DIGEST_SHA256_LEN),
        require_tlv_bytes(
            relay_result.route_reply.payload,
            relay_result.route_reply.payload_len,
            TLV_ROUTE_REPLY_SHA256_COMMITMENT,
            SEMANTIC_DIGEST_SHA256_LEN),
        SEMANTIC_DIGEST_SHA256_LEN));
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    timing_entry = find_event_timing(&anchor, ANCHOR_B);
    assert(timing_entry != NULL);
    assert(mesh_event_timing_local_tx_slot(&timing_entry->timing));
    mesh_relay_note_tx_sent(&relay,
                            &relay_result.route_reply,
                            1040u);
    assert(relay.route_reply_ack_expectation.active);
    assert(mesh_relay_handle_rx(&relay,
                                &anchor_result.route_reply_ack.packet,
                                anchor_result.route_reply_ack.payload,
                                anchor_result.route_reply_ack.payload_len,
                                ANCHOR_A,
                                70u,
                                1041u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_OK);
    assert(has_action(&ack_result, MESH_RELAY_ACTION_ROUTE_REPLY_ACKED));
    assert(!relay.route_reply_ack_expectation.active);

    selected = route_selected(&anchor.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->hop_count == 1u);
    assert(selected->link_quality == 70u);

    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1050u, &report_tx) ==
           PROTO_OK);
    assert(report_tx.next_hop_id == ANCHOR_B);
}

static void test_child_route_reply_preserves_upstream_and_transit_custody(void)
{
    const uint32_t route_epoch = 73u;
    struct mesh_relay gateway;
    struct mesh_relay anchor_a;
    struct mesh_relay anchor_b;
    struct mesh_outbound gateway_adv;
    struct mesh_outbound transit_tx;
    struct mesh_outbound route_request;
    struct mesh_outbound route_reply;
    struct mesh_relay_result result;
    struct mesh_event_params params = channel9_params(1500u);
    struct mesh_event_timing proposed_timing = {0};
    struct route_candidate upstream_before;
    struct mesh_upstream_ancestry_entry ancestry_before;
    struct mesh_pending_tx pending_before;
    struct proto_packet transit_report;
    const struct route_candidate *selected;
    const struct mesh_upstream_ancestry_entry *ancestry;
    const struct mesh_downlink_entry *downlink;
    const struct mesh_relay_event_timing_entry *timing;
    uint8_t transit_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t transit_payload_len;
    uint32_t gateway_route_adv_seq_before;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(&anchor_a,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(&anchor_b,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    route_epoch);

    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              701u,
                                              1000u,
                                              &gateway_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &gateway_adv.packet,
                                gateway_adv.payload,
                                gateway_adv.payload_len,
                                GATEWAY,
                                96u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    selected = route_selected(&anchor_a.upstream);
    ancestry = find_upstream_ancestry(&anchor_a, GATEWAY);
    assert(selected != NULL);
    assert(ancestry != NULL);
    upstream_before = *selected;
    ancestry_before = *ancestry;
    gateway_route_adv_seq_before = anchor_a.gateway_route_adv_seq;

    seed_downlink(&anchor_a,
                  ANCHOR_B,
                  ANCHOR_B,
                  route_epoch,
                  1u,
                  90u,
                  1011u);
    transit_payload_len = build_valid_click_report(&transit_report,
                                                    ANCHOR_B,
                                                    730u,
                                                    11u,
                                                    1234,
                                                    transit_payload,
                                                    sizeof(transit_payload));
    assert(mesh_relay_start_tx(&anchor_a,
                               &transit_report,
                               transit_payload,
                               transit_payload_len,
                               1020u,
                               &transit_tx) == PROTO_OK);
    assert(transit_tx.next_hop_id == GATEWAY);
    assert(mesh_relay_tx_active(&anchor_a));
    pending_before = anchor_a.pending;

    assert(mesh_event_timing_negotiate(&proposed_timing,
                                       &params,
                                       true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);
    assert(mesh_relay_prepare_route_request_with_timing(&anchor_a,
                                                        ANCHOR_B,
                                                        &proposed_timing,
                                                        1030u,
                                                        1030u,
                                                        0x73007300u,
                                                        &route_request) ==
           PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(&anchor_b,
                                                    &route_request.packet,
                                                    route_request.payload,
                                                    route_request.payload_len,
                                                    ANCHOR_A,
                                                    1040u,
                                                    0u,
                                                    &route_reply) == PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor_a,
                                &route_reply.packet,
                                route_reply.payload,
                                route_reply.payload_len,
                                ANCHOR_B,
                                88u,
                                1050u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(has_action(&result,
                      MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    assert(result.route_reply_ack.next_hop_id == ANCHOR_B);

    downlink = mesh_relay_find_downlink(&anchor_a, ANCHOR_B);
    assert(downlink != NULL);
    assert(downlink->next_hop_id == ANCHOR_B);
    assert(downlink->route_epoch == route_epoch);
    selected = route_selected(&anchor_a.upstream);
    ancestry = find_upstream_ancestry(&anchor_a, GATEWAY);
    assert_route_candidate_equal(selected, &upstream_before);
    assert_upstream_ancestry_equal(ancestry, &ancestry_before);
    assert(anchor_a.upstream.current_epoch == route_epoch);
    assert(anchor_a.gateway_route_adv_seq == gateway_route_adv_seq_before);

    timing = find_event_timing(&anchor_a, ANCHOR_B);
    assert(timing != NULL);
    assert(timing->direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert(mesh_event_timing_local_tx_slot(&timing->timing));
    assert(mesh_relay_tx_active(&anchor_a));
    assert(anchor_a.pending.state == pending_before.state);
    assert(anchor_a.pending.packet.msg_type == pending_before.packet.msg_type);
    assert(anchor_a.pending.packet.src_id == pending_before.packet.src_id);
    assert(anchor_a.pending.packet.dst_id == pending_before.packet.dst_id);
    assert(anchor_a.pending.packet.session_id == pending_before.packet.session_id);
    assert(anchor_a.pending.packet.seq == pending_before.packet.seq);
    assert(anchor_a.pending.next_hop_id == pending_before.next_hop_id);
    assert(anchor_a.pending.payload_len == pending_before.payload_len);
    assert(memcmp(anchor_a.pending.payload,
                  pending_before.payload,
                  pending_before.payload_len) == 0);
}

static void test_gateway_route_reply_preserves_epoch_seed_and_installs_ancestry(void)
{
    const uint32_t route_epoch = 91u;
    struct mesh_relay origin;
    struct mesh_relay responder;
    struct mesh_outbound route_request;
    struct mesh_outbound route_reply;
    struct mesh_relay_result result;
    struct route_candidate responder_route =
        direct_gateway_route(GATEWAY, route_epoch, 95u);
    const struct route_candidate *selected;
    const struct mesh_upstream_ancestry_entry *ancestry;
    uint32_t request_slot_seed;
    uint32_t reply_slot_seed;

    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(&responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    route_epoch);
    assert(route_upsert_candidate(&responder.upstream,
                                  &responder_route) == PROTO_OK);
    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            2000u,
                                            0x90909191u,
                                            &route_request) == PROTO_OK);
    request_slot_seed = require_tlv_u32(route_request.payload,
                                        route_request.payload_len,
                                        TLV_SLOT_SEED);
    assert(require_tlv_u32(route_request.payload,
                           route_request.payload_len,
                           TLV_ROUTE_EPOCH) == route_epoch);

    assert(mesh_relay_build_route_reply_for_request(&responder,
                                                    &route_request.packet,
                                                    route_request.payload,
                                                    route_request.payload_len,
                                                    ANCHOR_A,
                                                    2010u,
                                                    0u,
                                                    &route_reply) == PROTO_OK);
    assert(require_tlv_u32(route_reply.payload,
                           route_reply.payload_len,
                           TLV_ROUTE_EPOCH) == route_epoch);
    reply_slot_seed = require_tlv_u32(route_reply.payload,
                                      route_reply.payload_len,
                                      TLV_SLOT_SEED);
    assert(reply_slot_seed == request_slot_seed);

    /* Receipt independently validates the deterministic slot seed against
     * the origin, target, request ID, and reply epoch. */
    assert(mesh_relay_handle_rx(&origin,
                                &route_reply.packet,
                                route_reply.payload,
                                route_reply.payload_len,
                                ANCHOR_B,
                                92u,
                                2020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(has_action(&result,
                      MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    selected = route_selected(&origin.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->route_epoch == route_epoch);
    assert(origin.upstream.current_epoch == route_epoch);
    ancestry = find_upstream_ancestry(&origin, ANCHOR_B);
    assert(ancestry != NULL);
    assert(ancestry->route_epoch == route_epoch);
    assert(ancestry->path.count == 3u);
    assert(ancestry->path.node_ids[0] == GATEWAY);
    assert(ancestry->path.node_ids[1] == ANCHOR_B);
    assert(ancestry->path.node_ids[2] == ANCHOR_A);
}

static void build_route_reply_for_collision_epoch(
    uint32_t route_epoch,
    struct mesh_relay *responder,
    struct mesh_outbound *reply)
{
    struct mesh_relay origin;
    struct mesh_outbound request;
    struct route_candidate route =
        direct_gateway_route(GATEWAY, route_epoch, 90u);

    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    route_epoch);
    assert(route_upsert_candidate(&responder->upstream, &route) == PROTO_OK);
    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &request,
                                          1000u) == PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(responder,
                                                    &request.packet,
                                                    request.payload,
                                                    request.payload_len,
                                                    ANCHOR_A,
                                                    1010u,
                                                    0u,
                                                    reply) == PROTO_OK);
}

static void test_route_reply_ack_full_commitment_rejects_crc_collision_and_replay(void)
{
    const uint32_t collision_epoch_a = 4080u;
    const uint32_t collision_epoch_b = 4096u;
    const uint16_t deliberate_collision_crc = 36036u;
    struct mesh_relay sender_a;
    struct mesh_relay sender_b;
    struct mesh_relay origin_a;
    struct mesh_relay origin_b;
    struct mesh_outbound reply_a;
    struct mesh_outbound reply_b;
    struct mesh_relay_result receive_a;
    struct mesh_relay_result receive_b;
    struct mesh_relay_result ack_result;
    struct mesh_outbound malformed_outbound;
    struct mesh_outbound padded_ack;
    const uint8_t *commitment_a;
    const uint8_t *commitment_b;
    size_t padded_ack_len;

    build_route_reply_for_collision_epoch(collision_epoch_a,
                                          &sender_a,
                                          &reply_a);
    build_route_reply_for_collision_epoch(collision_epoch_b,
                                          &sender_b,
                                          &reply_b);

    assert(reply_a.packet.src_id == reply_b.packet.src_id);
    assert(reply_a.packet.dst_id == reply_b.packet.dst_id);
    assert(reply_a.packet.session_id == reply_b.packet.session_id);
    assert(reply_a.packet.seq == reply_b.packet.seq);
    assert(require_tlv_u16(reply_a.payload,
                           reply_a.payload_len,
                           TLV_REPLY_NONCE) ==
           require_tlv_u16(reply_b.payload,
                           reply_b.payload_len,
                           TLV_REPLY_NONCE));
    assert(require_tlv_u16(reply_a.payload,
                           reply_a.payload_len,
                           TLV_METRIC_CRC) ==
           deliberate_collision_crc);
    assert(require_tlv_u16(reply_b.payload,
                           reply_b.payload_len,
                           TLV_METRIC_CRC) ==
           deliberate_collision_crc);
    commitment_a = require_tlv_bytes(
        reply_a.payload,
        reply_a.payload_len,
        TLV_ROUTE_REPLY_SHA256_COMMITMENT,
        SEMANTIC_DIGEST_SHA256_LEN);
    commitment_b = require_tlv_bytes(
        reply_b.payload,
        reply_b.payload_len,
        TLV_ROUTE_REPLY_SHA256_COMMITMENT,
        SEMANTIC_DIGEST_SHA256_LEN);
    assert(!semantic_digest_equal(commitment_a,
                                  commitment_b,
                                  SEMANTIC_DIGEST_SHA256_LEN));

    mesh_relay_init(&origin_a,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    collision_epoch_a);
    mesh_relay_init(&origin_b,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    collision_epoch_b);
    assert(mesh_relay_handle_rx(&origin_a,
                                &reply_a.packet,
                                reply_a.payload,
                                reply_a.payload_len,
                                ANCHOR_B,
                                90u,
                                1020u,
                                &receive_a) == PROTO_OK);
    assert(receive_a.status == PROTO_OK);
    assert(has_action(&receive_a,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(mesh_relay_handle_rx(&origin_b,
                                &reply_b.packet,
                                reply_b.payload,
                                reply_b.payload_len,
                                ANCHOR_B,
                                90u,
                                1020u,
                                &receive_b) == PROTO_OK);
    assert(receive_b.status == PROTO_OK);
    assert(has_action(&receive_b,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(receive_a.route_reply_ack.payload_len ==
           MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN);

    malformed_outbound = reply_a;
    malformed_outbound.next_hop_id = sender_a.local_id;
    mesh_relay_note_tx_sent(&sender_a, &malformed_outbound, 1021u);
    assert(!sender_a.route_reply_ack_expectation.active);
    mesh_relay_note_tx_sent(&sender_a, &reply_a, 1021u);
    assert(sender_a.route_reply_ack_expectation.active);
    padded_ack = receive_a.route_reply_ack;
    padded_ack_len = padded_ack.payload_len;
    assert(tlv_append_u16(padded_ack.payload,
                          sizeof(padded_ack.payload),
                          &padded_ack_len,
                          TLV_REPLY_NONCE,
                          1u) == PROTO_OK);
    padded_ack.payload_len = (uint16_t)padded_ack_len;
    padded_ack.packet.payload_len = padded_ack.payload_len;
    assert(mesh_relay_handle_rx(&sender_a,
                                &padded_ack.packet,
                                padded_ack.payload,
                                padded_ack.payload_len,
                                ANCHOR_A,
                                90u,
                                1022u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_ERR_MALFORMED);
    assert(sender_a.route_reply_ack_expectation.active);

    assert(mesh_relay_handle_rx(&sender_a,
                                &receive_b.route_reply_ack.packet,
                                receive_b.route_reply_ack.payload,
                                receive_b.route_reply_ack.payload_len,
                                ANCHOR_A,
                                90u,
                                1023u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_ERR_STALE);
    assert(has_action(&ack_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&ack_result,
                       MESH_RELAY_ACTION_ROUTE_REPLY_ACKED));
    assert(sender_a.route_reply_ack_expectation.active);

    assert(mesh_relay_handle_rx(&sender_a,
                                &receive_a.route_reply_ack.packet,
                                receive_a.route_reply_ack.payload,
                                receive_a.route_reply_ack.payload_len,
                                ANCHOR_A,
                                90u,
                                1024u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_OK);
    assert(has_action(&ack_result,
                      MESH_RELAY_ACTION_ROUTE_REPLY_ACKED));
    assert(!sender_a.route_reply_ack_expectation.active);

    assert(mesh_relay_handle_rx(&sender_a,
                                &receive_a.route_reply_ack.packet,
                                receive_a.route_reply_ack.payload,
                                receive_a.route_reply_ack.payload_len,
                                ANCHOR_A,
                                90u,
                                1025u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_ERR_STALE);
    assert(!has_action(&ack_result,
                       MESH_RELAY_ACTION_ROUTE_REPLY_ACKED));

    mesh_relay_note_tx_sent(&sender_a, &reply_b, 1026u);
    assert(sender_a.route_reply_ack_expectation.active);
    assert(mesh_relay_handle_rx(&sender_a,
                                &receive_a.route_reply_ack.packet,
                                receive_a.route_reply_ack.payload,
                                receive_a.route_reply_ack.payload_len,
                                ANCHOR_A,
                                90u,
                                1027u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_ERR_STALE);
    assert(sender_a.route_reply_ack_expectation.active);
    assert(mesh_relay_handle_rx(&sender_a,
                                &receive_b.route_reply_ack.packet,
                                receive_b.route_reply_ack.payload,
                                receive_b.route_reply_ack.payload_len,
                                ANCHOR_A,
                                90u,
                                1028u,
                                &ack_result) == PROTO_OK);
    assert(ack_result.status == PROTO_OK);
    assert(has_action(&ack_result,
                      MESH_RELAY_ACTION_ROUTE_REPLY_ACKED));
    assert(!sender_a.route_reply_ack_expectation.active);
}

static void test_route_reply_requires_exact_commitment_before_route_mutation(void)
{
    struct mesh_relay sender;
    struct mesh_relay receiver;
    struct mesh_outbound valid_reply;
    struct mesh_outbound malformed_reply;
    struct mesh_relay_result result;
    const uint8_t *commitment;
    const uint8_t *metric_crc;
    size_t commitment_offset;
    size_t metric_crc_offset;

    build_route_reply_for_collision_epoch(4080u, &sender, &valid_reply);
    assert(valid_reply.payload_len >=
           PROTO_TLV_HEADER_LEN + SEMANTIC_DIGEST_SHA256_LEN);

    mesh_relay_init(&receiver,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    4080u);
    assert(mesh_relay_validate_route_reply(&receiver,
                                           &valid_reply.packet,
                                           valid_reply.payload,
                                           valid_reply.payload_len,
                                           ANCHOR_B,
                                           1020u) == PROTO_OK);
    malformed_reply = valid_reply;
    malformed_reply.payload_len -= (uint16_t)(
        PROTO_TLV_HEADER_LEN + SEMANTIC_DIGEST_SHA256_LEN);
    malformed_reply.packet.payload_len = malformed_reply.payload_len;
    assert(mesh_relay_validate_route_reply(&receiver,
                                           &malformed_reply.packet,
                                           malformed_reply.payload,
                                           malformed_reply.payload_len,
                                           ANCHOR_B,
                                           1020u) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_relay_handle_rx(&receiver,
                                &malformed_reply.packet,
                                malformed_reply.payload,
                                malformed_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                1020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(route_selected(&receiver.upstream) == NULL);

    mesh_relay_init(&receiver,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    4080u);
    malformed_reply = valid_reply;
    commitment = require_tlv_bytes(
        malformed_reply.payload,
        malformed_reply.payload_len,
        TLV_ROUTE_REPLY_SHA256_COMMITMENT,
        SEMANTIC_DIGEST_SHA256_LEN);
    commitment_offset =
        (size_t)(commitment - malformed_reply.payload);
    malformed_reply.payload[commitment_offset] ^= 0x01u;
    assert(mesh_relay_validate_route_reply(&receiver,
                                           &malformed_reply.packet,
                                           malformed_reply.payload,
                                           malformed_reply.payload_len,
                                           ANCHOR_B,
                                           1021u) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_relay_handle_rx(&receiver,
                                &malformed_reply.packet,
                                malformed_reply.payload,
                                malformed_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                1021u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(route_selected(&receiver.upstream) == NULL);

    mesh_relay_init(&receiver,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    4080u);
    malformed_reply = valid_reply;
    metric_crc = require_tlv_bytes(malformed_reply.payload,
                                   malformed_reply.payload_len,
                                   TLV_METRIC_CRC,
                                   sizeof(uint16_t));
    metric_crc_offset =
        (size_t)(metric_crc - malformed_reply.payload);
    malformed_reply.payload[metric_crc_offset] ^= 0x01u;
    assert(mesh_relay_validate_route_reply(&receiver,
                                           &malformed_reply.packet,
                                           malformed_reply.payload,
                                           malformed_reply.payload_len,
                                           ANCHOR_B,
                                           1022u) ==
           PROTO_ERR_MALFORMED);
    assert(mesh_relay_handle_rx(&receiver,
                                &malformed_reply.packet,
                                malformed_reply.payload,
                                malformed_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                1022u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(route_selected(&receiver.upstream) == NULL);
}

static void test_direct_route_reply_capacity_deadline_wraps_to_valid_zero(void)
{
    const uint32_t reply_received_ms =
        UINT32_MAX - RELAY_CAPACITY_HINT_VALIDITY_MS + 1u;
    const uint32_t request_ms = reply_received_ms - 20u;
    struct mesh_relay origin;
    struct mesh_relay responder;
    struct mesh_outbound route_request;
    struct mesh_relay_result responder_result;
    struct mesh_relay_result origin_result;
    const struct route_candidate *selected;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 11u);
    assert(mesh_relay_note_direct_gateway_route(
               &responder, request_ms) == PROTO_OK);
    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &route_request,
                                          request_ms) == PROTO_OK);
    assert(mesh_relay_handle_rx(&responder,
                                &route_request.packet,
                                route_request.payload,
                                route_request.payload_len,
                                ANCHOR_A,
                                90u,
                                request_ms + 5u,
                                &responder_result) == PROTO_OK);
    assert(has_action(&responder_result,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY));

    assert(mesh_relay_handle_rx(&origin,
                                &responder_result.route_reply.packet,
                                responder_result.route_reply.payload,
                                responder_result.route_reply.payload_len,
                                ANCHOR_B,
                                80u,
                                reply_received_ms,
                                &origin_result) == PROTO_OK);
    assert(origin_result.status == PROTO_OK);
    selected = route_selected(&origin.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(selected->capacity_observed_at_ms == reply_received_ms);
    assert(selected->capacity_valid_until_ms == 0u);
    assert(selected->capacity_hint_valid);
}

static void test_route_reply_timing_conflict_preserves_discovery_without_route(void)
{
    struct mesh_relay origin;
    struct mesh_relay responder;
    struct mesh_outbound route_request;
    struct mesh_relay_result responder_result;
    struct mesh_relay_result origin_result;
    struct route_candidate direct = direct_gateway_route(GATEWAY, 81u, 90u);
    struct route_candidate existing = direct_gateway_route(ANCHOR_C, 81u, 95u);
    struct mesh_event_params params = channel9_params(1500u);
    struct mesh_event_timing proposed_timing = {0};
    struct mesh_event_timing conflicting_timing = {0};
    const struct route_candidate *selected;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 81u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 81u);
    assert(route_upsert_candidate(&responder.upstream, &direct) == PROTO_OK);
    assert(route_upsert_candidate(&origin.upstream, &existing) == PROTO_OK);

    assert(mesh_event_timing_negotiate(&proposed_timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);
    conflicting_timing = proposed_timing;
    mesh_event_timing_set_local_first_slot_tx(&conflicting_timing, false);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &origin,
               ANCHOR_C,
               &conflicting_timing,
               MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
               MESH_RELAY_EVENT_TIMINGS,
               NULL) == PROTO_OK);

    assert(mesh_relay_prepare_route_request_with_timing(&origin,
                                                        GATEWAY,
                                                        &proposed_timing,
                                                        1000u,
                                                        1000u,
                                                        0x12345678u,
                                                        &route_request) == PROTO_OK);
    assert(origin.route_discovery.active);
    assert(mesh_relay_handle_rx(&responder,
                                &route_request.packet,
                                route_request.payload,
                                route_request.payload_len,
                                ANCHOR_A,
                                90u,
                                1010u,
                                &responder_result) == PROTO_OK);
    assert(responder_result.status == PROTO_OK);
    assert(has_action(&responder_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));

    assert(mesh_relay_handle_rx(&origin,
                                &responder_result.route_reply.packet,
                                responder_result.route_reply.payload,
                                responder_result.route_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                1040u,
                                &origin_result) == PROTO_OK);
    assert(origin_result.status == PROTO_ERR_BUSY);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&origin_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(!has_action(&origin_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    assert(origin.route_discovery.active);
    assert(origin.route_discovery.target_id == GATEWAY);
    selected = route_selected(&origin.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_C);
    for (size_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        assert(!origin.upstream.candidates[i].valid ||
               origin.upstream.candidates[i].next_hop_id != ANCHOR_B);
    }
    assert(channel9_timing_present(&origin, ANCHOR_C));
    assert(!channel9_timing_present(&origin, ANCHOR_B));
}

static void test_multihop_route_reply_forward_uses_channel_five(void)
{
    struct mesh_relay origin;
    struct mesh_relay intermediate;
    struct mesh_relay missing_reverse;
    struct mesh_relay busy_intermediate;
    struct mesh_relay responder;
    struct mesh_outbound request;
    struct mesh_outbound busy_report_tx;
    struct mesh_relay_result intermediate_request;
    struct mesh_relay_result busy_request;
    struct mesh_relay_result responder_reply;
    struct mesh_relay_result forwarded_reply;
    struct mesh_relay_result forwarded_retry;
    struct mesh_relay_result rejected_reply;
    struct proto_packet busy_report;
    struct route_candidate direct = direct_gateway_route(GATEWAY, 40u, 90u);
    uint8_t busy_payload[] = {0x42u};

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 40u);
    mesh_relay_init(&intermediate, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 40u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 40u);
    assert(route_upsert_candidate(&responder.upstream, &direct) == PROTO_OK);

    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &request,
                                          1000u) == PROTO_OK);
    request.packet.ttl = 4u;
    assert(mesh_relay_handle_rx(&intermediate,
                                &request.packet,
                                request.payload,
                                request.payload_len,
                                ANCHOR_A,
                                80u,
                                1010u,
                                &intermediate_request) == PROTO_OK);
    assert(has_action(&intermediate_request, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    assert(mesh_relay_handle_rx(&responder,
                                &intermediate_request.route_request.packet,
                                intermediate_request.route_request.payload,
                                intermediate_request.route_request.payload_len,
                                ANCHOR_B,
                                80u,
                                1020u,
                                &responder_reply) == PROTO_OK);
    assert(has_action(&responder_reply, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(responder_reply.route_reply.next_hop_id == ANCHOR_B);
    assert(responder_reply.route_reply.radio_channel == UWB_CHANNEL_WAKE_CONTACT);

    assert(mesh_relay_handle_rx(&intermediate,
                                &responder_reply.route_reply.packet,
                                responder_reply.route_reply.payload,
                                responder_reply.route_reply.payload_len,
                                ANCHOR_C,
                                80u,
                                1030u,
                                &forwarded_reply) == PROTO_OK);
    assert(has_action(&forwarded_reply, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(has_action(&forwarded_reply, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(forwarded_reply.route_reply.next_hop_id == ANCHOR_A);
    assert(forwarded_reply.route_reply.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(forwarded_reply.route_reply_ack.packet.msg_type == MSG_ROUTE_REPLY_ACK);
    assert(forwarded_reply.route_reply_ack.packet.dst_id == ANCHOR_C);
    assert(forwarded_reply.route_reply_ack.next_hop_id == ANCHOR_C);

    /*
     * The first application handoff can fail after core admission.  An exact
     * upstream retry must rebuild both the downstream forward and the
     * conditional upstream ACK instead of being treated as ACK-sticky before
     * downstream custody exists.
     */
    assert(mesh_relay_handle_rx(&intermediate,
                                &responder_reply.route_reply.packet,
                                responder_reply.route_reply.payload,
                                responder_reply.route_reply.payload_len,
                                ANCHOR_C,
                                80u,
                                1031u,
                                &forwarded_retry) == PROTO_OK);
    assert(forwarded_retry.status == PROTO_OK);
    assert(has_action(&forwarded_retry,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(has_action(&forwarded_retry,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(!has_action(&forwarded_retry, MESH_RELAY_ACTION_DROP));

    mesh_relay_init(&missing_reverse,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    40u);
    assert(mesh_relay_handle_rx(&missing_reverse,
                                &responder_reply.route_reply.packet,
                                responder_reply.route_reply.payload,
                                responder_reply.route_reply.payload_len,
                                ANCHOR_C,
                                80u,
                                1032u,
                                &rejected_reply) == PROTO_OK);
    assert(rejected_reply.status == PROTO_ERR_NOT_FOUND);
    assert(has_action(&rejected_reply, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&rejected_reply,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&rejected_reply,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));

    mesh_relay_init(&busy_intermediate,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    40u);
    assert(mesh_relay_handle_rx(&busy_intermediate,
                                &request.packet,
                                request.payload,
                                request.payload_len,
                                ANCHOR_A,
                                80u,
                                1040u,
                                &busy_request) == PROTO_OK);
    assert(has_action(&busy_request, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(route_upsert_candidate(&busy_intermediate.upstream,
                                  &direct) == PROTO_OK);
    assert(report_init_click_packet(&busy_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    41u,
                                    1u,
                                    sizeof(busy_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&busy_intermediate,
                               &busy_report,
                               busy_payload,
                               sizeof(busy_payload),
                               1041u,
                               &busy_report_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&busy_intermediate,
                                &responder_reply.route_reply.packet,
                                responder_reply.route_reply.payload,
                                responder_reply.route_reply.payload_len,
                                ANCHOR_C,
                                80u,
                                1042u,
                                &rejected_reply) == PROTO_OK);
    assert(rejected_reply.status == PROTO_ERR_BUSY);
    assert(has_action(&rejected_reply, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&rejected_reply,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&rejected_reply,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
}

static void test_concurrent_route_replies_use_their_discovery_predecessor(void)
{
    struct mesh_relay origin;
    struct mesh_relay second_origin;
    struct mesh_relay intermediate;
    struct mesh_relay responder;
    struct mesh_outbound request_one;
    struct mesh_outbound request_two;
    struct mesh_relay_result request_one_forward;
    struct mesh_relay_result request_two_forward;
    struct mesh_relay_result reply_one;
    struct mesh_relay_result reply_two;
    struct mesh_relay_result forwarded_reply;
    struct route_candidate direct = direct_gateway_route(GATEWAY, 44u, 90u);

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 44u);
    mesh_relay_init(&second_origin, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_C, GATEWAY, 44u);
    mesh_relay_init(&intermediate, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 44u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_D, GATEWAY, 44u);
    assert(route_upsert_candidate(&responder.upstream, &direct) == PROTO_OK);

    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &request_one,
                                          1000u) == PROTO_OK);
    assert(mesh_relay_build_route_request(&second_origin,
                                          GATEWAY,
                                          &request_two,
                                          2000u) == PROTO_OK);
    assert(request_one.packet.session_id != request_two.packet.session_id);
    request_one.packet.ttl = 4u;
    request_two.packet.ttl = 4u;

    assert(mesh_relay_handle_rx(&intermediate,
                                &request_one.packet,
                                request_one.payload,
                                request_one.payload_len,
                                ANCHOR_A,
                                80u,
                                2010u,
                                &request_one_forward) == PROTO_OK);
    assert(has_action(&request_one_forward, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(mesh_relay_handle_rx(&intermediate,
                                &request_two.packet,
                                request_two.payload,
                                request_two.payload_len,
                                ANCHOR_C,
                                80u,
                                2020u,
                                &request_two_forward) == PROTO_OK);
    assert(has_action(&request_two_forward, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(mesh_relay_expire_routes(&intermediate, UINT32_MAX) == 0u);

    assert(mesh_relay_handle_rx(&responder,
                                &request_one_forward.route_request.packet,
                                request_one_forward.route_request.payload,
                                request_one_forward.route_request.payload_len,
                                ANCHOR_B,
                                80u,
                                2030u,
                                &reply_one) == PROTO_OK);
    assert(has_action(&reply_one, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(mesh_relay_handle_rx(&responder,
                                &request_two_forward.route_request.packet,
                                request_two_forward.route_request.payload,
                                request_two_forward.route_request.payload_len,
                                ANCHOR_B,
                                80u,
                                2040u,
                                &reply_two) == PROTO_OK);
    assert(has_action(&reply_two, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));

    assert(mesh_relay_handle_rx(&intermediate,
                                &reply_two.route_reply.packet,
                                reply_two.route_reply.payload,
                                reply_two.route_reply.payload_len,
                                ANCHOR_D,
                                80u,
                                2050u,
                                &forwarded_reply) == PROTO_OK);
    assert(has_action(&forwarded_reply, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(forwarded_reply.route_reply.packet.session_id ==
           request_two.packet.session_id);
    assert(forwarded_reply.route_reply.next_hop_id == ANCHOR_C);

    assert(mesh_relay_handle_rx(&intermediate,
                                &reply_one.route_reply.packet,
                                reply_one.route_reply.payload,
                                reply_one.route_reply.payload_len,
                                ANCHOR_D,
                                80u,
                                2060u,
                                &forwarded_reply) == PROTO_OK);
    assert(has_action(&forwarded_reply, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(forwarded_reply.route_reply.packet.session_id ==
           request_one.packet.session_id);
    assert(forwarded_reply.route_reply.next_hop_id == ANCHOR_A);
}

static void test_relay_required_route_request_ignores_direct_gateway_copy(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound route_req;
    struct mesh_outbound normal_route_req;
    struct mesh_relay_result direct_result;
    struct mesh_relay_result normal_direct_result;
    struct mesh_relay_result relay_result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 1u, 90u);

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 50u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &anchor,
               GATEWAY,
               NULL,
               0u,
               MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED,
               0u,
               &route_req,
               1000u) == PROTO_OK);
    assert(require_tlv_u8(route_req.payload,
                          route_req.payload_len,
                          TLV_ROUTE_REQUEST_FLAGS) ==
           MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED);

    assert(mesh_relay_handle_rx(&gateway,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                90u,
                                1010u,
                                &direct_result) == PROTO_OK);
    assert(direct_result.status == PROTO_ERR_STALE);
    assert(has_action(&direct_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&direct_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&direct_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    assert(mesh_relay_build_route_request_with_timing_flags(
               &anchor,
               GATEWAY,
               NULL,
               0u,
               0u,
               0u,
               &normal_route_req,
               1100u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &normal_route_req.packet,
                                normal_route_req.payload,
                                normal_route_req.payload_len,
                                ANCHOR_A,
                                90u,
                                1110u,
                                &normal_direct_result) == PROTO_OK);
    assert(normal_direct_result.status == PROTO_ERR_STALE);
    assert(has_action(&normal_direct_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&normal_direct_result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&normal_direct_result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1020u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(relay_result.route_reply.next_hop_id == ANCHOR_A);
    assert(tlv_present(relay_result.route_reply.payload,
                       relay_result.route_reply.payload_len,
                       TLV_ROUTE_REQUEST_FLAGS) == PROTO_ERR_NOT_FOUND);
    assert(!has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
}

struct exact_hop2_timed_request_fixture {
    struct mesh_relay responder;
    struct mesh_relay origin;
    struct mesh_outbound request;
    struct mesh_event_timing upstream_timing;
    struct mesh_event_timing proposed_timing;
};

static struct mesh_event_params production_channel9_params(
    uint32_t first_event_time_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = MESH_RADIO_EVENT_INTERVAL_MS,
        .event_window_ms = MESH_RADIO_EVENT_WINDOW_MS,
        .first_event_time_ms = first_event_time_ms,
        .guard_ms = MESH_RADIO_EVENT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = MESH_RADIO_EVENT_MAX_MISSES,
        .supervision_timeout_ms = MESH_RADIO_EVENT_SUPERVISION_MS,
    };

    return params;
}

static void init_exact_hop2_timed_request_fixture(
    struct exact_hop2_timed_request_fixture *fixture,
    uint32_t upstream_first_event_ms,
    uint32_t proposed_first_event_ms)
{
    const uint32_t route_epoch = 73u;
    const uint32_t request_ms = 1000u;
    const uint8_t exact_two_hop_flags =
        MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED |
        MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(2u);
    struct mesh_relay gateway;
    struct mesh_relay parent;
    struct mesh_outbound gateway_adv;
    struct mesh_relay_result parent_adv_result;
    struct mesh_relay_result responder_adv_result;
    struct mesh_event_params params;
    const struct route_candidate *selected;
    const struct mesh_relay_event_timing_entry *upstream;

    assert(fixture != NULL);
    memset(fixture, 0, sizeof(*fixture));
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(&parent,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(&fixture->responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    route_epoch);
    mesh_relay_init(&fixture->origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_C,
                    GATEWAY,
                    route_epoch);

    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              701u,
                                              500u,
                                              &gateway_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&parent,
                                &gateway_adv.packet,
                                gateway_adv.payload,
                                gateway_adv.payload_len,
                                GATEWAY,
                                96u,
                                510u,
                                &parent_adv_result) == PROTO_OK);
    assert(has_action(&parent_adv_result,
                      MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(mesh_relay_handle_rx(
               &fixture->responder,
               &parent_adv_result.gateway_route_adv.packet,
               parent_adv_result.gateway_route_adv.payload,
               parent_adv_result.gateway_route_adv.payload_len,
               ANCHOR_A,
               94u,
               520u,
               &responder_adv_result) == PROTO_OK);
    selected = route_selected(&fixture->responder.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_A);
    assert(selected->gateway_id == GATEWAY);
    assert(selected->hop_count == 1u);

    params = production_channel9_params(upstream_first_event_ms);
    assert(mesh_event_timing_negotiate(&fixture->upstream_timing,
                                       &params,
                                       true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&fixture->upstream_timing, true);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &fixture->responder,
               ANCHOR_A,
               &fixture->upstream_timing,
               MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
               2u,
               NULL) == PROTO_OK);
    upstream = find_event_timing(&fixture->responder, ANCHOR_A);
    assert(upstream != NULL);
    assert(upstream->direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);

    params = production_channel9_params(proposed_first_event_ms);
    assert(mesh_event_timing_negotiate(&fixture->proposed_timing,
                                       &params,
                                       true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&fixture->proposed_timing, true);
    assert(mesh_relay_prepare_route_request_with_timing_flags(
               &fixture->origin,
               GATEWAY,
               &fixture->proposed_timing,
               request_ms,
               exact_two_hop_flags,
               MESH_RADIO_EVENT_ACCEPT_DELAY_MS,
               request_ms,
               0x73007300u,
               &fixture->request) == PROTO_OK);
    assert(require_tlv_u8(fixture->request.payload,
                          fixture->request.payload_len,
                          TLV_ROUTE_REQUEST_FLAGS) == exact_two_hop_flags);
}

static void assert_event_timing_entry_equal(
    const struct mesh_relay_event_timing_entry *actual,
    const struct mesh_relay_event_timing_entry *expected)
{
    assert(actual != NULL);
    assert(expected != NULL);
    assert(actual->valid == expected->valid);
    assert(actual->next_hop_id == expected->next_hop_id);
    assert(actual->direction == expected->direction);
    assert_event_timing_equal(&actual->timing, &expected->timing);
}

static void test_exact_hop2_route_request_installs_nonconflicting_downstream(void)
{
    const uint32_t upstream_first_event_ms = 5000u;
    const uint32_t request_ms = 1000u;
    struct exact_hop2_timed_request_fixture fixture;
    struct mesh_relay preview;
    struct mesh_outbound expected_reply;
    struct mesh_relay_result result;
    struct mesh_relay_event_timing_entry upstream_before;
    struct mesh_event_timing expected_downstream;
    const struct mesh_relay_event_timing_entry *upstream;
    const struct mesh_relay_event_timing_entry *downstream;
    uint8_t upstream_wire_before[64];
    uint8_t upstream_wire_after[64];
    size_t upstream_wire_before_len = 0u;
    size_t upstream_wire_after_len = 0u;

    init_exact_hop2_timed_request_fixture(
        &fixture,
        upstream_first_event_ms,
        upstream_first_event_ms + (MESH_RADIO_EVENT_INTERVAL_MS / 2u));
    upstream = find_event_timing(&fixture.responder, ANCHOR_A);
    assert(upstream != NULL);
    upstream_before = *upstream;
    assert(mesh_append_event_timing_tlvs_at(upstream_wire_before,
                                            sizeof(upstream_wire_before),
                                            &upstream_wire_before_len,
                                            &upstream_before.timing,
                                            request_ms) == PROTO_OK);

    /* The pure construction seam produces the exact reply without reserving
     * the child's cadence.  The receive transaction may commit that cadence
     * only after it can produce this reply successfully. */
    preview = fixture.responder;
    assert(mesh_relay_build_route_reply_for_request(
               &preview,
               &fixture.request.packet,
               fixture.request.payload,
               fixture.request.payload_len,
               ANCHOR_C,
               request_ms,
               0u,
               &expected_reply) == PROTO_OK);
    assert(require_tlv_u8(expected_reply.payload,
                          expected_reply.payload_len,
                          TLV_HOP_COUNT) == 2u);
    assert(channel9_timing_count(&fixture.responder) == 1u);
    assert(find_event_timing(&fixture.responder, ANCHOR_C) == NULL);

    assert(mesh_relay_handle_rx(&fixture.responder,
                                &fixture.request.packet,
                                fixture.request.payload,
                                fixture.request.payload_len,
                                ANCHOR_C,
                                92u,
                                request_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.route_reply.packet.msg_type == MSG_ROUTE_REPLY);
    assert(result.route_reply.packet.src_id == expected_reply.packet.src_id);
    assert(result.route_reply.packet.dst_id == expected_reply.packet.dst_id);
    assert(result.route_reply.packet.session_id ==
           expected_reply.packet.session_id);
    assert(result.route_reply.packet.seq == expected_reply.packet.seq);
    assert(result.route_reply.packet.message_age_ms ==
           expected_reply.packet.message_age_ms);
    assert(result.route_reply.packet.flags == expected_reply.packet.flags);
    assert(result.route_reply.packet.ttl == expected_reply.packet.ttl);
    assert(result.route_reply.packet.payload_len ==
           expected_reply.packet.payload_len);
    assert(result.route_reply.payload_len == expected_reply.payload_len);
    assert(memcmp(result.route_reply.payload,
                  expected_reply.payload,
                  expected_reply.payload_len) == 0);
    assert(result.route_reply.radio_channel == expected_reply.radio_channel);
    assert(result.route_reply.next_hop_id == expected_reply.next_hop_id);
    assert(result.route_reply.next_hop_id == ANCHOR_C);
    assert(result.route_reply.earliest_tx_valid ==
           expected_reply.earliest_tx_valid);
    assert(result.route_reply.earliest_tx_ms == expected_reply.earliest_tx_ms);
    assert(require_tlv_u8(result.route_reply.payload,
                          result.route_reply.payload_len,
                          TLV_HOP_COUNT) == 2u);

    upstream = find_event_timing(&fixture.responder, ANCHOR_A);
    assert_event_timing_entry_equal(upstream, &upstream_before);
    assert(mesh_append_event_timing_tlvs_at(upstream_wire_after,
                                            sizeof(upstream_wire_after),
                                            &upstream_wire_after_len,
                                            &upstream->timing,
                                            request_ms) == PROTO_OK);
    assert(upstream_wire_after_len == upstream_wire_before_len);
    assert(memcmp(upstream_wire_after,
                  upstream_wire_before,
                  upstream_wire_before_len) == 0);

    downstream = find_event_timing(&fixture.responder, ANCHOR_C);
    assert(downstream != NULL);
    assert(downstream->direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert(mesh_event_timing_from_tlvs_at(&expected_downstream,
                                          fixture.request.payload,
                                          fixture.request.payload_len,
                                          request_ms,
                                          true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&expected_downstream, false);
    assert_event_timing_equal(&downstream->timing, &expected_downstream);
    assert(channel9_timing_count(&fixture.responder) == 2u);
}

static void test_exact_hop2_route_request_omits_conflicting_timing(void)
{
    const uint32_t first_event_ms = 5000u;
    struct exact_hop2_timed_request_fixture fixture;
    struct mesh_relay_result result;
    struct mesh_relay_event_timing_entry upstream_before;
    const struct mesh_relay_event_timing_entry *upstream;
    const struct mesh_downlink_entry *reverse;

    init_exact_hop2_timed_request_fixture(
        &fixture, first_event_ms, first_event_ms);
    upstream = find_event_timing(&fixture.responder, ANCHOR_A);
    assert(upstream != NULL);
    upstream_before = *upstream;

    assert(mesh_relay_handle_rx(&fixture.responder,
                                &fixture.request.packet,
                                fixture.request.payload,
                                fixture.request.payload_len,
                                ANCHOR_C,
                                92u,
                                1000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.route_reply.packet.msg_type == MSG_ROUTE_REPLY);
    assert(result.route_reply.next_hop_id == ANCHOR_C);
    assert(require_tlv_u8(result.route_reply.payload,
                          result.route_reply.payload_len,
                          TLV_HOP_COUNT) == 2u);
    assert(tlv_present(result.route_reply.payload,
                       result.route_reply.payload_len,
                       TLV_MESH_EVENT_INTERVAL_MS) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(result.route_reply.payload,
                       result.route_reply.payload_len,
                       TLV_MESH_EVENT_WINDOW_MS) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(result.route_reply.payload,
                       result.route_reply.payload_len,
                       TLV_MESH_NEXT_EVENT_TIME_MS) == PROTO_ERR_NOT_FOUND);
    assert(tlv_present(result.route_reply.payload,
                       result.route_reply.payload_len,
                       TLV_MESH_EVENT_GUARD_MS) == PROTO_ERR_NOT_FOUND);

    reverse = mesh_relay_find_downlink(&fixture.responder, ANCHOR_C);
    assert(reverse != NULL);
    assert(reverse->target_id == ANCHOR_C);
    assert(reverse->next_hop_id == ANCHOR_C);
    assert(find_event_timing(&fixture.responder, ANCHOR_C) == NULL);
    assert_event_timing_entry_equal(
        find_event_timing(&fixture.responder, ANCHOR_A), &upstream_before);
    assert(channel9_timing_count(&fixture.responder) == 1u);
}

static void test_exact_hop2_route_request_rejects_unsafe_capacity(void)
{
    const uint32_t upstream_first_event_ms = 5000u;
    const uint32_t half_phase_ms =
        upstream_first_event_ms + (MESH_RADIO_EVENT_INTERVAL_MS / 2u);
    struct exact_hop2_timed_request_fixture fixture;
    struct mesh_relay_result result;
    struct mesh_relay_event_timing_entry retained_before;
    const struct mesh_relay_event_timing_entry *retained;

    /* A second downstream peer is rejected even when aggregate storage has
     * room and the hop1 route itself remains usable. */
    init_exact_hop2_timed_request_fixture(
        &fixture, upstream_first_event_ms, half_phase_ms);
    mesh_relay_clear_channel9_timing(&fixture.responder, ANCHOR_A);
    seed_downlink(&fixture.responder,
                  ANCHOR_D,
                  ANCHOR_D,
                  73u,
                  1u,
                  90u,
                  900u);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &fixture.responder,
               ANCHOR_D,
               &fixture.proposed_timing,
               MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
               2u,
               NULL) == PROTO_OK);
    retained = find_event_timing(&fixture.responder, ANCHOR_D);
    assert(retained != NULL);
    retained_before = *retained;
    assert(mesh_relay_handle_rx(&fixture.responder,
                                &fixture.request.packet,
                                fixture.request.payload,
                                fixture.request.payload_len,
                                ANCHOR_C,
                                92u,
                                1000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(find_event_timing(&fixture.responder, ANCHOR_C) == NULL);
    assert_event_timing_entry_equal(
        find_event_timing(&fixture.responder, ANCHOR_D), &retained_before);

    /* A completely occupied timing table fails as capacity, without
     * replacing any owner or fabricating a reply. */
    init_exact_hop2_timed_request_fixture(
        &fixture, upstream_first_event_ms, half_phase_ms);
    for (uint8_t i = 1u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        fixture.responder.event_timings[i].next_hop_id =
            UINT64_C(0x7000000000000000) + i;
        fixture.responder.event_timings[i].timing =
            fixture.proposed_timing;
        fixture.responder.event_timings[i].direction =
            (i & 1u) != 0u ?
                MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM :
                MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM;
        fixture.responder.event_timings[i].valid = true;
    }
    assert(channel9_timing_count(&fixture.responder) ==
           MESH_RELAY_EVENT_TIMINGS);
    assert(mesh_relay_handle_rx(&fixture.responder,
                                &fixture.request.packet,
                                fixture.request.payload,
                                fixture.request.payload_len,
                                ANCHOR_C,
                                92u,
                                1000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_NO_SPACE);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(find_event_timing(&fixture.responder, ANCHOR_C) == NULL);
    assert(channel9_timing_count(&fixture.responder) ==
           MESH_RELAY_EVENT_TIMINGS);

    /* Ambiguous live ownership fails closed before the requested downstream
     * cadence or reverse route can become authoritative. */
    init_exact_hop2_timed_request_fixture(
        &fixture, upstream_first_event_ms, half_phase_ms);
    seed_downlink(&fixture.responder,
                  ANCHOR_D,
                  ANCHOR_A,
                  73u,
                  1u,
                  90u,
                  900u);
    retained = find_event_timing(&fixture.responder, ANCHOR_A);
    assert(retained != NULL);
    fixture.responder.event_timings[
        (size_t)(retained - fixture.responder.event_timings)].direction =
            MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS;
    retained = find_event_timing(&fixture.responder, ANCHOR_A);
    assert(retained != NULL);
    retained_before = *retained;
    assert(mesh_relay_handle_rx(&fixture.responder,
                                &fixture.request.packet,
                                fixture.request.payload,
                                fixture.request.payload_len,
                                ANCHOR_C,
                                92u,
                                1000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(find_event_timing(&fixture.responder, ANCHOR_C) == NULL);
    assert_event_timing_entry_equal(
        find_event_timing(&fixture.responder, ANCHOR_A), &retained_before);
}

static void test_exact_required_route_hops_filter_responder_and_origin(void)
{
    struct mesh_relay responder;
    struct mesh_relay gateway;
    struct mesh_relay first_hop;
    struct mesh_relay second_hop;
    struct mesh_relay origin;
    struct mesh_relay origin_before;
    struct mesh_outbound exact_one_request;
    struct mesh_outbound exact_two_request;
    struct mesh_relay_result exact_one_result;
    struct mesh_relay_result exact_two_result;
    struct mesh_relay_result gateway_adv_first_result;
    struct mesh_relay_result gateway_adv_second_result;
    struct mesh_relay_result rejected_reply_result;
    struct mesh_outbound gateway_adv;
    struct mesh_outbound exact_two_reply;
    struct route_candidate route = direct_gateway_route(GATEWAY, 1u, 90u);
    const uint8_t exact_one_flags =
        MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED |
        MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(1u);
    const uint8_t exact_two_flags =
        MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED |
        MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(2u);

    mesh_relay_init(&responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    1u);
    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    1u);
    assert(route_upsert_candidate(&responder.upstream, &route) == PROTO_OK);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               NULL,
               0u,
               exact_one_flags,
               0u,
               &exact_one_request,
               1200u) == PROTO_OK);
    assert(mesh_relay_handle_rx(&responder,
                                &exact_one_request.packet,
                                exact_one_request.payload,
                                exact_one_request.payload_len,
                                ANCHOR_A,
                                80u,
                                1210u,
                                &exact_one_result) == PROTO_OK);
    assert(exact_one_result.status == PROTO_OK);
    assert(has_action(&exact_one_result,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(require_tlv_u8(exact_one_result.route_reply.payload,
                          exact_one_result.route_reply.payload_len,
                          TLV_HOP_COUNT) == 1u);

    mesh_relay_init(&responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    1u);
    assert(route_upsert_candidate(&responder.upstream, &route) == PROTO_OK);
    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               NULL,
               0u,
               exact_two_flags,
               0u,
               &exact_two_request,
               1300u) == PROTO_OK);
    exact_two_request.packet.ttl = 2u;
    assert(mesh_relay_handle_rx(&responder,
                                &exact_two_request.packet,
                                exact_two_request.payload,
                                exact_two_request.payload_len,
                                ANCHOR_A,
                                80u,
                                1310u,
                                &exact_two_result) == PROTO_OK);
    assert(!has_action(&exact_two_result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(has_action(&exact_two_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    1u);
    mesh_relay_init(&first_hop,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    1u);
    mesh_relay_init(&second_hop,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_C,
                    GATEWAY,
                    1u);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              77u,
                                              1400u,
                                              &gateway_adv) == PROTO_OK);
    assert(mesh_relay_handle_rx(&first_hop,
                                &gateway_adv.packet,
                                gateway_adv.payload,
                                gateway_adv.payload_len,
                                GATEWAY,
                                90u,
                                1410u,
                                &gateway_adv_first_result) == PROTO_OK);
    assert(has_action(&gateway_adv_first_result,
                      MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(mesh_relay_handle_rx(
               &second_hop,
               &gateway_adv_first_result.gateway_route_adv.packet,
               gateway_adv_first_result.gateway_route_adv.payload,
               gateway_adv_first_result.gateway_route_adv.payload_len,
               ANCHOR_B,
               85u,
               1420u,
               &gateway_adv_second_result) == PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(
               &second_hop,
               &exact_two_request.packet,
               exact_two_request.payload,
               exact_two_request.payload_len,
               ANCHOR_A,
               1430u,
               0u,
               &exact_two_reply) == PROTO_OK);
    assert(require_tlv_u8(exact_two_reply.payload,
                          exact_two_reply.payload_len,
                          TLV_HOP_COUNT) == 2u);

    origin.route_discovery.active = true;
    origin.route_discovery.target_id = GATEWAY;
    origin.route_discovery.current_request_id =
        exact_one_result.route_reply.packet.session_id;
    origin.route_discovery.required_hop_count = 2u;
    origin_before = origin;
    assert(mesh_relay_handle_rx(&origin,
                                &exact_one_result.route_reply.packet,
                                exact_one_result.route_reply.payload,
                                exact_one_result.route_reply.payload_len,
                                ANCHOR_B,
                                80u,
                                1320u,
                                &rejected_reply_result) == PROTO_OK);
    assert(rejected_reply_result.status == PROTO_ERR_STALE);
    assert(rejected_reply_result.actions == MESH_RELAY_ACTION_DROP);
    assert(memcmp(&origin, &origin_before, sizeof(origin)) == 0);
}

static void test_route_request_carries_reply_rx_eta(void)
{
    struct mesh_relay origin;
    struct mesh_outbound route_req;
    uint16_t delay_ms = 0u;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               NULL,
               0u,
               0u,
               620u,
               &route_req,
               1000u) == PROTO_OK);
    assert(require_tlv_u16(route_req.payload,
                           route_req.payload_len,
                           TLV_ROUTE_REPLY_RX_DELAY_MS) == 620u);
    assert(mesh_route_request_reply_rx_delay_ms(&route_req, &delay_ms));
    assert(delay_ms == 620u);

    assert(mesh_route_request_set_reply_rx_delay_ms(&route_req, 125u) == PROTO_OK);
    assert(require_tlv_u16(route_req.payload,
                           route_req.payload_len,
                           TLV_ROUTE_REPLY_RX_DELAY_MS) == 125u);
}

static void test_route_reply_waits_for_request_reply_eta(void)
{
    struct mesh_relay relay;
    struct mesh_relay origin;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 1u, 90u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               NULL,
               0u,
               0u,
               620u,
               &route_req,
               1000u) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                2000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(result.route_reply.next_hop_id == ANCHOR_A);
    assert(result.route_reply.earliest_tx_ms == 2620u);
    assert(tlv_present(result.route_reply.payload,
                       result.route_reply.payload_len,
                       TLV_ROUTE_REPLY_RX_DELAY_MS) == PROTO_ERR_NOT_FOUND);
}

static void test_delayed_route_reply_rebases_embedded_channel9_timing(void)
{
    const uint32_t request_tx_ms = 1000u;
    const uint32_t request_rx_ms = 1100u;
    const uint16_t reply_delay_ms = 620u;
    struct mesh_relay origin;
    struct mesh_relay responder;
    struct mesh_outbound route_req;
    struct mesh_relay_result responder_result;
    struct mesh_relay_result origin_result;
    struct route_candidate direct = direct_gateway_route(GATEWAY, 1u, 90u);
    struct mesh_event_params params = channel9_params(4000u);
    struct mesh_event_timing proposed_timing = {0};
    const struct mesh_relay_event_timing_entry *responder_timing;
    const struct mesh_relay_event_timing_entry *origin_timing;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    assert(route_upsert_candidate(&responder.upstream, &direct) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&proposed_timing, &params, true) ==
           PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               &proposed_timing,
               request_tx_ms,
               0u,
               reply_delay_ms,
               &route_req,
               request_tx_ms) == PROTO_OK);
    assert(mesh_relay_handle_rx(&responder,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                request_rx_ms,
                                &responder_result) == PROTO_OK);
    assert(responder_result.status == PROTO_OK);
    assert(has_action(&responder_result,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(responder_result.route_reply.earliest_tx_ms ==
           request_rx_ms + reply_delay_ms);
    responder_timing = find_event_timing(&responder, ANCHOR_A);
    assert(responder_timing != NULL);
    assert(mesh_event_timing_local_rx_slot(&responder_timing->timing));
    assert(responder_timing->timing.next_event_time_ms ==
           params.first_event_time_ms + (request_rx_ms - request_tx_ms));
    assert(require_tlv_u32(responder_result.route_reply.payload,
                           responder_result.route_reply.payload_len,
                           TLV_MESH_NEXT_EVENT_TIME_MS) ==
           responder_timing->timing.next_event_time_ms -
               responder_result.route_reply.earliest_tx_ms);

    assert(mesh_relay_handle_rx(&origin,
                                &responder_result.route_reply.packet,
                                responder_result.route_reply.payload,
                                responder_result.route_reply.payload_len,
                                ANCHOR_B,
                                80u,
                                responder_result.route_reply.earliest_tx_ms,
                                &origin_result) == PROTO_OK);
    assert(origin_result.status == PROTO_OK);
    origin_timing = find_event_timing(&origin, ANCHOR_B);
    assert(origin_timing != NULL);
    assert(mesh_event_timing_local_tx_slot(&origin_timing->timing));
    assert(origin_timing->timing.next_event_time_ms ==
           responder_timing->timing.next_event_time_ms);
}

static void test_gateway_route_request_without_upstream_waits_for_route(void)
{
    struct mesh_relay relay;
    struct mesh_relay origin;
    struct mesh_outbound route_req;
    struct mesh_relay_result first_result;
    struct mesh_relay_result repaired_duplicate_result;
    struct mesh_relay_result ttl1_result;
    struct mesh_relay_result ttl1_route_result;
    struct mesh_relay_result retry_result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 1u, 90u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);

    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &route_req,
                                          1000u) == PROTO_OK);
    route_req.packet.ttl = 2u;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1010u,
                                &first_result) == PROTO_OK);
    assert(first_result.status == PROTO_OK);
    assert(!has_action(&first_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(has_action(&first_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(first_result.route_request.packet.ttl == route_req.packet.ttl - 1u);
    assert(!has_action(&first_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));

    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1020u,
                                &repaired_duplicate_result) == PROTO_OK);
    assert(repaired_duplicate_result.status == PROTO_OK);
    assert(has_action(&repaired_duplicate_result,
                      MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&repaired_duplicate_result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!has_action(&repaired_duplicate_result,
                       MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(repaired_duplicate_result.route_reply.next_hop_id == ANCHOR_A);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    route_req.packet.ttl = 1u;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1020u,
                                &ttl1_result) == PROTO_OK);
    assert(ttl1_result.status == PROTO_ERR_NOT_FOUND);
    assert(has_action(&ttl1_result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&ttl1_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&ttl1_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!has_action(&ttl1_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    route_req.packet.ttl = FLOOD_EPOCH_LOCAL_TTL;

    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1030u,
                                &retry_result) == PROTO_OK);
    assert(retry_result.status == PROTO_OK);
    assert(has_action(&retry_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&retry_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(retry_result.route_reply.next_hop_id == ANCHOR_A);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &route_req,
                                          1100u) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(route_req.packet.ttl == 1u);
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1110u,
                                &ttl1_route_result) == PROTO_OK);
    assert(ttl1_route_result.status == PROTO_OK);
    assert(has_action(&ttl1_route_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(!has_action(&ttl1_route_result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!has_action(&ttl1_route_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(ttl1_route_result.route_reply.next_hop_id == ANCHOR_A);
}

static void test_malformed_route_request_does_not_poison_downlink_route(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 60u, 90u);
    const uint8_t *hop_value = NULL;
    uint8_t hop_value_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 60u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_build_route_request(&relay, GATEWAY, &route_req, 1000u) ==
           PROTO_OK);

    route_req.packet.src_id = ANCHOR_A;
    assert(tlv_find(route_req.payload,
                    route_req.payload_len,
                    TLV_HOP_COUNT,
                    &hop_value,
                    &hop_value_len) == PROTO_OK);
    assert(hop_value_len == 1u);
    route_req.payload[(size_t)(hop_value - route_req.payload) - 1u] = 2u;

    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                70u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) == NULL);
}

static void test_duplicate_route_advertisement_field_does_not_mutate_route(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_outbound route_adv;
    struct mesh_relay_result result;
    size_t payload_len;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    60u);
    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A,
                    GATEWAY,
                    60u);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              1000u,
                                              1000u,
                                              &route_adv) == PROTO_OK);
    payload_len = route_adv.payload_len;
    assert(tlv_append_u8(route_adv.payload,
                         sizeof(route_adv.payload),
                         &payload_len,
                         TLV_HOP_COUNT,
                         7u) == PROTO_OK);
    route_adv.payload_len = (uint16_t)payload_len;
    route_adv.packet.payload_len = (uint16_t)payload_len;

    assert(mesh_relay_validate_gateway_route_adv(&anchor,
                                                 &route_adv.packet,
                                                 route_adv.payload,
                                                 route_adv.payload_len,
                                                 GATEWAY) != PROTO_OK);
    assert(mesh_relay_handle_rx(&anchor,
                                &route_adv.packet,
                                route_adv.payload,
                                route_adv.payload_len,
                                GATEWAY,
                                90u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
    assert(route_selected(&anchor.upstream) == NULL);
}

static void test_malformed_route_reply_does_not_poison_upstream_route(void)
{
    struct mesh_relay anchor;
    struct proto_packet route_reply = {
        .msg_type = MSG_ROUTE_REPLY,
        .flags = 0u,
        .src_id = GATEWAY,
        .dst_id = ANCHOR_A,
        .session_id = 1000u,
        .seq = 42u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_relay_result result;
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *quality_value = NULL;
    uint8_t quality_value_len = 0u;

    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 60u);
    assert(route_selected(&anchor.upstream) == NULL);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len, TLV_INITIATOR_ID, ANCHOR_A) ==
           PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len, TLV_RESPONDER_ID, GATEWAY) ==
           PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len, TLV_ROUTE_EPOCH, 60u) ==
           PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len, TLV_HOP_COUNT, 0u) ==
           PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len, TLV_QUALITY, 100u) ==
           PROTO_OK);
    route_reply.payload_len = (uint8_t)payload_len;

    assert(tlv_find(payload,
                    payload_len,
                    TLV_QUALITY,
                    &quality_value,
                    &quality_value_len) == PROTO_OK);
    assert(quality_value_len == 1u);
    payload[(size_t)(quality_value - payload) - 1u] = 2u;

    assert(mesh_relay_handle_rx(&anchor,
                                &route_reply,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                70u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    assert(!has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(route_selected(&anchor.upstream) == NULL);
}

static void test_reactive_route_and_report_flow_over_uwb_mesh_frames(void)
{
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct mesh_relay anchor;
    struct mesh_outbound route_req;
    struct mesh_outbound report_tx;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result gateway_result;
    struct mesh_relay_result anchor_result;
    struct proto_packet decoded_packet;
    struct proto_packet report;
    struct route_candidate route = direct_gateway_route(GATEWAY, 1u, 90u);
    uint8_t decoded_payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t decoded_payload_len = 0u;
    size_t payload_len;
    uint64_t previous_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 50u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           500u,
                                           1u,
                                           1200,
                                           payload,
                                           sizeof(payload));

    assert(mesh_relay_start_tx(&anchor, &report, payload, payload_len, 1000u, &report_tx) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_build_route_request(&anchor, GATEWAY, &route_req, 1000u) == PROTO_OK);

    decode_outbound_over_uwb(&route_req,
                             ANCHOR_A,
                             ANCHOR_B,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&relay,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                70u,
                                1010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));

    decode_outbound_over_uwb(&relay_result.route_reply,
                             ANCHOR_B,
                             ANCHOR_A,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&anchor,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                70u,
                                1040u,
                                &anchor_result) == PROTO_OK);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));

    assert(mesh_relay_start_tx(&anchor, &report, payload, payload_len, 1050u, &report_tx) ==
           PROTO_OK);
    assert(report_tx.next_hop_id == ANCHOR_B);
    decode_outbound_over_uwb(&report_tx,
                             ANCHOR_A,
                             ANCHOR_B,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&relay,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                70u,
                                1060u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == GATEWAY);

    decode_outbound_over_uwb(&relay_result.forward,
                             ANCHOR_B,
                             GATEWAY,
                             &decoded_packet,
                             decoded_payload,
                             sizeof(decoded_payload),
                             &decoded_payload_len,
                             &previous_hop_id);
    assert(mesh_relay_handle_rx(&gateway,
                                &decoded_packet,
                                decoded_payload,
                                decoded_payload_len,
                                previous_hop_id,
                                90u,
                                1070u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &decoded_packet,
                                              decoded_payload,
                                              decoded_payload_len,
                                              previous_hop_id,
                                              1071u,
                                              &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
}

static void test_channel9_guard_allows_one_upstream_and_one_downstream(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 90u, 90u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(2000u);
    struct mesh_relay_channel9_guard_status guard = {0};

    assert(MESH_RELAY_EVENT_TIMINGS >= 2u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 90u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 90u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_OK);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);
    assert(guard.active_peer_count == 0u);

    params = channel9_params(2050u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_OK);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert(guard.active_peer_count == 1u);
    assert(channel9_timing_count(&relay) == 2u);

    params = channel9_params(2100u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM);
    assert(channel9_timing_count(&relay) == 2u);
}

static void test_upstream_route_invalidation_preserves_downstream_connection(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 90u, 90u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(2000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 90u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 90u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  NULL) == PROTO_OK);
    params = channel9_params(2050u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  NULL) == PROTO_OK);

    mesh_relay_invalidate_upstream_route(&relay);

    assert(relay.upstream.selected_index == ROUTE_NO_SELECTION);
    assert(!relay.upstream.candidates[0].channel9_timing_valid);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) != NULL);
    assert(!channel9_timing_present(&relay, GATEWAY));
    assert(channel9_timing_present(&relay, ANCHOR_A));
    assert(channel9_timing_count(&relay) == 1u);
}

static void test_transit_abandon_preserves_upstream_connection(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 91u, 90u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(2000u);
    const struct route_candidate *selected;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 91u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 91u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  NULL) == PROTO_OK);
    params = channel9_params(2050u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  NULL) == PROTO_OK);

    mesh_relay_abandon_transit_reservations(&relay);

    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == GATEWAY);
    assert(selected->channel9_timing_valid);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) != NULL);
    assert(channel9_timing_present(&relay, GATEWAY));
    assert(!channel9_timing_present(&relay, ANCHOR_A));
    assert(channel9_timing_count(&relay) == 1u);
}

static void test_channel9_guard_rejects_second_peer_in_same_direction(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(3000u);
    struct mesh_relay_channel9_guard_status guard = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 91u);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 91u, 1u, 90u, 1000u);
    seed_downlink(&relay, ANCHOR_C, ANCHOR_C, 91u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);

    params = channel9_params(3200u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_C,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_ERR_BUSY);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_DIRECTION_BUSY);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert(guard.conflict_peer_id == ANCHOR_A);
    assert(guard.conflict_direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert(guard.active_peer_count == 1u);
    assert(channel9_timing_count(&relay) == 1u);
}

static void test_channel9_guard_rejects_third_peer(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 92u, 90u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(4000u);
    struct mesh_relay_channel9_guard_status guard = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 92u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 92u, 1u, 90u, 1000u);
    seed_downlink(&relay, ANCHOR_C, ANCHOR_C, 92u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);
    params = channel9_params(4050u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);

    params = channel9_params(4400u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_C,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_ERR_NO_SPACE);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_TOO_MANY_PEERS);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM);
    assert(guard.active_peer_count == 2u);
    assert(channel9_timing_count(&relay) == 2u);
}

static void test_channel9_guard_rejects_ambiguous_or_unknown_direction(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_A, 93u, 90u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(5000u);
    struct mesh_relay_channel9_guard_status guard = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 93u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_C, ANCHOR_A, 93u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_ERR_MALFORMED);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS);
    assert(channel9_timing_count(&relay) == 0u);

    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_ERR_MALFORMED);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER);
    assert(guard.direction == MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN);
    assert(channel9_timing_count(&relay) == 0u);
}

static void test_channel9_guard_rejects_overlapping_upstream_downstream_windows(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 94u, 90u);
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(6000u);
    struct mesh_relay_channel9_guard_status guard = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 94u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 94u, 1u, 90u, 1000u);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  GATEWAY,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_OK);

    params = channel9_params(6100u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded(&relay,
                                                  ANCHOR_A,
                                                  &timing,
                                                  2u,
                                                  &guard) == PROTO_ERR_BUSY);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT);
    assert(guard.conflict_peer_id == GATEWAY);
    assert(channel9_timing_count(&relay) == 1u);
}

static void test_channel9_guard_rejects_reverse_order_hardware_overlap(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing downstream = {0};
    struct mesh_event_timing upstream = {0};
    struct mesh_event_params params = {
        .event_interval_ms = MESH_RADIO_EVENT_INTERVAL_MS,
        .event_window_ms = MESH_RADIO_EVENT_WINDOW_MS,
        .first_event_time_ms = 105725u,
        .guard_ms = MESH_RADIO_EVENT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = MESH_RADIO_EVENT_MAX_MISSES,
        .supervision_timeout_ms = MESH_RADIO_EVENT_SUPERVISION_MS,
    };
    struct mesh_relay_channel9_guard_status guard = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 94u);
    assert(mesh_event_timing_negotiate(&downstream, &params, true) == PROTO_OK);
    assert(downstream.next_event_time_ms == 105725u);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &relay,
               ANCHOR_A,
               &downstream,
               MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
               2u,
               &guard) == PROTO_OK);

    /* This is the exact ordering that escaped on hardware: the proposed
     * upstream event starts 14 ms after an existing downstream event, while
     * each production event reserves 180 ms including guards. */
    params.first_event_time_ms = 105739u;
    assert(mesh_event_timing_negotiate(&upstream, &params, true) == PROTO_OK);
    assert(upstream.next_event_time_ms == 105739u);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &relay,
               GATEWAY,
               &upstream,
               MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
               2u,
               &guard) == PROTO_ERR_BUSY);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT);
    assert(guard.conflict_peer_id == ANCHOR_A);
    assert(channel9_timing_count(&relay) == 1u);
}

static void test_channel9_guard_rejects_nominal_touch_inside_physical_rx(void)
{
    enum {
        OLD_NOMINAL_RESERVATION_MS = 120u + (2u * 30u),
        REQUIRED_C5_OPPORTUNITY_MS = 20u,
    };
    struct mesh_relay relay;
    struct mesh_event_timing downstream = {0};
    struct mesh_event_timing upstream = {0};
    struct mesh_event_params params = {
        .event_interval_ms = MESH_RADIO_EVENT_INTERVAL_MS,
        .event_window_ms = MESH_RADIO_EVENT_WINDOW_MS,
        .first_event_time_ms = 106000u,
        .guard_ms = MESH_RADIO_EVENT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = MESH_RADIO_EVENT_MAX_MISSES,
        .supervision_timeout_ms = MESH_RADIO_EVENT_SUPERVISION_MS,
    };
    struct mesh_relay_channel9_guard_status guard = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 95u);
    assert(mesh_event_timing_negotiate(&downstream, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &relay,
               ANCHOR_A,
               &downstream,
               MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
               2u,
               &guard) == PROTO_OK);

    /* The old 120 + 2*30 reservation merely touched at this phase, so the
     * admission guard accepted it.  The real RX owner starts 50 ms early and
     * ends 60 ms late, leaving a 50 ms physical overlap instead. */
    params.first_event_time_ms =
        downstream.next_event_time_ms + OLD_NOMINAL_RESERVATION_MS;
    assert(mesh_event_timing_negotiate(&upstream, &params, true) == PROTO_OK);
    assert(mesh_relay_check_channel9_timing_guarded_direction(
               &relay,
               GATEWAY,
               &upstream,
               MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
               2u,
               &guard) == PROTO_ERR_BUSY);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_INTERVAL_CONFLICT);
    assert(guard.conflict_peer_id == ANCHOR_A);
    assert(channel9_timing_count(&relay) == 1u);

    /* A production half-phase must be admissible and still leave the
     * contract's Channel-5 opportunity outside both symmetric reservations. */
    assert(MESH_RADIO_EVENT_INTERVAL_MS % 2u == 0u);
    assert(MESH_RADIO_EVENT_GUARD_MS >= MESH_RADIO_EVENT_RETUNE_GUARD_MS);
    assert(MESH_RADIO_EVENT_GUARD_MS >= MESH_RADIO_EVENT_RX_LATE_GUARD_MS);
    assert(MESH_RADIO_EVENT_INTERVAL_MS / 2u >=
           MESH_RADIO_EVENT_WINDOW_MS +
               (2u * MESH_RADIO_EVENT_GUARD_MS) +
               MESH_RADIO_EVENT_RETUNE_GUARD_MS +
               REQUIRED_C5_OPPORTUNITY_MS);
    params.first_event_time_ms = downstream.next_event_time_ms +
                                 (MESH_RADIO_EVENT_INTERVAL_MS / 2u);
    assert(mesh_event_timing_negotiate(&upstream, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing_guarded_direction(
               &relay,
               GATEWAY,
               &upstream,
               MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
               2u,
               &guard) == PROTO_OK);
    assert(guard.reason == MESH_RELAY_CHANNEL9_GUARD_OK);
    assert(channel9_timing_count(&relay) == 2u);
}

static void test_channel9_report_tx_requires_negotiated_event(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 70u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(2000u);
    uint8_t payload[1] = {0x77u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 70u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    700u,
                                    1u,
                                    sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        2000u,
                                        &plan,
                                        &tx) == PROTO_ERR_STALE);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        1994u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        1995u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(tx.next_hop_id == GATEWAY);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    assert(mesh_relay_tx_active(&relay));
}

static void test_channel9_result_bundle_tx_requires_negotiated_event(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .collection_epoch_id = 721u,
        .bundle_id = 9u,
        .record_count = 1u,
    };
    struct result_bundle_record record = {0};
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 9u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_outbound tx;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(2200u);
    uint8_t result_payload[96];
    uint8_t records[160];
    uint8_t payload[256];
    size_t result_payload_len = 0u;
    size_t records_len = 0u;
    size_t payload_len = 0u;

    build_identity_command_result_payload(result_payload,
                                          sizeof(result_payload),
                                          64u,
                                          &result_id,
                                          &result_payload_len);
    record.result_id = result_id;
    record.payload_len = (uint16_t)result_payload_len;
    record.payload_crc = proto_crc16_ccitt_false(result_payload, result_payload_len);
    record.payload = result_payload;
    assert(result_bundle_record_append_tlv(records,
                                           sizeof(records),
                                           &records_len,
                                           &record) == PROTO_OK);
    bundle.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &bundle) == PROTO_OK);
    assert(sizeof(payload) - payload_len >= records_len);
    memcpy(&payload[payload_len], records, records_len);
    payload_len += records_len;
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &packet,
                                        payload,
                                        payload_len,
                                        &requirements,
                                        2200u,
                                        &plan,
                                        &tx) == PROTO_ERR_STALE);

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &packet,
                                        payload,
                                        payload_len,
                                        &requirements,
                                        2195u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(tx.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(tx.next_hop_id == GATEWAY);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    assert(tx.payload_len == payload_len);
}

static void test_relay_accepts_collection_result_into_bundle_queue(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
        .message_age_ms = 10u,
    };
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;

    build_collection_command_result_payload(payload,
                                            sizeof(payload),
                                            64u,
                                            &result_id,
                                            721u,
                                            &payload_len);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_result_bundle_pending(&relay));
    assert(mesh_relay_result_bundle_due_ms(&relay) == 1000u + MESH_RELAY_RESULT_BUNDLE_HOLD_MS);
    assert(result.hop_ack.packet.msg_type == MSG_MESH_HOP_ACK);
    assert(result.hop_ack.packet.dst_id == ANCHOR_B);
    assert(result.hop_ack.next_hop_id == ANCHOR_B);
}

static void test_relay_flushes_collection_bundle_when_queue_fills(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    struct command_result_id id_a = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet packet_a = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet packet_b = packet_a;
    struct mesh_relay_result result;
    struct result_bundle_header bundle = {0};
    struct result_bundle_record record = {0};
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    uint8_t header[64];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    size_t header_len = 0u;
    size_t cursor;

    id_b.node_id = ANCHOR_C;
    id_b.result_seq = 2u;
    packet_b.src_id = ANCHOR_C;
    packet_b.seq = 2u;
    build_collection_command_result_payload(payload_a,
                                            sizeof(payload_a),
                                            64u,
                                            &id_a,
                                            721u,
                                            &payload_a_len);
    build_collection_command_result_payload(payload_b,
                                            sizeof(payload_b),
                                            64u,
                                            &id_b,
                                            721u,
                                            &payload_b_len);
    packet_a.payload_len = (uint16_t)payload_a_len;
    packet_b.payload_len = (uint16_t)payload_b_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_a,
                                payload_a,
                                payload_a_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_result_bundle_pending(&relay));

    assert(mesh_relay_handle_rx(&relay,
                                &packet_b,
                                payload_b,
                                payload_b_len,
                                ANCHOR_C,
                                90u,
                                1001u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(mesh_relay_result_bundle_pending(&relay));
    assert(result.forward.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(result.forward.packet.flags == FLAG_GATEWAY_ACK_REQUIRED);
    assert(result.forward.packet.src_id == ANCHOR_A);
    assert(result.forward.packet.dst_id == GATEWAY);
    assert(result.forward.packet.session_id == 720u);
    assert(result.forward.next_hop_id == GATEWAY);
    assert(result_bundle_header_from_tlvs(result.forward.payload,
                                          result.forward.payload_len,
                                          &bundle) == PROTO_OK);
    assert(bundle.gateway_id == GATEWAY);
    assert(bundle.gateway_epoch == 72u);
    assert(bundle.command_seq == 720u);
    assert(bundle.collection_epoch_id == 721u);
    assert(bundle.record_count == MESH_RELAY_RESULT_BUNDLE_RECORDS);
    assert(result_bundle_header_append_tlvs(header,
                                            sizeof(header),
                                            &header_len,
                                            &bundle) == PROTO_OK);
    cursor = header_len;
    assert(result_bundle_record_next_from_tlvs(result.forward.payload,
                                               result.forward.payload_len,
                                               &cursor,
                                               &record) == PROTO_OK);
    assert_command_result_id_equal(&record.result_id, &id_a);
    assert(record.payload_len == payload_a_len);
    assert(record.payload_crc == proto_crc16_ccitt_false(payload_a, payload_a_len));
    assert(result_bundle_record_next_from_tlvs(result.forward.payload,
                                               result.forward.payload_len,
                                               &cursor,
                                               &record) == PROTO_OK);
    assert_command_result_id_equal(&record.result_id, &id_b);
    assert(record.payload_len == payload_b_len);
    assert(record.payload_crc == proto_crc16_ccitt_false(payload_b, payload_b_len));
    assert(cursor == result.forward.payload_len);
    mesh_relay_result_bundle_note_forwarded(&relay, &result.forward);
    assert(!mesh_relay_result_bundle_pending(&relay));
}

static void test_relay_flushes_collection_bundle_after_hold_deadline(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_relay_result result;
    struct result_bundle_header bundle = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    build_collection_command_result_payload(payload,
                                            sizeof(payload),
                                            64u,
                                            &result_id,
                                            721u,
                                            &payload_len);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_result_bundle_pending(&relay));
    assert(mesh_relay_tick(&relay,
                           1000u + MESH_RELAY_RESULT_BUNDLE_HOLD_MS - 1u,
                           &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_result_bundle_pending(&relay));

    assert(mesh_relay_tick(&relay,
                           1000u + MESH_RELAY_RESULT_BUNDLE_HOLD_MS,
                           &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_result_bundle_pending(&relay));
    assert(result.forward.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(result.forward.packet.message_age_ms ==
           MESH_RELAY_RESULT_BUNDLE_HOLD_MS);
    assert(result_bundle_header_from_tlvs(result.forward.payload,
                                          result.forward.payload_len,
                                          &bundle) == PROTO_OK);
    assert(bundle.record_count == 1u);
    mesh_relay_result_bundle_note_forwarded(&relay, &result.forward);
    assert(!mesh_relay_result_bundle_pending(&relay));
}

static void test_child_custody_bundle_snapshot_restores_after_reinit(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_child_custody_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
        .message_age_ms = 10u,
    };
    struct mesh_relay_result result;
    struct result_bundle_header bundle = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    build_collection_command_result_payload(payload,
                                            sizeof(payload),
                                            64u,
                                            &result_id,
                                            721u,
                                            &payload_len);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_export_child_custody_snapshot(&relay,
                                                    1010u,
                                                    &snapshot) == PROTO_OK);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     2000u) ==
           PROTO_ERR_MALFORMED);
    assert(!mesh_relay_result_bundle_pending(&restored));
    assert(mesh_relay_seed_route_freshness(
               &restored, 72u, 0u) == PROTO_OK);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     2000u) == PROTO_OK);
    assert(mesh_relay_result_bundle_pending(&restored));
    assert(mesh_relay_result_bundle_due_ms(&restored) == 2015u);

    assert(mesh_relay_tick(&restored, 2014u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tick(&restored, 2015u, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(result.forward.packet.message_age_ms == 35u);
    assert(result_bundle_header_from_tlvs(result.forward.payload,
                                          result.forward.payload_len,
                                          &bundle) == PROTO_OK);
    assert(bundle.record_count == 1u);
    mesh_relay_result_bundle_note_forwarded(&restored, &result.forward);
    assert(!mesh_relay_result_bundle_pending(&restored));
}

static void test_child_custody_snapshot_rejects_corrupt_bundle_payload(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_child_custody_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;

    build_collection_command_result_payload(payload,
                                            sizeof(payload),
                                            64u,
                                            &result_id,
                                            721u,
                                            &payload_len);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_export_child_custody_snapshot(&relay,
                                                    1001u,
                                                    &snapshot) == PROTO_OK);
    snapshot.result_bundle.records[0].payload[0] ^= 0x01u;

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     2000u) ==
           PROTO_ERR_MALFORMED);
    assert(!mesh_relay_result_bundle_pending(&restored));
}

static void test_crc16_collision_cannot_alias_restored_result_bundle_entry(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_child_custody_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x22334456),
        .seq = 26u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct mesh_relay_result result;
    uint8_t payload_a[64];
    uint8_t payload_b[64];
    size_t payload_len = 0u;

    build_deliberate_crc16_collision_results(payload_a,
                                              payload_b,
                                              sizeof(payload_a),
                                              true,
                                              &payload_len);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload_a,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                8000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(mesh_relay_export_child_custody_snapshot(&relay,
                                                    8001u,
                                                    &snapshot) == PROTO_OK);

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    3u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     9000u) == PROTO_OK);

    assert(mesh_relay_handle_rx(&restored,
                                &packet,
                                payload_b,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                9010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(restored.result_bundle.record_count == 1u);
    assert(memcmp(restored.result_bundle.records[0].payload,
                  payload_a,
                  payload_len) == 0);

    assert(mesh_relay_handle_rx(&restored,
                                &packet,
                                payload_a,
                                payload_len,
                                ANCHOR_A,
                                90u,
                                9020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_CUSTODY_ACCEPTED));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(restored.result_bundle.record_count == 1u);
}

static void test_child_custody_reservation_snapshot_restores_after_reinit(void)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = GATEWAY,
            .gateway_epoch = 3u,
            .command_seq = 0x22334455u,
            .node_id = ANCHOR_A,
            .node_boot_counter = 21u,
            .result_seq = 22u,
        },
        .result_len = UWB_MESH_MAX_PAYLOAD_LEN,
        .result_crc = 0x789au,
        .priority = 4u,
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_child_custody_snapshot snapshot;
    struct proto_packet packet = {
        .msg_type = MSG_RESULT_OFFER,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = UINT32_C(0x22334455),
        .seq = 22u,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint32_t expiry_ms =
        MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u;
    const uint32_t restored_at_ms = UINT32_MAX - expiry_ms + 1u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                payload_len,
                                ANCHOR_A,
                                80u,
                                4300u,
                                &result) == PROTO_OK);
    assert(relay.result_offer_reservation.valid);
    assert(mesh_relay_export_child_custody_snapshot(&relay,
                                                    4301u,
                                                    &snapshot) == PROTO_OK);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     restored_at_ms) ==
           PROTO_ERR_MALFORMED);
    assert(!restored.result_offer_reservation.valid);
    assert(mesh_relay_seed_route_freshness(
               &restored, 3u, 0u) == PROTO_OK);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     restored_at_ms) == PROTO_OK);
    assert(restored.result_offer_reservation.valid);
    assert(restored.result_offer_reservation.child_id == ANCHOR_A);
    assert_command_result_id_equal(&restored.result_offer_reservation.result_id,
                                   &offer.result_id);
    assert(restored.result_offer_reservation.result_len == offer.result_len);
    assert(semantic_digest_equal(
        restored.result_offer_reservation.result_digest,
        offer.result_digest,
        sizeof(offer.result_digest)));
    assert(restored.result_offer_reservation_deadline_ms == 0u);
    assert(mesh_relay_tick(&restored,
                           UINT32_MAX,
                           &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED));
    assert(restored.result_offer_reservation.valid);
    assert_command_result_id_equal(&restored.result_offer_reservation.result_id,
                                   &offer.result_id);
    assert(mesh_relay_tick(&restored,
                           0u,
                           &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED));
    assert(!restored.result_offer_reservation.valid);
}

static void test_child_custody_snapshot_export_reports_no_state(void)
{
    struct mesh_relay relay;
    struct mesh_relay_child_custody_snapshot snapshot;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(mesh_relay_export_child_custody_snapshot(&relay,
                                                    1000u,
                                                    &snapshot) ==
           PROTO_ERR_NOT_FOUND);
    assert(!snapshot.valid);
}

static void test_result_bundle_outbox_snapshot_restores_after_forward_handoff(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    struct command_result_id id_a = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet packet_a = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet packet_b = packet_a;
    struct mesh_relay_result result;
    struct mesh_outbound tx;
    struct result_bundle_header bundle = {0};
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;

    id_b.node_id = ANCHOR_C;
    id_b.result_seq = 2u;
    packet_b.src_id = ANCHOR_C;
    packet_b.seq = 2u;
    build_collection_command_result_payload(payload_a,
                                            sizeof(payload_a),
                                            64u,
                                            &id_a,
                                            721u,
                                            &payload_a_len);
    build_collection_command_result_payload(payload_b,
                                            sizeof(payload_b),
                                            64u,
                                            &id_b,
                                            721u,
                                            &payload_b_len);
    packet_a.payload_len = (uint16_t)payload_a_len;
    packet_b.payload_len = (uint16_t)payload_b_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_a,
                                payload_a,
                                payload_a_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_b,
                                payload_b,
                                payload_b_len,
                                ANCHOR_C,
                                90u,
                                1001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.packet.msg_type == MSG_RESULT_BUNDLE);
    mesh_relay_result_bundle_note_forwarded(&relay, &result.forward);
    assert(!mesh_relay_result_bundle_pending(&relay));

    assert(mesh_relay_start_tx(&relay,
                               &result.forward.packet,
                               result.forward.payload,
                               result.forward.payload_len,
                               1010u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, 1010u);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             1020u,
                                             &snapshot) == PROTO_OK);
    assert(snapshot.valid);
    assert(snapshot.pending.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(snapshot.record.packet_class == MSG_RESULT_BUNDLE);
    assert(snapshot.record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              2000u) == PROTO_OK);
    assert(mesh_relay_tx_active(&restored));
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(restored.pending.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(restored.pending.packet.message_age_ms == snapshot.record.age_ms_saturating);
    assert(restored.pending.payload_len == result.forward.payload_len);
    assert(memcmp(restored.pending.payload,
                  result.forward.payload,
                  result.forward.payload_len) == 0);
    assert(restored.outbox_record.valid);
    assert(restored.outbox_record.packet_class == MSG_RESULT_BUNDLE);
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(result_bundle_header_from_tlvs(restored.pending.payload,
                                          restored.pending.payload_len,
                                          &bundle) == PROTO_OK);
    assert(bundle.record_count == MESH_RELAY_RESULT_BUNDLE_RECORDS);
}

static void test_result_bundle_gateway_ack_timeout_preserves_outbox_for_route_recovery(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    struct command_result_id id_a = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet packet_a = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet packet_b = packet_a;
    struct mesh_relay_result result;
    struct mesh_outbound tx;
    uint32_t timeout_ms;
    uint32_t retry_ms;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;

    id_b.node_id = ANCHOR_C;
    id_b.result_seq = 2u;
    packet_b.src_id = ANCHOR_C;
    packet_b.seq = 2u;
    build_collection_command_result_payload(payload_a,
                                            sizeof(payload_a),
                                            64u,
                                            &id_a,
                                            721u,
                                            &payload_a_len);
    build_collection_command_result_payload(payload_b,
                                            sizeof(payload_b),
                                            64u,
                                            &id_b,
                                            721u,
                                            &payload_b_len);
    packet_a.payload_len = (uint16_t)payload_a_len;
    packet_b.payload_len = (uint16_t)payload_b_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_a,
                                payload_a,
                                payload_a_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_b,
                                payload_b,
                                payload_b_len,
                                ANCHOR_C,
                                90u,
                                1001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(result.forward.packet.msg_type == MSG_RESULT_BUNDLE);
    mesh_relay_result_bundle_note_forwarded(&relay, &result.forward);

    assert(mesh_relay_start_tx(&relay,
                               &result.forward.packet,
                               result.forward.payload,
                               result.forward.payload_len,
                               1010u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, 1010u);
    relay.upstream.candidates[relay.upstream.selected_index].failure_count =
        ROUTE_MAX_FAILURES - 1u;

    timeout_ms = relay.pending.gateway_ack_deadline_ms + 1u;
    assert(mesh_relay_tick(&relay, timeout_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(result.status == PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_WAIT_COLLECTION_EACK);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             timeout_ms + 1u,
                                             &snapshot) == PROTO_OK);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&restored.upstream, &route) == PROTO_OK);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              2000u) == PROTO_OK);
    assert(restored.pending.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(restored.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);

    retry_ms = restored.pending.retry_after_ms;
    assert(mesh_relay_tick(&restored, retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.packet.msg_type == MSG_RESULT_BUNDLE);
    assert(result.retransmit.next_hop_id == GATEWAY);
    assert(result.retransmit.payload_len == tx.payload_len);
    assert(memcmp(result.retransmit.payload, tx.payload, tx.payload_len) == 0);
}

static void test_result_bundle_outbox_snapshot_rejects_corrupt_payload(void)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay_outbox_snapshot snapshot;
    struct route_candidate route = direct_gateway_route(GATEWAY, 72u, 90u);
    struct command_result_id id_a = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 72u,
        .command_seq = 720u,
        .node_id = ANCHOR_B,
        .node_boot_counter = 4u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet packet_a = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 720u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet packet_b = packet_a;
    struct mesh_relay_result result;
    struct mesh_outbound tx;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;

    id_b.node_id = ANCHOR_C;
    id_b.result_seq = 2u;
    packet_b.src_id = ANCHOR_C;
    packet_b.seq = 2u;
    build_collection_command_result_payload(payload_a,
                                            sizeof(payload_a),
                                            64u,
                                            &id_a,
                                            721u,
                                            &payload_a_len);
    build_collection_command_result_payload(payload_b,
                                            sizeof(payload_b),
                                            64u,
                                            &id_b,
                                            721u,
                                            &payload_b_len);
    packet_a.payload_len = (uint16_t)payload_a_len;
    packet_b.payload_len = (uint16_t)payload_b_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_a,
                                payload_a,
                                payload_a_len,
                                ANCHOR_B,
                                90u,
                                1000u,
                                &result) == PROTO_OK);
    assert(mesh_relay_handle_rx(&relay,
                                &packet_b,
                                payload_b,
                                payload_b_len,
                                ANCHOR_C,
                                90u,
                                1001u,
                                &result) == PROTO_OK);
    mesh_relay_result_bundle_note_forwarded(&relay, &result.forward);
    assert(mesh_relay_start_tx(&relay,
                               &result.forward.packet,
                               result.forward.payload,
                               result.forward.payload_len,
                               1010u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&relay, &tx, 1010u);
    assert(mesh_relay_export_outbox_snapshot(&relay,
                                             1020u,
                                             &snapshot) == PROTO_OK);
    snapshot.pending.payload[snapshot.pending.payload_len - 1u] ^= 0x01u;

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(mesh_relay_restore_outbox_snapshot(&restored,
                                              &snapshot,
                                              2000u) == PROTO_ERR_MALFORMED);
    assert(!mesh_relay_tx_active(&restored));
}

static void test_channel9_report_delivery_and_gateway_ack_require_events(void)
{
    struct mesh_relay origin;
    struct mesh_relay gateway;
    struct route_candidate route = direct_gateway_route(GATEWAY, 71u, 90u);
    struct proto_packet report;
    struct proto_packet confirm_ack;
    struct mesh_outbound tx;
    struct mesh_outbound confirm;
    struct mesh_relay_result gateway_result;
    struct mesh_relay_result origin_result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(3000u);
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t confirm_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t payload_len;
    size_t confirm_ack_payload_len = 0u;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 71u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 71u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&origin, GATEWAY, &timing) == PROTO_OK);

    payload_len = build_valid_click_report(&report,
                                           ANCHOR_A,
                                           710u,
                                           2u,
                                           1200,
                                           payload,
                                           sizeof(payload));
    assert(mesh_relay_start_channel9_tx(&origin,
                                        &report,
                                        payload,
                                        payload_len,
                                        &requirements,
                                        3000u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    mesh_relay_note_channel9_unobserved_turn(
        &origin, tx.next_hop_id, plan.start_ms);
    mesh_relay_note_tx_sent(&origin, &tx, 3000u);

    assert(mesh_relay_handle_rx(&gateway,
                                &tx.packet,
                                tx.payload,
                                tx.payload_len,
                                ANCHOR_A,
                                90u,
                                3010u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &tx.packet,
                                              tx.payload,
                                              tx.payload_len,
                                              ANCHOR_A,
                                              3011u,
                                              &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_A);

    assert(mesh_relay_require_channel9_event(&gateway,
                                             gateway_result.gateway_ack.next_hop_id,
                                             &requirements,
                                             3010u,
                                             &plan) == PROTO_ERR_STALE);
    params = channel9_params(3010u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&gateway, ANCHOR_A, &timing) == PROTO_OK);
    assert(mesh_relay_require_channel9_event(&gateway,
                                             gateway_result.gateway_ack.next_hop_id,
                                             &requirements,
                                             3010u,
                                             &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);

    assert(mesh_relay_handle_rx(&origin,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                3020u,
                                &origin_result) == PROTO_OK);
    assert(origin_result.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.packet.msg_type == report.msg_type);
    assert(origin.pending.gateway_ack_confirm_pending);

    assert(mesh_relay_tick(&origin, 3021u, &origin_result) == PROTO_OK);
    assert(origin_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
    assert(origin_result.retransmit.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
    confirm = origin_result.retransmit;
    mesh_relay_note_tx_sent(&origin, &confirm, 3021u);

    build_gateway_ack_for_packet(&confirm_ack,
                                 confirm_ack_payload,
                                 sizeof(confirm_ack_payload),
                                 &confirm_ack_payload_len,
                                 ANCHOR_A,
                                 3u,
                                 &confirm.packet,
                                 confirm.payload,
                                 confirm.payload_len);
    assert(mesh_relay_handle_rx(&origin,
                                &confirm_ack,
                                confirm_ack_payload,
                                confirm_ack_payload_len,
                                GATEWAY,
                                90u,
                                3030u,
                                &origin_result) == PROTO_OK);
    assert(origin_result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
    assert(mesh_relay_commit_gateway_ack_confirm_terminal(
               &origin,
               &confirm.packet,
               confirm.payload,
               confirm.payload_len,
               3031u) == PROTO_OK);
    assert(!mesh_relay_tx_active(&origin));
}

static void test_channel9_payload_event_closes_while_outbox_waits_gateway_ack(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 72u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(3000u);
    uint8_t report_payload[1] = {0x79u};
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t report_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t ack_payload_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&origin, ANCHOR_B, &timing) == PROTO_OK);

    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    720u,
                                    2u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_channel9_tx(&origin,
                                        &report,
                                        report_payload,
                                        sizeof(report_payload),
                                        &requirements,
                                        3000u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    assert(tx.next_hop_id == ANCHOR_B);

    mesh_relay_note_channel9_unobserved_turn(
        &origin, tx.next_hop_id, plan.start_ms);
    mesh_relay_note_tx_sent(&origin, &tx, 3000u);

    assert(origin.event_timings[0].valid);
    assert(origin.event_timings[0].timing.event_counter == 1u);
    assert(origin.event_timings[0].timing.next_event_time_ms == 3100u);
    assert(!mesh_event_timing_local_tx_slot(&origin.event_timings[0].timing));
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(origin.pending.radio_channel == MESH_EVENT_CHANNEL);
    assert(origin.outbox_record.valid);
    assert(origin.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(!origin.outbox_record.gateway_acked);

    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             77u,
                             ack_payload,
                             sizeof(ack_payload),
                             &ack_payload_len,
                             &report,
                             report_payload,
                             sizeof(report_payload));

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                3010u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(origin.outbox_record.valid);
    assert(origin.event_timings[0].timing.event_counter == 1u);
    assert(origin.event_timings[0].timing.next_event_time_ms == 3100u);
    assert(mesh_packet_semantic_digest(&report,
                                       report_payload,
                                       sizeof(report_payload),
                                       report_digest));
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &origin, &report, report_digest) == PROTO_OK);
    assert(!mesh_relay_tx_active(&origin));
    assert(!origin.outbox_record.valid);
}

static void test_channel9_rx_observation_keeps_negotiated_event_timing(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(4000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 80u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);

    mesh_relay_note_channel9_rx(&relay, GATEWAY, 4000u, 4018u);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             4108u,
                                             &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
    assert(plan.start_ms == 4100u);
}

static void test_channel9_sender_skips_channel5_preempted_event_without_refresh(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 82u, 90u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(6000u);
    uint8_t payload[1] = {0x79u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 82u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    720u,
                                    1u,
                                    sizeof(payload)) == PROTO_OK);

    requirements.next_required_scan_start_ms = 6003u;
    requirements.next_required_scan_start_valid = true;
    requirements.retune_guard_ms = 5u;
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6000u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);
    assert(relay.event_timings[0].timing.next_event_time_ms == 6100u);

    requirements = clear_channel5_requirements();
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6050u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);
    assert(plan.start_ms == 6200u);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6100u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_WAIT);
    assert(plan.start_ms == 6200u);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);

    mesh_relay_note_channel9_missed(&relay, GATEWAY, NULL);
    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6200u,
                                        &plan,
                                        &tx) == PROTO_ERR_STALE);
    assert(!relay.event_timings[0].valid);
    assert(!relay.event_timings[0].timing.timing_fresh);
    assert(relay.event_timings[0].timing.fallback_required);
}

static void test_channel9_receiver_miss_advances_timing_and_diagnostics(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_diagnostics diagnostics = {0};
    struct mesh_event_params params = channel9_params(7000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 83u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, ANCHOR_A, &timing) == PROTO_OK);

    mesh_relay_note_channel9_missed(&relay, ANCHOR_A, &diagnostics);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);
    assert(relay.event_timings[0].timing.next_event_time_ms == 7100u);
    assert(diagnostics.ch9_event_misses == 1u);
}

static void test_channel9_timing_expires_idle_connection_state(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(5000u);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 81u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);

    assert(mesh_relay_expire_channel9_timings(&relay, 5200u) == 0u);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             5200u,
                                             &plan) == PROTO_OK);
    assert(mesh_relay_expire_channel9_timings(&relay, 5500u) == 1u);
    assert(mesh_relay_require_channel9_event(&relay,
                                             GATEWAY,
                                             &requirements,
                                             5500u,
                                             &plan) == PROTO_ERR_STALE);
}

static void test_mesh_hop_ack_is_protocol_valid(void)
{
    struct proto_packet acknowledged = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_B,
        .dst_id = GATEWAY,
        .session_id = 901u,
        .seq = 13u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 1u,
    };
    struct proto_packet packet = {
        .msg_type = MSG_MESH_HOP_ACK,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = 901u,
        .seq = 14u,
        .ttl = MESH_GATEWAY_ACK_TTL,
    };
    const uint8_t acknowledged_payload[1] = {0xa5u};
    uint8_t payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t encoded[PACKET_MAX_LEN];
    size_t payload_len = 0u;
    size_t written = 0u;

    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     acknowledged.seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(payload,
                                             sizeof(payload),
                                             &payload_len,
                                             &acknowledged,
                                             acknowledged_payload,
                                             sizeof(acknowledged_payload)) ==
           PROTO_OK);
    packet.payload_len = (uint8_t)payload_len;

    assert(proto_packet_encode(&packet,
                               payload,
                               encoded,
                               sizeof(encoded),
                               &written) == PROTO_OK);
    assert(written == proto_packet_encoded_len(packet.payload_len));
}

static void test_click_participants_choose_best_clean_cached_route(void)
{
    const uint64_t path_b[] = {GATEWAY, ANCHOR_B, ANCHOR_A};
    const uint64_t path_c[] = {GATEWAY, ANCHOR_C, ANCHOR_A};
    const uint64_t path_d[] = {GATEWAY, ANCHOR_D, ANCHOR_A};
    const uint64_t participants[] = {ANCHOR_A, ANCHOR_B};
    struct mesh_relay relay;
    struct route_table upstream_before;
    struct mesh_route_discovery_state discovery_before;
    struct mesh_anchor_downlink_store ancestry_before;
    struct proto_packet report;
    uint8_t payload[192];
    size_t payload_len;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A, GATEWAY, 81u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_B, 81u, 1u, 99u,
                                          1000u, path_b, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_C, 81u, 1u, 95u,
                                          1000u, path_c, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_D, 81u, 1u, 90u,
                                          1000u, path_d, 3u);
    assert(route_selected(&relay.upstream)->next_hop_id == ANCHOR_B);
    payload_len = build_click_report_with_participants(
        &report, ANCHOR_A, 81u, 1u, 1200,
        participants, 2u, payload, sizeof(payload));

    upstream_before = relay.upstream;
    discovery_before = relay.route_discovery;
    ancestry_before = *relay.anchor_downlink_store;
    assert(mesh_relay_select_next_hop_for_packet(&relay,
                                                  &report,
                                                  payload,
                                                  payload_len,
                                                  1100u,
                                                  &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_C);
    assert(memcmp(&relay.upstream, &upstream_before,
                  sizeof(upstream_before)) == 0);
    assert(memcmp(&relay.route_discovery, &discovery_before,
                  sizeof(discovery_before)) == 0);
    assert(memcmp(relay.anchor_downlink_store, &ancestry_before,
                  sizeof(ancestry_before)) == 0);
}

static void test_click_participants_avoid_interior_path_and_unknown_ancestry(void)
{
    const uint64_t path_via_participant[] = {
        GATEWAY, ANCHOR_B, ANCHOR_C, ANCHOR_A,
    };
    const uint64_t path_d[] = {
        GATEWAY, UINT64_C(0x777788889999aaaa), ANCHOR_D, ANCHOR_A,
    };
    const uint64_t path_c[] = {GATEWAY, ANCHOR_C, ANCHOR_A};
    const uint64_t participants_b[] = {ANCHOR_A, ANCHOR_B};
    const uint64_t participants_d[] = {ANCHOR_A, ANCHOR_D};
    struct route_candidate missing_ancestry = {
        .next_hop_id = ANCHOR_B,
        .gateway_id = GATEWAY,
        .route_epoch = 82u,
        .last_seen_ms = 1000u,
        .hop_count = 1u,
        .link_quality = 99u,
        .valid = true,
    };
    struct mesh_relay relay;
    struct proto_packet report;
    uint8_t payload[192];
    size_t payload_len;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A, GATEWAY, 82u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_C, 82u, 2u, 100u,
                                          1000u,
                                          path_via_participant, 4u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_D, 82u, 2u, 90u,
                                          1000u, path_d, 4u);
    payload_len = build_click_report_with_participants(
        &report, ANCHOR_A, 82u, 1u, 1200,
        participants_b, 2u, payload, sizeof(payload));
    assert(mesh_relay_select_next_hop_for_packet(&relay,
                                                  &report,
                                                  payload,
                                                  payload_len,
                                                  1100u,
                                                  &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_D);

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A, GATEWAY, 82u);
    assert(route_upsert_candidate(&relay.upstream, &missing_ancestry) ==
           PROTO_OK);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_C, 82u, 1u, 90u,
                                          1000u, path_c, 3u);
    assert(route_selected(&relay.upstream)->next_hop_id == ANCHOR_B);
    payload_len = build_click_report_with_participants(
        &report, ANCHOR_A, 83u, 1u, 1200,
        participants_d, 2u, payload, sizeof(payload));
    assert(mesh_relay_select_next_hop_for_packet(&relay,
                                                  &report,
                                                  payload,
                                                  payload_len,
                                                  1100u,
                                                  &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_C);
}

static void test_click_route_ignores_local_and_gateway_path_endpoints(void)
{
    const uint64_t path_b[] = {GATEWAY, ANCHOR_B, ANCHOR_A};
    const uint64_t path_c[] = {GATEWAY, ANCHOR_C, ANCHOR_A};
    const uint64_t participants[] = {ANCHOR_A, GATEWAY};
    struct mesh_relay relay;
    struct proto_packet report;
    uint8_t payload[192];
    size_t payload_len;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A, GATEWAY, 83u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_B, 83u, 1u, 99u,
                                          1000u, path_b, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_C, 83u, 1u, 90u,
                                          1000u, path_c, 3u);
    payload_len = build_click_report_with_participants(
        &report, ANCHOR_A, 84u, 1u, 1200,
        participants, 2u, payload, sizeof(payload));
    assert(mesh_relay_select_next_hop_for_packet(&relay,
                                                  &report,
                                                  payload,
                                                  payload_len,
                                                  1100u,
                                                  &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
    assert(route_selected(&relay.upstream)->next_hop_id == ANCHOR_B);
}

static void test_click_route_falls_back_immediately_without_route_mutation(void)
{
    const uint64_t path_b[] = {GATEWAY, ANCHOR_B, ANCHOR_A};
    const uint64_t path_c[] = {GATEWAY, ANCHOR_C, ANCHOR_A};
    const uint64_t path_d[] = {GATEWAY, ANCHOR_D, ANCHOR_A};
    const uint64_t participants[] = {
        ANCHOR_A, ANCHOR_C, ANCHOR_B, ANCHOR_D,
    };
    struct mesh_relay relay;
    struct mesh_relay before;
    struct mesh_anchor_downlink_store ancestry_before;
    struct proto_packet report;
    uint8_t payload[192];
    size_t payload_len;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A, GATEWAY, 84u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_B, 84u, 1u, 99u,
                                          1000u, path_b, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_C, 84u, 1u, 95u,
                                          1000u, path_c, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_D, 84u, 1u, 90u,
                                          1000u, path_d, 3u);
    payload_len = build_click_report_with_participants(
        &report, ANCHOR_A, 85u, 1u, 1200,
        participants, 4u, payload, sizeof(payload));

    before = relay;
    ancestry_before = *relay.anchor_downlink_store;
    assert(mesh_relay_select_next_hop_for_packet(&relay,
                                                  &report,
                                                  payload,
                                                  payload_len,
                                                  1100u,
                                                  &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
    assert(memcmp(&relay, &before, sizeof(before)) == 0);
    assert(memcmp(relay.anchor_downlink_store, &ancestry_before,
                  sizeof(ancestry_before)) == 0);
    assert(!relay.route_discovery.active);
}

static void test_click_route_freezes_clean_next_hop_through_retry(void)
{
    const uint64_t path_b[] = {GATEWAY, ANCHOR_B, ANCHOR_A};
    const uint64_t path_c[] = {GATEWAY, ANCHOR_C, ANCHOR_A};
    const uint64_t path_d[] = {GATEWAY, ANCHOR_D, ANCHOR_A};
    const uint64_t participants[] = {ANCHOR_A, ANCHOR_B};
    struct mesh_relay relay;
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[192];
    size_t payload_len;
    uint32_t timeout_ms;
    uint32_t retry_ms;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_A, GATEWAY, 85u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_B, 85u, 1u, 99u,
                                          1000u, path_b, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_C, 85u, 1u, 95u,
                                          1000u, path_c, 3u);
    seed_upstream_candidate_with_ancestry(&relay, ANCHOR_D, 85u, 1u, 90u,
                                          1000u, path_d, 3u);
    payload_len = build_click_report_with_participants(
        &report, ANCHOR_A, 86u, 1u, 1200,
        participants, 2u, payload, sizeof(payload));
    assert(mesh_relay_start_tx(&relay, &report, payload, payload_len,
                               1100u, &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_C);
    assert(relay.pending.next_hop_id == ANCHOR_C);
    mesh_relay_note_tx_sent(&relay, &tx, 1100u);

    relay.upstream.selected_index =
        route_candidate_index_for_next_hop(&relay, ANCHOR_D);
    assert(route_selected(&relay.upstream)->next_hop_id == ANCHOR_D);
    timeout_ms = relay.pending.gateway_ack_deadline_ms + 1u;
    assert(mesh_relay_tick(&relay, timeout_ms, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(relay.pending.next_hop_id == ANCHOR_C);
    retry_ms = relay.pending.retry_after_ms;
    assert(retry_ms != 0u);
    assert(mesh_relay_tick(&relay, retry_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(result.retransmit.next_hop_id == ANCHOR_C);
    assert(relay.pending.next_hop_id == ANCHOR_C);
    assert(!relay.route_discovery.active);
}

static void test_direct_gateway_route_probe_marks_route_ready(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_prepare_route_request(&relay,
                                            GATEWAY,
                                            1000u,
                                            0x12345678u,
                                            &route_req) == PROTO_OK);
    assert(relay.route_discovery.active);
    assert(mesh_relay_note_direct_gateway_route(&relay, 1020u) == PROTO_OK);
    assert(!relay.route_discovery.active);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == GATEWAY);
}

static void test_direct_gateway_route_probe_clears_hold_down(void)
{
    struct mesh_relay relay;
    const struct route_candidate *selected;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(mesh_relay_note_direct_gateway_route(&relay, 1000u) == PROTO_OK);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == GATEWAY);

    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2000u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2100u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2200u) ==
           ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure_at(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK, 2300u) ==
           ROUTE_DELIVERY_DISCOVER);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_ERR_NOT_FOUND);

    assert(mesh_relay_note_direct_gateway_route(&relay, 2400u) == PROTO_OK);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == GATEWAY);
    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->failure_count == 0u);
    assert(selected->hold_down_until_ms == 0u);
    assert(selected->last_success_ms == 2400u);
}

static void test_remove_direct_gateway_route_selects_relay_candidate(void)
{
    struct mesh_relay relay;
    struct route_candidate relayed = {
        .next_hop_id = ANCHOR_B,
        .gateway_id = GATEWAY,
        .route_epoch = 7u,
        .last_seen_ms = 1010u,
        .hop_count = 1u,
        .link_quality = 95u,
        .valid = true,
    };
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(mesh_relay_note_direct_gateway_route(&relay, 1000u) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &relayed) == PROTO_OK);
    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == GATEWAY);

    mesh_relay_remove_direct_gateway_route(&relay);

    assert(mesh_relay_select_next_hop(&relay, GATEWAY, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
}

static void test_gateway_control_reverse_route_preserves_parent_quarantine(void)
{
    struct mesh_relay relay;
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = ANCHOR_A,
        .session_id = 84u,
        .seq = 84u,
        .ttl = MESH_DEFAULT_TTL - 1u,
    };
    const struct route_candidate alternate = {
        .next_hop_id = ANCHOR_C,
        .gateway_id = GATEWAY,
        .route_epoch = 7u,
        .last_seen_ms = 1190u,
        .hop_count = 2u,
        .link_quality = 60u,
        .valid = true,
    };
    const struct route_candidate *parent;
    const struct route_candidate *selected;
    uint32_t hold_until_ms;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(mesh_relay_note_gateway_control_reverse_route(
               &relay,
               &command,
               ANCHOR_B,
               78u,
               MESH_DEFAULT_TTL,
               1000u) == PROTO_OK);

    for (uint8_t retry = 0u;
         retry < ROUTE_RETRIES_PER_CANDIDATE;
         retry++) {
        assert(route_record_failure_at(&relay.upstream,
                                       ROUTE_FAILURE_GATEWAY_ACK,
                                       1100u + retry) ==
               ROUTE_DELIVERY_RETRY_CURRENT);
    }
    parent = find_route_candidate(&relay, ANCHOR_B);
    assert(parent != NULL);
    assert(parent->failure_count == ROUTE_RETRIES_PER_CANDIDATE);

    /* A gateway downlink observed through the peer is not proof that the
     * reverse EVENT_ACCEPT or Channel-9 uplink path recovered. */
    assert(mesh_relay_note_gateway_control_reverse_route(
               &relay,
               &command,
               ANCHOR_B,
               91u,
               MESH_DEFAULT_TTL,
               1200u) == PROTO_OK);
    parent = find_route_candidate(&relay, ANCHOR_B);
    assert(parent != NULL);
    assert(parent->failure_count == ROUTE_RETRIES_PER_CANDIDATE);
    assert(parent->last_seen_ms == 1200u);
    assert(parent->link_quality == 91u);

    assert(route_upsert_candidate(&relay.upstream, &alternate) == PROTO_OK);
    assert(route_record_failure_at(&relay.upstream,
                                   ROUTE_FAILURE_GATEWAY_ACK,
                                   1300u) ==
           ROUTE_DELIVERY_TRY_ALTERNATE);
    parent = find_route_candidate(&relay, ANCHOR_B);
    assert(parent != NULL);
    assert(parent->hold_down_valid);
    hold_until_ms = parent->hold_down_until_ms;
    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_C);

    assert(mesh_relay_note_gateway_control_reverse_route(
               &relay,
               &command,
               ANCHOR_B,
               96u,
               MESH_DEFAULT_TTL,
               1400u) == PROTO_OK);
    parent = find_route_candidate(&relay, ANCHOR_B);
    assert(parent != NULL);
    assert(parent->hold_down_valid);
    assert(parent->hold_down_until_ms == hold_until_ms);
    assert(parent->last_seen_ms == 1400u);
    assert(parent->link_quality == 96u);
    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_C);

    assert(route_record_candidate_success_at(&relay.upstream,
                                             ANCHOR_B,
                                             GATEWAY,
                                             1500u) == PROTO_OK);
    parent = find_route_candidate(&relay, ANCHOR_B);
    assert(parent != NULL);
    assert(!parent->hold_down_valid);
    assert(parent->hold_down_until_ms == 0u);
    assert(parent->failure_count == 0u);
    assert(parent->last_success_ms == 1500u);
    selected = route_selected(&relay.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
}

static void test_forced_gateway_control_parent_rejects_deeper_cycle_edge(void)
{
    static const uint64_t f1_path[] = {
        GATEWAY, ANCHOR_A, ANCHOR_B,
    };
    static const uint64_t f2_path[] = {
        GATEWAY, ANCHOR_A, ANCHOR_B, ANCHOR_C,
    };
    static const uint64_t f3_path[] = {
        GATEWAY, ANCHOR_A, ANCHOR_B, ANCHOR_C, ANCHOR_D,
    };
    static const uint64_t f3_alternate_path[] = {
        GATEWAY, ANCHOR_A, ANCHOR_C, ANCHOR_D, ANCHOR_B,
    };
    struct mesh_relay f1;
    struct mesh_relay f2;
    struct mesh_relay f3;
    const struct route_candidate *f1_deeper_parent;
    const struct mesh_upstream_ancestry_entry *f1_deeper_ancestry;
    struct route_candidate candidate_before;
    struct mesh_upstream_ancestry_entry ancestry_before;

    mesh_relay_init(&f1, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 7u);
    mesh_relay_init(&f2, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 7u);
    mesh_relay_init(&f3, MESH_RELAY_ROLE_ANCHOR, ANCHOR_D, GATEWAY, 7u);

    seed_upstream_candidate_with_ancestry(
        &f1, ANCHOR_A, 7u, 1u, 90u, 1000u,
        f1_path, (uint8_t)(sizeof(f1_path) / sizeof(f1_path[0])));
    seed_upstream_candidate_with_ancestry(
        &f2, ANCHOR_B, 7u, 2u, 90u, 1001u,
        f2_path, (uint8_t)(sizeof(f2_path) / sizeof(f2_path[0])));
    seed_upstream_candidate_with_ancestry(
        &f3, ANCHOR_C, 7u, 3u, 90u, 1002u,
        f3_path, (uint8_t)(sizeof(f3_path) / sizeof(f3_path[0])));
    f1.gateway_route_adv_seq = 77u;
    f2.gateway_route_adv_seq = 77u;
    f3.gateway_route_adv_seq = 77u;
    mark_upstream_ancestry_gateway_route_seq(&f1, ANCHOR_A, 77u);
    mark_upstream_ancestry_gateway_route_seq(&f2, ANCHOR_B, 77u);
    mark_upstream_ancestry_gateway_route_seq(&f3, ANCHOR_C, 77u);

    /* These are the two legal gatewayward edges in F3 -> F2 -> F1 -> D. */
    assert(mesh_relay_validate_forced_gateway_control_parent(
               &f3, ANCHOR_C, 3u) == PROTO_OK);
    assert(mesh_relay_validate_forced_gateway_control_parent(
               &f2, ANCHOR_B, 2u) == PROTO_OK);
    assert(mesh_relay_validate_forced_gateway_control_parent(
               &f1, ANCHOR_A, 1u) == PROTO_OK);

    /* An ancestry from an older advertisement cannot authorize a parent for
     * a newer gateway control, even when its depth is otherwise exact. */
    mark_upstream_ancestry_gateway_route_seq(&f2, ANCHOR_B, 76u);
    assert(mesh_relay_validate_forced_gateway_control_parent(
               &f2, ANCHOR_B, 2u) == PROTO_ERR_MALFORMED);
    mark_upstream_ancestry_gateway_route_seq(&f2, ANCHOR_B, 77u);
    assert(mesh_relay_validate_forced_gateway_control_parent(
               &f2, ANCHOR_B, 2u) == PROTO_OK);

    /* F1 can also hear a valid advertisement from F3 on another branch.
     * Choosing that deeper anchor from a one-hop control copy would close the
     * observed F3 -> F2 -> F1 -> F3 loop, so advertised depth must win over
     * the control packet's apparently shallow TTL. */
    seed_upstream_candidate_with_ancestry(
        &f1, ANCHOR_D, 7u, 3u, 70u, 1003u,
        f3_alternate_path,
        (uint8_t)(sizeof(f3_alternate_path) /
                  sizeof(f3_alternate_path[0])));
    mark_upstream_ancestry_gateway_route_seq(&f1, ANCHOR_D, 77u);
    f1_deeper_parent = find_route_candidate(&f1, ANCHOR_D);
    f1_deeper_ancestry = find_upstream_ancestry(&f1, ANCHOR_D);
    assert(f1_deeper_parent != NULL);
    assert(f1_deeper_ancestry != NULL);
    candidate_before = *f1_deeper_parent;
    ancestry_before = *f1_deeper_ancestry;

    assert(mesh_relay_validate_forced_gateway_control_parent(
               &f1, ANCHOR_D, 1u) == PROTO_ERR_MALFORMED);
    assert_route_candidate_equal(find_route_candidate(&f1, ANCHOR_D),
                                 &candidate_before);
    assert_upstream_ancestry_equal(find_upstream_ancestry(&f1, ANCHOR_D),
                                   &ancestry_before);
}


static void test_downlink_recency_tie_break_survives_uptime_wrap(void)
{
    struct mesh_relay gateway;
    const struct mesh_downlink_entry *selected;

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    17u);
    gateway.downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = ANCHOR_A,
        .next_hop_id = ANCHOR_B,
        .gateway_id = GATEWAY,
        .route_epoch = 17u,
        .last_seen_ms = UINT32_MAX - 4u,
        .hop_count = 1u,
        .quality = 90u,
        .valid = true,
    };
    gateway.downlinks[1] = gateway.downlinks[0];
    gateway.downlinks[1].next_hop_id = ANCHOR_C;
    gateway.downlinks[1].last_seen_ms = 3u;

    selected = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_C);
}



static void test_direct_gateway_probe_route_answers_pending_request(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_outbound route_reply;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 11u);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               NULL,
               0u,
               0u,
               25u,
               &route_req,
               1000u) == PROTO_OK);
    assert(mesh_relay_note_direct_gateway_route(&relay, 1040u) == PROTO_OK);

    assert(mesh_relay_build_route_reply_for_request(&relay,
                                                    &route_req.packet,
                                                    route_req.payload,
                                                    route_req.payload_len,
                                                    ANCHOR_A,
                                                    1050u,
                                                    3u,
                                                    &route_reply) == PROTO_OK);
    assert(route_reply.packet.msg_type == MSG_ROUTE_REPLY);
    assert(route_reply.packet.src_id == GATEWAY);
    assert(route_reply.packet.dst_id == ANCHOR_A);
    assert(route_reply.packet.ttl == MESH_NETWORK_MAX_HOPS);
    assert(route_reply.next_hop_id == ANCHOR_A);
    assert(route_reply.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(route_reply.earliest_tx_ms ==
           1075u + (3u * RREP_RESPONDER_SLOT_MS));
    assert(require_tlv_u8(route_reply.payload,
                          route_reply.payload_len,
                          TLV_HOP_COUNT) == 1u);
}

static void test_route_reply_scheduled_deadline_wraps_to_valid_zero(void)
{
    const uint32_t reply_now_ms =
        UINT32_MAX -
        (25u + 3u * RREP_RESPONDER_SLOT_MS) + 1u;
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_outbound route_reply;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 11u);
    assert(mesh_relay_build_route_request_with_timing_flags(
               &origin,
               GATEWAY,
               NULL,
               0u,
               0u,
               25u,
               &route_req,
               1000u) == PROTO_OK);
    assert(mesh_relay_note_direct_gateway_route(&relay, 1040u) == PROTO_OK);

    assert(mesh_relay_build_route_reply_for_request(&relay,
                                                    &route_req.packet,
                                                    route_req.payload,
                                                    route_req.payload_len,
                                                    ANCHOR_A,
                                                    reply_now_ms,
                                                    3u,
                                                    &route_reply) == PROTO_OK);
    assert(route_reply.earliest_tx_ms == 0u);
    assert(route_reply.earliest_tx_valid);
}

static void test_route_reply_split_horizon_rejects_requester_as_gateway_parent(void)
{
    struct mesh_relay requester;
    struct mesh_relay child;
    struct mesh_outbound route_req;
    struct mesh_outbound route_reply;
    const struct route_candidate child_parent = {
        .next_hop_id = ANCHOR_A,
        .gateway_id = GATEWAY,
        .route_epoch = 11u,
        .last_seen_ms = 1040u,
        .hop_count = 1u,
        .link_quality = 90u,
        .valid = true,
    };

    mesh_relay_init(&requester, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);
    mesh_relay_init(&child, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 11u);

    assert(mesh_relay_build_route_request_with_timing_flags(
               &requester,
               GATEWAY,
               NULL,
               0u,
               0u,
               25u,
               &route_req,
               1000u) == PROTO_OK);
    assert(route_upsert_candidate(&child.upstream, &child_parent) == PROTO_OK);

    assert(mesh_relay_build_route_reply_for_request(&child,
                                                    &route_req.packet,
                                                    route_req.payload,
                                                    route_req.payload_len,
                                                    ANCHOR_A,
                                                    1050u,
                                                    3u,
                                                    &route_reply) ==
           PROTO_ERR_NOT_FOUND);
}

static void test_route_control_wire_admission_precedes_route_mutation(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_relay gateway;
    struct mesh_outbound route_req;
    struct mesh_outbound route_adv;
    struct mesh_relay_result result;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 11u);
    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &route_req,
                                          1000u) == PROTO_OK);

    route_req.packet.ttl = 0u;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) == NULL);

    route_req.packet.ttl = 3u;
    route_req.packet.seq++;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1020u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) == NULL);

    route_req.packet.ttl = 1u;
    route_req.packet.seq++;
    memset(&route_req.payload[route_req.payload_len],
           0,
           MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN + 1u - route_req.payload_len);
    route_req.payload_len = MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN + 1u;
    route_req.packet.payload_len = route_req.payload_len;
    assert(mesh_relay_handle_rx(&relay,
                                &route_req.packet,
                                route_req.payload,
                                route_req.payload_len,
                                ANCHOR_A,
                                80u,
                                1030u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) == NULL);

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY, GATEWAY, 11u);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              77u,
                                              1040u,
                                              &route_adv) == PROTO_OK);
    route_adv.packet.ttl = 0u;
    assert(mesh_relay_handle_rx(&relay,
                                &route_adv.packet,
                                route_adv.payload,
                                route_adv.payload_len,
                                GATEWAY,
                                90u,
                                1050u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(route_selected(&relay.upstream) == NULL);
}

static void test_max_depth_route_control_builders_fit_wire_bounds(void)
{
    const uint64_t relay_base = UINT64_C(0xa900000000001000);
    const uint64_t requester_id = UINT64_C(0xaa00000000001000);
    struct mesh_relay gateway;
    struct mesh_relay deepest_relay;
    struct mesh_relay requester;
    struct mesh_outbound route_adv;
    struct mesh_outbound route_req;
    struct mesh_outbound route_reply;
    struct mesh_relay_result result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(5000u);
    struct mesh_route_path reply_path = {0};
    uint64_t previous_hop_id = GATEWAY;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY, GATEWAY, 17u);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              91u,
                                              1000u,
                                              &route_adv) == PROTO_OK);

    for (uint8_t hop = 0u; hop < MESH_NETWORK_MAX_HOPS; hop++) {
        const uint64_t local_id = relay_base + hop;

        mesh_relay_init(&deepest_relay,
                        MESH_RELAY_ROLE_ANCHOR,
                        local_id,
                        GATEWAY,
                        17u);
        assert(mesh_relay_handle_rx(&deepest_relay,
                                    &route_adv.packet,
                                    route_adv.payload,
                                    route_adv.payload_len,
                                    previous_hop_id,
                                    (uint8_t)(95u - hop),
                                    1010u + hop,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(route_adv.payload_len <= MESH_GATEWAY_ROUTE_ADV_MAX_PAYLOAD_LEN);
        if (hop + 1u < MESH_NETWORK_MAX_HOPS) {
            assert(has_action(&result,
                              MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
            route_adv = result.gateway_route_adv;
            previous_hop_id = local_id;
        } else {
            const struct route_candidate *selected =
                route_selected(&deepest_relay.upstream);

            assert(selected != NULL);
            assert(selected->hop_count == MESH_NETWORK_MAX_HOPS - 1u);
            assert(!has_action(&result,
                               MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
        }
    }

    assert(route_adv.payload_len ==
           MESH_GATEWAY_ROUTE_ADV_LEGACY_MAX_PAYLOAD_LEN);
    mesh_relay_init(&requester,
                    MESH_RELAY_ROLE_ANCHOR,
                    requester_id,
                    GATEWAY,
                    17u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_build_route_request_with_timing_flags(
               &requester,
               GATEWAY,
               &timing,
               5000u,
               MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED,
               125u,
               &route_req,
               1100u) == PROTO_OK);
    assert(route_req.payload_len <= MESH_ROUTE_REQUEST_MAX_PAYLOAD_LEN);
    assert(mesh_relay_build_route_reply_for_request(&deepest_relay,
                                                    &route_req.packet,
                                                    route_req.payload,
                                                    route_req.payload_len,
                                                    requester_id,
                                                    1200u,
                                                    0u,
                                                    &route_reply) == PROTO_OK);
    assert(route_reply.payload_len == MESH_ROUTE_REPLY_MAX_PAYLOAD_LEN);
    assert(route_reply.payload_len <= PACKET_MAX_PAYLOAD_LEN);
    assert(mesh_route_path_from_tlvs(route_reply.payload,
                                     route_reply.payload_len,
                                     &reply_path) == PROTO_OK);
    assert(reply_path.count == MESH_ROUTE_PATH_MAX_NODES);
    assert(reply_path.node_ids[0] == GATEWAY);
    assert(reply_path.node_ids[reply_path.count - 1u] ==
           deepest_relay.local_id);
}

static size_t build_clicker_self_test_packet(struct proto_packet *packet,
                                             uint8_t *payload,
                                             size_t payload_cap,
                                             uint32_t event_seq,
                                             uint16_t seq)
{
    const struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = event_seq,
        .failure_code = 0u,
        .battery_mv = 3000u,
    };
    size_t payload_len = 0u;

    assert(report_append_self_test_tlvs(payload,
                                        payload_cap,
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(packet,
                                        ANCHOR_A,
                                        GATEWAY,
                                        event_seq,
                                        seq,
                                        (uint8_t)payload_len) == PROTO_OK);
    return payload_len;
}

static size_t build_clicker_event_control(struct proto_packet *packet,
                                          uint8_t msg_type,
                                          uint64_t source_id,
                                          uint8_t *payload,
                                          size_t payload_cap,
                                          uint32_t session_id,
                                          uint16_t seq,
                                          uint32_t now_ms)
{
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(now_ms + 500u);
    size_t payload_len = 0u;

    if (msg_type != MSG_MESH_EVENT_END) {
        assert(mesh_event_timing_negotiate(&timing, &params, true) ==
               PROTO_OK);
        assert(mesh_event_timing_bind_proposal_session(&timing, session_id));
        if (msg_type == MSG_MESH_EVENT_UPDATE) {
            assert(mesh_append_event_update_tlvs_at(payload,
                                                    payload_cap,
                                                    &payload_len,
                                                    &timing,
                                                    now_ms) == PROTO_OK);
        } else {
            assert(mesh_append_event_timing_tlvs_at(payload,
                                                    payload_cap,
                                                    &payload_len,
                                                    &timing,
                                                    now_ms) == PROTO_OK);
        }
        if (msg_type == MSG_MESH_EVENT_PROPOSE) {
            assert(tlv_append_u64(payload,
                                  payload_cap,
                                  &payload_len,
                                  TLV_MESH_EVENT_BOOT_NONCE,
                                  UINT64_C(0x123456789abcdef0)) == PROTO_OK);
        }
    }
    assert(mesh_init_event_control(packet,
                                   msg_type,
                                   source_id,
                                   ANCHOR_A,
                                   session_id,
                                   seq,
                                   (uint8_t)payload_len) == PROTO_OK);
    return payload_len;
}

static void test_clicker_leaf_rejects_command_result_origin(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 61u,
        .command_seq = UINT32_C(0x23456789),
        .node_id = ANCHOR_A,
        .node_boot_counter = 9u,
        .result_seq = 17u,
    };
    struct mesh_relay clicker;
    struct mesh_relay before;
    struct route_candidate route =
        direct_gateway_route(ANCHOR_B, 61u, 90u);
    struct proto_packet packet;
    struct mesh_outbound outbound;
    uint8_t payload[96];
    size_t payload_len = 0u;

    route.hop_count = 1u;
    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    result_id.command_seq,
                                    result_id.result_seq,
                                    (uint8_t)payload_len,
                                    false) == PROTO_OK);
    mesh_relay_init(&clicker,
                    MESH_RELAY_ROLE_CLICKER,
                    ANCHOR_A,
                    GATEWAY,
                    61u);
    assert(route_upsert_candidate(&clicker.upstream, &route) == PROTO_OK);

    before = clicker;
    assert(mesh_relay_start_tx(&clicker,
                               &packet,
                               payload,
                               payload_len,
                               900u,
                               &outbound) == PROTO_ERR_ARG);
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);

    assert(mesh_relay_start_result_offer(&clicker,
                                         &packet,
                                         payload,
                                         payload_len,
                                         901u,
                                         &outbound) == PROTO_ERR_ARG);
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);
}

static void test_clicker_leaf_route_requests_only_own_gateway(void)
{
    struct mesh_relay clicker;
    struct mesh_relay before;
    struct mesh_outbound request;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&clicker,
                    MESH_RELAY_ROLE_CLICKER,
                    ANCHOR_A,
                    GATEWAY,
                    61u);
    assert(mesh_relay_downlink_capacity(&clicker) == 0u);
    assert(mesh_relay_downlink_at(&clicker, 0u) == NULL);
    assert(mesh_relay_find_downlink(&clicker, ANCHOR_B) == NULL);
    assert(mesh_relay_select_next_hop(&clicker,
                                      ANCHOR_B,
                                      &next_hop_id) == PROTO_ERR_NOT_FOUND);

    before = clicker;
    assert(mesh_relay_prepare_route_request(&clicker,
                                            ANCHOR_B,
                                            1000u,
                                            UINT32_C(0x10203040),
                                            &request) == PROTO_ERR_ARG);
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);

    assert(mesh_relay_prepare_route_request(&clicker,
                                            GATEWAY,
                                            1000u,
                                            UINT32_C(0x50607080),
                                            &request) == PROTO_OK);
    assert(clicker.route_discovery.active);
    assert(clicker.route_discovery.target_id == GATEWAY);
    assert(clicker.route_discovery.current_request_id ==
           request.packet.session_id);
    assert(request.packet.msg_type == MSG_ROUTE_REQ);
    assert(request.packet.src_id == ANCHOR_A);
    assert(request.packet.dst_id == MESH_BROADCAST_ID);
    assert(require_tlv_u64(request.payload,
                           request.payload_len,
                           TLV_RESPONDER_ID) == GATEWAY);
}

static void test_clicker_leaf_route_reply_requires_latest_active_request(void)
{
    struct mesh_relay clicker;
    struct mesh_relay responder;
    struct mesh_relay alternate_responder;
    struct mesh_relay before;
    struct mesh_outbound first_request;
    struct mesh_outbound latest_request;
    struct mesh_outbound first_reply;
    struct mesh_outbound latest_reply;
    struct mesh_outbound alternate_reply;
    struct mesh_relay_result result;
    struct route_candidate gateway_route =
        direct_gateway_route(GATEWAY, 62u, 95u);
    struct route_table upstream_after_install;
    struct mesh_route_discovery_state discovery_after_install;
    struct mesh_relay_event_timing_entry
        timings_after_install[MESH_RELAY_EVENT_TIMINGS];
    const struct route_candidate *selected;
    uint32_t latest_request_ms;
    uint32_t accepted_ms;
    uint32_t completed_until_ms;

    mesh_relay_init(&clicker,
                    MESH_RELAY_ROLE_CLICKER,
                    ANCHOR_A,
                    GATEWAY,
                    62u);
    mesh_relay_init(&responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_B,
                    GATEWAY,
                    62u);
    mesh_relay_init(&alternate_responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_C,
                    GATEWAY,
                    62u);
    assert(route_upsert_candidate(&responder.upstream, &gateway_route) ==
           PROTO_OK);
    assert(route_upsert_candidate(&alternate_responder.upstream,
                                  &gateway_route) == PROTO_OK);

    /* A well-formed reply without an active request is inert. */
    assert(mesh_relay_build_route_request(&clicker,
                                          GATEWAY,
                                          &first_request,
                                          900u) == PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(
               &responder,
               &first_request.packet,
               first_request.payload,
               first_request.payload_len,
               ANCHOR_A,
               910u,
               0u,
               &first_reply) == PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &first_reply.packet,
                                              first_reply.payload,
                                              first_reply.payload_len,
                                              ANCHOR_B);

    assert(mesh_relay_prepare_route_request(&clicker,
                                            GATEWAY,
                                            1000u,
                                            UINT32_C(0x11223344),
                                            &first_request) == PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(
               &responder,
               &first_request.packet,
               first_request.payload,
               first_request.payload_len,
               ANCHOR_A,
               1010u,
               0u,
               &first_reply) == PROTO_OK);

    latest_request_ms = clicker.route_discovery.next_request_ms;
    assert(mesh_relay_prepare_route_request(&clicker,
                                            GATEWAY,
                                            latest_request_ms,
                                            UINT32_C(0x55667788),
                                            &latest_request) == PROTO_OK);
    assert(latest_request.packet.session_id !=
           first_request.packet.session_id);
    assert(clicker.route_discovery.current_request_id ==
           latest_request.packet.session_id);
    assert(mesh_relay_build_route_reply_for_request(
               &responder,
               &latest_request.packet,
               latest_request.payload,
               latest_request.payload_len,
               ANCHOR_A,
               latest_request_ms + 10u,
               0u,
               &latest_reply) == PROTO_OK);
    alternate_responder.next_seq = 100u;
    assert(mesh_relay_build_route_reply_for_request(
               &alternate_responder,
               &latest_request.packet,
               latest_request.payload,
               latest_request.payload_len,
               ANCHOR_A,
               latest_request_ms + 11u,
               0u,
               &alternate_reply) == PROTO_OK);
    assert(alternate_reply.packet.session_id ==
           latest_reply.packet.session_id);
    assert(alternate_reply.packet.seq != latest_reply.packet.seq);

    before = clicker;
    assert(mesh_relay_handle_rx(&clicker,
                                &first_reply.packet,
                                first_reply.payload,
                                first_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                latest_request_ms + 15u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);

    accepted_ms = latest_request_ms + 20u;
    assert(mesh_relay_handle_rx(&clicker,
                                &latest_reply.packet,
                                latest_reply.payload,
                                latest_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                accepted_ms,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions ==
           (MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK |
            MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    assert(!clicker.route_discovery.active);
    assert(clicker.route_discovery.completed_request_id ==
           latest_reply.packet.session_id);
    selected = route_selected(&clicker.upstream);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(selected->gateway_id == GATEWAY);

    /*
     * Completion retention repairs only the exact accepted reply. A later
     * same-request reply with a new transport identity cannot reopen route
     * installation after discovery has closed.
     */
    before = clicker;
    assert(mesh_relay_handle_rx(&clicker,
                                &alternate_reply.packet,
                                alternate_reply.payload,
                                alternate_reply.payload_len,
                                ANCHOR_C,
                                90u,
                                accepted_ms + 1u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);

    /*
     * The retained reply identity also binds the physical upstream hop. The
     * duplicate fast path runs before route-reply payload validation, so an
     * exact packet replayed by another peer must not receive the lost-ACK
     * repair response.
     */
    before = clicker;
    assert(mesh_relay_handle_rx(&clicker,
                                &latest_reply.packet,
                                latest_reply.payload,
                                latest_reply.payload_len,
                                ANCHOR_C,
                                90u,
                                accepted_ms + 2u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);

    /*
     * If the first reply ACK is lost, an exact bounded duplicate is ACKed
     * without replaying route installation or timing mutation.
     */
    upstream_after_install = clicker.upstream;
    discovery_after_install = clicker.route_discovery;
    memcpy(timings_after_install,
           clicker.event_timings,
           sizeof(timings_after_install));
    assert(mesh_relay_handle_rx(&clicker,
                                &latest_reply.packet,
                                latest_reply.payload,
                                latest_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                accepted_ms + 3u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(result.actions ==
           (MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK |
            MESH_RELAY_ACTION_DROP));
    assert(memcmp(&clicker.upstream,
                  &upstream_after_install,
                  sizeof(upstream_after_install)) == 0);
    assert(memcmp(&clicker.route_discovery,
                  &discovery_after_install,
                  sizeof(discovery_after_install)) == 0);
    assert(memcmp(clicker.event_timings,
                  timings_after_install,
                  sizeof(timings_after_install)) == 0);

    completed_until_ms =
        clicker.route_discovery.completed_request_until_ms;
    assert(completed_until_ms == accepted_ms + ROUTE_DEDUP_WINDOW_MS);
    before = clicker;
    assert(mesh_relay_handle_rx(&clicker,
                                &latest_reply.packet,
                                latest_reply.payload,
                                latest_reply.payload_len,
                                ANCHOR_B,
                                90u,
                                completed_until_ms,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(!has_action(&result,
                       MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);
}

static void test_clicker_leaf_pending_controls_require_correlation(void)
{
    struct mesh_relay clicker;
    struct mesh_relay before;
    struct route_candidate route =
        direct_gateway_route(ANCHOR_B, 63u, 90u);
    struct proto_packet report;
    struct proto_packet hop_ack;
    struct proto_packet busy = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t report_payload[64];
    uint8_t ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t busy_payload[64];
    uint8_t report_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t report_payload_len;
    size_t ack_payload_len = 0u;
    size_t busy_payload_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&clicker,
                    MESH_RELAY_ROLE_CLICKER,
                    ANCHOR_A,
                    GATEWAY,
                    63u);
    assert(route_upsert_candidate(&clicker.upstream, &route) == PROTO_OK);
    report_payload_len = build_clicker_self_test_packet(&report,
                                                        report_payload,
                                                        sizeof(report_payload),
                                                        501u,
                                                        41u);
    assert(mesh_relay_start_tx(&clicker,
                               &report,
                               report_payload,
                               report_payload_len,
                               4000u,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&clicker, &tx, 4001u);

    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             42u,
                             ack_payload,
                             sizeof(ack_payload),
                             &ack_payload_len,
                             &report,
                             report_payload,
                             report_payload_len);
    assert(mesh_relay_handle_rx(&clicker,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                4010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
    assert(mesh_relay_tx_active(&clicker));
    assert(clicker.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
    assert(clicker.outbox_record.expiry_s == COMMAND_RESULT_EXPIRY_DEFAULT_S);
    assert(mesh_packet_semantic_digest(&report,
                                       report_payload,
                                       report_payload_len,
                                       report_digest));
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &clicker, &report, report_digest) == PROTO_OK);
    assert(!mesh_relay_tx_active(&clicker));

    mesh_relay_init(&clicker,
                    MESH_RELAY_ROLE_CLICKER,
                    ANCHOR_A,
                    GATEWAY,
                    63u);
    assert(route_upsert_candidate(&clicker.upstream, &route) == PROTO_OK);
    report_payload_len = build_clicker_self_test_packet(&report,
                                                        report_payload,
                                                        sizeof(report_payload),
                                                        502u,
                                                        44u);
    assert(mesh_relay_start_tx(&clicker,
                               &report,
                               report_payload,
                               report_payload_len,
                               5000u,
                               &tx) == PROTO_OK);
    mesh_relay_note_tx_sent(&clicker, &tx, 5001u);

    assert(tlv_append_u32(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID,
                          report.session_id) == PROTO_OK);
    assert(mesh_append_requested_seq(busy_payload,
                                     sizeof(busy_payload),
                                     &busy_payload_len,
                                     report.seq) == PROTO_OK);
    assert(tlv_append_u16(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_RETRY_AFTER_MS,
                          RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    assert(tlv_append_u8(busy_payload,
                         sizeof(busy_payload),
                         &busy_payload_len,
                         TLV_RELAY_CAPACITY_STATE,
                         RELAY_CAP_YELLOW) == PROTO_OK);
    assert(tlv_append_u16(busy_payload,
                          sizeof(busy_payload),
                          &busy_payload_len,
                          TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                          RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    busy.msg_type = MSG_RELAY_BUSY;
    busy.src_id = ANCHOR_C;
    busy.dst_id = ANCHOR_A;
    busy.session_id = report.session_id;
    busy.seq = 45u;
    busy.ttl = 1u;
    busy.payload_len = (uint16_t)busy_payload_len;

    before = clicker;
    assert(mesh_relay_handle_rx(&clicker,
                                &busy,
                                busy_payload,
                                busy_payload_len,
                                ANCHOR_C,
                                80u,
                                5010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_MALFORMED);
    assert(result.actions == MESH_RELAY_ACTION_DROP);
    assert(memcmp(&clicker, &before, sizeof(clicker)) == 0);

    busy.src_id = ANCHOR_B;
    assert(mesh_relay_handle_rx_with_random(&clicker,
                                            &busy,
                                            busy_payload,
                                            busy_payload_len,
                                            ANCHOR_B,
                                            90u,
                                            5011u,
                                            UINT32_C(0x99aabbcc),
                                            &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_TX_RELAY_BUSY);
    assert(clicker.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
}

static void test_clicker_leaf_forbidden_receive_is_inert(void)
{
    const struct command_result_id result_id = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 64u,
        .command_seq = UINT32_C(0x10293847),
        .node_id = ANCHOR_C,
        .node_boot_counter = 7u,
        .result_seq = 51u,
    };
    struct result_offer offer = {
        .result_id = result_id,
        .result_len = 64u,
        .result_crc = UINT16_C(0x1234),
        .priority = 1u,
    };
    const struct result_grant grant = {
        .result_id = result_id,
        .granted_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .max_bytes = 64u,
        .event_offset_hint = 0u,
    };
    struct mesh_relay clicker;
    struct mesh_relay origin;
    struct mesh_relay gateway;
    struct route_candidate route =
        direct_gateway_route(ANCHOR_B, 64u, 90u);
    struct mesh_outbound route_request;
    struct mesh_outbound route_adv;
    struct proto_packet command;
    struct proto_packet broadcast_command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 600u,
        .seq = 52u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    struct proto_packet transit_report;
    struct proto_packet offer_packet = {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = ANCHOR_C,
        .dst_id = ANCHOR_A,
        .session_id = UINT32_C(0x10293847),
        .seq = 51u,
        .ttl = 1u,
    };
    struct proto_packet grant_packet = {
        .msg_type = MSG_RESULT_GRANT,
        .src_id = ANCHOR_B,
        .dst_id = ANCHOR_A,
        .session_id = UINT32_C(0x10293847),
        .seq = 53u,
        .ttl = 1u,
    };
    struct self_test_report_fields transit_fields = {
        .clicker_id = ANCHOR_C,
        .event_seq = 601u,
        .failure_code = 0u,
        .battery_mv = 3000u,
    };
    uint8_t command_payload[16];
    uint8_t broadcast_payload[96];
    uint8_t transit_payload[64];
    uint8_t offer_payload[96];
    uint8_t grant_payload[96];
    uint8_t digest_source[8] = {0x5au};
    size_t command_payload_len = 0u;
    size_t broadcast_payload_len = 0u;
    size_t transit_payload_len = 0u;
    size_t offer_payload_len = 0u;
    size_t grant_payload_len = 0u;

    route.hop_count = 1u;
    mesh_relay_init(&clicker,
                    MESH_RELAY_ROLE_CLICKER,
                    ANCHOR_A,
                    GATEWAY,
                    64u);
    assert(route_upsert_candidate(&clicker.upstream, &route) == PROTO_OK);

    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_C,
                    GATEWAY,
                    64u);
    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &route_request,
                                          2000u) == PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &route_request.packet,
                                              route_request.payload,
                                              route_request.payload_len,
                                              ANCHOR_C);

    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY,
                    GATEWAY,
                    64u);
    assert(mesh_relay_build_gateway_route_adv(&gateway,
                                              602u,
                                              2001u,
                                              &route_adv) == PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &route_adv.packet,
                                              route_adv.payload,
                                              route_adv.payload_len,
                                              GATEWAY);

    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                             GATEWAY,
                             ANCHOR_A,
                             603u,
                             54u,
                             (uint8_t)command_payload_len) == PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &command,
                                              command_payload,
                                              command_payload_len,
                                              GATEWAY);

    assert(mesh_append_command_id(broadcast_payload,
                                  sizeof(broadcast_payload),
                                  &broadcast_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(broadcast_payload,
                         sizeof(broadcast_payload),
                         &broadcast_payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(broadcast_payload,
                         sizeof(broadcast_payload),
                         &broadcast_payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(broadcast_payload,
                          sizeof(broadcast_payload),
                          &broadcast_payload_len,
                          TLV_COMMAND_SEQ,
                          broadcast_command.session_id) == PROTO_OK);
    assert(tlv_append_u32(broadcast_payload,
                          sizeof(broadcast_payload),
                          &broadcast_payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          604u) == PROTO_OK);
    assert(tlv_append_u16(broadcast_payload,
                          sizeof(broadcast_payload),
                          &broadcast_payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          64u) == PROTO_OK);
    assert(tlv_append_u16(broadcast_payload,
                          sizeof(broadcast_payload),
                          &broadcast_payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          3u) == PROTO_OK);
    broadcast_command.payload_len = (uint16_t)broadcast_payload_len;
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &broadcast_command,
                                              broadcast_payload,
                                              broadcast_payload_len,
                                              GATEWAY);

    assert(report_append_self_test_tlvs(transit_payload,
                                        sizeof(transit_payload),
                                        &transit_payload_len,
                                        &transit_fields) == PROTO_OK);
    assert(report_init_self_test_packet(&transit_report,
                                        ANCHOR_C,
                                        GATEWAY,
                                        transit_fields.event_seq,
                                        55u,
                                        (uint8_t)transit_payload_len) ==
           PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &transit_report,
                                              transit_payload,
                                              transit_payload_len,
                                              ANCHOR_C);

    assert(semantic_digest_sha256(digest_source,
                                  sizeof(digest_source),
                                  offer.result_digest));
    assert(result_offer_append_tlvs(offer_payload,
                                    sizeof(offer_payload),
                                    &offer_payload_len,
                                    &offer) == PROTO_OK);
    offer_packet.payload_len = (uint16_t)offer_payload_len;
    assert(mesh_packet_rx_semantics_validate(&offer_packet,
                                             offer_payload,
                                             offer_payload_len,
                                             ANCHOR_C,
                                             ANCHOR_A,
                                             GATEWAY) == PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &offer_packet,
                                              offer_payload,
                                              offer_payload_len,
                                              ANCHOR_C);

    assert(result_grant_append_tlvs(grant_payload,
                                    sizeof(grant_payload),
                                    &grant_payload_len,
                                    &grant) == PROTO_OK);
    grant_packet.payload_len = (uint16_t)grant_payload_len;
    assert(mesh_packet_rx_semantics_validate(&grant_packet,
                                             grant_payload,
                                             grant_payload_len,
                                             ANCHOR_B,
                                             ANCHOR_A,
                                             GATEWAY) == PROTO_OK);
    assert_rx_rejected_without_relay_mutation(&clicker,
                                              &grant_packet,
                                              grant_payload,
                                              grant_payload_len,
                                              ANCHOR_B);
}

static void test_clicker_leaf_event_controls_require_selected_parent(void)
{
    static const uint8_t msg_types[] = {
        MSG_MESH_EVENT_PROPOSE,
        MSG_MESH_EVENT_ACCEPT,
        MSG_MESH_EVENT_UPDATE,
        MSG_MESH_EVENT_END,
    };

    for (size_t i = 0u; i < sizeof(msg_types) / sizeof(msg_types[0]); i++) {
        struct mesh_relay clicker;
        struct route_candidate route =
            direct_gateway_route(ANCHOR_B, 65u, 90u);
        struct proto_packet packet;
        struct mesh_relay_result result;
        struct mesh_duplicate_entry
            duplicates_before[MESH_RELAY_DUP_CACHE_SIZE];
        uint8_t payload[96];
        uint8_t duplicate_next_before;
        size_t payload_len;
        uint32_t session_id = 700u + (uint32_t)i;

        route.hop_count = 1u;
        mesh_relay_init(&clicker,
                        MESH_RELAY_ROLE_CLICKER,
                        ANCHOR_A,
                        GATEWAY,
                        65u);
        assert(route_upsert_candidate(&clicker.upstream, &route) == PROTO_OK);
        payload_len = build_clicker_event_control(&packet,
                                                  msg_types[i],
                                                  ANCHOR_C,
                                                  payload,
                                                  sizeof(payload),
                                                  session_id,
                                                  (uint16_t)(60u + i),
                                                  3000u);
        assert(mesh_packet_rx_semantics_validate(&packet,
                                                 payload,
                                                 payload_len,
                                                 ANCHOR_C,
                                                 ANCHOR_A,
                                                 GATEWAY) == PROTO_OK);
        assert_rx_rejected_without_relay_mutation(&clicker,
                                                  &packet,
                                                  payload,
                                                  payload_len,
                                                  ANCHOR_C);

        packet.src_id = ANCHOR_B;
        assert(mesh_packet_rx_semantics_validate(&packet,
                                                 payload,
                                                 payload_len,
                                                 ANCHOR_B,
                                                 ANCHOR_A,
                                                 GATEWAY) == PROTO_OK);
        memcpy(duplicates_before,
               clicker.duplicates,
               sizeof(duplicates_before));
        duplicate_next_before = clicker.duplicate_next;
        assert(mesh_relay_handle_rx(&clicker,
                                    &packet,
                                    payload,
                                    payload_len,
                                    ANCHOR_B,
                                    90u,
                                    3000u,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
        assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
        assert(!has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK));
        assert(!has_action(&result,
                           MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(clicker.duplicate_next == duplicate_next_before);
        assert(memcmp(clicker.duplicates,
                      duplicates_before,
                      sizeof(duplicates_before)) == 0);

        /*
         * The application owner, rather than the relay cache, must see every
         * exact retry so it can replay a lost response or idempotently absorb
         * a repeated transition.
         */
        assert(mesh_relay_handle_rx(&clicker,
                                    &packet,
                                    payload,
                                    payload_len,
                                    ANCHOR_B,
                                    90u,
                                    3001u,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
        assert(clicker.duplicate_next == duplicate_next_before);
        assert(memcmp(clicker.duplicates,
                      duplicates_before,
                      sizeof(duplicates_before)) == 0);
    }
}

static void test_transit_reverse_route_commits_only_after_exact_custody(void)
{
    const uint8_t payload[] = {
        TLV_MESH_TEST_PADDING, 1u, 0x52u,
    };
    const struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = ANCHOR_C,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x63757374),
        .seq = UINT16_C(0x726f),
        .ttl = MESH_DEFAULT_TTL - 1u,
        .payload_len = sizeof(payload),
    };
    struct proto_packet malformed_packet = packet;
    struct mesh_relay relay;
    struct mesh_relay failed;
    struct mesh_relay gateway;
    struct mesh_relay_result admission;
    struct mesh_relay_result result;
    struct mesh_outbound retained;
    struct mesh_outbound unrelated;
    struct mesh_pending_tx pending_before_commit;
    struct mesh_downlink_entry stale_direct;
    const struct mesh_downlink_entry *selected;
    struct route_candidate route = direct_gateway_route(GATEWAY, 77u, 95u);
    struct proto_packet unrelated_packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = UINT32_C(0x756e7265),
        .seq = UINT16_C(0x6c61),
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(payload),
    };

    mesh_relay_init(
        &relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 77u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_C, ANCHOR_C, 77u, 1u, 100u, 1000u);
    selected = mesh_relay_find_downlink(&relay, ANCHOR_C);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_C);
    stale_direct = *selected;

    /* Malformed and otherwise unretained candidates cannot teach a route. */
    malformed_packet.flags = 0u;
    retained = (struct mesh_outbound) {
        .packet = malformed_packet,
        .next_hop_id = GATEWAY,
    };
    assert(mesh_relay_commit_transit_reverse_route(
               &relay, &retained, 1010u) == PROTO_ERR_ARG);
    selected = mesh_relay_find_downlink(&relay, ANCHOR_C);
    assert(selected != NULL);
    assert(memcmp(selected, &stale_direct, sizeof(stale_direct)) == 0);

    retained.packet = packet;
    assert(mesh_relay_commit_transit_reverse_route(
               &relay, &retained, 1020u) == PROTO_ERR_NOT_FOUND);
    selected = mesh_relay_find_downlink(&relay, ANCHOR_C);
    assert(selected != NULL);
    assert(memcmp(selected, &stale_direct, sizeof(stale_direct)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                ANCHOR_B,
                                91u,
                                1030u,
                                &admission) == PROTO_OK);
    assert(admission.status == PROTO_OK);
    assert(has_action(&admission, MESH_RELAY_ACTION_FORWARD));
    assert(has_action(&admission, MESH_RELAY_ACTION_SEND_HOP_ACK));
    assert(admission.forward.ingress_previous_hop_id == ANCHOR_B);
    selected = mesh_relay_find_downlink(&relay, ANCHOR_C);
    assert(selected != NULL);
    assert(memcmp(selected, &stale_direct, sizeof(stale_direct)) == 0);

    /* A later owner can make this forwarding admission fail to retain. The
     * unowned outbound must still be unable to mutate reverse selection. */
    failed = relay;
    assert(mesh_relay_start_tx(&failed,
                               &unrelated_packet,
                               payload,
                               sizeof(payload),
                               1040u,
                               &unrelated) == PROTO_OK);
    assert(mesh_relay_start_tx(&failed,
                               &admission.forward.packet,
                               admission.forward.payload,
                               admission.forward.payload_len,
                               1050u,
                               &retained) != PROTO_OK);
    assert(mesh_relay_commit_transit_reverse_route(
               &failed, &admission.forward, 1060u) == PROTO_ERR_NOT_FOUND);
    selected = mesh_relay_find_downlink(&failed, ANCHOR_C);
    assert(selected != NULL);
    assert(memcmp(selected, &stale_direct, sizeof(stale_direct)) == 0);

    assert(mesh_relay_start_tx(&relay,
                               &admission.forward.packet,
                               admission.forward.payload,
                               admission.forward.payload_len,
                               1070u,
                               &retained) == PROTO_OK);
    assert(mesh_relay_bind_transit_previous_hop(
               &relay, &retained, ANCHOR_B) == PROTO_OK);
    pending_before_commit = relay.pending;
    assert(mesh_relay_commit_transit_reverse_route(
               &relay, &retained, 1080u) == PROTO_OK);
    assert(memcmp(&relay.pending,
                  &pending_before_commit,
                  sizeof(pending_before_commit)) == 0);
    selected = mesh_relay_find_downlink(&relay, ANCHOR_C);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_B);
    assert(selected->route_epoch == 77u);
    assert(selected->discovery_flood_epoch_id == 0u);
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(&relay); i++) {
        const struct mesh_downlink_entry *candidate =
            mesh_relay_downlink_at(&relay, i);

        assert(candidate != NULL);
        assert(!candidate->valid || candidate->target_id != ANCHOR_C ||
               candidate->next_hop_id != ANCHOR_C);
    }

    /* A mutable downlink can change again after custody is captured. The
     * gateway ACK must nevertheless return over the exact ingress edge. */
    seed_downlink(&relay, ANCHOR_C, ANCHOR_C, 77u, 1u, 100u, 1081u);
    selected = mesh_relay_find_downlink(&relay, ANCHOR_C);
    assert(selected != NULL);
    assert(selected->next_hop_id == ANCHOR_C);
    mesh_relay_note_tx_sent(&relay, &retained, 1090u);

    mesh_relay_init(
        &gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 77u);
    assert(mesh_relay_handle_rx(&gateway,
                                &retained.packet,
                                retained.payload,
                                retained.payload_len,
                                ANCHOR_A,
                                95u,
                                1100u,
                                &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
    assert(mesh_relay_commit_gateway_delivery(&gateway,
                                              &retained.packet,
                                              retained.payload,
                                              retained.payload_len,
                                              ANCHOR_A,
                                              1110u,
                                              &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    assert(result.gateway_ack.next_hop_id == ANCHOR_A);
    assert(mesh_relay_handle_rx(&relay,
                                &result.gateway_ack.packet,
                                result.gateway_ack.payload,
                                result.gateway_ack.payload_len,
                                GATEWAY,
                                95u,
                                1120u,
                                &admission) == PROTO_OK);
    assert(admission.actions ==
           MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
    assert(!has_action(&admission, MESH_RELAY_ACTION_FORWARD));
    assert(relay.pending.gateway_ack_confirm_pending);
    assert(relay.pending.transit_previous_hop_id == ANCHOR_B);
}

static size_t build_ordinary_gateway_report(uint8_t msg_type,
                                            uint64_t source_id,
                                            uint32_t identity,
                                            uint16_t seq,
                                            struct proto_packet *packet,
                                            uint8_t *payload,
                                            size_t payload_cap)
{
    size_t payload_len = 0u;

    assert(packet != NULL);
    assert(payload != NULL);
    switch (msg_type) {
    case MSG_CLICK_REPORT:
        payload_len = build_valid_click_report(packet,
                                               source_id,
                                               identity,
                                               seq,
                                               1200,
                                               payload,
                                               payload_cap);
        break;
    case MSG_SELF_TEST_REPORT: {
        const struct self_test_report_fields fields = {
            .clicker_id = source_id,
            .event_seq = identity,
            .failure_code = 0u,
            .battery_mv = 3000u,
        };

        assert(report_append_self_test_tlvs(payload,
                                            payload_cap,
                                            &payload_len,
                                            &fields) == PROTO_OK);
        assert(report_init_self_test_packet(packet,
                                            source_id,
                                            GATEWAY,
                                            identity,
                                            (uint16_t)identity == 0u ?
                                                1u : (uint16_t)identity,
                                            (uint8_t)payload_len) == PROTO_OK);
        break;
    }
    case MSG_MESH_DATA:
        assert(payload_cap >= 3u);
        payload[0] = TLV_MESH_TEST_PADDING;
        payload[1] = 1u;
        payload[2] = (uint8_t)identity;
        payload_len = 3u;
        *packet = (struct proto_packet) {
            .msg_type = MSG_MESH_DATA,
            .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            .src_id = source_id,
            .dst_id = GATEWAY,
            .session_id = identity,
            .seq = seq,
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = (uint16_t)payload_len,
        };
        break;
    default:
        assert(false);
    }
    return payload_len;
}

static void test_ordinary_report_source_retires_on_exact_parent_hop_ack(void)
{
    static const uint8_t msg_types[] = {
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_MESH_DATA,
    };

    for (size_t i = 0u; i < sizeof(msg_types) / sizeof(msg_types[0]); i++) {
        struct mesh_relay source;
        struct route_candidate route =
            direct_gateway_route(ANCHOR_B, 78u, 90u);
        struct proto_packet packet;
        struct proto_packet wrong_ack;
        struct proto_packet exact_ack;
        struct mesh_outbound tx;
        struct mesh_relay_result result;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        uint8_t wrong_payload[UWB_MESH_MAX_PAYLOAD_LEN];
        uint8_t wrong_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        uint8_t exact_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
        uint8_t wrong_digest[SEMANTIC_DIGEST_SHA256_LEN];
        size_t payload_len;
        size_t wrong_ack_payload_len = 0u;
        size_t exact_ack_payload_len = 0u;
        enum mesh_relay_role role =
            msg_types[i] == MSG_SELF_TEST_REPORT ?
                MESH_RELAY_ROLE_CLICKER : MESH_RELAY_ROLE_ANCHOR;
        const struct route_candidate *parent;

        route.hop_count = 1u;
        mesh_relay_init(&source, role, ANCHOR_A, GATEWAY, 78u);
        assert(route_upsert_candidate(&source.upstream, &route) == PROTO_OK);
        source.upstream.candidates[source.upstream.selected_index].failure_count =
            ROUTE_RETRIES_PER_CANDIDATE;
        parent = find_route_candidate(&source, ANCHOR_B);
        assert(parent != NULL);
        assert(parent->failure_count == ROUTE_RETRIES_PER_CANDIDATE);
        payload_len = build_ordinary_gateway_report(
            msg_types[i],
            ANCHOR_A,
            UINT32_C(0x78000000) + (uint32_t)i + 1u,
            (uint16_t)(100u + i),
            &packet,
            payload,
            sizeof(payload));
        assert(mesh_relay_start_tx(&source,
                                   &packet,
                                   payload,
                                   payload_len,
                                   1000u,
                                   &tx) == PROTO_OK);
        assert(tx.next_hop_id == ANCHOR_B);
        mesh_relay_note_tx_sent(&source, &tx, 1010u);

        memcpy(wrong_payload, payload, payload_len);
        wrong_payload[payload_len - 1u] ^= UINT8_C(0x01);
        build_hop_ack_for_packet(&wrong_ack,
                                 ANCHOR_B,
                                 ANCHOR_A,
                                 (uint16_t)(200u + i),
                                 wrong_ack_payload,
                                 sizeof(wrong_ack_payload),
                                 &wrong_ack_payload_len,
                                 &packet,
                                 wrong_payload,
                                 payload_len);
        assert(mesh_relay_handle_rx(&source,
                                    &wrong_ack,
                                    wrong_ack_payload,
                                    wrong_ack_payload_len,
                                    ANCHOR_B,
                                    90u,
                                    1020u,
                                    &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(mesh_relay_tx_active(&source));

        build_hop_ack_for_packet(&exact_ack,
                                 ANCHOR_B,
                                 ANCHOR_A,
                                 (uint16_t)(210u + i),
                                 exact_ack_payload,
                                 sizeof(exact_ack_payload),
                                 &exact_ack_payload_len,
                                 &packet,
                                 payload,
                                 payload_len);
        assert(mesh_relay_handle_rx(&source,
                                    &exact_ack,
                                    exact_ack_payload,
                                    exact_ack_payload_len,
                                    ANCHOR_C,
                                    90u,
                                    1021u,
                                    &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(mesh_relay_tx_active(&source));

        assert(mesh_relay_handle_rx(&source,
                                    &exact_ack,
                                    exact_ack_payload,
                                    exact_ack_payload_len,
                                    ANCHOR_B,
                                    90u,
                                    1030u,
                                    &result) == PROTO_OK);
        assert(result.actions ==
               MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
        parent = find_route_candidate(&source, ANCHOR_B);
        assert(parent != NULL);
        assert(parent->failure_count == 0u);
        assert(!parent->hold_down_valid);
        assert(parent->hold_down_until_ms == 0u);
        assert(parent->last_success_ms == 1030u);
        assert(source.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
        assert(source.outbox_record.valid);
        assert(source.outbox_record.delivery_state ==
               MESH_RELAY_DELIVERY_CUSTODY_ACCEPTED);

        assert(mesh_packet_semantic_digest(&packet,
                                           payload,
                                           payload_len,
                                           digest));
        memcpy(wrong_digest, digest, sizeof(wrong_digest));
        wrong_digest[sizeof(wrong_digest) - 1u] ^= UINT8_C(0x01);
        assert(mesh_relay_commit_next_hop_custody_terminal(
                   &source, &packet, wrong_digest) == PROTO_ERR_MALFORMED);
        assert(mesh_relay_tx_active(&source));
        assert(mesh_relay_commit_next_hop_custody_terminal(
                   &source, &packet, digest) == PROTO_OK);
        assert(!mesh_relay_tx_active(&source));
        assert(!source.outbox_record.valid);
    }
}

static void test_ordinary_transit_relay_retires_on_parent_hop_ack(void)
{
    static const uint8_t msg_types[] = {
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_MESH_DATA,
    };

    for (size_t i = 0u; i < sizeof(msg_types) / sizeof(msg_types[0]); i++) {
        struct mesh_relay relay;
        struct route_candidate route =
            direct_gateway_route(ANCHOR_C, 79u, 90u);
        struct proto_packet packet;
        struct proto_packet wrong_ack;
        struct proto_packet exact_ack;
        struct mesh_outbound retained;
        struct mesh_relay_result admission;
        struct mesh_relay_result result;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        uint8_t wrong_payload[UWB_MESH_MAX_PAYLOAD_LEN];
        uint8_t wrong_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        uint8_t exact_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        size_t payload_len;
        size_t wrong_ack_payload_len = 0u;
        size_t exact_ack_payload_len = 0u;
        const struct route_candidate *parent;

        route.hop_count = 1u;
        mesh_relay_init(
            &relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 79u);
        assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
        relay.upstream.candidates[relay.upstream.selected_index].failure_count =
            ROUTE_RETRIES_PER_CANDIDATE;
        parent = find_route_candidate(&relay, ANCHOR_C);
        assert(parent != NULL);
        assert(parent->failure_count == ROUTE_RETRIES_PER_CANDIDATE);
        payload_len = build_ordinary_gateway_report(
            msg_types[i],
            ANCHOR_A,
            UINT32_C(0x79000000) + (uint32_t)i + 1u,
            (uint16_t)(120u + i),
            &packet,
            payload,
            sizeof(payload));
        assert(mesh_relay_handle_rx(&relay,
                                    &packet,
                                    payload,
                                    payload_len,
                                    ANCHOR_A,
                                    85u,
                                    2000u,
                                    &admission) == PROTO_OK);
        assert(has_action(&admission, MESH_RELAY_ACTION_FORWARD));
        assert(has_action(&admission, MESH_RELAY_ACTION_SEND_HOP_ACK));
        assert(admission.hop_ack.next_hop_id == ANCHOR_A);
        assert(mesh_relay_start_tx(&relay,
                                   &admission.forward.packet,
                                   admission.forward.payload,
                                   admission.forward.payload_len,
                                   2010u,
                                   &retained) == PROTO_OK);
        assert(mesh_relay_bind_transit_previous_hop(
                   &relay, &retained, ANCHOR_A) == PROTO_OK);
        mesh_relay_note_tx_sent(&relay, &retained, 2020u);

        memcpy(wrong_payload, payload, payload_len);
        wrong_payload[payload_len - 1u] ^= UINT8_C(0x01);
        build_hop_ack_for_packet(&wrong_ack,
                                 ANCHOR_C,
                                 ANCHOR_B,
                                 (uint16_t)(220u + i),
                                 wrong_ack_payload,
                                 sizeof(wrong_ack_payload),
                                 &wrong_ack_payload_len,
                                 &packet,
                                 wrong_payload,
                                 payload_len);
        assert(mesh_relay_handle_rx(&relay,
                                    &wrong_ack,
                                    wrong_ack_payload,
                                    wrong_ack_payload_len,
                                    ANCHOR_C,
                                    90u,
                                    2030u,
                                    &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(mesh_relay_tx_active(&relay));

        build_hop_ack_for_packet(&exact_ack,
                                 ANCHOR_C,
                                 ANCHOR_B,
                                 (uint16_t)(230u + i),
                                 exact_ack_payload,
                                 sizeof(exact_ack_payload),
                                 &exact_ack_payload_len,
                                 &packet,
                                 payload,
                                 payload_len);
        assert(mesh_relay_handle_rx(&relay,
                                    &exact_ack,
                                    exact_ack_payload,
                                    exact_ack_payload_len,
                                    ANCHOR_C,
                                    90u,
                                    2040u,
                                    &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
        parent = find_route_candidate(&relay, ANCHOR_C);
        assert(parent != NULL);
        assert(parent->failure_count == 0u);
        assert(!parent->hold_down_valid);
        assert(parent->hold_down_until_ms == 0u);
        assert(parent->last_success_ms == 2040u);
        assert(!mesh_relay_tx_active(&relay));
        assert(!relay.outbox_record.valid);
    }
}

static void expect_current_packet_same_parent_retry(
    struct mesh_relay *source,
    uint64_t expected_parent_id,
    const struct proto_packet *original_packet,
    const uint8_t *original_payload,
    size_t original_payload_len,
    const uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint8_t expected_miss_count,
    uint32_t *now_ms)
{
    struct mesh_relay_result result;
    const struct route_candidate *parent;
    uint8_t retry_digest[SEMANTIC_DIGEST_SHA256_LEN];

    assert(source != NULL);
    assert(original_packet != NULL);
    assert(original_payload != NULL);
    assert(now_ms != NULL);

    *now_ms = source->pending.gateway_ack_deadline_ms;
    assert(mesh_relay_tick(source, *now_ms, &result) == PROTO_OK);
    assert(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(source->pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(source->pending.next_hop_id == expected_parent_id);
    assert(source->pending.parent_hop_ack_miss_count == expected_miss_count);
    parent = route_selected(&source->upstream);
    assert(parent != NULL);
    assert(parent->next_hop_id == expected_parent_id);
    assert(!parent->hold_down_valid);

    *now_ms = source->pending.retry_after_ms;
    assert(mesh_relay_tick(source, *now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(!has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(result.retransmit.next_hop_id == expected_parent_id);
    assert(result.retransmit.packet.msg_type == original_packet->msg_type);
    assert(result.retransmit.packet.src_id == original_packet->src_id);
    assert(result.retransmit.packet.dst_id == original_packet->dst_id);
    assert(result.retransmit.packet.session_id == original_packet->session_id);
    assert(result.retransmit.packet.seq == original_packet->seq);
    assert(mesh_packet_semantic_digest(&result.retransmit.packet,
                                       result.retransmit.payload,
                                       result.retransmit.payload_len,
                                       retry_digest));
    assert(semantic_digest_equal(original_digest,
                                 retry_digest,
                                 sizeof(retry_digest)));
    assert(result.retransmit.payload_len == original_payload_len);
    assert(memcmp(result.retransmit.payload,
                  original_payload,
                  original_payload_len) == 0);
    mesh_relay_note_tx_sent(source, &result.retransmit, *now_ms);
}

static void test_exact_hop_custody_restarts_same_parent_retry_budget(void)
{
    struct mesh_relay source;
    struct route_candidate route =
        direct_gateway_route(ANCHOR_B, 81u, 90u);
    struct proto_packet accepted;
    struct proto_packet next;
    struct proto_packet hop_ack;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    const struct route_candidate *parent;
    uint8_t accepted_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t next_payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t hop_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t accepted_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t next_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t accepted_payload_len;
    size_t next_payload_len;
    size_t hop_ack_payload_len = 0u;
    uint32_t now_ms = 1000u;

    route.hop_count = 1u;
    mesh_relay_init(
        &source, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 81u);
    assert(route_upsert_candidate(&source.upstream, &route) == PROTO_OK);

    accepted_payload_len = build_ordinary_gateway_report(
        MSG_CLICK_REPORT,
        ANCHOR_A,
        UINT32_C(0x81000001),
        181u,
        &accepted,
        accepted_payload,
        sizeof(accepted_payload));
    assert(mesh_relay_start_tx(&source,
                               &accepted,
                               accepted_payload,
                               accepted_payload_len,
                               now_ms,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&source, &tx, now_ms);
    assert(source.pending.parent_hop_ack_miss_count == 0u);
    assert(mesh_packet_semantic_digest(&accepted,
                                       accepted_payload,
                                       accepted_payload_len,
                                       accepted_digest));
    for (uint8_t miss = 0u; miss < 2u; miss++) {
        expect_current_packet_same_parent_retry(&source,
                                                ANCHOR_B,
                                                &accepted,
                                                accepted_payload,
                                                accepted_payload_len,
                                                accepted_digest,
                                                (uint8_t)(miss + 1u),
                                                &now_ms);
    }
    build_hop_ack_for_packet(&hop_ack,
                             ANCHOR_B,
                             ANCHOR_A,
                             281u,
                             hop_ack_payload,
                             sizeof(hop_ack_payload),
                             &hop_ack_payload_len,
                             &accepted,
                             accepted_payload,
                             accepted_payload_len);
    assert(mesh_relay_handle_rx(&source,
                                &hop_ack,
                                hop_ack_payload,
                                hop_ack_payload_len,
                                ANCHOR_B,
                                90u,
                                now_ms + 10u,
                                &result) == PROTO_OK);
    assert(result.actions ==
           MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
    parent = find_route_candidate(&source, ANCHOR_B);
    assert(parent != NULL);
    assert(parent->failure_count == 0u);
    assert(parent->last_success_ms == now_ms + 10u);
    assert(mesh_relay_commit_next_hop_custody_terminal(
               &source, &accepted, accepted_digest) == PROTO_OK);
    assert(!mesh_relay_tx_active(&source));

    now_ms += 1000u;
    next_payload_len = build_ordinary_gateway_report(
        MSG_CLICK_REPORT,
        ANCHOR_A,
        UINT32_C(0x81000002),
        182u,
        &next,
        next_payload,
        sizeof(next_payload));
    assert(mesh_packet_semantic_digest(&next,
                                       next_payload,
                                       next_payload_len,
                                       next_digest));
    assert(mesh_relay_start_tx(&source,
                               &next,
                               next_payload,
                               next_payload_len,
                               now_ms,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&source, &tx, now_ms);
    assert(source.pending.parent_hop_ack_miss_count == 0u);

    for (uint8_t miss = 0u; miss < ROUTE_RETRIES_PER_CANDIDATE; miss++) {
        expect_current_packet_same_parent_retry(&source,
                                                ANCHOR_B,
                                                &next,
                                                next_payload,
                                                next_payload_len,
                                                next_digest,
                                                (uint8_t)(miss + 1u),
                                                &now_ms);
    }

    now_ms = source.pending.gateway_ack_deadline_ms;
    assert(mesh_relay_tick(&source, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(!has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(mesh_relay_tx_active(&source));
    assert(source.pending.packet.msg_type == next.msg_type);
    assert(source.pending.packet.src_id == next.src_id);
    assert(source.pending.packet.session_id == next.session_id);
    assert(source.pending.packet.seq == next.seq);
    assert(source.pending.payload_len == next_payload_len);
    assert(memcmp(source.pending.payload,
                  next_payload,
                  next_payload_len) == 0);
    parent = find_route_candidate(&source, ANCHOR_B);
    assert(parent != NULL);
    assert(parent->hold_down_valid);
    assert(route_selected(&source.upstream) == NULL);
}

static void test_stale_route_debt_does_not_consume_fresh_packet_retries(void)
{
    struct mesh_relay source;
    struct route_candidate route =
        direct_gateway_route(ANCHOR_B, 82u, 90u);
    struct proto_packet packet;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    const struct route_candidate *parent;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t payload_len;
    uint32_t now_ms = 1000u;

    route.hop_count = 1u;
    mesh_relay_init(
        &source, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 82u);
    assert(route_upsert_candidate(&source.upstream, &route) == PROTO_OK);
    source.upstream.candidates[source.upstream.selected_index].failure_count =
        ROUTE_RETRIES_PER_CANDIDATE;
    parent = route_selected(&source.upstream);
    assert(parent != NULL);
    assert(parent->next_hop_id == ANCHOR_B);
    assert(parent->failure_count == ROUTE_RETRIES_PER_CANDIDATE);

    payload_len = build_ordinary_gateway_report(
        MSG_CLICK_REPORT,
        ANCHOR_A,
        UINT32_C(0x82000001),
        183u,
        &packet,
        payload,
        sizeof(payload));
    assert(mesh_packet_semantic_digest(&packet,
                                       payload,
                                       payload_len,
                                       digest));
    assert(mesh_relay_start_tx(&source,
                               &packet,
                               payload,
                               payload_len,
                               now_ms,
                               &tx) == PROTO_OK);
    assert(tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&source, &tx, now_ms);
    assert(source.pending.parent_hop_ack_miss_count == 0u);

    for (uint8_t miss = 0u; miss < ROUTE_RETRIES_PER_CANDIDATE; miss++) {
        expect_current_packet_same_parent_retry(&source,
                                                ANCHOR_B,
                                                &packet,
                                                payload,
                                                payload_len,
                                                digest,
                                                (uint8_t)(miss + 1u),
                                                &now_ms);
    }

    now_ms = source.pending.gateway_ack_deadline_ms;
    assert(mesh_relay_tick(&source, now_ms, &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED));
    assert(!has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(mesh_relay_tx_active(&source));
    assert(source.pending.packet.msg_type == packet.msg_type);
    assert(source.pending.packet.src_id == packet.src_id);
    assert(source.pending.packet.session_id == packet.session_id);
    assert(source.pending.packet.seq == packet.seq);
    assert(source.pending.payload_len == payload_len);
    assert(memcmp(source.pending.payload, payload, payload_len) == 0);
}

static void test_gateway_adjacent_relay_consumes_exact_gateway_ack(void)
{
    static const uint8_t msg_types[] = {
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_MESH_DATA,
    };

    for (size_t i = 0u; i < sizeof(msg_types) / sizeof(msg_types[0]); i++) {
        struct mesh_relay relay;
        struct route_candidate route =
            direct_gateway_route(GATEWAY, 80u, 95u);
        struct proto_packet packet;
        struct proto_packet wrong_ack;
        struct proto_packet exact_ack;
        struct proto_packet confirm_ack;
        struct mesh_outbound retained;
        struct mesh_outbound confirm;
        struct mesh_relay_result admission;
        struct mesh_relay_result result;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        uint8_t wrong_payload[UWB_MESH_MAX_PAYLOAD_LEN];
        uint8_t wrong_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        uint8_t exact_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        uint8_t confirm_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
        size_t payload_len;
        size_t wrong_ack_payload_len = 0u;
        size_t exact_ack_payload_len = 0u;
        size_t confirm_ack_payload_len = 0u;

        mesh_relay_init(
            &relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 80u);
        assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
        seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 80u, 1u, 90u, 3000u);
        payload_len = build_ordinary_gateway_report(
            msg_types[i],
            ANCHOR_A,
            UINT32_C(0x7a000000) + (uint32_t)i + 1u,
            (uint16_t)(140u + i),
            &packet,
            payload,
            sizeof(payload));
        assert(mesh_relay_handle_rx(&relay,
                                    &packet,
                                    payload,
                                    payload_len,
                                    ANCHOR_A,
                                    90u,
                                    3010u,
                                    &admission) == PROTO_OK);
        assert(has_action(&admission, MESH_RELAY_ACTION_FORWARD));
        assert(has_action(&admission, MESH_RELAY_ACTION_SEND_HOP_ACK));
        assert(mesh_relay_start_tx(&relay,
                                   &admission.forward.packet,
                                   admission.forward.payload,
                                   admission.forward.payload_len,
                                   3020u,
                                   &retained) == PROTO_OK);
        assert(mesh_relay_bind_transit_previous_hop(
                   &relay, &retained, ANCHOR_A) == PROTO_OK);
        mesh_relay_note_tx_sent(&relay, &retained, 3030u);

        memcpy(wrong_payload, payload, payload_len);
        wrong_payload[payload_len - 1u] ^= UINT8_C(0x01);
        build_gateway_ack_for_packet(&wrong_ack,
                                     wrong_ack_payload,
                                     sizeof(wrong_ack_payload),
                                     &wrong_ack_payload_len,
                                     ANCHOR_A,
                                     (uint16_t)(240u + i),
                                     &packet,
                                     wrong_payload,
                                     payload_len);
        assert(mesh_relay_handle_rx(&relay,
                                    &wrong_ack,
                                    wrong_ack_payload,
                                    wrong_ack_payload_len,
                                    GATEWAY,
                                    95u,
                                    3040u,
                                    &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_NONE);
        assert(mesh_relay_tx_active(&relay));

        build_gateway_ack_for_packet(&exact_ack,
                                     exact_ack_payload,
                                     sizeof(exact_ack_payload),
                                     &exact_ack_payload_len,
                                     ANCHOR_A,
                                     (uint16_t)(250u + i),
                                     &packet,
                                     payload,
                                     payload_len);
        assert(mesh_relay_handle_rx(&relay,
                                    &exact_ack,
                                    exact_ack_payload,
                                    exact_ack_payload_len,
                                    GATEWAY,
                                    95u,
                                    3050u,
                                    &result) == PROTO_OK);
        assert(result.actions ==
               MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
        assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
        assert(!has_action(
            &result,
            MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING));
        assert(mesh_relay_tx_active(&relay));
        assert(relay.pending.gateway_ack_confirm_pending);
        assert(relay.pending.packet.src_id == ANCHOR_A);

        assert(mesh_relay_tick(&relay, 3051u, &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_RETRANSMIT);
        assert(result.retransmit.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
        assert(result.retransmit.packet.src_id == ANCHOR_A);
        assert(result.retransmit.next_hop_id == GATEWAY);
        confirm = result.retransmit;
        mesh_relay_note_tx_sent(&relay, &confirm, 3051u);
        build_gateway_ack_for_packet(&confirm_ack,
                                     confirm_ack_payload,
                                     sizeof(confirm_ack_payload),
                                     &confirm_ack_payload_len,
                                     ANCHOR_A,
                                     (uint16_t)(260u + i),
                                     &confirm.packet,
                                     confirm.payload,
                                     confirm.payload_len);
        assert(mesh_relay_handle_rx(&relay,
                                    &confirm_ack,
                                    confirm_ack_payload,
                                    confirm_ack_payload_len,
                                    GATEWAY,
                                    95u,
                                    3060u,
                                    &result) == PROTO_OK);
        assert(result.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
        assert(mesh_relay_tx_active(&relay));
        assert(mesh_relay_commit_gateway_ack_confirm_terminal(
                   &relay,
                   &confirm.packet,
                   confirm.payload,
                   confirm.payload_len,
                   3061u) == PROTO_OK);
        assert(!mesh_relay_tx_active(&relay));
        assert(!relay.outbox_record.valid);
    }
}

static void test_relayed_gateway_history_replaces_only_after_ack_confirm(void)
{
    static const uint64_t sources[] = {ANCHOR_A, ANCHOR_C, ANCHOR_D};
    static const uint8_t msg_types[] = {
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_MESH_DATA,
        MSG_CLICK_REPORT,
        MSG_SELF_TEST_REPORT,
        MSG_MESH_DATA,
    };
    struct mesh_relay gateway;
    struct mesh_relay relay;
    struct route_candidate route =
        direct_gateway_route(GATEWAY, 81u, 95u);
    struct proto_packet latest_packets[sizeof(sources) / sizeof(sources[0])];
    uint8_t latest_payloads[sizeof(sources) / sizeof(sources[0])]
                           [UWB_MESH_MAX_PAYLOAD_LEN];
    size_t latest_payload_lens[sizeof(sources) / sizeof(sources[0])] = {0u};
    uint32_t now_ms = 4000u;

    mesh_relay_init(
        &gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 81u);
    mesh_relay_init(
        &relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 81u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    for (size_t round = 0u;
         round < sizeof(msg_types) / sizeof(msg_types[0]);
         round++) {
        for (size_t child = 0u;
             child < sizeof(sources) / sizeof(sources[0]);
             child++) {
            struct proto_packet packet;
            struct mesh_gateway_ack_confirm_identity confirm_identity;
            struct mesh_outbound retained;
            struct mesh_outbound gateway_ack;
            struct mesh_outbound confirm;
            struct mesh_relay_result relay_result;
            struct mesh_relay_result gateway_result;
            uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
            size_t payload_len = build_ordinary_gateway_report(
                msg_types[round],
                sources[child],
                UINT32_C(0x7b000000) + (uint32_t)(round * 16u + child + 1u),
                (uint16_t)(300u + round * 8u + child),
                &packet,
                payload,
                sizeof(payload));

            assert(mesh_relay_handle_rx(&relay,
                                        &packet,
                                        payload,
                                        payload_len,
                                        sources[child],
                                        90u,
                                        now_ms++,
                                        &relay_result) == PROTO_OK);
            assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
            assert(has_action(
                &relay_result, MESH_RELAY_ACTION_SEND_HOP_ACK));
            assert(relay_result.hop_ack.next_hop_id == sources[child]);
            assert(mesh_relay_start_tx(&relay,
                                       &relay_result.forward.packet,
                                       relay_result.forward.payload,
                                       relay_result.forward.payload_len,
                                       now_ms++,
                                       &retained) == PROTO_OK);
            assert(mesh_relay_bind_transit_previous_hop(
                       &relay, &retained, sources[child]) == PROTO_OK);
            mesh_relay_note_tx_sent(&relay, &retained, now_ms++);

            assert(mesh_relay_handle_rx(&gateway,
                                        &retained.packet,
                                        retained.payload,
                                        retained.payload_len,
                                        ANCHOR_B,
                                        95u,
                                        now_ms++,
                                        &gateway_result) == PROTO_OK);
            assert(gateway_result.status == PROTO_OK);
            assert(gateway_result.actions ==
                   MESH_RELAY_ACTION_DELIVER_LOCAL);
            assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                      &retained.packet,
                                                      retained.payload,
                                                      retained.payload_len,
                                                      ANCHOR_B,
                                                      now_ms++,
                                                      &gateway_result) ==
                   PROTO_OK);
            assert(gateway_result.actions ==
                   MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
            assert(gateway_result.gateway_ack.packet.dst_id ==
                   sources[child]);
            assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_B);
            gateway_ack = gateway_result.gateway_ack;
            assert(mesh_relay_gateway_identity_confirmation_pending(
                &gateway,
                retained.packet.src_id,
                retained.packet.msg_type,
                retained.packet.session_id,
                retained.packet.seq,
                now_ms));

            /* Until ACK_CONFIRM arrives, the exact history remains live and
             * repairs a lost gateway ACK without semantic redelivery. */
            assert(mesh_relay_handle_rx(&gateway,
                                        &retained.packet,
                                        retained.payload,
                                        retained.payload_len,
                                        ANCHOR_B,
                                        95u,
                                        now_ms++,
                                        &gateway_result) == PROTO_OK);
            assert(gateway_result.status == PROTO_ERR_STALE);
            assert(has_action(
                &gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
            assert(!has_action(
                &gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));

            assert(mesh_relay_handle_rx(&relay,
                                        &gateway_ack.packet,
                                        gateway_ack.payload,
                                        gateway_ack.payload_len,
                                        GATEWAY,
                                        95u,
                                        now_ms++,
                                        &relay_result) == PROTO_OK);
            assert(relay_result.actions ==
                   MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
            assert(!has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
            assert(mesh_relay_tick(&relay, now_ms++, &relay_result) ==
                   PROTO_OK);
            assert(relay_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
            confirm = relay_result.retransmit;
            assert(confirm.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
            assert(confirm.packet.src_id == sources[child]);
            assert(confirm.packet.dst_id == GATEWAY);
            assert(confirm.packet.ttl == retained.packet.ttl);
            assert(confirm.packet.ttl < MESH_GATEWAY_ACK_TTL);
            assert(confirm.next_hop_id == GATEWAY);
            assert(mesh_packet_rx_semantics_validate(&confirm.packet,
                                                     confirm.payload,
                                                     confirm.payload_len,
                                                     ANCHOR_B,
                                                     GATEWAY,
                                                     GATEWAY) == PROTO_OK);
            mesh_relay_note_tx_sent(&relay, &confirm, now_ms++);

            assert(mesh_relay_handle_rx(&gateway,
                                        &confirm.packet,
                                        confirm.payload,
                                        confirm.payload_len,
                                        ANCHOR_B,
                                        95u,
                                        now_ms++,
                                        &gateway_result) == PROTO_OK);
            assert(gateway_result.actions ==
                   MESH_RELAY_ACTION_DELIVER_LOCAL);
            assert(mesh_relay_gateway_ack_confirm_history_match(
                       &gateway,
                       &confirm.packet,
                       confirm.payload,
                       confirm.payload_len,
                       &confirm_identity) == PROTO_OK);
            assert(confirm_identity.msg_type == retained.packet.msg_type);
            assert(confirm_identity.session_id == retained.packet.session_id);
            assert(confirm_identity.seq == retained.packet.seq);
            assert(!mesh_relay_gateway_identity_confirmation_pending(
                &gateway,
                retained.packet.src_id,
                retained.packet.msg_type,
                retained.packet.session_id,
                retained.packet.seq,
                now_ms));
            assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                      &confirm.packet,
                                                      confirm.payload,
                                                      confirm.payload_len,
                                                      ANCHOR_B,
                                                      now_ms++,
                                                      &gateway_result) ==
                   PROTO_OK);
            assert(gateway_result.actions ==
                   MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
            assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_B);
            assert(mesh_relay_handle_rx(&relay,
                                        &gateway_result.gateway_ack.packet,
                                        gateway_result.gateway_ack.payload,
                                        gateway_result.gateway_ack.payload_len,
                                        GATEWAY,
                                        95u,
                                        now_ms++,
                                        &relay_result) == PROTO_OK);
            assert(relay_result.actions ==
                   MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
            assert(mesh_relay_commit_gateway_ack_confirm_terminal(
                       &relay,
                       &confirm.packet,
                       confirm.payload,
                       confirm.payload_len,
                       now_ms++) == PROTO_OK);
            assert(!mesh_relay_tx_active(&relay));

            if (round + 1u ==
                sizeof(msg_types) / sizeof(msg_types[0])) {
                latest_packets[child] = retained.packet;
                memcpy(latest_payloads[child],
                       retained.payload,
                       retained.payload_len);
                latest_payload_lens[child] = retained.payload_len;
            }
        }
    }

    for (size_t child = 0u;
         child < sizeof(sources) / sizeof(sources[0]);
         child++) {
        struct mesh_relay_result result;
        uint8_t conflicting_payload[UWB_MESH_MAX_PAYLOAD_LEN];

        now_ms += ROUTE_DEDUP_WINDOW_MS + 1u;
        assert(mesh_relay_handle_rx(&gateway,
                                    &latest_packets[child],
                                    latest_payloads[child],
                                    latest_payload_lens[child],
                                    ANCHOR_B,
                                    95u,
                                    now_ms++,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_ERR_STALE);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(result.gateway_ack.next_hop_id == ANCHOR_B);

        memcpy(conflicting_payload,
               latest_payloads[child],
               latest_payload_lens[child]);
        conflicting_payload[latest_payload_lens[child] - 1u] ^=
            UINT8_C(0x01);
        assert(mesh_relay_handle_rx(&gateway,
                                    &latest_packets[child],
                                    conflicting_payload,
                                    latest_payload_lens[child],
                                    ANCHOR_B,
                                    95u,
                                    now_ms++,
                                    &result) == PROTO_OK);
        assert(result.status == PROTO_ERR_MALFORMED);
        assert(result.actions == MESH_RELAY_ACTION_DROP);
    }
}

static void test_direct_gateway_history_survives_lost_ack_and_confirm_ack(void)
{
    enum {
        report_count =
            MESH_RELAY_GATEWAY_ACK_GUARANTEED_IDENTITIES_PER_ORIGIN + 3u,
    };
    struct mesh_relay gateway;
    struct mesh_relay source;
    struct route_candidate route =
        direct_gateway_route(GATEWAY, 82u, 95u);
    uint32_t now_ms = 9000u;

    mesh_relay_init(
        &gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 82u);
    mesh_relay_init(
        &source, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 82u);
    assert(route_upsert_candidate(&source.upstream, &route) == PROTO_OK);

    for (uint16_t ordinal = 0u; ordinal < report_count; ordinal++) {
        struct proto_packet packet;
        struct mesh_gateway_ack_confirm_identity confirm_identity;
        struct mesh_outbound tx;
        struct mesh_outbound first_gateway_ack;
        struct mesh_outbound confirm;
        struct mesh_relay_result source_result;
        struct mesh_relay_result gateway_result;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t payload_len = build_ordinary_gateway_report(
            MSG_CLICK_REPORT,
            ANCHOR_A,
            UINT32_C(0x82000000) + (uint32_t)ordinal + 1u,
            (uint16_t)(400u + ordinal),
            &packet,
            payload,
            sizeof(payload));

        assert(mesh_relay_start_tx(&source,
                                   &packet,
                                   payload,
                                   payload_len,
                                   now_ms++,
                                   &tx) == PROTO_OK);
        assert(tx.next_hop_id == GATEWAY);
        mesh_relay_note_tx_sent(&source, &tx, now_ms++);

        assert(mesh_relay_handle_rx(&gateway,
                                    &tx.packet,
                                    tx.payload,
                                    tx.payload_len,
                                    ANCHOR_A,
                                    95u,
                                    now_ms++,
                                    &gateway_result) == PROTO_OK);
        assert(gateway_result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &tx.packet,
                                                  tx.payload,
                                                  tx.payload_len,
                                                  ANCHOR_A,
                                                  now_ms++,
                                                  &gateway_result) ==
               PROTO_OK);
        assert(gateway_result.actions ==
               MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
        assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_A);
        first_gateway_ack = gateway_result.gateway_ack;
        assert(mesh_relay_gateway_identity_confirmation_pending(
            &gateway,
            tx.packet.src_id,
            tx.packet.msg_type,
            tx.packet.session_id,
            tx.packet.seq,
            now_ms));

        /* Drop the first gateway ACK. The retry must replay that ACK from
         * history without asking the host to accept the report again. */
        assert(mesh_relay_handle_rx(&gateway,
                                    &tx.packet,
                                    tx.payload,
                                    tx.payload_len,
                                    ANCHOR_A,
                                    95u,
                                    now_ms++,
                                    &gateway_result) == PROTO_OK);
        assert(gateway_result.status == PROTO_ERR_STALE);
        assert(has_action(
            &gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(!has_action(
            &gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(gateway_result.gateway_ack.packet.dst_id ==
               first_gateway_ack.packet.dst_id);
        assert(gateway_result.gateway_ack.packet.session_id ==
               first_gateway_ack.packet.session_id);
        assert(gateway_result.gateway_ack.payload_len ==
               first_gateway_ack.payload_len);
        assert(memcmp(gateway_result.gateway_ack.payload,
                      first_gateway_ack.payload,
                      first_gateway_ack.payload_len) == 0);

        assert(mesh_relay_handle_rx(&source,
                                    &gateway_result.gateway_ack.packet,
                                    gateway_result.gateway_ack.payload,
                                    gateway_result.gateway_ack.payload_len,
                                    GATEWAY,
                                    95u,
                                    now_ms++,
                                    &source_result) == PROTO_OK);
        assert(source_result.actions ==
               MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING);
        assert(source.pending.gateway_ack_confirm_pending);
        assert(mesh_relay_tick(&source, now_ms++, &source_result) ==
               PROTO_OK);
        assert(source_result.actions == MESH_RELAY_ACTION_RETRANSMIT);
        confirm = source_result.retransmit;
        assert(confirm.packet.msg_type == MSG_GATEWAY_ACK_CONFIRM);
        assert(confirm.packet.src_id == ANCHOR_A);
        assert(confirm.next_hop_id == GATEWAY);
        mesh_relay_note_tx_sent(&source, &confirm, now_ms++);

        assert(mesh_relay_handle_rx(&gateway,
                                    &confirm.packet,
                                    confirm.payload,
                                    confirm.payload_len,
                                    ANCHOR_A,
                                    95u,
                                    now_ms++,
                                    &gateway_result) == PROTO_OK);
        assert(gateway_result.actions == MESH_RELAY_ACTION_DELIVER_LOCAL);
        assert(mesh_relay_gateway_ack_confirm_history_match(
                   &gateway,
                   &confirm.packet,
                   confirm.payload,
                   confirm.payload_len,
                   &confirm_identity) == PROTO_OK);
        assert(confirm_identity.msg_type == tx.packet.msg_type);
        assert(confirm_identity.session_id == tx.packet.session_id);
        assert(confirm_identity.seq == tx.packet.seq);
        assert(mesh_relay_commit_gateway_delivery(&gateway,
                                                  &confirm.packet,
                                                  confirm.payload,
                                                  confirm.payload_len,
                                                  ANCHOR_A,
                                                  now_ms++,
                                                  &gateway_result) ==
               PROTO_OK);
        assert(gateway_result.actions ==
               MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
        assert(!mesh_relay_gateway_identity_confirmation_pending(
            &gateway,
            tx.packet.src_id,
            tx.packet.msg_type,
            tx.packet.session_id,
            tx.packet.seq,
            now_ms));

        /* Drop the first ACK of ACK_CONFIRM as well. The duplicate terminal
         * proof must be ACKed again without consuming another history slot. */
        assert(mesh_relay_handle_rx(&gateway,
                                    &confirm.packet,
                                    confirm.payload,
                                    confirm.payload_len,
                                    ANCHOR_A,
                                    95u,
                                    now_ms++,
                                    &gateway_result) == PROTO_OK);
        assert(gateway_result.status == PROTO_ERR_STALE);
        assert(has_action(
            &gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
        assert(!has_action(
            &gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
        assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_A);

        assert(mesh_relay_handle_rx(&source,
                                    &gateway_result.gateway_ack.packet,
                                    gateway_result.gateway_ack.payload,
                                    gateway_result.gateway_ack.payload_len,
                                    GATEWAY,
                                    95u,
                                    now_ms++,
                                    &source_result) == PROTO_OK);
        assert(source_result.actions ==
               MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
        assert(mesh_relay_commit_gateway_ack_confirm_terminal(
                   &source,
                   &confirm.packet,
                   confirm.payload,
                   confirm.payload_len,
                   now_ms++) == PROTO_OK);
        assert(!mesh_relay_tx_active(&source));
        assert(!source.outbox_record.valid);
    }
}


int main(void)
{
    test_mesh_relay_result_preserves_required_simultaneous_outputs();
    test_route_reply_nonce_value_and_identity();
    test_clicker_leaf_rejects_command_result_origin();
    test_clicker_leaf_route_requests_only_own_gateway();
    test_clicker_leaf_route_reply_requires_latest_active_request();
    test_clicker_leaf_pending_controls_require_correlation();
    test_clicker_leaf_forbidden_receive_is_inert();
    test_clicker_leaf_event_controls_require_selected_parent();
    test_transit_reverse_route_commits_only_after_exact_custody();
    test_ordinary_report_source_retires_on_exact_parent_hop_ack();
    test_ordinary_transit_relay_retires_on_parent_hop_ack();
    test_exact_hop_custody_restarts_same_parent_retry_budget();
    test_stale_route_debt_does_not_consume_fresh_packet_retries();
    test_gateway_adjacent_relay_consumes_exact_gateway_ack();
    test_relayed_gateway_history_replaces_only_after_ack_confirm();
    test_direct_gateway_history_survives_lost_ack_and_confirm_ack();
    test_relay_replays_hop_ack_for_accepted_gateway_report_duplicate();
    test_range_retry_attempts_do_not_conflict_in_relay_dedup();
    test_pending_retry_requires_exact_payload();
    test_duplicate_cache_expires_by_time_window();
    test_channel9_tx_plans_next_local_tx_slot_without_mutating_timing();
    test_status_tlvs_report_selected_route();
    test_status_tlvs_report_missing_route_reason();
    test_legacy_route_beacons_are_dropped();
    test_malformed_broadcast_command_fails_closed();
    test_gateway_command_envelope_rejects_before_state_mutation();
    test_group_scope_broadcast_fails_closed_before_local_delivery();
    test_targeted_gateway_command_commits_only_after_local_admission();
    test_command_flood_broadcast_deduplicates_logical_sequence();
    test_single_assignment_table_relays_three_copies_across_two_hops();
    test_scheduled_survey_controls_reuse_compact_relay_wave();
    test_collection_eack_broadcast_deduplicates_exact_round_only();
    test_collection_eack_received_list_confirms_pending_result();
    test_recovery_eack_releases_only_exact_packet_and_bundle();
    test_four_eack_broadcasts_cover_fifty_anchor_rosters();
    test_collection_eack_received_list_schedules_retry_when_not_received();
    test_collection_eack_missing_list_schedules_patient_retry();
    test_collection_eack_missing_list_absent_node_confirms_delivery();
    test_collection_eack_closed_stops_pending_without_success();
    test_collection_eack_closed_roster_bitmap_stops_pending_without_list();
    test_collection_result_survives_route_loss_until_eack();
    test_click_preemption_preserves_pending_collection_result();
    test_collection_result_timeout_uses_collection_retry_round();
    test_collection_result_expiry_requires_exact_terminal_commit();
    test_collection_outbox_snapshot_restores_after_reinit();
    test_collection_outbox_snapshot_preserves_retry_round_delay();
    test_collection_outbox_snapshot_rejects_corrupt_payload();
    test_crc16_collision_cannot_alias_outbox_snapshot_payload();
    test_collection_outbox_snapshot_rejects_completed_record();
    test_collection_eack_broadcast_rejects_wrong_gateway_epoch();
    test_collection_recovery_eack_refloods_fresh_attempt_across_epoch();
    test_downlink_route_selection_uses_weighted_quality();
    test_start_tx_accepts_aged_upstream_route_until_failures();
    test_route_normalization_prevents_direct_fastpath_misclassification();
    test_downlink_next_hop_send_completes_immediately();
    test_downlink_send_refreshes_route_age();
    test_ttl_zero_packet_is_dropped_without_ack();
    test_zero_session_or_sequence_mesh_packets_are_rejected();
    test_busy_relay_transfers_gateway_report_to_bounded_queue();
    test_live_duplicate_child_retry_replays_hop_ack_without_reforward();
    test_busy_relay_still_delivers_direct_local_command();
    test_busy_gateway_still_accepts_direct_route_probe();
    test_busy_relay_repairs_queued_report_hop_ack();
    test_busy_relay_sends_result_busy_for_command_result();
    test_result_busy_preserves_command_result_identity();
    test_result_offer_gets_result_grant_when_parent_has_capacity();
    test_result_offer_reservation_expires_without_sliding();
    test_result_offer_reservation_deadline_zero_wraps_safely();
    test_result_transfer_requires_matching_offer_len_digest();
    test_crc16_collision_cannot_cross_result_offer_reservation_snapshot();
    test_result_offer_rejects_wrong_gateway_epoch();
    test_result_offer_rejects_cross_child_and_zero_identity();
    test_result_offer_gets_result_busy_when_parent_busy();
    test_result_offer_gets_result_busy_when_parent_capacity_red();
    test_large_command_result_starts_result_offer();
    test_missing_result_grant_uses_identity_scoped_fresh_random_backoff();
    test_missing_result_grant_retries_exact_identity_then_exhausts();
    test_result_offer_deadline_is_terminal();
    test_result_grant_releases_pending_command_result();
    test_forwarded_child_result_offer_snapshot_restores_after_reinit();
    test_forwarded_child_result_payload_snapshot_restores_after_grant();
    test_result_busy_retries_result_offer_not_payload();
    test_result_busy_preserves_proven_route_across_deadline_wrap();
    test_result_busy_ignores_mismatched_command_result_identity();
    test_relay_busy_defers_matching_pending_tx();
    test_consecutive_relay_busy_retries_preserve_packet_and_escalate();
    test_result_busy_and_small_grant_use_fresh_random_backoff();
    test_collection_retry_uses_fresh_random_for_same_identity_round();
    test_alternate_parent_selection_preserves_immediate_retry();
    test_local_gateway_bound_tx_waits_for_gateway_ack();
    test_gateway_reset_replays_immutable_original_after_new_epoch();
    test_transit_ack_confirm_without_original_expires_explicitly();
    test_delayed_old_ack_at_retention_boundary_expires_original();
    test_cancel_tx_if_matches_never_releases_another_exact_owner();
    test_local_gateway_bound_tx_accepts_batched_gateway_ack();
    test_start_tx_initializes_earliest_tx_time();
    test_relayed_tx_waits_for_gateway_ack_after_next_hop_send();
    test_gateway_ack_timeout_retries_then_requests_route_discovery();
    test_child_retry_is_acked_while_transit_custody_has_no_parent();
    test_gateway_ack_timeout_handles_ms_wrap();
    test_deferred_retransmit_waits_for_actual_radio_send();
    test_route_discovery_reaches_ttl_eight_with_backoff();
    test_route_discovery_preprobe_gate_is_same_target_and_due_aware();
    test_route_discovery_retry_sequence_handles_wrap_and_jitter();
    test_idle_route_solicit_without_upstream_is_forwarded_once();
    test_route_request_capacity_failure_does_not_poison_retry();
    test_better_route_request_copy_updates_pending_forward();
    test_route_request_retry_uses_new_flood_identity();
    test_unanswered_timed_route_request_does_not_reserve_channel9();
    test_parent_relay_replies_without_child_route_discovery();
    test_parent_relay_rejects_existing_child_route_request_while_connected();
    test_route_request_expires_stale_channel9_before_capacity_check();
    test_parent_relay_rejects_unrelated_child_route_request_while_connected();
    test_gateway_route_advertisement_seeds_and_floods_parent_candidates();
    test_gateway_route_advertisement_retains_distinct_parent_copy();
    test_gateway_route_epoch_serial_wrap_and_ambiguity();
    test_route_ancestry_blocks_three_node_cycle_and_accepts_churn();
    test_gateway_route_advertisement_snapshot_rebuild_is_exact();
    test_gateway_route_advertisement_reports_busy_capacity();
    test_route_discovery_ready_resets_attempt_budget();
    test_retry_and_route_discovery_backoff_apply_jitter();
    test_collection_retry_delay_uses_symmetric_jitter();
    test_held_down_candidate_can_return_after_hold_down();
    test_forced_route_invalidation_clears_routes_and_discovery_state();
    test_configured_route_epoch_transition_is_atomic_and_runs_once();
    test_local_route_clear_preserves_gateway_epoch();
    test_forwarded_gateway_bound_packet_sends_hop_ack();
    test_hop_ack_transfers_terminal_custody_without_timeout_extension();
    test_wrong_hop_ack_cannot_transfer_terminal_custody();
    test_assignment_result_transfers_terminal_custody_to_parent();
    test_assignment_claim_transfers_terminal_custody_to_parent();
    test_assignment_immediate_ack_miss_enters_prompt_retry_backoff();
    test_local_acks_require_expected_physical_hop();
    test_hop_ack_outbox_survives_reset_until_gateway_ack();
    test_gateway_reaches_anchor_behind_relay_and_receives_result();
    test_crc16_collision_cannot_alias_duplicate_or_gateway_ack_history();
    test_gateway_ack_confirm_barrier_requires_exact_accepted_history();
    test_gateway_origin_reboot_releases_unconfirmable_ack_debt();
    test_gateway_ack_batch_and_roster_transitions_preserve_live_debt();
    test_gateway_ack_history_holds_full_source_backlog_before_confirm();
    test_gateway_ack_overflow_preserves_all_origin_guarantees();
    test_gateway_ordinary_command_result_duplicate_remains_sticky();
    test_gateway_ack_history_isolates_noisy_origin();
    test_gateway_ack_history_rejects_malformed_batch_metadata();
    test_gateway_ack_history_reserves_maximum_append_only_roster();
    test_gateway_ack_candidate_replaces_only_source_oldest_identity();
    test_gateway_route_probe_churn_cannot_exhaust_ack_history();
    test_gateway_delivery_commit_rejects_invalid_contract();
    test_generic_gateway_ack_cannot_complete_collection_eack_custody();
    test_gateway_delivers_direct_clicker_self_test_report_and_acks();
    test_gateway_ack_uses_exact_ingress_peer_across_route_churn();
    test_late_gateway_ack_bypasses_only_unrelated_core_owner();
    test_reactive_gateway_route_request_and_reply();
    test_child_route_reply_preserves_upstream_and_transit_custody();
    test_gateway_route_reply_preserves_epoch_seed_and_installs_ancestry();
    test_route_reply_ack_full_commitment_rejects_crc_collision_and_replay();
    test_route_reply_requires_exact_commitment_before_route_mutation();
    test_direct_route_reply_capacity_deadline_wraps_to_valid_zero();
    test_route_reply_timing_conflict_preserves_discovery_without_route();
    test_multihop_route_reply_forward_uses_channel_five();
    test_concurrent_route_replies_use_their_discovery_predecessor();
    test_relay_required_route_request_ignores_direct_gateway_copy();
    test_exact_hop2_route_request_installs_nonconflicting_downstream();
    test_exact_hop2_route_request_omits_conflicting_timing();
    test_exact_hop2_route_request_rejects_unsafe_capacity();
    test_exact_required_route_hops_filter_responder_and_origin();
    test_gateway_route_request_without_upstream_waits_for_route();
    test_malformed_route_request_does_not_poison_downlink_route();
    test_duplicate_route_advertisement_field_does_not_mutate_route();
    test_malformed_route_reply_does_not_poison_upstream_route();
    test_reactive_route_and_report_flow_over_uwb_mesh_frames();
    test_route_request_carries_reply_rx_eta();
    test_route_reply_waits_for_request_reply_eta();
    test_delayed_route_reply_rebases_embedded_channel9_timing();
    test_channel9_guard_allows_one_upstream_and_one_downstream();
    test_upstream_route_invalidation_preserves_downstream_connection();
    test_transit_abandon_preserves_upstream_connection();
    test_channel9_guard_rejects_second_peer_in_same_direction();
    test_channel9_guard_rejects_third_peer();
    test_channel9_guard_rejects_ambiguous_or_unknown_direction();
    test_channel9_guard_rejects_overlapping_upstream_downstream_windows();
    test_channel9_guard_rejects_reverse_order_hardware_overlap();
    test_channel9_guard_rejects_nominal_touch_inside_physical_rx();
    test_channel9_report_tx_requires_negotiated_event();
    test_channel9_result_bundle_tx_requires_negotiated_event();
    test_relay_accepts_collection_result_into_bundle_queue();
    test_relay_flushes_collection_bundle_when_queue_fills();
    test_relay_flushes_collection_bundle_after_hold_deadline();
    test_child_custody_bundle_snapshot_restores_after_reinit();
    test_child_custody_snapshot_rejects_corrupt_bundle_payload();
    test_crc16_collision_cannot_alias_restored_result_bundle_entry();
    test_child_custody_reservation_snapshot_restores_after_reinit();
    test_child_custody_snapshot_export_reports_no_state();
    test_result_bundle_outbox_snapshot_restores_after_forward_handoff();
    test_result_bundle_gateway_ack_timeout_preserves_outbox_for_route_recovery();
    test_result_bundle_outbox_snapshot_rejects_corrupt_payload();
    test_channel9_report_delivery_and_gateway_ack_require_events();
    test_channel9_payload_event_closes_while_outbox_waits_gateway_ack();
    test_channel9_rx_observation_keeps_negotiated_event_timing();
    test_channel9_sender_skips_channel5_preempted_event_without_refresh();
    test_channel9_receiver_miss_advances_timing_and_diagnostics();
    test_channel9_timing_expires_idle_connection_state();
    test_mesh_hop_ack_is_protocol_valid();
    test_click_participants_choose_best_clean_cached_route();
    test_click_participants_avoid_interior_path_and_unknown_ancestry();
    test_click_route_ignores_local_and_gateway_path_endpoints();
    test_click_route_falls_back_immediately_without_route_mutation();
    test_click_route_freezes_clean_next_hop_through_retry();
    test_direct_gateway_route_probe_marks_route_ready();
    test_direct_gateway_route_probe_clears_hold_down();
    test_remove_direct_gateway_route_selects_relay_candidate();
    test_gateway_control_reverse_route_preserves_parent_quarantine();
    test_forced_gateway_control_parent_rejects_deeper_cycle_edge();
    test_downlink_recency_tie_break_survives_uptime_wrap();
    test_direct_gateway_probe_route_answers_pending_request();
    test_route_reply_scheduled_deadline_wraps_to_valid_zero();
    test_route_reply_split_horizon_rejects_requester_as_gateway_parent();
    test_route_control_wire_admission_precedes_route_mutation();
    test_max_depth_route_control_builders_fit_wire_bounds();
    return 0;
}
