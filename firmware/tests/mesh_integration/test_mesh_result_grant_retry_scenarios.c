#include "mesh_relay.h"

#include "dwm3000_timing.h"
#include "mesh.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHILD_COUNT 50u
#define SWEEP_SEEDS 64u
#define RADIO_TURNAROUND_GUARD_US 1000u
#define GATEWAY_ID 0x9999888877776666ull
#define PARENT_ID 0x5555666677778888ull
#define ROUTE_EPOCH 3u
#define START_MS 1000u

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

struct child_state {
    struct mesh_relay relay;
    struct command_result_id result_id;
    struct proto_packet result_packet;
    struct mesh_outbound offer;
    uint8_t payload[96];
    size_t payload_len;
    uint32_t tx_at_ms;
    uint8_t terminal_count;
    bool tx_scheduled;
    bool delivered;
    bool terminal;
};

static struct child_state children[CHILD_COUNT];
static uint64_t result_offer_airtime_us;

static uint32_t mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static uint32_t fresh_random(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return mix32(*state);
}

static uint64_t child_id_for(uint8_t index)
{
    return 0xA100000000000000ull + index + 1u;
}

static bool build_result_payload(struct child_state *child)
{
    uint8_t padding[24];

    child->payload_len = 0u;
    CHECK(command_result_id_append_tlvs(child->payload,
                                        sizeof(child->payload),
                                        &child->payload_len,
                                        &child->result_id) == PROTO_OK);
    CHECK(mesh_append_command_result(child->payload,
                                     sizeof(child->payload),
                                     &child->payload_len,
                                     CMD_GET_STATUS,
                                     COMMAND_OK,
                                     0u) == PROTO_OK);
    memset(padding, 0xA5, sizeof(padding));
    while (child->payload_len < 64u) {
        size_t remaining = 64u - child->payload_len;
        uint8_t chunk_len = remaining > sizeof(padding) + 2u ?
                            (uint8_t)sizeof(padding) :
                            (uint8_t)(remaining - 2u);

        CHECK(tlv_append_bytes(child->payload,
                               sizeof(child->payload),
                               &child->payload_len,
                               TLV_MESH_TEST_PADDING,
                               padding,
                               chunk_len) == PROTO_OK);
    }
    CHECK(child->payload_len == 64u);
    return true;
}

static bool initialise_children(void)
{
    for (uint8_t i = 0u; i < CHILD_COUNT; i++) {
        struct child_state *child = &children[i];
        struct route_candidate route = {
            .next_hop_id = PARENT_ID,
            .gateway_id = GATEWAY_ID,
            .route_epoch = ROUTE_EPOCH,
            .last_seen_ms = START_MS,
            .hop_count = 1u,
            .link_quality = 90u,
            .valid = true,
        };
        uint64_t child_id = child_id_for(i);

        memset(child, 0, sizeof(*child));
        child->result_id.gateway_id = GATEWAY_ID;
        child->result_id.gateway_epoch = ROUTE_EPOCH;
        child->result_id.command_seq = 0x10000u + i;
        child->result_id.node_id = child_id;
        child->result_id.node_boot_counter = 0x20000u + i;
        child->result_id.result_seq = (uint16_t)i + 1u;
        CHECK(build_result_payload(child));
        CHECK(mesh_init_command_result(&child->result_packet,
                                       child_id,
                                       GATEWAY_ID,
                                       child->result_id.command_seq,
                                       child->result_id.result_seq,
                                       (uint8_t)child->payload_len,
                                       false) == PROTO_OK);
        mesh_relay_init(&child->relay,
                        MESH_RELAY_ROLE_ANCHOR,
                        child_id,
                        GATEWAY_ID,
                        ROUTE_EPOCH);
        CHECK(route_upsert_candidate(&child->relay.upstream, &route) == PROTO_OK);
        CHECK(mesh_relay_start_result_offer(&child->relay,
                                            &child->result_packet,
                                            child->payload,
                                            child->payload_len,
                                            START_MS,
                                            &child->offer) == PROTO_OK);
        if (i == 0u) {
            result_offer_airtime_us = dwm3000_timing_airtime_us_ceil(
                DWM3000_TIMING_PHY_CH5_MESH_CONTROL,
                PACKET_HEADER_LEN + child->offer.payload_len);
            CHECK(result_offer_airtime_us > 0u);
        }

        /* The deliberately simultaneous first opportunity collides for all children. */
        mesh_relay_note_tx_sent(&child->relay, &child->offer, START_MS + 1u);
        CHECK(child->relay.outbox_record.retry_round == 1u);
    }
    return true;
}

