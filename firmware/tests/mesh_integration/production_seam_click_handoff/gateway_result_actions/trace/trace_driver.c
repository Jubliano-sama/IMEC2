#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "semantic_digest.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IMEC_MESH_RELAY_GATEWAY_ONLY
#define IMEC_MESH_RELAY_GATEWAY_ONLY 0
#endif

#define TEST_GATEWAY_ID UINT64_C(0x0102030405060708)
#define TEST_ANCHOR_A_ID UINT64_C(0x1112131415161718)
#define TEST_ANCHOR_B_ID UINT64_C(0x2122232425262728)
#define TEST_GATEWAY_EPOCH 3u
#define TEST_RESULT_LEN 96u

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr,                                                    \
                    "requirement failed at %s:%d: %s\n",                     \
                    __FILE__,                                                  \
                    __LINE__,                                                  \
                    #condition);                                               \
            exit(2);                                                           \
        }                                                                      \
    } while (0)

struct result_fixture {
    struct command_result_id result_id;
    struct result_offer offer;
    struct proto_packet offer_packet;
    struct proto_packet result_packet;
    uint8_t offer_payload[96];
    size_t offer_payload_len;
    uint8_t result_payload[TEST_RESULT_LEN];
    size_t result_payload_len;
};

struct bundle_fixture {
    struct command_result_id result_id;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

static void trace_hex(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0u; i < length; i++) {
        printf("%02x", bytes[i]);
    }
}

static void trace_packet(const char *label,
                         const struct proto_packet *packet,
                         const uint8_t *payload,
                         size_t payload_len)
{
    REQUIRE(label != NULL);
    REQUIRE(packet != NULL);
    REQUIRE(packet->payload_len == payload_len);
    REQUIRE(payload_len == 0u || payload != NULL);

    printf("PACKET %s type=%u flags=%u src=%016" PRIx64
           " dst=%016" PRIx64 " session=%08" PRIx32
           " seq=%u ttl=%u wire_len=%u age=%" PRIu32 " payload=",
           label,
           packet->msg_type,
           packet->flags,
           packet->src_id,
           packet->dst_id,
           packet->session_id,
           packet->seq,
           packet->ttl,
           packet->payload_len,
           packet->message_age_ms);
    trace_hex(payload, payload_len);
    putchar('\n');
}

static void trace_outbound(const char *label,
                           const struct mesh_outbound *out)
{
    REQUIRE(label != NULL);
    REQUIRE(out != NULL);
    REQUIRE(out->packet.payload_len == out->payload_len);

    printf("OUT %s type=%u flags=%u src=%016" PRIx64
           " dst=%016" PRIx64 " session=%08" PRIx32
           " seq=%u ttl=%u wire_len=%u age=%" PRIu32
           " channel=%u next=%016" PRIx64
           " owner=%" PRIu32 " queued=%" PRIu32 "/%u"
           " earliest=%" PRIu32 "/%u flood_retry=%u payload=",
           label,
           out->packet.msg_type,
           out->packet.flags,
           out->packet.src_id,
           out->packet.dst_id,
           out->packet.session_id,
           out->packet.seq,
           out->packet.ttl,
           out->packet.payload_len,
           out->packet.message_age_ms,
           out->radio_channel,
           out->next_hop_id,
           out->handoff_owner_generation,
           out->queued_at_ms,
           out->queued_at_valid ? 1u : 0u,
           out->earliest_tx_ms,
           out->earliest_tx_valid ? 1u : 0u,
           out->flood_retry_count);
    trace_hex(out->payload, out->payload_len);
    putchar('\n');
}

static void trace_result(const char *label,
                         const struct mesh_relay_result *result)
{
    const uint32_t primary_actions =
        MESH_RELAY_ACTION_FORWARD |
        MESH_RELAY_ACTION_SEND_ROUTE_REQ |
        MESH_RELAY_ACTION_SEND_ROUTE_REPLY |
        MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV |
        MESH_RELAY_ACTION_RETRANSMIT |
        MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL;

    REQUIRE(label != NULL);
    REQUIRE(result != NULL);

    printf("RESULT %s status=%d actions=%08" PRIx32
           " route_target=%016" PRIx64
           " backup=%016" PRIx64 "/%u route_changed=%u\n",
           label,
           result->status,
           result->actions,
           result->route_discovery_target_id,
           result->route_reply_backup_next_hop_id,
           result->route_reply_backup_valid ? 1u : 0u,
           result->route_state_changed ? 1u : 0u);

    if ((result->actions & primary_actions) != 0u) {
        trace_outbound("primary", &result->forward);
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) != 0u) {
        trace_outbound("gateway_ack", &result->gateway_ack);
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u) {
        trace_outbound("hop_ack", &result->hop_ack);
    }
    if ((result->actions & (MESH_RELAY_ACTION_SEND_RELAY_BUSY |
                            MESH_RELAY_ACTION_SEND_RESULT_BUSY)) != 0u) {
        trace_outbound("busy", &result->busy);
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT) != 0u) {
        trace_outbound("result_grant", &result->result_grant);
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) != 0u) {
        trace_outbound("route_reply_ack", &result->route_reply_ack);
    }
}

static void require_specialized_pending_idle(const struct mesh_relay *gateway)
{
    REQUIRE(gateway != NULL);
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    REQUIRE(gateway->pending.state == MESH_RELAY_TX_IDLE);
#endif
}

