#include "mesh_relay.h"
#include "app_mesh_route_reply_ack.h"
#include "dwm3000_timing.h"
#include "uwb.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "Deferred route reply regressions require active assertions"
#endif

#include "deferred_route_reply_time.inc"

#define C UINT64_C(0x3333333333333301)
#define B UINT64_C(0xc1c2306a5138ab2d)
#define A UINT64_C(0x0f6d3a3bdac0f858)
#define G UINT64_C(0x9999888877776666)
#define OTHER UINT64_C(0x7777777777777777)
#define EPOCH 17u
#define REPLY_DELAY_MS 300u

struct cold_chain {
    struct mesh_relay c;
    struct mesh_relay b;
    struct mesh_relay a;
    struct mesh_outbound origin;
    struct mesh_outbound from_b;
    struct mesh_outbound from_a;
    uint32_t queued_ms;
    uint32_t deadline_ms;
};

static bool action(const struct mesh_relay_result *result,
                   enum mesh_relay_action expected)
{
    return (result->actions & expected) != 0u;
}

static uint8_t *value(struct mesh_outbound *out, uint8_t type, uint8_t len)
{
    const uint8_t *found = NULL;
    uint8_t found_len = 0u;

    assert(tlv_find_unique(out->payload, out->payload_len, type,
                           &found, &found_len) == PROTO_OK);
    assert(found_len == len);
    return &out->payload[found - out->payload];
}

