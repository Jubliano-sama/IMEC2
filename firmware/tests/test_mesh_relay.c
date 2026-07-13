#include "mesh_relay.h"

#include "mesh.h"
#include "report.h"
#include "uwb.h"

#include <assert.h>
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

    assert(sizeof(result) <= (3u * sizeof(struct mesh_outbound)) + 32u);

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

static void seed_downlink(struct mesh_relay *relay,
                          uint64_t target_id,
                          uint64_t next_hop_id,
                          uint32_t epoch,
                          uint8_t hop_count,
                          uint8_t quality,
                          uint32_t now_ms)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        struct mesh_downlink_entry *entry = &relay->downlinks[i];

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
    const struct persistent_outbox_record *record = &relay->outbox_record;

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

static void test_relay_forwards_gateway_bound_packet_and_reforwards_duplicate(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet report = {0};
    struct mesh_relay_result result;
    uint8_t payload[1] = {0xAAu};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);

    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 42u, 9u, sizeof(payload)) == PROTO_OK);
    report.ttl = 4u;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                95u,
                                2000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(result.forward.next_hop_id == GATEWAY);
    assert(result.forward.packet.src_id == ANCHOR_A);
    assert(result.forward.packet.dst_id == GATEWAY);
    assert(result.forward.packet.session_id == report.session_id);
    assert(result.forward.packet.seq == report.seq);
    assert(result.forward.packet.ttl == 3u);
    assert(result.forward.payload_len == sizeof(payload));
    assert(memcmp(result.forward.payload, payload, sizeof(payload)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                95u,
                                2001u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == GATEWAY);
    assert(result.forward.packet.ttl == 3u);
    assert(result.forward.packet.seq == report.seq);
    assert(result.forward.payload_len == sizeof(payload));
    assert(memcmp(result.forward.payload, payload, sizeof(payload)) == 0);
}

static void test_duplicate_cache_expires_by_time_window(void)
{
    struct mesh_relay relay;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .flags = 0u,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = 55u,
        .seq = 7u,
        .ttl = 1u,
        .payload_len = 0u,
    };
    struct mesh_relay_result result;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                ANCHOR_A,
                                80u,
                                1000u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                ANCHOR_A,
                                80u,
                                1001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                NULL,
                                0u,
                                ANCHOR_A,
                                80u,
                                61001u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
}