static void trace_gateway_state(const char *label,
                                const struct mesh_relay *gateway,
                                const struct mesh_gateway_ack_store *ack_store)
{
    size_t route_count = 0u;
    size_t downlink_count = 0u;
    size_t duplicate_count = 0u;
    size_t flood_count = 0u;
    size_t timing_count = 0u;
    size_t ack_origin_count = 0u;
    size_t ack_identity_count = 0u;

    REQUIRE(label != NULL);
    REQUIRE(gateway != NULL);
    REQUIRE(ack_store != NULL);
    require_specialized_pending_idle(gateway);

    for (size_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        route_count += gateway->upstream.candidates[i].valid ? 1u : 0u;
    }
    for (size_t i = 0u; i < mesh_relay_downlink_capacity(gateway); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(gateway, i);

        downlink_count += entry != NULL && entry->valid ? 1u : 0u;
    }
    for (size_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        duplicate_count += gateway->duplicates[i].valid ? 1u : 0u;
    }
    for (size_t i = 0u; i < MESH_RELAY_FLOOD_SEEN_SIZE; i++) {
        flood_count += gateway->flood_seen[i].valid ? 1u : 0u;
    }
    for (size_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        timing_count += gateway->event_timings[i].valid ? 1u : 0u;
    }
    for (size_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX; i++) {
        ack_origin_count += ack_store->origin_src_ids[i] != 0u ? 1u : 0u;
    }
    for (size_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        ack_identity_count += ack_store->identities[i].owner_state != 0u ? 1u : 0u;
    }

    printf("STATE %s pending=%u tx_active=%u outbox=%u/%u"
           " bundle=%u/%" PRIu32 " offer=%u deadline=%" PRIu32
           " routes=%zu epoch=%" PRIu32 "/%u downlinks=%zu"
           " duplicates=%zu floods=%zu timings=%zu"
           " route_discovery=%u/%016" PRIx64 "/%" PRIu32
           " reply_expect=%u/%016" PRIx64 "/%" PRIu32
           " command_replay=%u/%" PRIu32
           " diag=%u,%u,%u next_seq=%u duplicate_next=%u flood_next=%u"
           " ack_origins=%zu ack_ids=%zu\n",
           label,
           gateway->pending.state,
           mesh_relay_tx_active(gateway) ? 1u : 0u,
           gateway->outbox_record.valid ? 1u : 0u,
           gateway->outbox_record.delivery_state,
           mesh_relay_result_bundle_pending(gateway) ? 1u : 0u,
           mesh_relay_result_bundle_due_ms(gateway),
           gateway->result_offer_reservation.valid ? 1u : 0u,
           gateway->result_offer_reservation_deadline_ms,
           route_count,
           gateway->upstream.current_epoch,
           gateway->upstream.selected_index,
           downlink_count,
           duplicate_count,
           flood_count,
           timing_count,
           gateway->route_discovery.active ? 1u : 0u,
           gateway->route_discovery.target_id,
           gateway->route_discovery.current_request_id,
           gateway->route_reply_ack_expectation.active ? 1u : 0u,
           gateway->route_reply_ack_expectation.peer_id,
           gateway->route_reply_ack_expectation.session_id,
           gateway->command_replay.initialized ? 1u : 0u,
           gateway->command_replay.newest_command_seq,
           gateway->diagnostics.flood_suppression_count,
           gateway->diagnostics.route_reply_retry_count,
           gateway->diagnostics.busy_response_count,
           gateway->next_seq,
           gateway->duplicate_next,
           gateway->flood_seen_next,
           ack_origin_count,
           ack_identity_count);

    if (gateway->result_offer_reservation.valid) {
        const struct mesh_result_offer_reservation *reservation =
            &gateway->result_offer_reservation;

        printf("OFFER_STATE %s child=%016" PRIx64
               " gateway=%016" PRIx64 " epoch=%u command=%" PRIu32
               " node=%016" PRIx64 " boot=%" PRIu32 " result=%u"
               " len=%u digest=",
               label,
               reservation->child_id,
               reservation->result_id.gateway_id,
               reservation->result_id.gateway_epoch,
               reservation->result_id.command_seq,
               reservation->result_id.node_id,
               reservation->result_id.node_boot_counter,
               reservation->result_id.result_seq,
               reservation->result_len);
        trace_hex(reservation->result_digest,
                  sizeof(reservation->result_digest));
        putchar('\n');
    }

    if (gateway->result_bundle.active) {
        const struct mesh_result_bundle_queue *bundle =
            &gateway->result_bundle;

        printf("BUNDLE_STATE %s gateway=%016" PRIx64
               " epoch=%u command=%" PRIu32 " collection=%" PRIu32
               " due=%" PRIu32 " count=%u\n",
               label,
               bundle->gateway_id,
               bundle->gateway_epoch,
               bundle->command_seq,
               bundle->collection_epoch_id,
               bundle->due_ms,
               bundle->record_count);
        for (size_t i = 0u; i < MESH_RELAY_RESULT_BUNDLE_RECORDS; i++) {
            const struct mesh_result_bundle_entry *entry =
                &bundle->records[i];

            if (!entry->valid) {
                continue;
            }
            printf("BUNDLE_RECORD %s i=%zu gateway=%016" PRIx64
                   " epoch=%u command=%" PRIu32 " node=%016" PRIx64
                   " boot=%" PRIu32 " result=%u len=%u crc=%u"
                   " age=%" PRIu32 " queued=%" PRIu32 " digest=",
                   label,
                   i,
                   entry->result_id.gateway_id,
                   entry->result_id.gateway_epoch,
                   entry->result_id.command_seq,
                   entry->result_id.node_id,
                   entry->result_id.node_boot_counter,
                   entry->result_id.result_seq,
                   entry->payload_len,
                   entry->payload_crc,
                   entry->message_age_ms,
                   entry->queued_at_ms);
            trace_hex(entry->semantic_digest,
                      sizeof(entry->semantic_digest));
            printf(" payload=");
            trace_hex(entry->payload, entry->payload_len);
            putchar('\n');
        }
    }

    for (size_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
        const struct route_candidate *candidate =
            &gateway->upstream.candidates[i];

        if (!candidate->valid) {
            continue;
        }
        printf("ROUTE_STATE %s i=%zu next=%016" PRIx64
               " gateway=%016" PRIx64 " epoch=%" PRIu32
               " seen=%" PRIu32 " success=%" PRIu32
               " hold=%" PRIu32 "/%u cost=%u free=%u hop=%u"
               " quality=%u failures=%u cap=%u busy=%u cap_at=%" PRIu32
               " cap_until=%" PRIu32 "/%u timing=%u\n",
               label,
               i,
               candidate->next_hop_id,
               candidate->gateway_id,
               candidate->route_epoch,
               candidate->last_seen_ms,
               candidate->last_success_ms,
               candidate->hold_down_until_ms,
               candidate->hold_down_valid ? 1u : 0u,
               candidate->route_cost,
               candidate->queue_free_hint,
               candidate->hop_count,
               candidate->link_quality,
               candidate->failure_count,
               candidate->relay_capacity_state,
               candidate->channel9_busy_hint,
               candidate->capacity_observed_at_ms,
               candidate->capacity_valid_until_ms,
               candidate->capacity_hint_valid ? 1u : 0u,
               candidate->channel9_timing_valid ? 1u : 0u);
    }

    for (size_t i = 0u; i < mesh_relay_downlink_capacity(gateway); i++) {
        const struct mesh_downlink_entry *entry =
            mesh_relay_downlink_at(gateway, i);

        if (entry == NULL || !entry->valid) {
            continue;
        }
        printf("DOWNLINK_STATE %s i=%zu target=%016" PRIx64
               " next=%016" PRIx64 " gateway=%016" PRIx64
               " epoch=%" PRIu32 " seen=%" PRIu32
               " flood=%" PRIu32 " hop=%u quality=%u failures=%u\n",
               label,
               i,
               entry->target_id,
               entry->next_hop_id,
               entry->gateway_id,
               entry->route_epoch,
               entry->last_seen_ms,
               entry->discovery_flood_epoch_id,
               entry->hop_count,
               entry->quality,
               entry->failure_count);
    }

    for (size_t i = 0u; i < MESH_RELAY_DUP_CACHE_SIZE; i++) {
        const struct mesh_duplicate_entry *entry = &gateway->duplicates[i];

        if (!entry->valid) {
            continue;
        }
        printf("DUP_STATE %s i=%zu type=%u src=%016" PRIx64
               " dst=%016" PRIx64 " session=%" PRIu32 " seq=%u"
               " seen=%" PRIu32 " busy_at=%" PRIu32
               " busy_interval=%u semantic=%u accepted=%u digest=",
               label,
               i,
               entry->msg_type,
               entry->src_id,
               entry->dst_id,
               entry->session_id,
               entry->seq,
               entry->last_seen_ms,
               entry->busy_response_at_ms,
               entry->busy_response_interval_ms,
               entry->semantic_identity_valid ? 1u : 0u,
               entry->delivery_accepted ? 1u : 0u);
        trace_hex(entry->semantic_digest, sizeof(entry->semantic_digest));
        putchar('\n');
    }

    for (size_t i = 0u; i < MESH_RELAY_FLOOD_SEEN_SIZE; i++) {
        const struct flood_seen_entry *entry = &gateway->flood_seen[i];

        if (!entry->valid) {
            continue;
        }
        printf("FLOOD_STATE %s i=%zu gateway=%016" PRIx64
               " epoch=%" PRIu32 " flood=%" PRIu32 " type=%u"
               " origin=%016" PRIx64 " request=%" PRIu32
               " hop=%u metric=%u prev=%016" PRIx64
               " backup=%016" PRIx64 " due=%" PRIu32
               " forward=%u heard=%u expires=%" PRIu32 "\n",
               label,
               i,
               entry->gateway_id,
               entry->gateway_epoch,
               entry->flood_epoch_id,
               entry->flood_type,
               entry->origin_id,
               entry->origin_request_id,
               entry->best_hop_count,
               entry->best_metric,
               entry->best_previous_hop,
               entry->backup_previous_hop,
               entry->forward_due_ms,
               entry->forward_count,
               entry->heard_count,
               entry->expires_at_ms);
    }

    for (size_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &gateway->event_timings[i];

        if (!entry->valid) {
            continue;
        }
        printf("TIMING_STATE %s i=%zu peer=%016" PRIx64
               " direction=%u interval=%" PRIu32 " window=%" PRIu32
               " next=%" PRIu32 " guard=%" PRIu32 "\n",
               label,
               i,
               entry->next_hop_id,
               entry->direction,
               entry->timing.event_interval_ms,
               (uint32_t)entry->timing.event_window_ms,
               entry->timing.next_event_time_ms,
               (uint32_t)entry->timing.guard_ms);
    }

    for (size_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_ORIGIN_MAX; i++) {
        if (ack_store->origin_src_ids[i] == 0u) {
            continue;
        }
        printf("ACK_ORIGIN_STATE %s i=%zu src=%016" PRIx64
               " batch=%" PRIu32 "\n",
               label,
               i,
               ack_store->origin_src_ids[i],
               ack_store->origin_batch_ids[i]);
    }
    for (size_t i = 0u; i < MESH_RELAY_GATEWAY_ACK_CAPACITY; i++) {
        const struct mesh_gateway_ack_identity_entry *entry =
            &ack_store->identities[i];

        if (entry->owner_state == 0u) {
            continue;
        }
        printf("ACK_ID_STATE %s i=%zu owner=%u type=%u session=%" PRIu32
               " seq=%u expires=%" PRIu32 " digest=",
               label,
               i,
               entry->owner_state,
               entry->msg_type,
               entry->session_id,
               entry->seq,
               entry->expires_at_ms);
        trace_hex(entry->semantic_digest, sizeof(entry->semantic_digest));
        putchar('\n');
    }
    printf("ACK_BITS %s candidate=", label);
    trace_hex(ack_store->candidate_identity_bits,
              sizeof(ack_store->candidate_identity_bits));
    printf(" confirmed=");
    trace_hex(ack_store->confirmed_identity_bits,
              sizeof(ack_store->confirmed_identity_bits));
    putchar('\n');
}