static void receive(struct mesh_relay *relay, const struct mesh_outbound *out,
                    uint64_t previous, uint32_t now,
                    struct mesh_relay_result *result)
{
    assert(out->radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(mesh_relay_handle_rx_with_random(relay, &out->packet,
               out->payload, out->payload_len, previous, 90u, now, 0u,
               result) == PROTO_OK);
    assert(result->status == PROTO_OK);
}

static void cold_chain_init(struct cold_chain *chain, uint32_t start_ms,
                            const struct mesh_event_timing *timing,
                            uint8_t flags)
{
    struct mesh_relay_result result;
    struct mesh_route_path path;
    uint32_t now = start_ms;

    memset(chain, 0, sizeof(*chain));
    mesh_relay_init(&chain->c, MESH_RELAY_ROLE_CLICKER, C, G, EPOCH);
    mesh_relay_init(&chain->b, MESH_RELAY_ROLE_ANCHOR, B, G, EPOCH);
    mesh_relay_init(&chain->a, MESH_RELAY_ROLE_ANCHOR, A, G, EPOCH);
    /* Reach the multihop discovery ring through actual bounded retries. */
    for (unsigned attempt = 0u; attempt < MESH_NETWORK_MAX_HOPS; ++attempt) {
        assert(mesh_relay_prepare_route_request_with_timing_flags(
                   &chain->c, G, timing, now, flags, REPLY_DELAY_MS,
                   now, 0u, &chain->origin) == PROTO_OK);
        if (chain->origin.packet.ttl >= 4u) {
            break;
        }
        now = chain->c.route_discovery.next_request_ms;
    }
    assert(chain->origin.packet.ttl >= 4u);
    assert(chain->c.route_discovery.active);
    receive(&chain->b, &chain->origin, C, now + 10u, &result);
    assert(action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    chain->from_b = result.route_request;
    receive(&chain->a, &chain->from_b, B,
            chain->from_b.earliest_tx_ms + 10u, &result);
    assert(action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    assert(!action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    chain->from_a = result.route_request;
    chain->queued_ms = chain->from_a.queued_at_ms;
    chain->deadline_ms = chain->queued_ms + 2000u;
    assert(chain->from_a.queued_at_valid);
    assert(route_selected(&chain->a.upstream) == NULL);
    assert(route_selected(&chain->b.upstream) == NULL);
    assert(route_selected(&chain->c.upstream) == NULL);
    assert(mesh_route_path_from_tlvs(chain->from_a.payload,
               chain->from_a.payload_len, &path) == PROTO_OK);
    assert(path.count == 3u);
    assert(path.node_ids[0] == C && path.node_ids[1] == B &&
           path.node_ids[2] == A);
}

static void probe_succeeded(struct cold_chain *chain)
{
    /* Physical direct-probe success is the boundary; no route is preseeded. */
    assert(mesh_relay_note_direct_gateway_route(&chain->a,
               chain->queued_ms + 40u) == PROTO_OK);
    assert(route_selected(&chain->a.upstream) != NULL);
    assert(route_selected(&chain->a.upstream)->next_hop_id == G);
}

static void receive_a_near_wrap(struct cold_chain *chain, uint32_t queued)
{
    struct mesh_relay_result result;

    assert((int32_t)(queued - chain->from_b.earliest_tx_ms) > 0);
    mesh_relay_init(&chain->a, MESH_RELAY_ROLE_ANCHOR, A, G, EPOCH);
    receive(&chain->a, &chain->from_b, B, queued, &result);
    assert(action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REQ));
    chain->from_a = result.route_request;
    chain->queued_ms = chain->from_a.queued_at_ms;
    chain->deadline_ms = chain->queued_ms + 2000u;
}

static void assert_rejected(struct mesh_relay *relay,
                            const struct mesh_outbound *request,
                            uint64_t previous, uint32_t deadline, uint32_t now,
                            uint32_t random)
{
    struct mesh_relay before;
    struct mesh_outbound out;
    struct mesh_outbound before_out;

    memcpy(&before, relay, sizeof(before));
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before_out, &out, sizeof(out));
    assert(mesh_relay_build_route_reply_for_forwarded_request(relay,
               request, previous, deadline, now, random, &out) != PROTO_OK);
    /* These are byte snapshots of the same objects, including their padding. */
    assert(memcmp(relay, &before, sizeof(before)) == 0);
    assert(memcmp(&out, &before_out, sizeof(out)) == 0);
}

static void assert_ack_rejected(struct mesh_relay *relay,
                                const struct mesh_outbound *ack,
                                uint64_t previous)
{
    struct mesh_relay before;

    memcpy(&before, relay, sizeof(before));
    assert(mesh_relay_accept_route_reply_ack(relay, &ack->packet,
               ack->payload, ack->payload_len, previous) != PROTO_OK);
    assert(memcmp(relay, &before, sizeof(before)) == 0);
}

static void test_cold_chain_reply_and_exact_ack_propagation(void)
{
    struct cold_chain chain;
    struct mesh_outbound reply;
    struct mesh_outbound conditional_ack;
    struct mesh_outbound bad_ack;
    struct mesh_outbound input_before;
    struct mesh_relay_result at_b;
    struct mesh_relay_result at_c;
    struct mesh_route_path path;
    uint32_t now;

    cold_chain_init(&chain, 1000u, NULL, 0u);
    assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                    chain.queued_ms + 50u, 0u);
    probe_succeeded(&chain);
    memcpy(&input_before, &chain.from_a, sizeof(input_before));
    /* The on-air predecessor remains valid; a locally appended tail does not. */
    assert(mesh_relay_validate_route_request(&chain.a, &chain.from_b.packet,
               chain.from_b.payload, chain.from_b.payload_len, B,
               chain.queued_ms) == PROTO_OK);
    assert(mesh_relay_validate_route_request(&chain.a, &chain.from_a.packet,
               chain.from_a.payload, chain.from_a.payload_len, B,
               chain.queued_ms) != PROTO_OK);
    assert(mesh_relay_build_route_reply_for_request(&chain.a,
               &chain.from_a.packet, chain.from_a.payload,
               chain.from_a.payload_len, B, chain.queued_ms + 50u,
               0u, &reply) != PROTO_OK);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
               &chain.from_a, B, chain.deadline_ms, chain.queued_ms + 50u,
               0u, &reply) == PROTO_OK);
    assert(memcmp(&chain.from_a, &input_before, sizeof(input_before)) == 0);
    assert(reply.packet.msg_type == MSG_ROUTE_REPLY);
    assert(reply.packet.src_id == G && reply.packet.dst_id == C);
    assert(reply.packet.session_id == chain.origin.packet.session_id);
    assert(reply.next_hop_id == B && reply.packet.flags == 0u);
    assert(reply.earliest_tx_valid);
    assert(reply.earliest_tx_ms == chain.queued_ms + REPLY_DELAY_MS);
    assert(mesh_relay_validate_route_reply(&chain.b, &reply.packet,
               reply.payload, reply.payload_len, A,
               reply.earliest_tx_ms) == PROTO_OK);
    assert(mesh_route_path_from_tlvs(reply.payload, reply.payload_len,
               &path) == PROTO_OK);
    assert(path.count == 2u && path.node_ids[0] == G && path.node_ids[1] == A);
    now = reply.earliest_tx_ms + 1u;
    mesh_relay_note_tx_sent(&chain.a, &reply, now);
    assert(chain.a.route_reply_ack_expectation.active);
    assert(chain.a.route_reply_ack_expectation.peer_id == B);
    receive(&chain.b, &reply, A, now + 1u, &at_b);
    assert(action(&at_b, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(action(&at_b, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(at_b.route_reply.next_hop_id == C);
    conditional_ack = at_b.route_reply_ack;
    /* Core produces a conditional ACK; the application owns its withholding. */
    assert(chain.a.route_reply_ack_expectation.active);
    mesh_relay_note_tx_sent(&chain.b, &at_b.route_reply, now + 2u);
    assert(chain.b.route_reply_ack_expectation.active);
    /* Lost downstream reception leaves both exact ACK expectations retained. */
    receive(&chain.b, &reply, A, now + 3u, &at_b);
    assert(action(&at_b, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
    assert(action(&at_b, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(chain.a.route_reply_ack_expectation.active);
    assert(chain.b.route_reply_ack_expectation.active);
    receive(&chain.c, &at_b.route_reply, B, now + 4u, &at_c);
    assert(action(&at_c, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(action(&at_c, MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY));
    assert(!chain.c.route_discovery.active);
    assert(route_selected(&chain.c.upstream)->next_hop_id == B);
    assert(route_selected(&chain.c.upstream)->hop_count == 2u);
    assert(route_selected(&chain.b.upstream)->next_hop_id == A);
    assert(route_selected(&chain.b.upstream)->hop_count == 1u);
    assert_ack_rejected(&chain.a, &at_c.route_reply_ack, C);
    assert_ack_rejected(&chain.b, &at_c.route_reply_ack, OTHER);
    bad_ack = at_c.route_reply_ack;
    bad_ack.packet.session_id++;
    assert_ack_rejected(&chain.b, &bad_ack, C);
    bad_ack = at_c.route_reply_ack;
    bad_ack.payload[bad_ack.payload_len - 1u] ^= 1u;
    assert_ack_rejected(&chain.b, &bad_ack, C);
    assert(mesh_relay_accept_route_reply_ack(&chain.b,
               &at_c.route_reply_ack.packet, at_c.route_reply_ack.payload,
               at_c.route_reply_ack.payload_len, C) == PROTO_OK);
    assert(!chain.b.route_reply_ack_expectation.active);
    assert(chain.a.route_reply_ack_expectation.active);
    assert(mesh_relay_accept_route_reply_ack(&chain.a,
               &conditional_ack.packet, conditional_ack.payload,
               conditional_ack.payload_len, B) == PROTO_OK);
    assert(!chain.a.route_reply_ack_expectation.active);
    assert_ack_rejected(&chain.a, &conditional_ack, B);
}

static void test_malformed_forwarded_requests_are_atomic(void)
{
    struct cold_chain chain;

    cold_chain_init(&chain, 1000u, NULL, MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED);
    probe_succeeded(&chain);
    for (unsigned variant = 0u; variant < 25u; ++variant) {
        struct mesh_outbound request = chain.from_a;
        uint64_t previous = B;
        uint32_t now = chain.queued_ms + 50u;

        switch (variant) {
        case 0: request.packet.src_id = OTHER; break;
        case 1: request.packet.dst_id = G; break;
        case 2: request.packet.flags = 1u; break;
        case 3: request.packet.session_id = 0u; break;
        case 4: request.packet.seq = 0u; break;
        case 5: request.packet.ttl = 0u; break;
        case 6: request.packet.payload_len--; break;
        case 7: request.packet.msg_type = MSG_ROUTE_REPLY; break;
        case 8: request.radio_channel = 9u; break;
        case 9: request.next_hop_id = B; break;
        case 10: request.queued_at_valid = false; break;
        case 11: request.queued_at_ms = now + 1u; break;
        case 12: previous = 0u; break;
        case 13: previous = A; break;
        case 14: previous = C; break;
        case 15: previous = MESH_BROADCAST_ID; break;
        case 16:
            proto_put_u64_le(value(&request, TLV_ROUTE_NODE_PATH, 24u), OTHER);
            break;
        case 17:
            proto_put_u64_le(value(&request, TLV_ROUTE_NODE_PATH, 24u) + 16u,
                             OTHER);
            break;
        case 18:
            proto_put_u64_le(value(&request, TLV_ROUTE_NODE_PATH, 24u) + 8u,
                             OTHER);
            break;
        case 19:
            proto_put_u64_le(value(&request, TLV_ROUTE_NODE_PATH, 24u) + 8u, A);
            break;
        case 20: *value(&request, TLV_HOP_COUNT, 1u) = 0u; break;
        case 21: *value(&request, TLV_ROUTE_REQUEST_FLAGS, 1u) = 0x80u; break;
        case 22: proto_put_u32_le(value(&request, TLV_SLOT_SEED, 4u), 0u); break;
        case 23:
            request.payload_len--;
            request.packet.payload_len = request.payload_len;
            break;
        case 24:
            proto_put_u32_le(value(&request, TLV_ROUTE_EPOCH, 4u), 0u);
            break;
        default: assert(false); break;
        }
        assert_rejected(&chain.a, &request, previous, chain.deadline_ms, now, 0u);
    }
    /* A received predecessor has no local-tail provenance for this API. */
    assert_rejected(&chain.a, &chain.from_b, B, chain.deadline_ms,
                    chain.queued_ms + 50u, 0u);
}

static void test_original_delay_deadline_and_wrap(void)
{
    struct cold_chain chain;
    struct mesh_outbound reply;

    for (unsigned wrap = 0u; wrap < 2u; ++wrap) {
        const uint32_t start = wrap ? UINT32_MAX - 10000u : 1000u;
        uint32_t due;

        cold_chain_init(&chain, start, NULL, 0u);
        if (wrap) {
            /* Decode the real predecessor just before uptime wraps. */
            receive_a_near_wrap(&chain, UINT32_MAX - 199u);
        }
        probe_succeeded(&chain);
        due = chain.queued_ms + REPLY_DELAY_MS + 3u * RREP_RESPONDER_SLOT_MS;
        assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
                   &chain.from_a, B, due + 1u, chain.queued_ms + 50u,
                   3u, &reply) == PROTO_OK);
        assert(reply.earliest_tx_valid && reply.earliest_tx_ms == due);
        assert_rejected(&chain.a, &chain.from_a, B, due,
                        chain.queued_ms + 50u, 3u);
        assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                        chain.deadline_ms, 0u);
        assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                        chain.deadline_ms + 1u, 0u);
        assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
                   &chain.from_a, B, chain.deadline_ms,
                   chain.queued_ms + REPLY_DELAY_MS + 10u, 0u,
                   &reply) == PROTO_OK);
        assert(reply.earliest_tx_ms == chain.queued_ms + REPLY_DELAY_MS + 10u);
        if (wrap) {
            assert(due < chain.queued_ms);
        }
    }
}

/* Only RF and contact bookkeeping are fake below. The included branch and
 * its admission policy are production code, and success requires a real
 * downstream core ACK before the fake RF boundary can emit an upstream ACK. */
static struct mesh_relay *handoff_sender;
static struct mesh_relay *handoff_receiver;
static struct mesh_outbound emitted_upstream_ack;
static uint32_t handoff_now;
static unsigned handoff_attempts;
static unsigned upstream_acks;
static uint64_t contact_peer;
static bool downstream_delivers;
static bool downstream_acked;
static bool sender_mode;
static uint64_t sender_now;
static uint64_t sender_owner_end;
static uint32_t sender_config_delay;
static uint32_t sender_ack_delay;
static uint32_t sender_embedded_delay;
static uint32_t sender_wait_overrun;
static int sender_ack_result;
static unsigned sender_attempts;
static unsigned sender_transmissions;
static unsigned sender_ack_waits;
static unsigned sender_contacts;
static unsigned sender_unbounded;
static struct mesh_outbound sender_ack;

#define mesh_runtime (*handoff_sender)
#define C5_CONTACT_PURPOSE_ROUTE_REPLY 1u
#define MESH_C5_CONTROL_ACCEPTED_EXCHANGE 1u
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define IS_ENABLED(option) (option)
#define LOG_WRN(...) status_debug_printf(__VA_ARGS__)
#define LOG_INF(...) status_debug_printf(__VA_ARGS__)
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3
#define DEVICE_ROLE ROLE_ANCHOR
#define MESH_C5_CONTROL_WAKE_IF_NEEDED 2u
#define FW_C5_TX_INTENT_CAUSAL_RESPONSE 1u
#define K_NO_WAIT 0

static void status_debug_printf(const char *format, ...)
{
    (void)format;
}

static uint32_t k_uptime_get_32(void)
{
    return sender_mode ? (uint32_t)sender_now : handoff_now;
}

static uint32_t mesh_c5_exchange_expires_at(unsigned purpose)
{
    assert(purpose == C5_CONTACT_PURPOSE_ROUTE_REPLY);
    return handoff_now + 100u;
}

static void mesh_c5_contact_accept(uint64_t peer, unsigned purpose,
                                   uint32_t expires, const char *reason)
{
    assert(purpose == C5_CONTACT_PURPOSE_ROUTE_REPLY);
    assert(expires == handoff_now + 100u);
    assert(reason != NULL);
    contact_peer = peer;
}

static bool mesh_c5_contact_peer_active(uint64_t peer, uint32_t now)
{
    assert(now == handoff_now);
    return peer == contact_peer;
}

static void mesh_c5_contact_clear(const char *reason)
{
    assert(reason != NULL);
    contact_peer = 0u;
}

static bool mesh_send_route_reply_action(struct mesh_relay_result *result,
                                         const char *reason)
{
    struct mesh_relay_result received;

    assert(reason != NULL);
    ++handoff_attempts;
    mesh_relay_note_tx_sent(handoff_sender, &result->route_reply, handoff_now);
    assert(handoff_sender->route_reply_ack_expectation.active);
    if (!downstream_delivers) {
        mesh_relay_abandon_route_reply_ack(handoff_sender, &result->route_reply);
        return false;
    }
    receive(handoff_receiver, &result->route_reply, handoff_sender->local_id,
            handoff_now + 1u, &received);
    assert(action(&received, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    assert(mesh_relay_accept_route_reply_ack(handoff_sender,
               &received.route_reply_ack.packet, received.route_reply_ack.payload,
               received.route_reply_ack.payload_len,
               handoff_receiver->local_id) == PROTO_OK);
    downstream_acked = true;
    return true;
}

static uint64_t encoded_airtime_ms(const struct mesh_outbound *out)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t length = 0u;

    assert(uwb_mesh_frame_encode(1u, handoff_sender->local_id,
               out->next_hop_id, &out->packet, out->payload,
               frame, sizeof(frame), &length) == PROTO_OK);
    return (dwm3000_timing_airtime_us_ceil(
                DWM3000_TIMING_PHY_CH5_MESH_CONTROL, length) + 999u) / 1000u;
}

static int mesh_send_c5_causal_response(const struct mesh_outbound *out,
                                        unsigned purpose, unsigned mode,
                                        const char *reason)
{
    if (sender_mode) {
        assert(mode == MESH_C5_CONTROL_WAKE_IF_NEEDED);
        assert(sender_owner_end == 0u);
        sender_now += sender_config_delay + encoded_airtime_ms(out);
        ++sender_unbounded;
        ++sender_transmissions;
        return 0;
    }
    assert(downstream_acked);
    assert(!handoff_sender->route_reply_ack_expectation.active);
    assert(purpose == C5_CONTACT_PURPOSE_ROUTE_REPLY);
    assert(mode == MESH_C5_CONTROL_ACCEPTED_EXCHANGE && reason != NULL);
    assert(out->next_hop_id == contact_peer);
    emitted_upstream_ack = *out;
    ++upstream_acks;
    return 0;
}

static int64_t k_uptime_get(void)
{
    assert(sender_mode);
    return (int64_t)sender_now;
}

static void k_msleep(uint32_t delay)
{
    assert(sender_mode);
    sender_now += delay;
}

static void mesh_route_embedded_wait_before_reply(const struct mesh_outbound *out)
{
    assert(sender_mode && out != NULL);
    sender_now += sender_embedded_delay;
}

static void mesh_wait_until_ms(uint32_t deadline)
{
    assert(!uptime_deadline_reached((uint32_t)sender_now, deadline));
    sender_now += (uint32_t)(deadline - (uint32_t)sender_now) + sender_wait_overrun;
}

static void mesh_c5_contact_exchange(uint64_t peer, unsigned purpose,
                                     uint32_t end, const char *reason)
{
    assert(peer != 0u && purpose == C5_CONTACT_PURPOSE_ROUTE_REPLY);
    assert(end == (uint32_t)sender_now + MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS);
    assert(reason != NULL);
    ++sender_contacts;
}

static int mesh_propose_event_after_channel5_contact(uint64_t peer,
                                                    const char *reason)
{
    assert(peer != 0u && reason != NULL);
    return 0;
}

static int mesh_send_c5_control_attempt(const struct mesh_outbound *out,
                                       unsigned purpose, unsigned mode,
                                       const char *reason, uint64_t latest_start,
                                       void *observation, const void *authorization,
                                       unsigned intent, bool assignment_awake)
{
    const uint64_t airtime = encoded_airtime_ms(out);

    assert(sender_mode && sender_owner_end != 0u);
    assert(purpose == C5_CONTACT_PURPOSE_ROUTE_REPLY);
    assert(mode == MESH_C5_CONTROL_ACCEPTED_EXCHANGE);
    assert(intent == FW_C5_TX_INTENT_CAUSAL_RESPONSE && !assignment_awake);
    assert(observation == NULL && authorization == NULL && reason != NULL);
    assert(latest_start == sender_owner_end - airtime);
    ++sender_attempts;
    sender_now += sender_config_delay;
    /* Model the driver's last-start check after nonzero configuration/SPI. */
    if (sender_now >= latest_start) {
        return -ETIMEDOUT;
    }
    sender_now += airtime;
    assert(sender_now < sender_owner_end);
    ++sender_transmissions;
    return 0;
}

static int mesh_listen_for_route_reply_ack(const struct mesh_outbound *out,
                                          uint8_t attempt)
{
    assert(sender_mode && out != NULL && attempt <= RREP_RETRY_COUNT_PER_HOP);
    ++sender_ack_waits;
    sender_now += sender_ack_delay;
    if (sender_ack_result != 0) {
        return sender_ack_result;
    }
    return mesh_relay_accept_route_reply_ack(handoff_sender, &sender_ack.packet,
               sender_ack.payload, sender_ack.payload_len, sender_ack.packet.src_id);
}

static struct mesh_outbound mesh_route_reply_backup_scratch;
static int mesh_route_reply_scratch_lock;

static int k_mutex_lock(int *lock, int timeout)
{
    assert(timeout == K_NO_WAIT && *lock == 0);
    *lock = 1;
    return 0;
}

static void k_mutex_unlock(int *lock)
{
    assert(*lock == 1);
    *lock = 0;
}

#include "deferred_route_reply_train.inc"
#include "deferred_route_reply_sender.inc"

static void sender_reset(struct cold_chain *chain, const struct mesh_outbound *reply,
                          uint64_t initial_ms, uint64_t owner_end)
{
    struct mesh_relay_result received;

    receive(&chain->b, reply, A, reply->earliest_tx_ms, &received);
    assert(action(&received, MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK));
    sender_ack = received.route_reply_ack;
    handoff_sender = &chain->a;
    sender_mode = true;
    sender_now = initial_ms;
    sender_owner_end = owner_end;
    sender_config_delay = 1u;
    sender_ack_delay = 1u;
    sender_ack_result = 0;
    sender_embedded_delay = 0u;
    sender_wait_overrun = 0u;
    sender_attempts = sender_transmissions = sender_ack_waits = 0u;
    sender_contacts = sender_unbounded = 0u;
    assert(mesh_route_reply_scratch_lock == 0);
}

static void test_sender_enforces_owner_after_waits_configuration_and_retry(void)
{
    struct cold_chain chain;
    struct mesh_outbound reply;
    uint64_t airtime;

    cold_chain_init(&chain, 1000u, NULL, 0u);
    probe_succeeded(&chain);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
               &chain.from_a, B, chain.deadline_ms, chain.queued_ms + 50u,
               0u, &reply) == PROTO_OK);
    handoff_sender = &chain.a;
    airtime = encoded_airtime_ms(&reply);
    assert(airtime > 0u);
    for (unsigned scenario = 0u; scenario < 8u; ++scenario) {
        uint32_t deadline = chain.deadline_ms;
        const uint32_t *owner = &deadline;
        bool sent;

        sender_reset(&chain, &reply, chain.queued_ms + 50u, deadline);
        switch (scenario) {
        case 0: break;
        case 1: sender_embedded_delay = deadline - (uint32_t)sender_now; break;
        case 2: sender_wait_overrun = deadline - reply.earliest_tx_ms; break;
        case 3: sender_now = deadline; break;
        case 4:
            deadline = reply.earliest_tx_ms + (uint32_t)airtime;
            sender_owner_end = deadline;
            break;
        case 5:
            deadline = reply.earliest_tx_ms + (uint32_t)airtime + 2u;
            sender_owner_end = deadline;
            sender_config_delay = 3u;
            break;
        case 6:
            deadline = reply.earliest_tx_ms + (uint32_t)airtime + 10u;
            sender_owner_end = deadline;
            sender_ack_delay = 20u;
            sender_ack_result = -ETIMEDOUT;
            break;
        case 7:
            owner = NULL;
            sender_owner_end = 0u;
            sender_config_delay = 5000u;
            break;
        default: assert(false); break;
        }
        sent = mesh_send_route_reply_outbound_action(&reply, false, 0u,
                                                     "deferred-test", owner);
        assert(sent == (scenario == 0u || scenario == 7u));
        if (scenario >= 1u && scenario <= 3u) {
            assert(sender_contacts == 0u && sender_attempts == 0u);
        }
        if (scenario >= 1u && scenario <= 5u) {
            assert(sender_transmissions == 0u && sender_ack_waits == 0u);
        }
        if (scenario == 4u) {
            assert(sender_attempts == 0u);
        }
        if (scenario == 5u) {
            assert(sender_attempts == 1u);
        }
        if (scenario == 6u) {
            assert(sender_attempts == 1u && sender_transmissions == 1u);
            assert(sender_ack_waits == 1u);
        }
        if (scenario == 0u || scenario == 7u) {
            assert(sender_transmissions == 1u && sender_ack_waits == 1u);
            assert(!chain.a.route_reply_ack_expectation.active);
        }
        assert(sender_unbounded == (scenario == 7u ? 1u : 0u));
    }
    sender_mode = false;
}

static void test_sender_zero_deadline_preserves_full_uptime_epoch(void)
{
    struct cold_chain chain;
    struct mesh_outbound reply;
    const uint32_t deadline = 0u;
    const uint64_t end = UINT64_C(1) << 33;

    cold_chain_init(&chain, UINT32_MAX - 10000u, NULL, 0u);
    receive_a_near_wrap(&chain, UINT32_MAX - 499u);
    probe_succeeded(&chain);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
               &chain.from_a, B, deadline, chain.queued_ms + 50u,
               0u, &reply) == PROTO_OK);
    sender_reset(&chain, &reply, end - 450u, end);
    assert(mesh_send_route_reply_outbound_action(&reply, false, 0u,
                                                 "wrapped-owner", &deadline));
    assert(sender_transmissions == 1u && sender_now < end);
    sender_reset(&chain, &reply, end - 450u, end);
    sender_wait_overrun = 200u;
    assert(!mesh_send_route_reply_outbound_action(&reply, false, 0u,
                                                  "wrapped-owner", &deadline));
    assert(sender_now == end);
    assert(sender_contacts == 0u && sender_attempts == 0u);
    assert(sender_transmissions == 0u);
    sender_mode = false;
}