static void test_channel9_tx_requires_local_tx_slot(void)
{
    struct mesh_relay relay;
    struct mesh_event_timing timing = {0};
    struct mesh_event_params params = channel9_params(1000u);
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_plan plan = {0};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 7u);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&timing, false);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1000u,
                                                &plan) == PROTO_ERR_BUSY);

    mesh_event_note_missed(&timing, NULL);
    assert(mesh_relay_set_channel9_timing(&relay, GATEWAY, &timing) == PROTO_OK);
    assert(mesh_relay_require_channel9_tx_event(&relay,
                                                GATEWAY,
                                                &requirements,
                                                1100u,
                                                &plan) == PROTO_OK);
    assert(plan.action == MESH_EVENT_PLAN_START);
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
    assert(route_record_failure(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
    assert(route_record_failure(&relay.upstream, ROUTE_FAILURE_GATEWAY_ACK) == ROUTE_DELIVERY_RETRY_CURRENT);
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

static void test_survey_discovery_broadcast_delivers_and_floods(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    const uint8_t payload[] = {0x15u, 0x04u, 0x78u, 0x56u, 0x34u, 0x12u};
    struct proto_packet packet = {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0x12345678u,
        .seq = 9u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                GATEWAY,
                                80u,
                                3000u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.msg_type == MSG_SURVEY_DISCOVERY_START);
    assert(result.forward.packet.dst_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.ttl == 2u);
    assert(result.forward.payload_len == sizeof(payload));
    assert(memcmp(result.forward.payload, payload, sizeof(payload)) == 0);

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                GATEWAY,
                                80u,
                                3010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_broadcast_command_delivers_without_flooding(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    uint8_t payload[8];
    size_t payload_len = 0u;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0x12345679u,
        .seq = 12u,
        .ttl = 3u,
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
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_command_flood_broadcast_delivers_and_forwards_once(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    struct proto_packet duplicate_packet;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 1001u,
        .seq = 13u,
        .ttl = 3u,
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
    assert(result.forward.packet.ttl == 2u);
    assert(result.forward.queued_at_ms == 3020u);
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
    assert(require_tlv_u32(result.forward.payload,
                           result.forward.payload_len,
                           TLV_FLOOD_PACKET_AGE_MS) == 0u);

    duplicate_packet = packet;
    duplicate_packet.seq = 99u;
    assert(mesh_relay_handle_rx(&relay,
                                &duplicate_packet,
                                payload,
                                payload_len,
                                GATEWAY,
                                80u,
                                3030u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_collection_eack_broadcast_delivers_and_forwards_once(void)
{
    const struct gateway_collection_eack eack = {
        .gateway_id = GATEWAY,
        .gateway_epoch = 13u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .membership_epoch = 3u,
        .expected_count = 12u,
        .received_count = 5u,
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
        .session_id = 3003u,
        .seq = 21u,
        .ttl = 3u,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
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
    assert(result.forward.packet.ttl == 2u);
    assert(result.forward.queued_at_ms == 3040u);
    assert(result.forward.earliest_tx_ms >= 3040u);
    assert(result.forward.earliest_tx_ms < 3040u + FLOOD_WAVE_MS);
    assert(result.forward.payload_len == payload_len);
    assert(memcmp(result.forward.payload, payload, payload_len) == 0);

    duplicate_packet = packet;
    duplicate_packet.seq = 22u;
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
        .session_id = 3003u,
        .seq = 23u,
        .ttl = 2u,
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
        .session_id = 3004u,
        .seq = 24u,
        .ttl = 2u,
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
        .session_id = 3005u,
        .seq = 25u,
        .ttl = 2u,
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
        .session_id = 3005u,
        .seq = 25u,
        .ttl = 2u,
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
        .eack_format = EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
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
        .session_id = 3006u,
        .seq = 26u,
        .ttl = 2u,
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
        .session_id = 3014u,
        .seq = 31u,
        .ttl = 2u,
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
        .session_id = 3007u,
        .seq = 26u,
        .ttl = 2u,
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
    assert(mesh_relay_defer_tx(&relay, 5100u));
    assert(mesh_relay_tx_active(&relay));
    assert(mesh_relay_tx_active_local_collection_result(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == 5100u + RELAY_BUSY_RETRY_MIN_MS);
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

static void test_collection_result_expires_without_retrying_forever(void)
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
    struct mesh_outbound tx;
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
    assert(result.status == PROTO_ERR_STALE);
    assert(!mesh_relay_tx_active(&relay));
    assert(!has_action(&result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_COLLECTION_RETRY));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_EXPIRED);
    assert(!relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.age_ms_saturating == 1000u);
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

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 13u);
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
        .expected_count = 12u,
        .received_count = 5u,
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
        .session_id = 3003u,
        .seq = 23u,
        .ttl = 3u,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
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

static void test_busy_survey_discovery_broadcast_still_forwards(void)
{
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct route_candidate route = direct_gateway_route(GATEWAY, 14u, 90u);
    const uint8_t payload[] = {0x1Au, 0x04u, 0xE8u, 0x03u, 0x00u, 0x00u};
    const uint8_t report_payload[] = {0x42u};
    struct proto_packet report;
    struct mesh_outbound tx;
    struct proto_packet packet = {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 55u,
        .seq = 10u,
        .ttl = 3u,
        .payload_len = sizeof(payload),
    };

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 14u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    56u,
                                    11u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               3100u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_handle_rx(&relay,
                                &packet,
                                payload,
                                sizeof(payload),
                                GATEWAY,
                                80u,
                                3110u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(result.forward.next_hop_id == MESH_BROADCAST_ID);
    assert(result.forward.packet.ttl == 2u);
}

static void test_downlink_routes_survive_age_until_delivery_failure(void)
{
    struct mesh_relay gateway;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 30u);
    seed_downlink(&gateway, ANCHOR_A, ANCHOR_B, 30u, 2u, 75u, 1000u);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    assert(mesh_relay_expire_routes(&gateway, 31001u) == 0u);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_OK);
    assert(next_hop_id == ANCHOR_B);

    mesh_relay_note_delivery_failure(&gateway, ANCHOR_A);
    assert(mesh_relay_find_downlink(&gateway, ANCHOR_A) == NULL);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) == PROTO_ERR_NOT_FOUND);
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
    uint8_t payload[1] = {0x5Au};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 32u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 91u, 1u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay,
                               &report,
                               payload,
                               sizeof(payload),
                               31001u,
                               &tx) == PROTO_OK);
    assert(route_selected(&relay.upstream) != NULL);

    mesh_relay_cancel_tx(&relay);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    assert(route_selected(&relay.upstream) != NULL);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    assert(route_selected(&relay.upstream) != NULL);
    mesh_relay_note_delivery_failure(&relay, GATEWAY);
    assert(route_selected(&relay.upstream) == NULL);
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
    uint8_t payload[1] = {0x55u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 77u, 1u, sizeof(payload)) == PROTO_OK);
    report.ttl = 0u;

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                ANCHOR_A,
                                80u,
                                4000u,
                                &result) == PROTO_OK);

    assert(result.status == PROTO_ERR_STALE);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
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

static void test_busy_relay_does_not_ack_or_cache_new_forward(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet local_report;
    struct proto_packet incoming_report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t local_payload[1] = {0x33u};
    uint8_t incoming_payload[1] = {0x44u};
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    78u,
                                    2u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
                               4100u,
                               &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(report_init_click_packet(&incoming_report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    79u,
                                    3u,
                                    sizeof(incoming_payload)) == PROTO_OK);
    incoming_report.ttl = 4u;

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4110u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
    assert(relay.diagnostics.busy_response_count == 1u);
    assert(result.busy.packet.msg_type == MSG_RELAY_BUSY);
    assert(result.busy.packet.dst_id == ANCHOR_A);
    assert(result.busy.next_hop_id == ANCHOR_A);
    assert(tlv_find(result.busy.payload,
                    result.busy.payload_len,
                    TLV_REQUESTED_MSG_SESSION_ID,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    assert(proto_get_u32_le(value) == incoming_report.session_id);
    assert(tlv_find(result.busy.payload,
                    result.busy.payload_len,
                    TLV_REQUESTED_MSG_SEQ,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    assert(proto_get_u16_le(value) == incoming_report.seq);
    assert(tlv_find(result.busy.payload,
                    result.busy.payload_len,
                    TLV_RETRY_AFTER_MS,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    assert(proto_get_u16_le(value) == RELAY_BUSY_RETRY_MAX_MS);
    assert(require_tlv_u8(result.busy.payload,
                          result.busy.payload_len,
                          TLV_RELAY_CAPACITY_STATE) == RELAY_CAP_RED);
    assert(require_tlv_u16(result.busy.payload,
                           result.busy.payload_len,
                           TLV_CAPACITY_VALIDITY_INTERVAL_MS) ==
           RELAY_BUSY_RETRY_MAX_MS);

    mesh_relay_cancel_tx(&relay);
    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4120u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_busy_relay_drops_duplicate_forward(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_report;
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    uint8_t incoming_payload[1] = {0x45u};
    uint8_t local_payload[1] = {0x46u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&incoming_report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    80u,
                                    4u,
                                    sizeof(incoming_payload)) == PROTO_OK);
    incoming_report.ttl = 4u;

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4200u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));

    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    81u,
                                    5u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
                               4201u,
                               &tracked_report) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_report,
                                incoming_payload,
                                sizeof(incoming_payload),
                                ANCHOR_A,
                                80u,
                                4202u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_ERR_BUSY);
    assert(has_action(&result, MESH_RELAY_ACTION_DROP));
    assert(has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY));
    assert(!has_action(&result, MESH_RELAY_ACTION_FORWARD));
}

static void test_busy_relay_sends_result_busy_for_command_result(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 3u, 90u);
    struct proto_packet incoming_result = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 90u,
        .seq = 6u,
        .ttl = 4u,
        .payload_len = 1u,
    };
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    uint8_t incoming_payload[1] = {0x45u};
    uint8_t local_payload[1] = {0x46u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    81u,
                                    5u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
                               4201u,
                               &tracked_report) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &incoming_result,
                                incoming_payload,
                                sizeof(incoming_payload),
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
    struct proto_packet incoming_result = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 90u,
        .seq = 6u,
        .ttl = 4u,
    };
    struct proto_packet local_report;
    struct mesh_outbound tracked_report;
    struct mesh_relay_result result;
    struct result_busy decoded_busy = {0};
    uint8_t incoming_payload[96];
    uint8_t local_payload[1] = {0x46u};
    size_t incoming_payload_len = 0u;

    assert(command_result_id_append_tlvs(incoming_payload,
                                         sizeof(incoming_payload),
                                         &incoming_payload_len,
                                         &result_id) == PROTO_OK);
    incoming_result.payload_len = (uint16_t)incoming_payload_len;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&local_report,
                                    ANCHOR_B,
                                    GATEWAY,
                                    81u,
                                    5u,
                                    sizeof(local_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&relay,
                               &local_report,
                               local_payload,
                               sizeof(local_payload),
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
        .session_id = 91u,
        .seq = 7u,
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
    assert(relay.result_offer_reservation.result_crc == offer.result_crc);
}