static bool transmissions_collide(uint8_t index)
{
    const struct child_state *child = &children[index];
    uint64_t child_start_us = (uint64_t)child->tx_at_ms * 1000u;
    uint64_t child_end_us = child_start_us + result_offer_airtime_us +
                            RADIO_TURNAROUND_GUARD_US;

    for (uint8_t other = 0u; other < CHILD_COUNT; other++) {
        const struct child_state *peer = &children[other];
        uint64_t peer_start_us;
        uint64_t peer_end_us;

        if (other == index || !peer->tx_scheduled || peer->terminal || peer->delivered) {
            continue;
        }
        peer_start_us = (uint64_t)peer->tx_at_ms * 1000u;
        peer_end_us = peer_start_us + result_offer_airtime_us +
                      RADIO_TURNAROUND_GUARD_US;
        if (child_start_us < peer_end_us && peer_start_us < child_end_us) {
            return true;
        }
    }
    return false;
}

static bool deliver_matching_grant(struct child_state *child, uint32_t now_ms)
{
    const struct result_grant grant = {
        .result_id = child->result_id,
        .granted_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .max_bytes = (uint16_t)child->payload_len,
        .event_offset_hint = 0u,
    };
    struct proto_packet grant_packet = {
        .msg_type = MSG_RESULT_GRANT,
        .src_id = PARENT_ID,
        .dst_id = child->result_packet.src_id,
        .session_id = child->result_packet.session_id,
        .seq = child->result_packet.seq,
        .ttl = 1u,
    };
    struct mesh_relay_result result;
    uint8_t grant_payload[96];
    size_t grant_payload_len = 0u;

    CHECK(result_grant_append_tlvs(grant_payload,
                                   sizeof(grant_payload),
                                   &grant_payload_len,
                                   &grant) == PROTO_OK);
    grant_packet.payload_len = (uint16_t)grant_payload_len;
    CHECK(mesh_relay_handle_rx(&child->relay,
                               &grant_packet,
                               grant_payload,
                               grant_payload_len,
                               PARENT_ID,
                               90u,
                               now_ms,
                               &result) == PROTO_OK);
    CHECK((result.actions & MESH_RELAY_ACTION_RETRANSMIT) != 0u);
    CHECK(result.retransmit.packet.msg_type == MSG_COMMAND_RESULT);
    CHECK(result.retransmit.packet.src_id == child->result_packet.src_id);
    CHECK(result.retransmit.packet.dst_id == child->result_packet.dst_id);
    CHECK(result.retransmit.packet.session_id == child->result_packet.session_id);
    CHECK(result.retransmit.packet.seq == child->result_packet.seq);
    CHECK(result.retransmit.payload_len == child->payload_len);
    CHECK(memcmp(result.retransmit.payload,
                 child->payload,
                 child->payload_len) == 0);
    child->delivered = true;
    mesh_relay_cancel_tx(&child->relay);
    return true;
}

static bool grant_is_lost(uint32_t seed, uint8_t child_index, uint8_t attempt_count)
{
    uint32_t value = seed ^ ((uint32_t)child_index << 16) ^ attempt_count;

    if (attempt_count >= 5u) {
        return false;
    }
    return (mix32(value) % 100u) < 35u;
}