static void run_production_handoff(struct mesh_relay_result *result)
{
    bool route_reply_downstream_handoff_required =
        (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) != 0u;
    bool route_reply_downstream_handoff_acked = false;

#include "deferred_route_reply_handoff.inc"
}

static void test_application_withholds_ack_until_real_downstream_acceptance(void)
{
    struct cold_chain chain;
    struct mesh_outbound reply;
    struct mesh_relay_result at_b;

    cold_chain_init(&chain, 1000u, NULL,
                    MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(2u));
    probe_succeeded(&chain);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
               &chain.from_a, B, chain.deadline_ms, chain.queued_ms + 50u,
               0u, &reply) == PROTO_OK);
    handoff_now = reply.earliest_tx_ms + 1u;
    mesh_relay_note_tx_sent(&chain.a, &reply, handoff_now);
    receive(&chain.b, &reply, A, handoff_now, &at_b);
    handoff_sender = &chain.b;
    handoff_receiver = &chain.c;
    handoff_attempts = 0u;
    upstream_acks = 0u;
    contact_peer = 0u;
    downstream_delivers = false;
    downstream_acked = false;
    run_production_handoff(&at_b);
    assert(handoff_attempts == 1u && upstream_acks == 0u);
    assert(chain.a.route_reply_ack_expectation.active);
    assert(!chain.b.route_reply_ack_expectation.active);
    /* An exact upstream retry rebuilds the forward after a failed handoff. */
    ++handoff_now;
    receive(&chain.b, &reply, A, handoff_now, &at_b);
    downstream_delivers = true;
    run_production_handoff(&at_b);
    assert(handoff_attempts == 2u && upstream_acks == 1u);
    assert(downstream_acked && !chain.b.route_reply_ack_expectation.active);
    assert(route_selected(&chain.c.upstream)->hop_count == 2u);
    assert(!chain.c.route_discovery.active);
    assert(mesh_relay_accept_route_reply_ack(&chain.a,
               &emitted_upstream_ack.packet, emitted_upstream_ack.payload,
               emitted_upstream_ack.payload_len, B) == PROTO_OK);
    assert(!chain.a.route_reply_ack_expectation.active);
}