static void test_result_transfer_requires_matching_offer_len_crc(void)
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
        .session_id = 91u,
        .seq = 8u,
        .ttl = 1u,
    };
    struct proto_packet result_packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = ANCHOR_A,
        .dst_id = GATEWAY,
        .session_id = 91u,
        .seq = 9u,
        .ttl = 4u,
    };
    struct mesh_relay_result offer_result;
    struct mesh_relay_result result;
    uint8_t offer_payload[96];
    uint8_t result_payload[COLLECTION_BUNDLE_TARGET_BYTES + 32u];
    size_t offer_payload_len = 0u;
    size_t result_payload_len = 0u;

    build_identity_command_result_payload(result_payload,
                                          sizeof(result_payload),
                                          sizeof(result_payload),
                                          &result_id,
                                          &result_payload_len);
    offer.result_len = (uint16_t)result_payload_len;
    offer.result_crc = proto_crc16_ccitt_false(result_payload, result_payload_len);
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
    result_packet.seq++;
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
        .session_id = 93u,
        .seq = 9u,
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
        .session_id = 92u,
        .seq = 8u,
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
        .session_id = 93u,
        .seq = 9u,
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
                                    93u,
                                    10u,
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
        .session_id = 94u,
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
                                    94u,
                                    11u,
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
                                    97u,
                                    14u,
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
    assert(relay.pending.result_offer_active);
    assert(relay.pending.queued_at_ms == 4400u);
    assert(relay.outbox_record.valid);
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
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_LOCAL_CUSTODY_ACK);

    assert(mesh_relay_tick(&restored,
                           5000u + RELAY_BUSY_RETRY_MIN_MS,
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
        .session_id = 98u,
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
                                    98u,
                                    15u,
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
        .session_id = 95u,
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

    build_identity_command_result_payload(payload,
                                          sizeof(payload),
                                          64u,
                                          &result_id,
                                          &payload_len);
    assert(mesh_init_command_result(&result_packet,
                                    ANCHOR_A,
                                    GATEWAY,
                                    95u,
                                    12u,
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
    assert(has_action(&busy_result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.result_offer_active);

    assert(mesh_relay_tick(&relay,
                           4410u + RELAY_BUSY_RETRY_MIN_MS,
                           &tick_result) == PROTO_OK);
    assert(has_action(&tick_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(tick_result.retransmit.packet.msg_type == MSG_RESULT_OFFER);
    assert(tick_result.retransmit.packet.session_id == result_packet.session_id);
    assert(tick_result.retransmit.packet.seq == result_packet.seq);
    assert(tick_result.retransmit.next_hop_id == ANCHOR_B);
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT);
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
        .session_id = 96u,
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
                                    96u,
                                    13u,
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
    uint8_t payload[1] = {0x33u};
    uint8_t busy_payload[32];
    size_t busy_payload_len = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
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
                         RELAY_CAP_YELLOW) == PROTO_OK);
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

    assert(mesh_relay_handle_rx(&relay,
                                &busy,
                                busy_payload,
                                busy_payload_len,
                                ANCHOR_B,
                                80u,
                                5010u,
                                &result) == PROTO_OK);
    assert(result.status == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_RELAY_BUSY));
    assert(mesh_relay_tx_active(&relay));
    assert(relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    assert(relay.pending.retry_after_ms == 5010u + RELAY_BUSY_RETRY_MIN_MS);
}

static void test_local_gateway_bound_tx_waits_for_gateway_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet ack;
    uint8_t ack_payload[16];
    size_t ack_payload_len = 0u;
    uint8_t payload[1] = {0x42u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 88u, 7u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 5000u, &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));
    assert(tx.next_hop_id == ANCHOR_B);
    assert(tx.packet.seq == report.seq);
    assert_outbox_tracks_packet(&relay,
                                &report,
                                payload,
                                sizeof(payload),
                                5000u,
                                MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);

    assert(mesh_append_requested_seq(ack_payload, sizeof(ack_payload), &ack_payload_len, report.seq) == PROTO_OK);
    assert(mesh_init_gateway_ack(&ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 report.session_id,
                                 2u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&relay));
    assert(!relay.outbox_record.valid);
    assert(relay.outbox_record.delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED);
    assert(relay.outbox_record.gateway_acked);
    assert(relay.outbox_record.age_ms_saturating == 100u);
}

static void test_local_gateway_bound_tx_accepts_batched_gateway_ack(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct proto_packet ack;
    uint8_t ack_payload[32];
    uint8_t seq_list[4];
    size_t ack_payload_len = 0u;
    uint8_t payload[1] = {0x42u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 188u, 7u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 5000u, &tx) == PROTO_OK);
    assert(mesh_relay_tx_active(&relay));

    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     (uint16_t)(report.seq + 1u)) == PROTO_OK);
    proto_put_u16_le(&seq_list[0], (uint16_t)(report.seq + 2u));
    proto_put_u16_le(&seq_list[2], report.seq);
    assert(tlv_append_bytes(ack_payload,
                            sizeof(ack_payload),
                            &ack_payload_len,
                            TLV_MESH_ACK_SEQ_LIST,
                            seq_list,
                            sizeof(seq_list)) == PROTO_OK);
    assert(mesh_init_gateway_ack(&ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 report.session_id,
                                 2u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                90u,
                                5100u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
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
    assert(tx.earliest_tx_ms == 5000u);
}

static void test_relayed_tx_completes_after_next_hop_send(void)
{
    struct mesh_relay relay;
    struct route_candidate route = direct_gateway_route(GATEWAY, 5u, 80u);
    struct proto_packet report;
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t payload[1] = {0x43u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 5u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 89u, 8u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&relay, &report, payload, sizeof(payload), 6000u, &tx) == PROTO_OK);
    assert(tx.next_hop_id == GATEWAY);
    assert(!mesh_relay_tx_active(&relay));

    mesh_relay_note_tx_sent(&relay, &tx, 6050u);
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
    assert(relay.pending.retry_after_ms == now_ms + RELAY_BUSY_RETRY_MIN_MS);
    assert(relay.pending.gateway_ack_deadline_ms == 0u);
    assert(relay.pending.packet.msg_type == MSG_CLICK_REPORT);
    assert(relay.pending.packet.src_id == ANCHOR_A);
    assert(relay.pending.payload_len == sizeof(payload));
    assert(memcmp(relay.pending.payload, payload, sizeof(payload)) == 0);
    assert(relay.outbox_record.valid);
    assert(route_selected(&relay.upstream) == NULL);
    assert(relay.upstream.candidates[0].valid);
    assert(relay.upstream.candidates[0].hold_down_until_ms != 0u);
    assert(!relay.upstream.candidates[0].channel9_timing_valid);
    assert(find_route_candidate(&relay, ANCHOR_C) != NULL);
    assert(find_route_candidate(&relay, ANCHOR_C)->valid);
    assert(find_route_candidate(&relay, ANCHOR_C)->hold_down_until_ms ==
           UINT32_C(1000000000));
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) == NULL);
    assert(find_event_timing(&relay, GATEWAY) == NULL);
    assert(find_event_timing(&relay, ANCHOR_B) == NULL);
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

