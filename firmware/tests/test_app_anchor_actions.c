#include "app_anchor_actions.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned identifies, samples;
static int hardware_error;
static int identify(void *ctx) { (void)ctx; identifies++; return hardware_error; }
static int sample(void *ctx, uint16_t *mv)
{
    (void)ctx; samples++; *mv = 3712u; return hardware_error;
}
static const struct app_anchor_action_ops ops = {identify, sample, NULL};
static struct proto_packet command = {
    .msg_type = MSG_COMMAND, .src_id = 1u, .dst_id = 2u,
    .session_id = 100u, .seq = 1u,
};
static uint8_t payload[64];
static size_t length;
static struct app_anchor_actions state;

static void request(enum command_id id, uint32_t epoch, uint8_t hops)
{
    length = 0u;
    assert(tlv_append_u16(payload, sizeof(payload), &length, TLV_COMMAND_ID, id) == 0);
    assert(tlv_append_u32(payload, sizeof(payload), &length, TLV_DISCOVERY_ASSIGNMENT_EPOCH, epoch) == 0);
    assert(tlv_append_u8(payload, sizeof(payload), &length, TLV_HOP_COUNT, hops) == 0);
    command.payload_len = (uint16_t)length;
}

static struct app_anchor_action_result run(uint32_t boot, uint64_t now)
{
    struct app_anchor_action_result result;
    assert(app_anchor_action_execute(&state, &command, payload, length,
        2u, 1u, boot, now, &ops, &result) == 0);
    return result;
}

static void test_deadline_and_replay(void)
{
    uint64_t now = UINT64_C(0xffffffff) - 5000u;
    request(CMD_IDENTIFY_ANCHOR, 40u, 1u);
    assert(identifies == 0u);
    struct app_anchor_action_result first = run(10u, now);
    assert(first.status == COMMAND_OK && identifies == 1u);
    for (unsigned i = 0; i < 1000u; i++) {
        struct app_anchor_action_result retry = run(10u, now + i * 20u);
        assert(retry.sampled_at_ms == first.sampled_at_ms && identifies == 1u);
    }
    request(CMD_READ_ANCHOR_BATTERY, 40u, 1u);
    assert(run(10u, now + 21000u).status == COMMAND_DENIED && samples == 0u);
    command.session_id++; command.seq++;
    assert(run(10u, now + 22000u).status == COMMAND_OK && samples == 1u);
    assert(run(10u, now + 23000u).status == COMMAND_OK && samples == 1u);
    command.session_id--; command.seq--;
    request(CMD_IDENTIFY_ANCHOR, 40u, 1u);
    assert(run(10u, now + 24000u).status == COMMAND_INVALID_STATE && identifies == 1u);
    command.session_id += 2u; command.seq += 2u;
    command.message_age_ms = ANCHOR_ACTION_PER_HOP_MS;
    assert(run(10u, now + 25000u).status == COMMAND_TIMEOUT && identifies == 1u);
    command.message_age_ms = 0u;
    command.session_id = UINT32_MAX;
    memset(&state, 0, sizeof(state));
    assert(run(10u, now).status == COMMAND_OK);
    command.session_id = 1u;
    assert(run(10u, now + 20u).status == COMMAND_OK);
    /* A reset has a new volatile result identity. No cross-reset exactly-once claim. */
    assert(run(11u, now + 30u).boot_counter == 11u);
}