static void gateway_init(struct mesh_relay *gateway,
                         struct mesh_gateway_ack_store *ack_store)
{
    REQUIRE(gateway != NULL);
    REQUIRE(ack_store != NULL);

    mesh_gateway_ack_store_init(ack_store);
    mesh_relay_init(gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_EPOCH);
    REQUIRE(mesh_relay_attach_gateway_ack_store(gateway, ack_store) ==
            PROTO_OK);
    require_specialized_pending_idle(gateway);
}

static void append_padding_to(uint8_t *payload,
                              size_t payload_cap,
                              size_t *payload_len,
                              size_t target_len)
{
    const uint8_t padding[24] = {
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
        0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u,
    };

    REQUIRE(payload != NULL);
    REQUIRE(payload_len != NULL);
    REQUIRE(target_len <= payload_cap);
    while (*payload_len < target_len) {
        const size_t remaining = target_len - *payload_len;
        const size_t max_value_len = remaining - PROTO_TLV_HEADER_LEN;
        const uint8_t value_len =
            (uint8_t)(max_value_len > sizeof(padding) ?
                          sizeof(padding) :
                          max_value_len);

        REQUIRE(remaining >= PROTO_TLV_HEADER_LEN);
        REQUIRE(tlv_append_bytes(payload,
                                 payload_cap,
                                 payload_len,
                                 TLV_MESH_TEST_PADDING,
                                 padding,
                                 value_len) == PROTO_OK);
    }
    REQUIRE(*payload_len == target_len);
}

static void result_fixture_init(struct result_fixture *fixture,
                                uint64_t anchor_id,
                                uint32_t command_seq,
                                uint16_t result_seq)
{
    REQUIRE(fixture != NULL);
    memset(fixture, 0, sizeof(*fixture));

    fixture->result_id = (struct command_result_id) {
        .gateway_id = TEST_GATEWAY_ID,
        .gateway_epoch = TEST_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .node_id = anchor_id,
        .node_boot_counter = 7u,
        .result_seq = result_seq,
    };
    REQUIRE(command_result_id_append_tlvs(fixture->result_payload,
                                          sizeof(fixture->result_payload),
                                          &fixture->result_payload_len,
                                          &fixture->result_id) == PROTO_OK);
    REQUIRE(mesh_append_command_result(fixture->result_payload,
                                       sizeof(fixture->result_payload),
                                       &fixture->result_payload_len,
                                       CMD_GET_STATUS,
                                       COMMAND_OK,
                                       0u) == PROTO_OK);
    REQUIRE(tlv_append_u32(fixture->result_payload,
                           sizeof(fixture->result_payload),
                           &fixture->result_payload_len,
                           TLV_COLLECTION_EPOCH_ID,
                           UINT32_C(0x10203040)) == PROTO_OK);
    append_padding_to(fixture->result_payload,
                      sizeof(fixture->result_payload),
                      &fixture->result_payload_len,
                      TEST_RESULT_LEN);

    fixture->offer = (struct result_offer) {
        .result_id = fixture->result_id,
        .result_len = (uint16_t)fixture->result_payload_len,
        .result_crc = proto_crc16_ccitt_false(fixture->result_payload,
                                              fixture->result_payload_len),
        .priority = 4u,
    };
    REQUIRE(semantic_digest_sha256(fixture->result_payload,
                                   fixture->result_payload_len,
                                   fixture->offer.result_digest));
    REQUIRE(result_offer_append_tlvs(fixture->offer_payload,
                                     sizeof(fixture->offer_payload),
                                     &fixture->offer_payload_len,
                                     &fixture->offer) == PROTO_OK);
    fixture->offer_packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_OFFER,
        .src_id = anchor_id,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = command_seq,
        .seq = result_seq,
        .ttl = 1u,
        .payload_len = (uint16_t)fixture->offer_payload_len,
    };
    REQUIRE(mesh_init_command_result(&fixture->result_packet,
                                     anchor_id,
                                     TEST_GATEWAY_ID,
                                     command_seq,
                                     result_seq,
                                     (uint8_t)fixture->result_payload_len,
                                     false) == PROTO_OK);
}

static void bundle_fixture_init(struct bundle_fixture *fixture,
                                uint64_t anchor_id,
                                uint32_t command_seq,
                                uint16_t bundle_id)
{
    const uint8_t record_payload[] = {0x31u, 0x41u, 0x59u};
    uint8_t record_bytes[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t header_bytes[128];
    size_t record_len = 0u;
    size_t header_len = 0u;
    struct result_bundle_record record;
    struct result_bundle_header header;

    REQUIRE(fixture != NULL);
    memset(fixture, 0, sizeof(*fixture));

    fixture->result_id = (struct command_result_id) {
        .gateway_id = TEST_GATEWAY_ID,
        .gateway_epoch = TEST_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .node_id = anchor_id,
        .node_boot_counter = 8u,
        .result_seq = (uint16_t)(bundle_id + 1u),
    };
    record = (struct result_bundle_record) {
        .result_id = fixture->result_id,
        .payload_len = sizeof(record_payload),
        .payload_crc = proto_crc16_ccitt_false(record_payload,
                                               sizeof(record_payload)),
        .payload = record_payload,
    };
    REQUIRE(result_bundle_record_append_tlv(record_bytes,
                                            sizeof(record_bytes),
                                            &record_len,
                                            &record) == PROTO_OK);
    header = (struct result_bundle_header) {
        .gateway_id = TEST_GATEWAY_ID,
        .gateway_epoch = TEST_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .collection_epoch_id = UINT32_C(0x91929394),
        .bundle_id = bundle_id,
        .record_count = 1u,
        .bundle_crc = proto_crc16_ccitt_false(record_bytes, record_len),
    };
    REQUIRE(result_bundle_header_append_tlvs(header_bytes,
                                             sizeof(header_bytes),
                                             &header_len,
                                             &header) == PROTO_OK);
    REQUIRE(header_len + record_len <= sizeof(fixture->payload));
    memcpy(fixture->payload, header_bytes, header_len);
    memcpy(&fixture->payload[header_len], record_bytes, record_len);
    fixture->payload_len = header_len + record_len;
    fixture->packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_BUNDLE,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = anchor_id,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = command_seq,
        .seq = bundle_id,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = (uint16_t)fixture->payload_len,
    };
}