static void test_route_discovery_continues_at_ttl_six_with_backoff(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    uint32_t now_ms = 1000u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 11u);

    for (uint8_t attempt = 1u; attempt <= 8u; attempt++) {
        uint8_t expected_ttl = attempt == 1u ? 1u :
                               (attempt == 2u ? 2u :
                                (attempt == 3u ? 4u : 6u));

        assert(mesh_relay_prepare_route_request(&relay,
                                                GATEWAY,
                                                now_ms,
                                                0u,
                                                &route_req) == PROTO_OK);
        assert(relay.route_discovery.active);
        assert(relay.route_discovery.target_id == GATEWAY);
        assert(relay.route_discovery.attempts == attempt);
        assert(route_req.packet.ttl == expected_ttl);
        assert(route_req.queued_at_ms == now_ms);
        assert(route_req.earliest_tx_ms == now_ms);
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
    assert(route_req.packet.ttl == 6u);
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
        uint8_t expected_ttl = attempt == 1u ? 1u :
                               (attempt == 2u ? 2u :
                                (attempt == 3u ? 4u : 6u));

        assert(delay_ms >= base_ms[attempt - 1u]);
        assert(delay_ms <= base_ms[attempt - 1u] * 2u);
        assert(mesh_relay_prepare_route_request(&relay,
                                                GATEWAY,
                                                now_ms,
                                                random_value,
                                                &route_req) == PROTO_OK);
        assert(route_req.packet.ttl == expected_ttl);
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

static void test_better_route_request_copy_updates_pending_forward(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_outbound route_req;
    struct mesh_relay_result first;
    struct mesh_relay_result better;
    struct mesh_relay_result late;
    uint32_t first_due_ms;

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 12u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 12u);
    assert(mesh_relay_prepare_route_request(&origin,
                                            GATEWAY,
                                            1000u,
                                            0u,
                                            &route_req) == PROTO_OK);
    route_req.packet.ttl = 3u;

    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &route_req.packet,
                                            route_req.payload,
                                            route_req.payload_len,
                                            ANCHOR_A,
                                            20u,
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
                                            &route_req.packet,
                                            route_req.payload,
                                            route_req.payload_len,
                                            ANCHOR_C,
                                            90u,
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
                                            &route_req.packet,
                                            route_req.payload,
                                            route_req.payload_len,
                                            ANCHOR_C,
                                            100u,
                                            first_due_ms,
                                            9u,
                                            &late) == PROTO_OK);
    assert(late.status == PROTO_ERR_STALE);
    assert(has_action(&late, MESH_RELAY_ACTION_DROP));
    assert(!has_action(&late, MESH_RELAY_ACTION_UPDATE_ROUTE_REQ));
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
    struct mesh_outbound equivalent_adv;
    struct mesh_outbound next_adv;
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
           FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS);
    assert(require_tlv_u16(adv.payload,
                           adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS);
    assert(require_tlv_u8(adv.payload, adv.payload_len, TLV_FLOOD_RETRY_COUNT) ==
           FLOOD_DEFAULT_RETRY_COUNT);
    assert(require_tlv_u32(adv.payload, adv.payload_len, TLV_FLOOD_PACKET_AGE_MS) == 0u);

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
    assert(result_a.gateway_route_adv.earliest_tx_ms < 1010u + FLOOD_WAVE_MS);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_EPOCH_ID) == flood_epoch_id);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_SLOT_SEED) == slot_seed);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_MAX_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS);
    assert(require_tlv_u16(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS);
    assert(require_tlv_u8(result_a.gateway_route_adv.payload,
                          result_a.gateway_route_adv.payload_len,
                          TLV_FLOOD_RETRY_COUNT) ==
           FLOOD_DEFAULT_RETRY_COUNT);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_PACKET_AGE_MS) == 0u);
    assert(mesh_outbound_set_flood_packet_age_ms(&result_a.gateway_route_adv,
                                                 1234u) == PROTO_OK);
    assert(require_tlv_u32(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_FLOOD_PACKET_AGE_MS) == 1234u);
    assert(require_tlv_u8(result_a.gateway_route_adv.payload,
                          result_a.gateway_route_adv.payload_len,
                          TLV_HOP_COUNT) == 1u);
    assert(require_tlv_u8(result_a.gateway_route_adv.payload,
                          result_a.gateway_route_adv.payload_len,
                          TLV_PATH_QUALITY_MIN) == 82u);
    assert(require_tlv_u16(result_a.gateway_route_adv.payload,
                           result_a.gateway_route_adv.payload_len,
                           TLV_ACCUMULATED_COST) == 118u);

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

    assert(route_expire_stale(&relay.upstream,
                              2300u + ROUTE_PARENT_HOLDDOWN_MS + 1u,
                              ROUTE_CANDIDATE_MAX_AGE_MS) == 0u);
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

    mesh_relay_invalidate_routes(&relay);

    assert(relay.upstream.current_epoch == 15u);
    assert(route_selected(&relay.upstream) == NULL);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_B) == NULL);
    assert(!relay.event_timings[0].valid);
    assert(!relay.route_discovery.active);
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
    uint8_t payload[1] = {0x42u};

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 15u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 150u, 3u, sizeof(payload)) ==
           PROTO_OK);

    assert(mesh_relay_handle_rx(&relay,
                                &report,
                                payload,
                                sizeof(payload),
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
}

static void test_hop_ack_extends_gateway_ack_timeout(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 16u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t report_payload[1] = {0x43u};
    uint8_t ack_payload[8];
    size_t ack_payload_len = 0u;
    uint32_t original_deadline;
    uint32_t hop_ack_ms;

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
    original_deadline = origin.pending.gateway_ack_deadline_ms;
    hop_ack_ms = original_deadline - 10u;

    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     report.seq) == PROTO_OK);
    hop_ack.msg_type = MSG_MESH_HOP_ACK;
    hop_ack.src_id = ANCHOR_B;
    hop_ack.dst_id = ANCHOR_A;
    hop_ack.session_id = report.session_id;
    hop_ack.seq = 77u;
    hop_ack.ttl = MESH_GATEWAY_ACK_TTL;
    hop_ack.payload_len = (uint8_t)ack_payload_len;

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                hop_ack_ms,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_PROGRESS));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(origin.pending.gateway_ack_deadline_ms ==
           hop_ack_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS);

    assert(mesh_relay_tick(&origin, original_deadline + 1u, &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(mesh_relay_tx_active(&origin));

    assert(mesh_relay_tick(&origin,
                           origin.pending.gateway_ack_deadline_ms + 1u,
                           &result) == PROTO_OK);
    assert(result.actions == MESH_RELAY_ACTION_NONE);
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                origin.pending.retry_after_ms - 1u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_PROGRESS));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
}

static void test_hop_ack_waits_for_later_gateway_ack(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 16u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct proto_packet gateway_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    uint8_t report_payload[1] = {0x43u};
    uint8_t ack_payload[16];
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

    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     report.seq) == PROTO_OK);
    hop_ack.msg_type = MSG_MESH_HOP_ACK;
    hop_ack.src_id = ANCHOR_B;
    hop_ack.dst_id = ANCHOR_A;
    hop_ack.session_id = report.session_id;
    hop_ack.seq = 77u;
    hop_ack.ttl = MESH_GATEWAY_ACK_TTL;
    hop_ack.payload_len = (uint16_t)ack_payload_len;

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
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);

    assert(mesh_init_gateway_ack(&gateway_ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 report.session_id,
                                 78u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);
    assert(mesh_relay_handle_rx(&origin,
                                &gateway_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                1200u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
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
    uint8_t ack_payload[8];
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

    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     report.seq) == PROTO_OK);
    hop_ack.msg_type = MSG_MESH_HOP_ACK;
    hop_ack.src_id = ANCHOR_B;
    hop_ack.dst_id = ANCHOR_A;
    hop_ack.session_id = report.session_id;
    hop_ack.seq = 79u;
    hop_ack.ttl = MESH_GATEWAY_ACK_TTL;
    hop_ack.payload_len = (uint16_t)ack_payload_len;

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
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
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
    uint8_t ack_payload[8];
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

    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     result_packet.seq) == PROTO_OK);
    hop_ack.msg_type = MSG_MESH_HOP_ACK;
    hop_ack.src_id = ANCHOR_B;
    hop_ack.dst_id = ANCHOR_A;
    hop_ack.session_id = result_packet.session_id;
    hop_ack.seq = 81u;
    hop_ack.ttl = MESH_GATEWAY_ACK_TTL;
    hop_ack.payload_len = (uint16_t)ack_payload_len;
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
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&restored));
    assert(!restored.outbox_record.valid);
    assert(restored.outbox_record.delivery_state ==
           MESH_RELAY_DELIVERY_GATEWAY_ACKED);
    assert(restored.outbox_record.gateway_acked);
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
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(gateway_result.gateway_ack.next_hop_id == ANCHOR_B);
    assert(gateway_result.gateway_ack.packet.dst_id == ANCHOR_A);
}