static void test_schema_and_failures(void)
{
    struct app_anchor_action_result result;
    uint8_t response[80]; size_t response_len;
    const uint8_t *value; uint8_t value_len;
    memset(&state, 0, sizeof(state));
    request(CMD_READ_ANCHOR_BATTERY, 40u, 8u);
    hardware_error = -EIO;
    result = run(12u, 100u);
    assert(result.status == COMMAND_INTERNAL_ERROR);
    unsigned previous_samples = samples;
    hardware_error = 0;
    assert(run(12u, 200u).status == COMMAND_INTERNAL_ERROR && samples == previous_samples);
    assert(app_anchor_action_result_payload(CMD_READ_ANCHOR_BATTERY, 2u, 40u,
        &result, response, sizeof(response), &response_len) == 0);
    assert(tlv_find_unique(response, response_len, TLV_BATTERY_MV, &value, &value_len) == PROTO_ERR_NOT_FOUND);
    command.session_id++; command.seq++;
    result = run(12u, 300u);
    assert(result.status == COMMAND_OK && result.battery_mv == 3712u);
    assert(app_anchor_action_result_payload(CMD_READ_ANCHOR_BATTERY, 2u, 40u,
        &result, response, sizeof(response), &response_len) == 0);
    assert(tlv_find_unique(response, response_len, TLV_BATTERY_MV, &value, &value_len) == 0);
    assert(value_len == 2u && proto_get_u16_le(value) == 3712u);
    for (size_t cap = 0u; cap < response_len; cap++) {
        size_t produced;
        assert(app_anchor_action_result_payload(CMD_READ_ANCHOR_BATTERY, 2u, 40u,
            &result, response, cap, &produced) == PROTO_ERR_NO_SPACE);
    }
    previous_samples = samples;
    for (size_t truncated = 0u; truncated < ANCHOR_ACTION_REQUEST_MAX_LEN; truncated++) {
        length = truncated; command.payload_len = (uint16_t)length;
        assert(run(12u, 400u).status == COMMAND_MALFORMED_PAYLOAD);
    }
    for (unsigned hops = 0u; hops < 256u; hops++) {
        request(CMD_READ_ANCHOR_BATTERY, 40u, (uint8_t)hops);
        uint32_t epoch; uint8_t decoded_hops;
        bool valid = hops >= 1u && hops <= 8u;
        assert((app_anchor_action_request(payload, length, &epoch, &decoded_hops) == 0) == valid);
        if (valid) assert(app_anchor_action_delivery_ms(decoded_hops) == hops * GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
    }
    request(CMD_READ_ANCHOR_BATTERY, 0u, 1u);
    assert(run(12u, 500u).status == COMMAND_MALFORMED_PAYLOAD);
    request(CMD_READ_ANCHOR_BATTERY, 40u, 1u);
    command.src_id = 9u;
    assert(run(12u, 500u).status == COMMAND_MALFORMED_PAYLOAD);
    command.src_id = 1u; command.dst_id = UINT64_MAX;
    assert(run(12u, 500u).status == COMMAND_MALFORMED_PAYLOAD);
    command.dst_id = 2u;
    assert(samples == previous_samples);
}

static void test_rgb_deadline(void)
{
    unsigned colors[8] = {0};
    for (uint32_t elapsed = 0u; elapsed < 20000u; elapsed++) {
        uint32_t next;
        uint8_t color = app_anchor_identify_color(elapsed, &next);
        assert(color == 0u || color == 1u || color == 2u || color == 4u);
        if (elapsed < 10000u) {
            assert(next > 0u && elapsed + next <= 10000u);
            colors[color]++;
        } else {
            assert(color == 0u && next == 0u);
        }
    }
    assert(colors[0] && colors[1] && colors[2] && colors[4]);
    uint32_t next;
    assert(app_anchor_identify_color(UINT32_MAX, &next) == 0u && next == 0u);
}

static const uint64_t result_anchor = UINT64_C(0xa000000012345678);
static const uint64_t result_gateway = UINT64_C(0xb000000087654321);
static const uint32_t result_session = UINT32_C(0x80000001);

static void assert_result_rx(const uint8_t *response, size_t response_len,
                             int expected)
{
    const uint64_t relay = UINT64_C(0xc000000011112222);
    struct proto_packet packet;

    assert(response_len <= UINT8_MAX);
    assert(mesh_init_command_result(&packet, result_anchor, result_gateway,
               result_session, UINT16_MAX, (uint8_t)response_len, false) ==
           PROTO_OK);
    assert(mesh_packet_rx_envelope_validate(&packet, response, response_len,
               result_anchor, result_gateway, result_gateway,
               UWB_CHANNEL_MESH_PAYLOAD, false) == expected);
    assert(mesh_packet_rx_envelope_validate(&packet, response, response_len,
               result_anchor, relay, result_gateway,
               UWB_CHANNEL_MESH_PAYLOAD, false) == expected);
    packet.ttl--;
    packet.message_age_ms = 1234u;
    assert(mesh_packet_rx_envelope_validate(&packet, response, response_len,
               relay, result_gateway, result_gateway,
               UWB_CHANNEL_MESH_PAYLOAD, false) == expected);
}

static size_t action_result_payload(enum command_id id, enum command_status status,
                                    uint8_t *response, size_t capacity)
{
    const struct app_anchor_action_result result = {
        .status = status,
        .reason = status == COMMAND_OK ? 0u : EIO,
        .battery_mv = 3712u,
        .boot_counter = UINT32_C(0xf0000001),
        .sampled_at_ms = UINT64_C(0x100000002),
    };
    size_t response_len;

    assert(app_anchor_action_result_payload(id, result_anchor, 40u, &result,
               response, capacity, &response_len) == PROTO_OK);
    return response_len;
}

static void test_reset_preserves_action_access_and_retry_identity(void)
{
    const enum command_id ids[] = {CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY};
    /* The request retains the GUI's known route epoch. The anchor may restore
     * an older durable epoch or have no assignment after RAM is cleared. */
    const uint32_t observed_epochs[] = {40u, 12u, 0u};

    hardware_error = 0;
    command.message_age_ms = 0u;
    for (size_t action = 0u; action < 2u; action++) {
        for (size_t boot = 0u; boot < 3u; boot++) {
            uint8_t response[128];
            size_t response_len;
            const uint8_t *value;
            uint8_t value_len;
            unsigned previous = action == 0u ? identifies : samples;

            /* A real reset clears the action cache and restarts uptime. An
             * exact request can execute once again under a new boot identity. */
            memset(&state, 0, sizeof(state));
            request(ids[action], 40u, 2u);
            struct app_anchor_action_result result = run(20u + (uint32_t)boot, 5u);
            assert(result.status == COMMAND_OK);
            assert(result.boot_counter == 20u + boot);
            assert((action == 0u ? identifies : samples) == previous + 1u);
            assert(run(20u + (uint32_t)boot, 6u).sampled_at_ms == 5u);
            assert((action == 0u ? identifies : samples) == previous + 1u);
            assert(app_anchor_action_result_payload(ids[action], result_anchor,
                       observed_epochs[boot], &result, response, sizeof(response),
                       &response_len) == PROTO_OK);
            assert(tlv_find_unique(response, response_len,
                       TLV_DISCOVERY_ASSIGNMENT_EPOCH, &value, &value_len) == PROTO_OK);
            assert(value_len == 4u && proto_get_u32_le(value) == observed_epochs[boot]);
            assert_result_rx(response, response_len, PROTO_OK);

            /* Epoch metadata is not a local admission gate, but mutation of
             * the retained request still cannot replay under the same ID. */
            request(ids[action], 39u, 2u);
            assert(run(20u + (uint32_t)boot, 7u).status == COMMAND_DENIED);
            assert((action == 0u ? identifies : samples) == previous + 1u);
        }
    }
}

static void test_produced_action_results_pass_direct_and_forwarded_rx(void)
{
    const enum command_id ids[] = {CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY};

    for (size_t command_index = 0u; command_index < 2u; command_index++) {
        for (unsigned status = COMMAND_OK; status <= COMMAND_INTERNAL_ERROR;
             status++) {
            uint8_t response[128];
            const size_t response_len = action_result_payload(ids[command_index],
                (enum command_status)status, response, sizeof(response));

            assert_result_rx(response, response_len, PROTO_OK);
            /* Error replies must not advertise either successful action output. */
            if (status != COMMAND_OK) {
                size_t malformed_len = response_len;

                assert(tlv_append_u16(response, sizeof(response), &malformed_len,
                           TLV_BATTERY_MV, 3712u) == PROTO_OK);
                assert_result_rx(response, malformed_len, PROTO_ERR_MALFORMED);
                malformed_len = response_len;
                assert(tlv_append_u32(response, sizeof(response), &malformed_len,
                           TLV_DURATION_MS, 10000u) == PROTO_OK);
                assert_result_rx(response, malformed_len, PROTO_ERR_MALFORMED);
            }
        }
    }
}

static void test_action_result_schema_rejects_malformed_fields(void)
{
    const enum command_id ids[] = {CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY};
    const uint8_t extras[] = {
        TLV_GATEWAY_ID, TLV_GATEWAY_EPOCH, TLV_COMMAND_SEQ, TLV_NODE_ID,
        TLV_RESULT_SEQ, TLV_COLLECTION_EPOCH_ID, TLV_HOP_COUNT, UINT8_MAX,
    };

    for (size_t command_index = 0u; command_index < 2u; command_index++) {
        uint8_t response[128], malformed[128];
        const size_t response_len = action_result_payload(ids[command_index],
            COMMAND_OK, response, sizeof(response));

        for (size_t offset = 0u; offset < response_len;) {
            const size_t width = response[offset + 1u];
            const size_t field_len = PROTO_TLV_HEADER_LEN + width;
            const size_t tail_len = response_len - offset - field_len;

            /* Every emitted field is required exactly once. */
            memcpy(malformed, response, offset);
            memcpy(&malformed[offset], &response[offset + field_len], tail_len);
            assert_result_rx(malformed, response_len - field_len,
                             PROTO_ERR_MALFORMED);
            memcpy(malformed, response, response_len);
            memcpy(&malformed[response_len], &response[offset], field_len);
            assert_result_rx(malformed, response_len + field_len,
                             PROTO_ERR_MALFORMED);
            /* Preserve valid TLV framing while making each width too small/big. */
            for (size_t new_width = width - 1u; new_width <= width + 1u;
                 new_width += 2u) {
                memcpy(malformed, response, offset + PROTO_TLV_HEADER_LEN);
                malformed[offset + 1u] = (uint8_t)new_width;
                memset(&malformed[offset + PROTO_TLV_HEADER_LEN], 0, new_width);
                memcpy(&malformed[offset + PROTO_TLV_HEADER_LEN + new_width],
                       &response[offset + field_len], tail_len);
                assert_result_rx(malformed, response_len - width + new_width,
                                 PROTO_ERR_MALFORMED);
            }
            offset += field_len;
        }
        for (size_t truncated = 0u; truncated < response_len; truncated++) {
            assert_result_rx(response, truncated, PROTO_ERR_MALFORMED);
        }
        for (size_t i = 0u; i < sizeof(extras); i++) {
            size_t malformed_len = response_len;

            memcpy(malformed, response, response_len);
            assert(tlv_append_u32(malformed, sizeof(malformed), &malformed_len,
                       extras[i], 1u) == PROTO_OK);
            assert_result_rx(malformed, malformed_len, PROTO_ERR_MALFORMED);
        }
        {
            size_t malformed_len = response_len;

            memcpy(malformed, response, response_len);
            if (ids[command_index] == CMD_IDENTIFY_ANCHOR) {
                assert(tlv_append_u16(malformed, sizeof(malformed), &malformed_len,
                           TLV_BATTERY_MV, 3712u) == PROTO_OK);
            } else {
                assert(tlv_append_u32(malformed, sizeof(malformed), &malformed_len,
                           TLV_DURATION_MS, 10000u) == PROTO_OK);
            }
            assert_result_rx(malformed, malformed_len, PROTO_ERR_MALFORMED);
        }
    }
}

static void test_action_result_identity_and_output_values(void)
{
    const enum command_id ids[] = {CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY};
    const struct {
        uint8_t type;
        uint64_t value;
    } invalid[] = {
        {TLV_ANCHOR_ID, 0u},
        {TLV_ANCHOR_ID, UINT64_C(0xd000000012345678)},
        {TLV_NODE_BOOT_COUNTER, 0u},
        {TLV_COMMAND_STATUS, COMMAND_INTERNAL_ERROR + 1u},
        {TLV_COMMAND_STATUS, UINT16_MAX},
        {TLV_COMMAND_STATUS, COMMAND_INTERNAL_ERROR}, /* Output with error. */
        {TLV_COMMAND_ID, 0u},
        {TLV_COMMAND_ID, CMD_GET_STATUS}, /* Boot counter is not collection ID. */
        {TLV_DURATION_MS, 0u},
        {TLV_DURATION_MS, 9999u},
        {TLV_DURATION_MS, 10001u},
        {TLV_DURATION_MS, UINT32_MAX},
    };

    for (size_t command_index = 0u; command_index < 2u; command_index++) {
        uint8_t response[128], malformed[128];
        const size_t response_len = action_result_payload(ids[command_index],
            COMMAND_OK, response, sizeof(response));

        for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            const uint8_t *value;
            uint8_t width;
            size_t offset;

            if (invalid[i].type == TLV_DURATION_MS &&
                ids[command_index] != CMD_IDENTIFY_ANCHOR) {
                continue;
            }
            assert(tlv_find_unique(response, response_len, invalid[i].type,
                       &value, &width) == PROTO_OK);
            offset = (size_t)(value - response);
            memcpy(malformed, response, response_len);
            for (uint8_t byte = 0u; byte < width; byte++) {
                malformed[offset + byte] = (uint8_t)(invalid[i].value >> (byte * 8u));
            }
            assert_result_rx(malformed, response_len, PROTO_ERR_MALFORMED);
        }
    }
}