static void require_result_exact(const struct mesh_relay_result *result,
                                 int expected_status,
                                 uint32_t expected_actions)
{
    REQUIRE(result != NULL);
    REQUIRE(result->status == expected_status);
    REQUIRE(result->actions == expected_actions);
}

static void scenario_result_offer_and_command_result(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct result_fixture accepted;
    struct result_fixture competing;
    struct mesh_relay_result result;
    const uint32_t commit_ms = 4320u;
    uint32_t reservation_deadline;

    puts("SCENARIO result_offer_and_command_result");
    gateway_init(&gateway, &ack_store);
    result_fixture_init(&accepted,
                        TEST_ANCHOR_A_ID,
                        UINT32_C(0x22334455),
                        22u);
    result_fixture_init(&competing,
                        TEST_ANCHOR_B_ID,
                        UINT32_C(0x33445566),
                        23u);

    trace_packet("offer.accepted",
                 &accepted.offer_packet,
                 accepted.offer_payload,
                 accepted.offer_payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &accepted.offer_packet,
                                 accepted.offer_payload,
                                 accepted.offer_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 4300u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_RESULT_GRANT);
    trace_result("offer.accepted", &result);
    trace_gateway_state("offer.accepted", &gateway, &ack_store);
    reservation_deadline = gateway.result_offer_reservation_deadline_ms;

    mesh_relay_note_tx_sent(&gateway, &result.result_grant, 4301u);
    trace_gateway_state("offer.grant_sent", &gateway, &ack_store);

    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &accepted.offer_packet,
                                 accepted.offer_payload,
                                 accepted.offer_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 4302u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_RESULT_GRANT);
    REQUIRE(gateway.result_offer_reservation_deadline_ms ==
            reservation_deadline);
    trace_result("offer.retry", &result);
    trace_gateway_state("offer.retry", &gateway, &ack_store);

    trace_packet("offer.competing",
                 &competing.offer_packet,
                 competing.offer_payload,
                 competing.offer_payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &competing.offer_packet,
                                 competing.offer_payload,
                                 competing.offer_payload_len,
                                 TEST_ANCHOR_B_ID,
                                 89u,
                                 4303u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_BUSY,
                         MESH_RELAY_ACTION_SEND_RESULT_BUSY |
                             MESH_RELAY_ACTION_DROP);
    trace_result("offer.competing", &result);
    trace_gateway_state("offer.competing", &gateway, &ack_store);

    trace_packet("command_result.receive",
                 &accepted.result_packet,
                 accepted.result_payload,
                 accepted.result_payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &accepted.result_packet,
                                 accepted.result_payload,
                                 accepted.result_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 4310u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_DELIVER_LOCAL);
    trace_result("command_result.receive", &result);
    trace_gateway_state("command_result.receive", &gateway, &ack_store);

    REQUIRE(mesh_relay_commit_gateway_delivery(&gateway,
                                               &accepted.result_packet,
                                               accepted.result_payload,
                                               accepted.result_payload_len,
                                               TEST_ANCHOR_A_ID,
                                               commit_ms,
                                               &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    trace_result("command_result.commit", &result);
    /*
     * The gateway ACK is terminal. MSG_GATEWAY_ACK_CONFIRM is gone, so the
     * commit path confirms the retained identity immediately: it is a
     * duplicate-detect tombstone, never outstanding confirmation debt.
     */
    printf("QUERY command_result.confirmation_pending=%u\n",
           mesh_relay_gateway_identity_confirmation_pending(
               &gateway,
               accepted.result_packet.src_id,
               accepted.result_packet.msg_type,
               accepted.result_packet.session_id,
               accepted.result_packet.seq,
               commit_ms + 1u) ? 1u : 0u);
    REQUIRE(!mesh_relay_gateway_identity_confirmation_pending(
                &gateway,
                accepted.result_packet.src_id,
                accepted.result_packet.msg_type,
                accepted.result_packet.session_id,
                accepted.result_packet.seq,
                commit_ms + 1u));
    trace_gateway_state("command_result.commit", &gateway, &ack_store);

    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &accepted.result_packet,
                                 accepted.result_payload,
                                 accepted.result_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 commit_ms + 10u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_DELIVER_LOCAL);
    trace_result("command_result.duplicate", &result);
    trace_gateway_state("command_result.duplicate", &gateway, &ack_store);

    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &accepted.result_packet,
                                 accepted.result_payload,
                                 accepted.result_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 commit_ms + ROUTE_DEDUP_WINDOW_MS + 1u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_DELIVER_LOCAL |
                             MESH_RELAY_ACTION_CHILD_CUSTODY_CHANGED);
    trace_result("command_result.duplicate_after_cache", &result);
    trace_gateway_state("command_result.duplicate_after_cache",
                        &gateway,
                        &ack_store);
}