static void test_gateway_delivers_direct_clicker_self_test_report_and_acks(void)
{
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

    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        GATEWAY,
                                        fields.event_seq,
                                        9u,
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

static void test_gateway_ack_previous_hop_fallback_must_not_be_rerouted(void)
{
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    struct proto_packet packet;
    struct mesh_outbound rerouted;
    struct self_test_report_fields fields = {
        .clicker_id = ANCHOR_A,
        .event_seq = 711u,
        .failure_code = 0u,
        .battery_mv = 0u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 72u);

    assert(report_append_self_test_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                        fields.clicker_id,
                                        GATEWAY,
                                        fields.event_seq,
                                        10u,
                                        (uint8_t)payload_len) == PROTO_OK);

    assert(mesh_relay_handle_rx(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                fields.clicker_id,
                                95u,
                                3100u,
                                &result) == PROTO_OK);

    assert(has_action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(result.gateway_ack.next_hop_id == fields.clicker_id);
    assert(mesh_relay_start_tx(&gateway,
                               &result.gateway_ack.packet,
                               result.gateway_ack.payload,
                               result.gateway_ack.payload_len,
                               3110u,
                               &rerouted) == PROTO_ERR_NOT_FOUND);
}

static void test_duplicate_retry_repairs_lost_gateway_ack(void)
{
    struct mesh_relay origin;
    struct mesh_relay relay;
    struct mesh_relay gateway;
    struct route_candidate origin_route = direct_gateway_route(ANCHOR_B, 52u, 85u);
    struct route_candidate relay_route = direct_gateway_route(GATEWAY, 52u, 95u);
    struct proto_packet report;
    struct mesh_outbound origin_tx;
    struct mesh_outbound relay_tx;
    struct mesh_outbound relay_retry_tx;
    struct mesh_relay_result origin_result;
    struct mesh_relay_result relay_result;
    struct mesh_relay_result gateway_result;
    uint8_t report_payload[1] = {0x46u};
    uint32_t gateway_ack_timeout_ms;
    uint32_t retry_ms;

    origin_route.hop_count = 1u;
    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 52u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 52u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 52u);
    assert(route_upsert_candidate(&origin.upstream, &origin_route) == PROTO_OK);
    assert(route_upsert_candidate(&relay.upstream, &relay_route) == PROTO_OK);
    seed_downlink(&relay, ANCHOR_A, ANCHOR_A, 52u, 2u, 80u, 1000u);
    assert(mesh_relay_find_downlink(&relay, ANCHOR_A) != NULL);

    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    520u,
                                    6u,
                                    sizeof(report_payload)) == PROTO_OK);
    assert(mesh_relay_start_tx(&origin,
                               &report,
                               report_payload,
                               sizeof(report_payload),
                               2000u,
                               &origin_tx) == PROTO_OK);
    assert(origin_tx.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&origin, &origin_tx, 2000u);
    gateway_ack_timeout_ms = origin.pending.gateway_ack_deadline_ms + 1u;

    assert(mesh_relay_handle_rx(&relay,
                                &origin_tx.packet,
                                origin_tx.payload,
                                origin_tx.payload_len,
                                ANCHOR_A,
                                80u,
                                2010u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(mesh_relay_tx_active(&origin));

    assert(mesh_relay_start_tx(&relay,
                               &relay_result.forward.packet,
                               relay_result.forward.payload,
                               relay_result.forward.payload_len,
                               2021u,
                               &relay_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &relay_tx.packet,
                                relay_tx.payload,
                                relay_tx.payload_len,
                                ANCHOR_B,
                                90u,
                                2030u,
                                &gateway_result) == PROTO_OK);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!mesh_relay_tx_active(&relay));

    assert(mesh_relay_tick(&origin, gateway_ack_timeout_ms, &origin_result) == PROTO_OK);
    assert(origin_result.actions == MESH_RELAY_ACTION_NONE);
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
    retry_ms = origin.pending.retry_after_ms;

    assert(mesh_relay_tick(&origin, retry_ms, &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_RETRANSMIT));
    assert(origin_result.retransmit.next_hop_id == ANCHOR_B);
    mesh_relay_note_tx_sent(&origin, &origin_result.retransmit, retry_ms);

    assert(mesh_relay_handle_rx(&relay,
                                &origin_result.retransmit.packet,
                                origin_result.retransmit.payload,
                                origin_result.retransmit.payload_len,
                                ANCHOR_A,
                                80u,
                                retry_ms + 10u,
                                &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_ERR_STALE);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));

    assert(mesh_relay_start_tx(&relay,
                               &relay_result.forward.packet,
                               relay_result.forward.payload,
                               relay_result.forward.payload_len,
                               retry_ms + 21u,
                               &relay_retry_tx) == PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway,
                                &relay_retry_tx.packet,
                                relay_retry_tx.payload,
                                relay_retry_tx.payload_len,
                                ANCHOR_B,
                                90u,
                                retry_ms + 30u,
                                &gateway_result) == PROTO_OK);
    assert(gateway_result.status == PROTO_ERR_STALE);
    assert(has_action(&gateway_result, MESH_RELAY_ACTION_SEND_GATEWAY_ACK));
    assert(!has_action(&gateway_result, MESH_RELAY_ACTION_DELIVER_LOCAL));

    assert(mesh_relay_handle_rx(&relay,
                                &gateway_result.gateway_ack.packet,
                                gateway_result.gateway_ack.payload,
                                gateway_result.gateway_ack.payload_len,
                                GATEWAY,
                                90u,
                                retry_ms + 50u,
                                &relay_result) == PROTO_OK);
    assert(has_action(&relay_result, MESH_RELAY_ACTION_FORWARD));
    assert(relay_result.forward.next_hop_id == ANCHOR_A);

    assert(mesh_relay_handle_rx(&origin,
                                &relay_result.forward.packet,
                                relay_result.forward.payload,
                                relay_result.forward.payload_len,
                                ANCHOR_B,
                                80u,
                                retry_ms + 60u,
                                &origin_result) == PROTO_OK);
    assert(has_action(&origin_result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
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
    struct route_candidate route = direct_gateway_route(GATEWAY, 50u, 90u);
    const struct mesh_relay_event_timing_entry *timing_entry;
    struct mesh_event_params params = channel9_params(1500u);
    uint8_t payload[1] = {0x66u};
    const struct route_candidate *selected;
    uint32_t relay_reply_flood_epoch;
    uint16_t relay_reply_nonce;
    uint16_t relay_reply_metric_crc;

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
    assert(route_req.payload_len <= 82u);
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
    relay_reply_flood_epoch = require_tlv_u32(relay_result.route_reply.payload,
                                              relay_result.route_reply.payload_len,
                                              TLV_FLOOD_EPOCH_ID);
    relay_reply_nonce = require_tlv_u16(relay_result.route_reply.payload,
                                        relay_result.route_reply.payload_len,
                                        TLV_REPLY_NONCE);
    relay_reply_metric_crc = require_tlv_u16(relay_result.route_reply.payload,
                                             relay_result.route_reply.payload_len,
                                             TLV_METRIC_CRC);

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
    assert(require_tlv_u32(anchor_result.route_reply_ack.payload,
                           anchor_result.route_reply_ack.payload_len,
                           TLV_FLOOD_EPOCH_ID) == relay_reply_flood_epoch);
    assert(require_tlv_u16(anchor_result.route_reply_ack.payload,
                           anchor_result.route_reply_ack.payload_len,
                           TLV_REPLY_NONCE) == relay_reply_nonce);
    assert(require_tlv_u16(anchor_result.route_reply_ack.payload,
                           anchor_result.route_reply_ack.payload_len,
                           TLV_METRIC_CRC) == relay_reply_metric_crc);
    assert(has_action(&anchor_result, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    timing_entry = find_event_timing(&anchor, ANCHOR_B);
    assert(timing_entry != NULL);
    assert(mesh_event_timing_local_tx_slot(&timing_entry->timing));
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

static void test_multihop_route_reply_forward_uses_channel_five(void)
{
    struct mesh_relay origin;
    struct mesh_relay intermediate;
    struct mesh_relay responder;
    struct mesh_outbound request;
    struct mesh_relay_result intermediate_request;
    struct mesh_relay_result responder_reply;
    struct mesh_relay_result forwarded_reply;
    struct route_candidate direct = direct_gateway_route(GATEWAY, 40u, 90u);

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 40u);
    mesh_relay_init(&intermediate, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 40u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_C, GATEWAY, 40u);
    assert(route_upsert_candidate(&responder.upstream, &direct) == PROTO_OK);

    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &request,
                                          1000u) == PROTO_OK);
    request.packet.ttl = 3u;
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
}

static void test_concurrent_route_replies_use_their_discovery_predecessor(void)
{
    struct mesh_relay origin;
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
    mesh_relay_init(&intermediate, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 44u);
    mesh_relay_init(&responder, MESH_RELAY_ROLE_ANCHOR, ANCHOR_D, GATEWAY, 44u);
    assert(route_upsert_candidate(&responder.upstream, &direct) == PROTO_OK);

    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &request_one,
                                          1000u) == PROTO_OK);
    assert(mesh_relay_build_route_request(&origin,
                                          GATEWAY,
                                          &request_two,
                                          2000u) == PROTO_OK);
    assert(request_one.packet.session_id != request_two.packet.session_id);
    request_one.packet.ttl = 3u;
    request_two.packet.ttl = 3u;

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
    struct route_candidate route = direct_gateway_route(GATEWAY, 50u, 90u);

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
    struct route_candidate route = direct_gateway_route(GATEWAY, 50u, 90u);

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
    struct route_candidate route = direct_gateway_route(GATEWAY, 60u, 90u);

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
    struct route_candidate route = direct_gateway_route(GATEWAY, 50u, 90u);
    uint8_t decoded_payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload[1] = {0x66u};
    size_t decoded_payload_len = 0u;
    uint64_t previous_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 50u);
    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 1u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 1u);
    assert(route_upsert_candidate(&relay.upstream, &route) == PROTO_OK);
    assert(report_init_click_packet(&report, ANCHOR_A, GATEWAY, 500u, 1u, sizeof(payload)) == PROTO_OK);

    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1000u, &report_tx) ==
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

    assert(mesh_relay_start_tx(&anchor, &report, payload, sizeof(payload), 1050u, &report_tx) ==
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

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 72u);
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
        .session_id = 91u,
        .seq = 7u,
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
                                4300u,
                                &result) == PROTO_OK);
    assert(relay.result_offer_reservation.valid);
    assert(mesh_relay_export_child_custody_snapshot(&relay,
                                                    4301u,
                                                    &snapshot) == PROTO_OK);

    mesh_relay_init(&restored, MESH_RELAY_ROLE_ANCHOR, ANCHOR_B, GATEWAY, 3u);
    assert(mesh_relay_restore_child_custody_snapshot(&restored,
                                                     &snapshot,
                                                     1u) == PROTO_OK);
    assert(restored.result_offer_reservation.valid);
    assert(restored.result_offer_reservation.child_id == ANCHOR_A);
    assert_command_result_id_equal(&restored.result_offer_reservation.result_id,
                                   &offer.result_id);
    assert(restored.result_offer_reservation.result_len == offer.result_len);
    assert(restored.result_offer_reservation.result_crc == offer.result_crc);
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
    assert(snapshot.record.delivery_state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);

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
    assert(restored.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
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
           MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
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
    struct mesh_outbound tx;
    struct mesh_relay_result gateway_result;
    struct mesh_relay_result origin_result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(3000u);
    uint8_t payload[1] = {0x78u};

    mesh_relay_init(&origin, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 71u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 71u);
    assert(route_upsert_candidate(&origin.upstream, &route) == PROTO_OK);
    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    assert(mesh_relay_set_channel9_timing(&origin, GATEWAY, &timing) == PROTO_OK);

    assert(report_init_click_packet(&report,
                                    ANCHOR_A,
                                    GATEWAY,
                                    710u,
                                    2u,
                                    sizeof(payload)) == PROTO_OK);
    assert(mesh_relay_start_channel9_tx(&origin,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        3000u,
                                        &plan,
                                        &tx) == PROTO_OK);
    assert(tx.radio_channel == MESH_EVENT_CHANNEL);
    mesh_relay_note_channel9_tx(&origin, tx.next_hop_id, plan.start_ms);
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
    assert(has_action(&origin_result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
}

static void test_channel9_payload_event_closes_while_outbox_waits_gateway_ack(void)
{
    struct mesh_relay origin;
    struct route_candidate route = direct_gateway_route(ANCHOR_B, 72u, 75u);
    struct proto_packet report;
    struct proto_packet hop_ack = {0};
    struct proto_packet gateway_ack = {0};
    struct mesh_outbound tx;
    struct mesh_relay_result result;
    struct mesh_event_timing timing = {0};
    struct mesh_event_plan plan = {0};
    struct mesh_channel5_requirements requirements = clear_channel5_requirements();
    struct mesh_event_params params = channel9_params(3000u);
    uint8_t report_payload[1] = {0x79u};
    uint8_t ack_payload[16];
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

    mesh_relay_note_channel9_tx(&origin, tx.next_hop_id, plan.start_ms);
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

    assert(mesh_append_requested_seq(ack_payload,
                                     sizeof(ack_payload),
                                     &ack_payload_len,
                                     report.seq) == PROTO_OK);
    hop_ack.msg_type = MSG_MESH_HOP_ACK;
    hop_ack.src_id = ANCHOR_B;
    hop_ack.dst_id = ANCHOR_A;
    hop_ack.session_id = report.session_id;
    hop_ack.seq = 77u;
    hop_ack.ttl = MESH_GATEWAY_ACK_TTL;
    hop_ack.payload_len = (uint16_t)ack_payload_len;

    assert(mesh_relay_handle_rx(&origin,
                                &hop_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                3010u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_HOP_PROGRESS));
    assert(!has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(mesh_relay_tx_active(&origin));
    assert(origin.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK);
    assert(origin.outbox_record.valid);
    assert(origin.outbox_record.delivery_state == MESH_RELAY_DELIVERY_WAIT_GATEWAY_ACK);
    assert(!origin.outbox_record.gateway_acked);
    assert(origin.event_timings[0].timing.event_counter == 1u);
    assert(origin.event_timings[0].timing.next_event_time_ms == 3100u);

    assert(mesh_init_gateway_ack(&gateway_ack,
                                 GATEWAY,
                                 ANCHOR_A,
                                 report.session_id,
                                 78u,
                                 (uint8_t)ack_payload_len) == PROTO_OK);
    assert(mesh_relay_handle_rx(&origin,
                                &gateway_ack,
                                ack_payload,
                                ack_payload_len,
                                ANCHOR_B,
                                80u,
                                3120u,
                                &result) == PROTO_OK);
    assert(has_action(&result, MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED));
    assert(!mesh_relay_tx_active(&origin));
    assert(!origin.outbox_record.valid);
    assert(origin.outbox_record.delivery_state == MESH_RELAY_DELIVERY_GATEWAY_ACKED);
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
    assert(plan.start_ms == 6100u);
    assert(relay.event_timings[0].timing.missed_event_count == 1u);

    assert(mesh_relay_start_channel9_tx(&relay,
                                        &report,
                                        payload,
                                        sizeof(payload),
                                        &requirements,
                                        6100u,
                                        &plan,
                                        &tx) == PROTO_ERR_BUSY);
    assert(plan.action == MESH_EVENT_PLAN_START);
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
    struct proto_packet packet = {
        .msg_type = MSG_MESH_HOP_ACK,
        .src_id = ANCHOR_A,
        .dst_id = ANCHOR_B,
        .session_id = 901u,
        .seq = 14u,
        .ttl = MESH_GATEWAY_ACK_TTL,
    };
    uint8_t payload[8];
    uint8_t encoded[PACKET_MAX_LEN];
    size_t payload_len = 0u;
    size_t written = 0u;

    assert(mesh_append_requested_seq(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     13u) == PROTO_OK);
    packet.payload_len = (uint8_t)payload_len;

    assert(proto_packet_encode(&packet,
                               payload,
                               encoded,
                               sizeof(encoded),
                               &written) == PROTO_OK);
    assert(written == proto_packet_encoded_len(packet.payload_len));
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

static void test_gateway_survey_reverse_route_is_current_epoch_and_timing_free(void)
{
    struct mesh_relay gateway;
    const struct mesh_downlink_entry *downlink;
    struct mesh_outbound route_request;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 17u);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) ==
           PROTO_ERR_NOT_FOUND);
    assert(mesh_relay_prepare_route_request(&gateway,
                                            ANCHOR_A,
                                            1100u,
                                            UINT32_C(0x12345678),
                                            &route_request) == PROTO_OK);
    assert(gateway.route_discovery.active);
    assert(mesh_relay_note_gateway_survey_reverse_route(&gateway,
                                                        ANCHOR_A,
                                                        ANCHOR_A,
                                                        91u,
                                                        1200u) == PROTO_OK);
    assert(!gateway.route_discovery.active);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) ==
           PROTO_OK);
    assert(next_hop_id == ANCHOR_A);
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->route_epoch == 17u);
    assert(downlink->hop_count == 1u);
    assert(downlink->quality == 91u);
    assert(downlink->last_seen_ms == 1200u);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        assert(!gateway.event_timings[i].valid);
    }

    assert(mesh_relay_note_gateway_survey_reverse_route(&gateway,
                                                        ANCHOR_A,
                                                        ANCHOR_B,
                                                        77u,
                                                        1300u) == PROTO_OK);
    assert(mesh_relay_select_next_hop(&gateway, ANCHOR_A, &next_hop_id) ==
           PROTO_OK);
    assert(next_hop_id == ANCHOR_B);
    downlink = mesh_relay_find_downlink(&gateway, ANCHOR_A);
    assert(downlink != NULL);
    assert(downlink->route_epoch == 17u);
    assert(downlink->hop_count == MESH_NETWORK_MAX_HOPS);
    assert(downlink->last_seen_ms == 1300u);
}