static void test_compact_timing_uses_original_queue_reference(void)
{
    struct cold_chain chain;
    struct mesh_event_timing timing;
    struct mesh_event_timing expected;
    struct mesh_event_timing decoded;
    struct mesh_outbound reply;
    const struct mesh_event_params params = {
        .event_interval_ms = 100u, .event_window_ms = 20u,
        .first_event_time_ms = 30000u, .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 20, .max_missed_events = 2u,
        .supervision_timeout_ms = 500u,
    };

    assert(mesh_event_timing_negotiate(&timing, &params, true) == PROTO_OK);
    cold_chain_init(&chain, 1000u, &timing, 0u);
    probe_succeeded(&chain);
    assert(mesh_event_timing_from_tlvs_at(&expected, chain.from_a.payload,
               chain.from_a.payload_len, chain.queued_ms, true) == PROTO_OK);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
               &chain.from_a, B, chain.deadline_ms, chain.queued_ms + 50u,
               3u, &reply) == PROTO_OK);
    assert(mesh_event_timing_from_tlvs_at(&decoded, reply.payload,
               reply.payload_len, reply.earliest_tx_ms, true) == PROTO_OK);
    assert(decoded.next_event_time_ms == expected.next_event_time_ms);
    assert(decoded.event_interval_ms == expected.event_interval_ms);
    assert(decoded.event_window_ms == expected.event_window_ms);
}