static void scenario_result_bundle(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct bundle_fixture bundle;
    struct mesh_relay_result result;
    const uint32_t commit_ms = 5200u;
    uint8_t mutated_payload[UWB_MESH_MAX_PAYLOAD_LEN];

    puts("SCENARIO result_bundle");
    gateway_init(&gateway, &ack_store);
    bundle_fixture_init(&bundle,
                        TEST_ANCHOR_B_ID,
                        UINT32_C(0x44556677),
                        31u);

    trace_packet("bundle.receive",
                 &bundle.packet,
                 bundle.payload,
                 bundle.payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &bundle.packet,
                                 bundle.payload,
                                 bundle.payload_len,
                                 TEST_ANCHOR_B_ID,
                                 88u,
                                 5190u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_DELIVER_LOCAL);
    trace_result("bundle.receive", &result);
    trace_gateway_state("bundle.receive", &gateway, &ack_store);

    REQUIRE(mesh_relay_commit_gateway_delivery(&gateway,
                                               &bundle.packet,
                                               bundle.payload,
                                               bundle.payload_len,
                                               TEST_ANCHOR_B_ID,
                                               commit_ms,
                                               &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    trace_result("bundle.commit", &result);
    /* Terminal gateway ACK: the bundle identity is confirmed at commit. */
    REQUIRE(!mesh_relay_gateway_identity_confirmation_pending(
                &gateway,
                bundle.packet.src_id,
                bundle.packet.msg_type,
                bundle.packet.session_id,
                bundle.packet.seq,
                commit_ms + 1u));
    printf("QUERY bundle.confirmation_pending=0\n");
    trace_gateway_state("bundle.commit", &gateway, &ack_store);

    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &bundle.packet,
                                 bundle.payload,
                                 bundle.payload_len,
                                 TEST_ANCHOR_B_ID,
                                 88u,
                                 commit_ms + 10u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_DELIVER_LOCAL);
    trace_result("bundle.duplicate_redelivery", &result);
    trace_gateway_state("bundle.duplicate_redelivery", &gateway, &ack_store);

    REQUIRE(mesh_relay_commit_gateway_delivery(&gateway,
                                               &bundle.packet,
                                               bundle.payload,
                                               bundle.payload_len,
                                               TEST_ANCHOR_B_ID,
                                               commit_ms + 11u,
                                               &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    trace_result("bundle.duplicate_commit", &result);
    trace_gateway_state("bundle.duplicate_commit", &gateway, &ack_store);

    memcpy(mutated_payload, bundle.payload, bundle.payload_len);
    mutated_payload[bundle.payload_len - 1u] ^= 1u;
    trace_packet("bundle.mutated_duplicate",
                 &bundle.packet,
                 mutated_payload,
                 bundle.payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &bundle.packet,
                                 mutated_payload,
                                 bundle.payload_len,
                                 TEST_ANCHOR_B_ID,
                                 88u,
                                 commit_ms + 12u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_MALFORMED,
                         MESH_RELAY_ACTION_DROP);
    trace_result("bundle.mutated_duplicate", &result);
    trace_gateway_state("bundle.mutated_duplicate", &gateway, &ack_store);
}

static void scenario_ordinary_command_result_duplicate(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct proto_packet packet;
    struct mesh_relay_result result;
    uint8_t payload[32];
    size_t payload_len = 0u;

    puts("SCENARIO ordinary_command_result_duplicate");
    gateway_init(&gateway, &ack_store);
    REQUIRE(mesh_append_command_result(payload,
                                       sizeof(payload),
                                       &payload_len,
                                       CMD_GET_STATUS,
                                       COMMAND_OK,
                                       9u) == PROTO_OK);
    REQUIRE(mesh_init_command_result(&packet,
                                     TEST_ANCHOR_A_ID,
                                     TEST_GATEWAY_ID,
                                     UINT32_C(0x51525354),
                                     32u,
                                     (uint8_t)payload_len,
                                     false) == PROTO_OK);

    trace_packet("ordinary_result.receive", &packet, payload, payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &packet,
                                 payload,
                                 payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 5300u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_DELIVER_LOCAL);
    trace_result("ordinary_result.receive", &result);
    trace_gateway_state("ordinary_result.receive", &gateway, &ack_store);

    REQUIRE(mesh_relay_commit_gateway_delivery(&gateway,
                                               &packet,
                                               payload,
                                               payload_len,
                                               TEST_ANCHOR_A_ID,
                                               5301u,
                                               &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    trace_result("ordinary_result.commit", &result);
    trace_gateway_state("ordinary_result.commit", &gateway, &ack_store);

    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &packet,
                                 payload,
                                 payload_len,
                                 TEST_ANCHOR_A_ID,
                                 90u,
                                 5310u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_STALE,
                         MESH_RELAY_ACTION_DROP |
                             MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    trace_result("ordinary_result.duplicate", &result);
    trace_gateway_state("ordinary_result.duplicate", &gateway, &ack_store);
}

static void build_route_request_fixture(struct mesh_outbound *request)
{
    struct mesh_relay origin;

    REQUIRE(request != NULL);
    mesh_relay_init(&origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_EPOCH);
    REQUIRE(mesh_relay_build_route_request(&origin,
                                           TEST_GATEWAY_ID,
                                           request,
                                           6100u) == PROTO_OK);
}

static void build_route_reply_fixture(struct mesh_outbound *reply)
{
    struct mesh_relay synthetic_origin;
    struct mesh_relay responder;
    struct mesh_outbound request;

    REQUIRE(reply != NULL);
    mesh_relay_init(&synthetic_origin,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_GATEWAY_ID,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_EPOCH);
    REQUIRE(mesh_relay_build_route_request(&synthetic_origin,
                                           TEST_ANCHOR_A_ID,
                                           &request,
                                           6200u) == PROTO_OK);
    mesh_relay_init(&responder,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_A_ID,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_EPOCH);
    REQUIRE(mesh_relay_build_route_reply_for_request(&responder,
                                                     &request.packet,
                                                     request.payload,
                                                     request.payload_len,
                                                     TEST_GATEWAY_ID,
                                                     6201u,
                                                     0u,
                                                     reply) == PROTO_OK);
}

static void build_route_reply_ack_fixture(const struct mesh_outbound *reply,
                                          struct proto_packet *ack_packet,
                                          uint8_t *ack_payload,
                                          size_t ack_payload_cap,
                                          size_t *ack_payload_len)
{
    const uint8_t *commitment = NULL;
    uint8_t commitment_len = 0u;

    REQUIRE(reply != NULL);
    REQUIRE(ack_packet != NULL);
    REQUIRE(ack_payload != NULL);
    REQUIRE(ack_payload_len != NULL);
    *ack_payload_len = 0u;
    REQUIRE(tlv_find_unique(reply->payload,
                            reply->payload_len,
                            TLV_ROUTE_REPLY_SHA256_COMMITMENT,
                            &commitment,
                            &commitment_len) == PROTO_OK);
    REQUIRE(commitment_len == SEMANTIC_DIGEST_SHA256_LEN);
    REQUIRE(tlv_append_bytes(ack_payload,
                             ack_payload_cap,
                             ack_payload_len,
                             TLV_ROUTE_REPLY_SHA256_COMMITMENT,
                             commitment,
                             commitment_len) == PROTO_OK);
    *ack_packet = (struct proto_packet) {
        .msg_type = MSG_ROUTE_REPLY_ACK,
        .src_id = TEST_ANCHOR_A_ID,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = reply->packet.session_id,
        .seq = 77u,
        .ttl = 1u,
        .payload_len = (uint16_t)*ack_payload_len,
    };
}

static void scenario_idle_route_control(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_outbound request;
    struct mesh_outbound reply;
    struct mesh_relay_result result;
    struct proto_packet ack_packet;
    uint8_t ack_payload[MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;

    puts("SCENARIO idle_route_control");

    build_route_request_fixture(&request);
    gateway_init(&gateway, &ack_store);
    trace_packet("route_request.receive",
                 &request.packet,
                 request.payload,
                 request.payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &request.packet,
                                 request.payload,
                                 request.payload_len,
                                 TEST_ANCHOR_A_ID,
                                 85u,
                                 6110u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_STALE,
                         MESH_RELAY_ACTION_DROP);
    trace_result("route_request.receive", &result);
    trace_gateway_state("route_request.receive", &gateway, &ack_store);

    build_route_reply_fixture(&reply);
    gateway_init(&gateway, &ack_store);
    trace_packet("route_reply.receive",
                 &reply.packet,
                 reply.payload,
                 reply.payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &reply.packet,
                                 reply.payload,
                                 reply.payload_len,
                                 TEST_ANCHOR_A_ID,
                                 84u,
                                 6210u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK |
                             MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY);
    trace_result("route_reply.receive", &result);
    trace_gateway_state("route_reply.receive", &gateway, &ack_store);

    build_route_reply_ack_fixture(&reply,
                                  &ack_packet,
                                  ack_payload,
                                  sizeof(ack_payload),
                                  &ack_payload_len);
    gateway_init(&gateway, &ack_store);
    trace_packet("route_reply_ack.receive",
                 &ack_packet,
                 ack_payload,
                 ack_payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &ack_packet,
                                 ack_payload,
                                 ack_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 84u,
                                 6220u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_STALE,
                         MESH_RELAY_ACTION_DROP);
    trace_result("route_reply_ack.receive", &result);
    trace_gateway_state("route_reply_ack.receive", &gateway, &ack_store);
}

static void scenario_route_reply_ack_commitment(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay requester;
    struct mesh_outbound request;
    struct mesh_outbound reply;
    struct mesh_relay_result result;
    struct proto_packet ack_packet;
    uint8_t ack_payload[MESH_ROUTE_REPLY_ACK_MAX_PAYLOAD_LEN];
    size_t ack_payload_len = 0u;

    puts("SCENARIO route_reply_ack_commitment");
    mesh_relay_init(&requester,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_EPOCH);
    REQUIRE(mesh_relay_prepare_route_request(&requester,
                                             TEST_GATEWAY_ID,
                                             6300u,
                                             UINT32_C(0x10203040),
                                             &request) == PROTO_OK);
    gateway_init(&gateway, &ack_store);
    REQUIRE(mesh_relay_build_route_reply_for_request(&gateway,
                                                     &request.packet,
                                                     request.payload,
                                                     request.payload_len,
                                                     TEST_ANCHOR_A_ID,
                                                     6301u,
                                                     0u,
                                                     &reply) == PROTO_OK);
    trace_packet("route_reply_commitment.request",
                 &request.packet,
                 request.payload,
                 request.payload_len);
    trace_outbound("route_reply_commitment.reply", &reply);
    mesh_relay_note_tx_sent(&gateway, &reply, 6302u);
    REQUIRE(gateway.route_reply_ack_expectation.active);
    trace_gateway_state("route_reply_commitment.sent",
                        &gateway,
                        &ack_store);

    build_route_reply_ack_fixture(&reply,
                                  &ack_packet,
                                  ack_payload,
                                  sizeof(ack_payload),
                                  &ack_payload_len);
    trace_packet("route_reply_commitment.ack",
                 &ack_packet,
                 ack_payload,
                 ack_payload_len);
    REQUIRE(mesh_relay_handle_rx(&gateway,
                                 &ack_packet,
                                 ack_payload,
                                 ack_payload_len,
                                 TEST_ANCHOR_A_ID,
                                 91u,
                                 6303u,
                                 &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_OK,
                         MESH_RELAY_ACTION_ROUTE_REPLY_ACKED);
    REQUIRE(!gateway.route_reply_ack_expectation.active);
    trace_result("route_reply_commitment.ack", &result);
    trace_gateway_state("route_reply_commitment.acked",
                        &gateway,
                        &ack_store);
}

static void scenario_ready_bundle_tick(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct result_fixture fixture;
    struct mesh_result_bundle_entry *entry;
    struct mesh_relay_result result;
    const uint32_t now_ms = 7510u;

    puts("SCENARIO ready_bundle_tick");
    gateway_init(&gateway, &ack_store);
    result_fixture_init(&fixture,
                        TEST_ANCHOR_A_ID,
                        UINT32_C(0x75767778),
                        44u);
    gateway.result_bundle = (struct mesh_result_bundle_queue) {
        .gateway_id = fixture.result_id.gateway_id,
        .gateway_epoch = fixture.result_id.gateway_epoch,
        .command_seq = fixture.result_id.command_seq,
        .collection_epoch_id = UINT32_C(0x10203040),
        .due_ms = now_ms,
        .record_count = 1u,
        .active = true,
    };
    entry = &gateway.result_bundle.records[0];
    entry->result_id = fixture.result_id;
    entry->payload_len = (uint16_t)fixture.result_payload_len;
    entry->payload_crc = proto_crc16_ccitt_false(fixture.result_payload,
                                                 fixture.result_payload_len);
    entry->message_age_ms = 17u;
    entry->queued_at_ms = now_ms - 10u;
    memcpy(entry->payload,
           fixture.result_payload,
           fixture.result_payload_len);
    REQUIRE(semantic_digest_sha256(entry->payload,
                                   entry->payload_len,
                                   entry->semantic_digest));
    entry->valid = true;
    REQUIRE(mesh_relay_result_bundle_pending(&gateway));
    REQUIRE(mesh_relay_result_bundle_due_ms(&gateway) == now_ms);
    trace_gateway_state("ready_bundle.before", &gateway, &ack_store);

    REQUIRE(mesh_relay_tick(&gateway, now_ms, &result) == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_MALFORMED,
                         MESH_RELAY_ACTION_NONE);
    REQUIRE(mesh_relay_result_bundle_pending(&gateway));
    REQUIRE(mesh_relay_result_bundle_due_ms(&gateway) ==
            now_ms + RELAY_BUSY_RETRY_MIN_MS);
    trace_result("ready_bundle.tick", &result);
    trace_gateway_state("ready_bundle.after", &gateway, &ack_store);
}

static void append_ack_identity(uint8_t *payload,
                                size_t payload_cap,
                                size_t *payload_len,
                                const struct proto_packet *acknowledged_packet,
                                const uint8_t *acknowledged_payload,
                                size_t acknowledged_payload_len)
{
    REQUIRE(mesh_append_requested_seq(payload,
                                      payload_cap,
                                      payload_len,
                                      acknowledged_packet->seq) == PROTO_OK);
    REQUIRE(mesh_append_ack_semantic_identity(payload,
                                              payload_cap,
                                              payload_len,
                                              acknowledged_packet,
                                              acknowledged_payload,
                                              acknowledged_payload_len) ==
            PROTO_OK);
}

static void build_relay_busy_payload(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len,
                                     uint32_t requested_session,
                                     uint16_t requested_seq)
{
    REQUIRE(tlv_append_u32(payload,
                           payload_cap,
                           payload_len,
                           TLV_REQUESTED_MSG_SESSION_ID,
                           requested_session) == PROTO_OK);
    REQUIRE(mesh_append_requested_seq(payload,
                                      payload_cap,
                                      payload_len,
                                      requested_seq) == PROTO_OK);
    REQUIRE(tlv_append_u16(payload,
                           payload_cap,
                           payload_len,
                           TLV_RETRY_AFTER_MS,
                           RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
    REQUIRE(tlv_append_u8(payload,
                          payload_cap,
                          payload_len,
                          TLV_RELAY_CAPACITY_STATE,
                          RELAY_CAP_GREEN) == PROTO_OK);
    REQUIRE(tlv_append_u16(payload,
                           payload_cap,
                           payload_len,
                           TLV_CAPACITY_VALIDITY_INTERVAL_MS,
                           RELAY_BUSY_RETRY_MIN_MS) == PROTO_OK);
}

static void run_noop_rx(struct mesh_relay *gateway,
                        struct mesh_gateway_ack_store *ack_store,
                        const char *label,
                        const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms)
{
    struct mesh_relay_result result;

    trace_packet(label, packet, payload, payload_len);
    REQUIRE(mesh_relay_handle_rx(gateway,
                                 packet,
                                 payload,
                                 payload_len,
                                 packet->src_id,
                                 80u,
                                 now_ms,
                                 &result) == PROTO_OK);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);
    trace_result(label, &result);
    trace_gateway_state(label, gateway, ack_store);
}

static void scenario_idle_local_responses(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct result_fixture acknowledged;
    struct proto_packet packet;
    struct result_grant grant;
    struct result_busy busy;
    uint8_t payload[128];
    size_t payload_len;

    puts("SCENARIO idle_local_responses");
    gateway_init(&gateway, &ack_store);
    result_fixture_init(&acknowledged,
                        TEST_ANCHOR_A_ID,
                        UINT32_C(0x81828384),
                        51u);

    payload_len = 0u;
    append_ack_identity(payload,
                        sizeof(payload),
                        &payload_len,
                        &acknowledged.result_packet,
                        acknowledged.result_payload,
                        acknowledged.result_payload_len);
    REQUIRE(mesh_init_gateway_ack(&packet,
                                  TEST_ANCHOR_A_ID,
                                  TEST_GATEWAY_ID,
                                  acknowledged.result_packet.session_id,
                                  52u,
                                  (uint8_t)payload_len) == PROTO_OK);
    run_noop_rx(&gateway,
                &ack_store,
                "idle.gateway_ack",
                &packet,
                payload,
                payload_len,
                8100u);

    packet.msg_type = MSG_MESH_HOP_ACK;
    packet.flags = 0u;
    packet.seq = 53u;
    packet.ttl = MESH_GATEWAY_ACK_TTL;
    run_noop_rx(&gateway,
                &ack_store,
                "idle.hop_ack",
                &packet,
                payload,
                payload_len,
                8110u);

    grant = (struct result_grant) {
        .result_id = acknowledged.result_id,
        .granted_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .max_bytes = TEST_RESULT_LEN,
        .event_offset_hint = 0u,
    };
    payload_len = 0u;
    REQUIRE(result_grant_append_tlvs(payload,
                                     sizeof(payload),
                                     &payload_len,
                                     &grant) == PROTO_OK);
    packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_GRANT,
        .src_id = TEST_ANCHOR_A_ID,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = acknowledged.result_id.command_seq,
        .seq = 54u,
        .ttl = 1u,
        .payload_len = (uint16_t)payload_len,
    };
    run_noop_rx(&gateway,
                &ack_store,
                "idle.result_grant",
                &packet,
                payload,
                payload_len,
                8120u);

    payload_len = 0u;
    build_relay_busy_payload(payload,
                             sizeof(payload),
                             &payload_len,
                             acknowledged.result_packet.session_id,
                             acknowledged.result_packet.seq);
    packet = (struct proto_packet) {
        .msg_type = MSG_RELAY_BUSY,
        .src_id = TEST_ANCHOR_A_ID,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = acknowledged.result_packet.session_id,
        .seq = 55u,
        .ttl = 1u,
        .payload_len = (uint16_t)payload_len,
    };
    run_noop_rx(&gateway,
                &ack_store,
                "idle.relay_busy",
                &packet,
                payload,
                payload_len,
                8130u);

    busy = (struct result_busy) {
        .result_id = acknowledged.result_id,
        .retry_after_ms = RELAY_BUSY_RETRY_MIN_MS,
        .capacity_state = RELAY_CAP_GREEN,
        .capacity_validity_interval_ms = RELAY_BUSY_RETRY_MIN_MS,
    };
    payload_len = 0u;
    REQUIRE(tlv_append_u32(payload,
                           sizeof(payload),
                           &payload_len,
                           TLV_REQUESTED_MSG_SESSION_ID,
                           acknowledged.result_packet.session_id) ==
            PROTO_OK);
    REQUIRE(mesh_append_requested_seq(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      acknowledged.result_packet.seq) ==
            PROTO_OK);
    REQUIRE(result_busy_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &busy) == PROTO_OK);
    packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_BUSY,
        .src_id = TEST_ANCHOR_A_ID,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = acknowledged.result_packet.session_id,
        .seq = 56u,
        .ttl = 1u,
        .payload_len = (uint16_t)payload_len,
    };
    run_noop_rx(&gateway,
                &ack_store,
                "idle.result_busy",
                &packet,
                payload,
                payload_len,
                8140u);
}

static size_t build_event_control_fixture(struct proto_packet *packet,
                                          uint8_t msg_type,
                                          uint32_t session_id,
                                          uint16_t seq,
                                          uint32_t now_ms,
                                          uint8_t *payload,
                                          size_t payload_cap)
{
    const struct mesh_event_params params = {
        .event_interval_ms = 100u,
        .event_window_ms = 20u,
        .first_event_time_ms = now_ms + 500u,
        .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 2u,
        .supervision_timeout_ms = 500u,
    };
    struct mesh_event_timing timing = {0};
    size_t payload_len = 0u;

    REQUIRE(packet != NULL);
    REQUIRE(payload != NULL);
    if (msg_type != MSG_MESH_EVENT_END) {
        REQUIRE(mesh_event_timing_negotiate(&timing, &params, true) ==
                PROTO_OK);
        REQUIRE(mesh_event_timing_bind_proposal_session(&timing,
                                                        session_id));
        if (msg_type == MSG_MESH_EVENT_UPDATE) {
            REQUIRE(mesh_append_event_update_tlvs_at(payload,
                                                     payload_cap,
                                                     &payload_len,
                                                     &timing,
                                                     now_ms) == PROTO_OK);
        } else {
            REQUIRE(mesh_append_event_timing_tlvs_at(payload,
                                                     payload_cap,
                                                     &payload_len,
                                                     &timing,
                                                     now_ms) == PROTO_OK);
        }
        if (msg_type == MSG_MESH_EVENT_PROPOSE) {
            REQUIRE(tlv_append_u64(payload,
                                   payload_cap,
                                   &payload_len,
                                   TLV_MESH_EVENT_BOOT_NONCE,
                                   UINT64_C(0x123456789abcdef0)) == PROTO_OK);
        }
    }
    REQUIRE(mesh_init_event_control(packet,
                                    msg_type,
                                    TEST_ANCHOR_A_ID,
                                    TEST_GATEWAY_ID,
                                    session_id,
                                    seq,
                                    (uint8_t)payload_len) == PROTO_OK);
    return payload_len;
}

static void scenario_clicker_only_control_envelopes(void)
{
    static const uint8_t msg_types[] = {
        MSG_MESH_EVENT_PROPOSE,
        MSG_MESH_EVENT_ACCEPT,
        MSG_MESH_EVENT_UPDATE,
        MSG_MESH_EVENT_END,
    };
    static const char *const labels[] = {
        "clicker_control.event_propose",
        "clicker_control.event_accept",
        "clicker_control.event_update",
        "clicker_control.event_end",
    };
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;

    puts("SCENARIO clicker_only_control_envelopes");
    gateway_init(&gateway, &ack_store);
    for (size_t i = 0u; i < ARRAY_SIZE(msg_types); i++) {
        struct proto_packet packet;
        struct mesh_relay_result result;
        uint8_t payload[96];
        const uint32_t now_ms = 8200u + (uint32_t)(i * 10u);
        const uint32_t session_id = UINT32_C(0x82830000) + (uint32_t)i;
        size_t payload_len;

        payload_len = build_event_control_fixture(
            &packet,
            msg_types[i],
            session_id,
            (uint16_t)(60u + i),
            now_ms,
            payload,
            sizeof(payload));
        trace_packet(labels[i], &packet, payload, payload_len);
        REQUIRE(mesh_relay_handle_rx(&gateway,
                                     &packet,
                                     payload,
                                     payload_len,
                                     TEST_ANCHOR_A_ID,
                                     86u,
                                     now_ms,
                                     &result) == PROTO_OK);
        require_result_exact(&result,
                             PROTO_OK,
                             MESH_RELAY_ACTION_DELIVER_LOCAL);
        trace_result(labels[i], &result);
        trace_gateway_state(labels[i], &gateway, &ack_store);
    }
}

static void trace_collection_state(
    const char *label,
    const struct gateway_collection_state *collection)
{
    REQUIRE(label != NULL);
    REQUIRE(collection != NULL);
    printf("COLLECTION %s gateway=%016" PRIx64
           " epoch=%u command=%" PRIu32 " collection=%" PRIu32
           " membership=%u expected=%u received=%u round=%u"
           " sequence=%u spread=%" PRIu32 " open=%u pending=%u\n",
           label,
           collection->gateway_id,
           collection->gateway_epoch,
           collection->command_seq,
           collection->collection_epoch_id,
           collection->membership_epoch,
           collection->expected_count,
           collection->received_count,
           collection->retry_round,
           collection->eack_sequence,
           collection->next_retry_spread_ms,
           collection->collection_open ? 1u : 0u,
           collection->eack_pending ? 1u : 0u);
}

static void scenario_eack_creation_retry_and_inbound(void)
{
    struct gateway_collection_state collection;
    struct mesh_outbound first_eack;
    struct mesh_outbound foreign_eack;
    struct mesh_outbound retry_eack;
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay gateway;
    struct mesh_relay_result result;
    int ret;

    puts("SCENARIO eack_creation_retry_and_inbound");
    REQUIRE(gateway_collection_start(
                &collection,
                TEST_GATEWAY_ID,
                TEST_GATEWAY_EPOCH,
                UINT32_C(0x84858687),
                UINT32_C(0x94959697),
                11u,
                2u,
                0u,
                gateway_command_collection_retry_spread_ms(0u)) ==
            PROTO_OK);
    trace_collection_state("eack.initial", &collection);
    REQUIRE(gateway_collection_prepare_eack_outbound(
                &collection,
                EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                &first_eack) == PROTO_OK);
    trace_outbound("eack.initial", &first_eack);

    REQUIRE(gateway_collection_advance_retry_round(&collection) ==
            PROTO_OK);
    trace_collection_state("eack.retry", &collection);
    REQUIRE(gateway_collection_prepare_eack_outbound(
                &collection,
                EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                &retry_eack) == PROTO_OK);
    trace_outbound("eack.retry", &retry_eack);

    foreign_eack = first_eack;
    foreign_eack.packet.src_id = TEST_ANCHOR_A_ID;
    gateway_init(&gateway, &ack_store);
    trace_packet("eack.role_impossible_receive",
                 &foreign_eack.packet,
                 foreign_eack.payload,
                 foreign_eack.payload_len);
    ret = mesh_relay_handle_rx(&gateway,
                               &foreign_eack.packet,
                               foreign_eack.payload,
                               foreign_eack.payload_len,
                               TEST_ANCHOR_A_ID,
                               100u,
                               8400u,
                               &result);
    REQUIRE(ret == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_MALFORMED,
                         MESH_RELAY_ACTION_DROP);
    printf("CALL eack.role_impossible_receive status=%d\n", ret);
    trace_result("eack.role_impossible_receive", &result);
    trace_gateway_state("eack.role_impossible_receive",
                        &gateway,
                        &ack_store);
}

static void scenario_gateway_route_adv_role_impossible(void)
{
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay builder;
    struct mesh_relay gateway;
    struct mesh_outbound adv;
    struct mesh_relay_result result;
    int ret;

    puts("SCENARIO gateway_route_adv_role_impossible");
    mesh_relay_init(&builder,
                    MESH_RELAY_ROLE_GATEWAY,
                    TEST_ANCHOR_A_ID,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_EPOCH);
    REQUIRE(mesh_relay_build_gateway_route_adv(&builder,
                                               UINT32_C(0x85868788),
                                               8500u,
                                               &adv) == PROTO_OK);
    gateway_init(&gateway, &ack_store);
    ret = mesh_relay_validate_gateway_route_adv(&gateway,
                                                &adv.packet,
                                                adv.payload,
                                                adv.payload_len,
                                                TEST_ANCHOR_A_ID);
    REQUIRE(ret == PROTO_ERR_MALFORMED);
    printf("CALL gateway_route_adv.public_validate status=%d\n", ret);
    trace_packet("gateway_route_adv.role_impossible_receive",
                 &adv.packet,
                 adv.payload,
                 adv.payload_len);
    ret = mesh_relay_handle_rx(&gateway,
                               &adv.packet,
                               adv.payload,
                               adv.payload_len,
                               TEST_ANCHOR_A_ID,
                               100u,
                               8510u,
                               &result);
    REQUIRE(ret == PROTO_OK);
    require_result_exact(&result,
                         PROTO_ERR_MALFORMED,
                         MESH_RELAY_ACTION_DROP);
    printf("CALL gateway_route_adv.role_impossible_receive status=%d\n",
           ret);
    trace_result("gateway_route_adv.role_impossible_receive", &result);
    trace_gateway_state("gateway_route_adv.role_impossible_receive",
                        &gateway,
                        &ack_store);
}

static void specialized_contract_guards(void)
{
#if IMEC_MESH_RELAY_GATEWAY_ONLY
    struct mesh_gateway_ack_store ack_store;
    struct mesh_relay relay;
    struct mesh_relay_result result;
    struct mesh_relay_outbox_snapshot snapshot = {0};
    struct result_fixture fixture;

    gateway_init(&relay, &ack_store);
    relay.pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK;
    memset(&result, 0xa5, sizeof(result));
    REQUIRE(mesh_relay_tick(&relay, 8600u, &result) ==
            PROTO_ERR_MALFORMED);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);
    result_fixture_init(&fixture,
                        TEST_ANCHOR_B_ID,
                        UINT32_C(0x86878889),
                        71u);
    memset(&result, 0xa5, sizeof(result));
    REQUIRE(mesh_relay_handle_rx(&relay,
                                 &fixture.offer_packet,
                                 fixture.offer_payload,
                                 fixture.offer_payload_len,
                                 TEST_ANCHOR_B_ID,
                                 90u,
                                 8601u,
                                 &result) == PROTO_ERR_MALFORMED);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_ID,
                    TEST_GATEWAY_EPOCH);
    memset(&result, 0xa5, sizeof(result));
    REQUIRE(mesh_relay_tick(&relay, 8610u, &result) ==
            PROTO_ERR_MALFORMED);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);
    memset(&result, 0xa5, sizeof(result));
    REQUIRE(mesh_relay_handle_rx(&relay,
                                 &fixture.offer_packet,
                                 fixture.offer_payload,
                                 fixture.offer_payload_len,
                                 TEST_ANCHOR_B_ID,
                                 90u,
                                 8611u,
                                 &result) == PROTO_ERR_MALFORMED);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_GATEWAY,
                    TEST_GATEWAY_ID,
                    TEST_ANCHOR_A_ID,
                    TEST_GATEWAY_EPOCH);
    memset(&result, 0xa5, sizeof(result));
    REQUIRE(mesh_relay_tick(&relay, 8620u, &result) ==
            PROTO_ERR_MALFORMED);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);
    memset(&result, 0xa5, sizeof(result));
    REQUIRE(mesh_relay_handle_rx(&relay,
                                 &fixture.offer_packet,
                                 fixture.offer_payload,
                                 fixture.offer_payload_len,
                                 TEST_ANCHOR_B_ID,
                                 90u,
                                 8621u,
                                 &result) == PROTO_ERR_MALFORMED);
    require_result_exact(&result, PROTO_OK, MESH_RELAY_ACTION_NONE);

    gateway_init(&relay, &ack_store);
    REQUIRE(mesh_relay_restore_outbox_snapshot(&relay,
                                               &snapshot,
                                               8630u) ==
            PROTO_ERR_MALFORMED);
    require_specialized_pending_idle(&relay);
#endif
}

int main(void)
{
    puts("IMEC_GATEWAY_RELAY_TRACE_V1");
    specialized_contract_guards();
    scenario_result_offer_and_command_result();
    scenario_result_bundle();
    scenario_ordinary_command_result_duplicate();
    scenario_idle_route_control();
    scenario_route_reply_ack_commitment();
    scenario_ready_bundle_tick();
    scenario_idle_local_responses();
    scenario_clicker_only_control_envelopes();
    scenario_eack_creation_retry_and_inbound();
    scenario_gateway_route_adv_role_impossible();
    puts("IMEC_GATEWAY_RELAY_TRACE_END");
    return 0;
}