static void test_gateway_survey_reverse_route_rejects_invalid_evidence(void)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 3u);
    mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR_A, GATEWAY, 3u);

    assert(mesh_relay_note_gateway_survey_reverse_route(&anchor,
                                                        ANCHOR_B,
                                                        ANCHOR_B,
                                                        80u,
                                                        1u) == PROTO_ERR_ARG);
    assert(mesh_relay_note_gateway_survey_reverse_route(&gateway,
                                                        0u,
                                                        ANCHOR_B,
                                                        80u,
                                                        1u) == PROTO_ERR_ARG);
    assert(mesh_relay_note_gateway_survey_reverse_route(&gateway,
                                                        ANCHOR_B,
                                                        0u,
                                                        80u,
                                                        1u) == PROTO_ERR_ARG);
    assert(mesh_relay_note_gateway_survey_reverse_route(&gateway,
                                                        ANCHOR_B,
                                                        ANCHOR_B,
                                                        101u,
                                                        1u) == PROTO_ERR_ARG);
    assert(mesh_relay_find_downlink(&gateway, ANCHOR_B) == NULL);
}

static void test_gateway_survey_reverse_route_reinstalls_beyond_table_capacity(void)
{
    struct mesh_relay gateway;
    const uint64_t anchor_base = UINT64_C(0xa700000000001000);
    const uint64_t relay_base = UINT64_C(0xa800000000001000);

    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY, 23u);
    for (size_t i = 0u; i < 50u; i++) {
        const uint64_t target_id = anchor_base + i;
        const uint64_t next_hop_id = i < 20u ? target_id : relay_base + (i % 5u);
        const struct mesh_downlink_entry *downlink;
        uint64_t selected_next_hop = 0u;

        assert(mesh_relay_note_gateway_survey_reverse_route(
                   &gateway,
                   target_id,
                   next_hop_id,
                   (uint8_t)(100u - i),
                   (uint32_t)(1000u + i)) == PROTO_OK);
        assert(mesh_relay_select_next_hop(&gateway,
                                          target_id,
                                          &selected_next_hop) == PROTO_OK);
        assert(selected_next_hop == next_hop_id);
        downlink = mesh_relay_find_downlink(&gateway, target_id);
        assert(downlink != NULL);
        assert(downlink->route_epoch == 23u);
    }
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
    assert(route_reply.next_hop_id == ANCHOR_A);
    assert(route_reply.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(route_reply.earliest_tx_ms ==
           1075u + (3u * RREP_RESPONDER_SLOT_MS));
    assert(require_tlv_u8(route_reply.payload,
                          route_reply.payload_len,
                          TLV_HOP_COUNT) == 1u);
}