static void test_epoch_and_split_horizon_remain_atomic(void)
{
    struct cold_chain chain;
    struct mesh_outbound reply;

    cold_chain_init(&chain, 1000u, NULL, 0u);
    probe_succeeded(&chain);
    chain.a.upstream.current_epoch = EPOCH + UINT32_C(0x80000000);
    chain.a.upstream.candidates[chain.a.upstream.selected_index].route_epoch =
        chain.a.upstream.current_epoch;
    assert(route_selected(&chain.a.upstream) != NULL);
    assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                    chain.queued_ms + 50u, 0u);
    /* Real direct recovery at B lets A learn an upstream path through its child. */
    cold_chain_init(&chain, 1000u, NULL, 0u);
    assert(mesh_relay_note_direct_gateway_route(&chain.b,
               chain.queued_ms + 20u) == PROTO_OK);
    {
        struct mesh_outbound request;
        struct mesh_relay_result result;

        assert(mesh_relay_prepare_route_request(&chain.a, G,
                   chain.queued_ms + 21u, 0u, &request) == PROTO_OK);
        receive(&chain.b, &request, A, chain.queued_ms + 22u, &result);
        assert(action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
        reply = result.route_reply;
        receive(&chain.a, &reply, B, reply.earliest_tx_ms + 1u, &result);
        assert(route_selected(&chain.a.upstream)->next_hop_id == B);
    }
    assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                    chain.queued_ms + 50u, 0u);
    /* Required depth belongs to C: A's one-hop reply reaches C at depth two. */
    for (uint8_t required = 1u; required <= 3u; required += 2u) {
        cold_chain_init(&chain, 1000u, NULL,
                        MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(required));
        probe_succeeded(&chain);
        assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                        chain.queued_ms + 50u, 0u);
    }
    cold_chain_init(&chain, 1000u, NULL,
                    MESH_ROUTE_REQ_REQUIRED_HOPS_ENCODE(2u));
    probe_succeeded(&chain);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&chain.a,
               &chain.from_a, B, chain.deadline_ms, chain.queued_ms + 50u,
               0u, &reply) == PROTO_OK);
    /* A legal maximum-depth parent still cannot extend past the network cap. */
    cold_chain_init(&chain, 1000u, NULL, 0u);
    {
        struct mesh_relay gateway;
        struct mesh_relay intermediate;
        struct mesh_outbound advertisement;
        struct mesh_relay_result result;
        uint64_t previous = G;

        mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, G, G, EPOCH);
        assert(mesh_relay_build_gateway_route_adv(&gateway, 100u,
                   chain.queued_ms, &advertisement) == PROTO_OK);
        for (uint8_t depth = 1u; depth <= MESH_NETWORK_MAX_HOPS; ++depth) {
            const uint64_t local = depth == MESH_NETWORK_MAX_HOPS ?
                A : UINT64_C(0x8000000000000000) + depth;
            struct mesh_relay *receiver = &chain.a;

            if (local != A) {
                mesh_relay_init(&intermediate, MESH_RELAY_ROLE_ANCHOR,
                                local, G, EPOCH);
                receiver = &intermediate;
            }
            receive(receiver, &advertisement, previous,
                    chain.queued_ms + depth, &result);
            if (local != A) {
                assert(action(&result, MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV));
                advertisement = result.gateway_route_adv;
                previous = local;
            }
        }
        assert(route_selected(&chain.a.upstream)->hop_count ==
               MESH_NETWORK_MAX_HOPS - 1u);
        assert_rejected(&chain.a, &chain.from_a, B, chain.deadline_ms,
                        chain.queued_ms + 50u, 0u);
    }
}

int main(void)
{
    test_cold_chain_reply_and_exact_ack_propagation();
    test_malformed_forwarded_requests_are_atomic();
    test_original_delay_deadline_and_wrap();
    test_compact_timing_uses_original_queue_reference();
    test_epoch_and_split_horizon_remain_atomic();
    test_application_withholds_ack_until_real_downstream_acceptance();
    test_sender_enforces_owner_after_waits_configuration_and_retry();
    test_sender_zero_deadline_preserves_full_uptime_epoch();
    puts("Deferred route reply: cold recovery, ACK identity, atomic rejection, timing passed");
    return 0;
}