static void test_action_schema_does_not_admit_partial_collection_identity(void)
{
    const struct command_result_id identity = {
        .gateway_id = UINT64_C(0xb000000087654321),
        .gateway_epoch = 9u,
        .command_seq = UINT32_C(0x80000001),
        .node_id = UINT64_C(0xa000000012345678),
        .node_boot_counter = 7u,
        .result_seq = UINT16_MAX,
    };
    uint8_t encoded_identity[80], response[128];
    size_t identity_len = 0u;

    assert(command_result_id_append_tlvs(encoded_identity,
               sizeof(encoded_identity), &identity_len, &identity) == PROTO_OK);
    /* All 62 nonempty partial identities still fail, even with a valid epoch. */
    for (unsigned mask = 0u; mask < 64u; mask++) {
        size_t response_len = 0u;
        size_t cursor = 0u;
        unsigned field = 0u;

        assert(tlv_append_u16(response, sizeof(response), &response_len,
                   TLV_COMMAND_ID, CMD_GET_STATUS) == PROTO_OK);
        assert(tlv_append_u16(response, sizeof(response), &response_len,
                   TLV_COMMAND_STATUS, COMMAND_OK) == PROTO_OK);
        assert(tlv_append_u8(response, sizeof(response), &response_len,
                   TLV_REASON, 0u) == PROTO_OK);
        while (cursor < identity_len) {
            const size_t field_len = PROTO_TLV_HEADER_LEN + encoded_identity[cursor + 1u];

            if ((mask & (1u << field)) != 0u) {
                memcpy(&response[response_len], &encoded_identity[cursor], field_len);
                response_len += field_len;
            }
            cursor += field_len;
            field++;
        }
        assert(field == 6u);
        if (mask == 0u) {
            assert_result_rx(response, response_len, PROTO_OK);
            continue;
        }
        assert_result_rx(response, response_len, PROTO_ERR_MALFORMED);
        assert(tlv_append_u32(response, sizeof(response), &response_len,
                   TLV_COLLECTION_EPOCH_ID, 0u) == PROTO_OK);
        assert_result_rx(response, response_len, PROTO_ERR_MALFORMED);
        proto_put_u32_le(&response[response_len - sizeof(uint32_t)], 3003u);
        assert_result_rx(response, response_len,
                         mask == 63u ? PROTO_OK : PROTO_ERR_MALFORMED);
    }
}