static bool run_retry_scenario(uint32_t seed,
                               bool drop_every_grant,
                               uint8_t *delivered_count,
                               uint8_t *terminal_count)
{
    uint32_t random_state = seed == 0u ? 1u : seed;
    uint32_t latest_event_ms = START_MS;

    CHECK(delivered_count != NULL);
    CHECK(terminal_count != NULL);
    *delivered_count = 0u;
    *terminal_count = 0u;
    CHECK(initialise_children());

    for (uint8_t cycle = 0u;
         cycle <= MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS;
         cycle++) {
        bool any_active = false;

        for (uint8_t i = 0u; i < CHILD_COUNT; i++) {
            struct child_state *child = &children[i];
            struct mesh_relay_result result;
            uint32_t timeout_ms;

            child->tx_scheduled = false;
            if (child->delivered || child->terminal) {
                continue;
            }
            any_active = true;
            CHECK(child->relay.pending.state == MESH_RELAY_TX_WAIT_RESULT_GRANT);
            timeout_ms = child->relay.pending.gateway_ack_deadline_ms;
            CHECK(mesh_relay_tick_with_random(&child->relay,
                                               timeout_ms,
                                               fresh_random(&random_state),
                                               &result) == PROTO_OK);
            if ((result.actions & MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL) != 0u) {
                CHECK(result.status ==
                      MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED);
                CHECK(result.terminal.packet.src_id == child->result_packet.src_id);
                CHECK(result.terminal.packet.session_id ==
                      child->result_packet.session_id);
                CHECK(result.terminal.packet.seq == child->result_packet.seq);
                CHECK(result.terminal.payload_len == child->payload_len);
                CHECK(memcmp(result.terminal.payload,
                             child->payload,
                             child->payload_len) == 0);
                child->terminal = true;
                child->terminal_count++;
                (*terminal_count)++;
                continue;
            }
            CHECK(result.actions == MESH_RELAY_ACTION_NONE);
            CHECK(child->relay.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF);
            child->tx_at_ms = child->relay.pending.retry_after_ms;
            CHECK(mesh_relay_tick_with_random(&child->relay,
                                               child->tx_at_ms,
                                               fresh_random(&random_state),
                                               &result) == PROTO_OK);
            CHECK((result.actions & MESH_RELAY_ACTION_RETRANSMIT) != 0u);
            CHECK(result.retransmit.packet.msg_type == MSG_RESULT_OFFER);
            CHECK(result.retransmit.packet.session_id ==
                  child->result_packet.session_id);
            CHECK(result.retransmit.packet.seq == child->result_packet.seq);
            mesh_relay_note_tx_sent(&child->relay,
                                    &result.retransmit,
                                    child->tx_at_ms);
            child->tx_scheduled = true;
            if (child->tx_at_ms > latest_event_ms) {
                latest_event_ms = child->tx_at_ms;
            }
        }

        if (!any_active) {
            break;
        }

        for (uint8_t i = 0u; i < CHILD_COUNT; i++) {
            struct child_state *child = &children[i];

            if (!child->tx_scheduled || transmissions_collide(i) ||
                drop_every_grant ||
                grant_is_lost(seed, i, child->relay.outbox_record.retry_round)) {
                continue;
            }
            CHECK(deliver_matching_grant(child, child->tx_at_ms + 1u));
            (*delivered_count)++;
        }
    }

    CHECK(latest_event_ms - START_MS <
          MESH_RELAY_RESULT_OFFER_EXPIRY_S * 1000u);
    for (uint8_t i = 0u; i < CHILD_COUNT; i++) {
        CHECK(children[i].delivered || children[i].terminal);
        CHECK(children[i].terminal_count <= 1u);
    }
    return true;
}

static bool test_fifty_children_collision_and_loss_sweep(void)
{
    uint32_t total_delivered = 0u;

    for (uint32_t seed = 1u; seed <= SWEEP_SEEDS; seed++) {
        uint8_t delivered_count;
        uint8_t terminal_count;

        CHECK(run_retry_scenario(seed,
                                 false,
                                 &delivered_count,
                                 &terminal_count));
        CHECK(delivered_count >= 48u);
        CHECK((uint16_t)delivered_count + terminal_count == CHILD_COUNT);
        total_delivered += delivered_count;
    }
    CHECK(total_delivered >= (SWEEP_SEEDS * CHILD_COUNT * 95u) / 100u);
    return true;
}

static bool test_fifty_children_all_loss_exhausts_once(void)
{
    uint8_t delivered_count;
    uint8_t terminal_count;

    CHECK(run_retry_scenario(0xC0FFEEu,
                             true,
                             &delivered_count,
                             &terminal_count));
    CHECK(delivered_count == 0u);
    CHECK(terminal_count == CHILD_COUNT);
    for (uint8_t i = 0u; i < CHILD_COUNT; i++) {
        struct mesh_relay_result result;

        CHECK(children[i].relay.outbox_record.retry_round ==
              MESH_RELAY_RESULT_OFFER_MAX_RF_ATTEMPTS);
        CHECK(children[i].relay.outbox_record.delivery_state ==
              MESH_RELAY_DELIVERY_RESULT_GRANT_ATTEMPTS_EXHAUSTED);
        CHECK(mesh_relay_tick_with_random(&children[i].relay,
                                           50000u,
                                           i,
                                           &result) == PROTO_OK);
        CHECK(result.actions ==
              MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL);
        CHECK(result.status ==
              MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED);
        CHECK(mesh_relay_commit_terminal_release(
                  &children[i].relay,
                  &result.terminal.packet,
                  result.terminal.payload,
                  result.terminal.payload_len) == PROTO_OK);
        CHECK(children[i].relay.pending.state == MESH_RELAY_TX_IDLE);
        CHECK(!children[i].relay.outbox_record.valid);
        CHECK(mesh_relay_tick_with_random(&children[i].relay,
                                           50001u,
                                           i,
                                           &result) == PROTO_OK);
        CHECK(result.actions == MESH_RELAY_ACTION_NONE);
    }
    return true;
}

int main(void)
{
    if (!test_fifty_children_collision_and_loss_sweep() ||
        !test_fifty_children_all_loss_exhausts_once()) {
        return 1;
    }
    return 0;
}