static void test_gateway_commands_use_channel5_control_lane(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
    };

    assert(!mesh_relay_packet_requires_channel9_payload_event(NULL));
    assert(!mesh_relay_packet_requires_channel9_payload_event(&packet));
    packet.msg_type = MSG_SURVEY_DISCOVERY_START;
    assert(!mesh_relay_packet_requires_channel9_payload_event(&packet));
    packet.msg_type = MSG_SURVEY_PAIR_PREPARE;
    assert(!mesh_relay_packet_requires_channel9_payload_event(&packet));

    packet.msg_type = MSG_CLICK_REPORT;
    assert(mesh_relay_packet_requires_channel9_payload_event(&packet));
    packet.msg_type = MSG_COMMAND_RESULT;
    assert(mesh_relay_packet_requires_channel9_payload_event(&packet));
    packet.msg_type = MSG_GATEWAY_ACK;
    assert(mesh_relay_packet_requires_channel9_payload_event(&packet));
}

int main(void)
{
    test_mesh_relay_result_preserves_required_simultaneous_outputs();
    test_route_reply_nonce_value_and_identity();
    test_gateway_commands_use_channel5_control_lane();
    test_relay_forwards_gateway_bound_packet_and_reforwards_duplicate();
    test_duplicate_cache_expires_by_time_window();
    test_channel9_tx_requires_local_tx_slot();
    test_status_tlvs_report_selected_route();
    test_status_tlvs_report_missing_route_reason();
    test_legacy_route_beacons_are_dropped();
    test_survey_discovery_broadcast_delivers_and_floods();
    test_broadcast_command_delivers_without_flooding();
    test_command_flood_broadcast_delivers_and_forwards_once();
    test_collection_eack_broadcast_delivers_and_forwards_once();
    test_collection_eack_received_list_confirms_pending_result();
    test_collection_eack_received_list_schedules_retry_when_not_received();
    test_collection_eack_missing_list_schedules_patient_retry();
    test_collection_eack_missing_list_absent_node_confirms_delivery();
    test_collection_eack_closed_stops_pending_without_success();
    test_collection_eack_closed_roster_bitmap_stops_pending_without_list();
    test_collection_result_survives_route_loss_until_eack();
    test_click_preemption_preserves_pending_collection_result();
    test_collection_result_timeout_uses_collection_retry_round();
    test_collection_result_expires_without_retrying_forever();
    test_collection_outbox_snapshot_restores_after_reinit();
    test_collection_outbox_snapshot_preserves_retry_round_delay();
    test_collection_outbox_snapshot_rejects_corrupt_payload();
    test_collection_outbox_snapshot_rejects_completed_record();
    test_collection_eack_broadcast_rejects_wrong_gateway_epoch();
    test_busy_survey_discovery_broadcast_still_forwards();
    test_downlink_routes_survive_age_until_delivery_failure();
    test_downlink_route_selection_uses_weighted_quality();
    test_start_tx_accepts_aged_upstream_route_until_failures();
    test_downlink_next_hop_send_completes_immediately();
    test_downlink_send_refreshes_route_age();
    test_ttl_zero_packet_is_dropped_without_ack();
    test_zero_session_or_sequence_mesh_packets_are_rejected();
    test_busy_relay_does_not_ack_or_cache_new_forward();
    test_busy_relay_drops_duplicate_forward();
    test_busy_relay_sends_result_busy_for_command_result();
    test_result_busy_preserves_command_result_identity();
    test_result_offer_gets_result_grant_when_parent_has_capacity();
    test_result_transfer_requires_matching_offer_len_crc();
    test_result_offer_rejects_wrong_gateway_epoch();
    test_result_offer_gets_result_busy_when_parent_busy();
    test_result_offer_gets_result_busy_when_parent_capacity_red();
    test_large_command_result_starts_result_offer();
    test_result_grant_releases_pending_command_result();
    test_forwarded_child_result_offer_snapshot_restores_after_reinit();
    test_forwarded_child_result_payload_snapshot_restores_after_grant();
    test_result_busy_retries_result_offer_not_payload();
    test_result_busy_ignores_mismatched_command_result_identity();
    test_relay_busy_defers_matching_pending_tx();
    test_local_gateway_bound_tx_waits_for_gateway_ack();
    test_local_gateway_bound_tx_accepts_batched_gateway_ack();
    test_start_tx_initializes_earliest_tx_time();
    test_relayed_tx_completes_after_next_hop_send();
    test_gateway_ack_timeout_retries_then_requests_route_discovery();
    test_gateway_ack_timeout_handles_ms_wrap();
    test_deferred_retransmit_waits_for_actual_radio_send();
    test_route_discovery_continues_at_ttl_six_with_backoff();
    test_route_discovery_retry_sequence_handles_wrap_and_jitter();
    test_idle_route_solicit_without_upstream_is_forwarded_once();
    test_better_route_request_copy_updates_pending_forward();
    test_route_request_retry_uses_new_flood_identity();
    test_unanswered_timed_route_request_does_not_reserve_channel9();
    test_parent_relay_replies_without_child_route_discovery();
    test_parent_relay_rejects_existing_child_route_request_while_connected();
    test_route_request_expires_stale_channel9_before_capacity_check();
    test_parent_relay_rejects_unrelated_child_route_request_while_connected();
    test_gateway_route_advertisement_seeds_and_floods_parent_candidates();
    test_gateway_route_advertisement_snapshot_rebuild_is_exact();
    test_gateway_route_advertisement_reports_busy_capacity();
    test_route_discovery_ready_resets_attempt_budget();
    test_retry_and_route_discovery_backoff_apply_jitter();
    test_collection_retry_delay_uses_symmetric_jitter();
    test_held_down_candidate_can_return_after_hold_down();
    test_forced_route_invalidation_clears_routes_and_discovery_state();
    test_local_route_clear_preserves_gateway_epoch();
    test_forwarded_gateway_bound_packet_sends_hop_ack();
    test_hop_ack_extends_gateway_ack_timeout();
    test_hop_ack_waits_for_later_gateway_ack();
    test_local_acks_require_expected_physical_hop();
    test_hop_ack_outbox_survives_reset_until_gateway_ack();
    test_gateway_reaches_anchor_behind_relay_and_receives_result();
    test_gateway_delivers_direct_clicker_self_test_report_and_acks();
    test_gateway_ack_previous_hop_fallback_must_not_be_rerouted();
    test_duplicate_retry_repairs_lost_gateway_ack();
    test_reactive_gateway_route_request_and_reply();
    test_multihop_route_reply_forward_uses_channel_five();
    test_concurrent_route_replies_use_their_discovery_predecessor();
    test_relay_required_route_request_ignores_direct_gateway_copy();
    test_gateway_route_request_without_upstream_waits_for_route();
    test_malformed_route_request_does_not_poison_downlink_route();
    test_malformed_route_reply_does_not_poison_upstream_route();
    test_reactive_route_and_report_flow_over_uwb_mesh_frames();
    test_route_request_carries_reply_rx_eta();
    test_route_reply_waits_for_request_reply_eta();
    test_channel9_guard_allows_one_upstream_and_one_downstream();
    test_upstream_route_invalidation_preserves_downstream_connection();
    test_transit_abandon_preserves_upstream_connection();
    test_channel9_guard_rejects_second_peer_in_same_direction();
    test_channel9_guard_rejects_third_peer();
    test_channel9_guard_rejects_ambiguous_or_unknown_direction();
    test_channel9_guard_rejects_overlapping_upstream_downstream_windows();
    test_channel9_report_tx_requires_negotiated_event();
    test_channel9_result_bundle_tx_requires_negotiated_event();
    test_relay_accepts_collection_result_into_bundle_queue();
    test_relay_flushes_collection_bundle_when_queue_fills();
    test_relay_flushes_collection_bundle_after_hold_deadline();
    test_child_custody_bundle_snapshot_restores_after_reinit();
    test_child_custody_snapshot_rejects_corrupt_bundle_payload();
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
    test_direct_gateway_route_probe_marks_route_ready();
    test_direct_gateway_route_probe_clears_hold_down();
    test_gateway_survey_reverse_route_is_current_epoch_and_timing_free();
    test_gateway_survey_reverse_route_rejects_invalid_evidence();
    test_gateway_survey_reverse_route_reinstalls_beyond_table_capacity();
    test_direct_gateway_probe_route_answers_pending_request();
    return 0;
}