static void test_consecutive_action_replies_drain_hop_custody(void)
{
    const uint64_t parent_id = UINT64_C(0xc000000011112222);
    const enum command_id commands[] = {
        CMD_READ_ANCHOR_BATTERY, CMD_IDENTIFY_ANCHOR,
    };
    struct mesh_relay child, parent, gateway;
    struct mesh_anchor_downlink_store child_routes, parent_routes;
    struct mesh_gateway_ack_store gateway_acks;
    struct route_candidate child_route = {
        .next_hop_id = parent_id, .gateway_id = result_gateway,
        .route_epoch = 40u, .last_seen_ms = 1000u,
        .hop_count = 1u, .link_quality = 90u, .valid = true,
    };
    struct route_candidate parent_route = child_route;

    parent_route.next_hop_id = result_gateway;
    parent_route.hop_count = 0u;
    mesh_relay_init(&child, MESH_RELAY_ROLE_ANCHOR,
                    result_anchor, result_gateway, 40u);
    mesh_relay_init(&parent, MESH_RELAY_ROLE_ANCHOR,
                    parent_id, result_gateway, 40u);
    mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY,
                    result_gateway, result_gateway, 40u);
    assert(mesh_relay_attach_anchor_downlink_store(&child, &child_routes) == PROTO_OK);
    assert(mesh_relay_attach_anchor_downlink_store(&parent, &parent_routes) == PROTO_OK);
    assert(mesh_relay_attach_gateway_ack_store(&gateway, &gateway_acks) == PROTO_OK);
    /* The fixture has one observed route at each hop, with no direct fallback. */
    assert(route_upsert_candidate(&child.upstream, &child_route) == PROTO_OK);
    assert(route_upsert_candidate(&parent.upstream, &parent_route) == PROTO_OK);

    for (size_t command_index = 0u; command_index < 2u; command_index++) {
        uint8_t response[128], digest[SEMANTIC_DIGEST_SHA256_LEN];
        const size_t response_len = action_result_payload(commands[command_index],
            COMMAND_OK, response, sizeof(response));
        const uint32_t now = 1000u + (uint32_t)command_index * 1000u;
        struct proto_packet packet;
        struct mesh_outbound child_tx, parent_tx, hop_ack, gateway_ack, wrong_ack;
        struct mesh_relay_result received;

        assert(!mesh_relay_tx_active(&child));
        assert(!mesh_relay_tx_active(&parent));
        assert(mesh_init_command_result(&packet, result_anchor, result_gateway,
                   result_session + (uint32_t)command_index,
                   (uint16_t)(20u + command_index), (uint8_t)response_len,
                   false) == PROTO_OK);
        assert(mesh_packet_semantic_digest(&packet, response, response_len, digest));
        assert(mesh_relay_start_tx(&child, &packet, response, response_len,
                   now, &child_tx) == PROTO_OK);
        assert(child_tx.next_hop_id == parent_id);
        mesh_relay_note_tx_sent(&child, &child_tx, now + 1u);
        assert(mesh_relay_handle_rx(&parent, &child_tx.packet,
                   child_tx.payload, child_tx.payload_len, result_anchor,
                   90u, now + 2u, &received) == PROTO_OK);
        assert((received.actions & MESH_RELAY_ACTION_FORWARD) != 0u);
        assert((received.actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u);
        parent_tx = received.forward;
        hop_ack = received.hop_ack;

        /* Retain the exact upstream copy before the child receives custody ACK. */
        assert(mesh_relay_start_tx(&parent, &parent_tx.packet,
                   parent_tx.payload, parent_tx.payload_len, now + 3u,
                   &parent_tx) == PROTO_OK);
        assert(mesh_relay_bind_transit_previous_hop(&parent, &parent_tx,
                   result_anchor) == PROTO_OK);
        assert(parent_tx.next_hop_id == result_gateway);
        assert(parent.pending.payload_len == response_len);
        assert(memcmp(parent.pending.payload, response, response_len) == 0);
        wrong_ack = hop_ack;
        wrong_ack.payload[wrong_ack.payload_len - 1u] ^= 1u;
        assert(mesh_relay_handle_rx(&child, &wrong_ack.packet,
                   wrong_ack.payload, wrong_ack.payload_len, parent_id,
                   90u, now + 4u, &received) == PROTO_OK);
        assert(received.actions == MESH_RELAY_ACTION_NONE);
        assert(mesh_relay_tx_active(&child));
        assert(mesh_relay_handle_rx(&child, &hop_ack.packet,
                   hop_ack.payload, hop_ack.payload_len, parent_id,
                   90u, now + 5u, &received) == PROTO_OK);
        assert(received.actions == MESH_RELAY_ACTION_TX_NEXT_HOP_CUSTODY_ACCEPTED);
        assert(child.pending.state == MESH_RELAY_TX_WAIT_TERMINAL_COMMIT);
        assert(mesh_relay_tx_active(&child));
        assert(mesh_relay_commit_next_hop_custody_terminal(
                   &child, &child_tx.packet, digest) == PROTO_OK);
        assert(!mesh_relay_tx_active(&child));
        assert(mesh_relay_tx_active(&parent));

        mesh_relay_note_tx_sent(&parent, &parent_tx, now + 6u);
        assert(mesh_relay_handle_rx(&gateway, &parent_tx.packet,
                   parent_tx.payload, parent_tx.payload_len, parent_id,
                   90u, now + 7u, &received) == PROTO_OK);
        assert((received.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
        assert((received.actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) == 0u);
        assert(mesh_relay_tx_active(&parent));
        assert(mesh_relay_commit_gateway_delivery(&gateway, &parent_tx.packet,
                   parent_tx.payload, parent_tx.payload_len, parent_id,
                   now + 8u, &received) == PROTO_OK);
        assert(received.actions == MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
        gateway_ack = received.gateway_ack;
        wrong_ack = gateway_ack;
        wrong_ack.payload[wrong_ack.payload_len - 1u] ^= 1u;
        assert(mesh_relay_handle_rx(&parent, &wrong_ack.packet,
                   wrong_ack.payload, wrong_ack.payload_len, result_gateway,
                   90u, now + 9u, &received) == PROTO_OK);
        assert(received.actions == MESH_RELAY_ACTION_NONE);
        assert(mesh_relay_tx_active(&parent));
        assert(mesh_relay_handle_rx(&parent, &gateway_ack.packet,
                   gateway_ack.payload, gateway_ack.payload_len, result_gateway,
                   90u, now + 10u, &received) == PROTO_OK);
        assert((received.actions & MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_FORWARD_PENDING) == 0u);
        assert(received.actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED);
        assert(!mesh_relay_tx_active(&parent));
        assert(!parent.pending.gateway_ack_forward_pending);
        assert(!parent.pending.gateway_ack_confirm_pending);
        assert(!mesh_relay_tx_active(&gateway));
        /* The next loop reuses both holders without reinitialization/cancellation. */
    }
}

int main(void)
{
    test_deadline_and_replay();
    test_schema_and_failures();
    test_rgb_deadline();
    test_reset_preserves_action_access_and_retry_identity();
    test_produced_action_results_pass_direct_and_forwarded_rx();
    test_action_result_schema_rejects_malformed_fields();
    test_action_result_identity_and_output_values();
    test_action_schema_does_not_admit_partial_collection_identity();
    test_consecutive_action_replies_drain_hop_custody();
    puts("anchor action admission, retries, failures, wire capacity and RGB deadlines passed");
    return 0;
}
