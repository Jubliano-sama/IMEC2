#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHILD_ID UINT64_C(0x000000000000d101)
#define RELAY_ID UINT64_C(0x000000000000d102)
#define GATEWAY_ID UINT64_C(0x000000000000d1ff)
#define ROUTE_EPOCH UINT32_C(41)
#define DUPLICATE_ACK_BURST 8u
#define POST_SETTLE_POLL_BOUND 32u

struct transit_operation {
    struct proto_packet packet;
    uint8_t payload[96];
    size_t payload_len;
    struct proto_packet gateway_ack;
    uint8_t gateway_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    size_t gateway_ack_payload_len;
    struct mesh_outbound gateway_confirm;
};

static int failures;
static const char *phase;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d [%s]: ", __FILE__, __LINE__, phase); \
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
            failures++;                                                      \
            return;                                                          \
        }                                                                    \
    } while (0)

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return result != NULL && (result->actions & action) != 0u;
}

static bool pending_identity_matches(const struct mesh_pending_tx *actual,
                                     const struct mesh_pending_tx *expected)
{
    return actual->state == expected->state &&
           actual->packet.msg_type == expected->packet.msg_type &&
           actual->packet.src_id == expected->packet.src_id &&
           actual->packet.dst_id == expected->packet.dst_id &&
           actual->packet.session_id == expected->packet.session_id &&
           actual->packet.seq == expected->packet.seq &&
           actual->payload_len == expected->payload_len &&
           memcmp(actual->payload, expected->payload,
                  expected->payload_len) == 0 &&
           actual->next_hop_id == expected->next_hop_id &&
           actual->gateway_ack_deadline_ms ==
               expected->gateway_ack_deadline_ms &&
           actual->retry_after_ms == expected->retry_after_ms &&
           actual->gateway_ack_forward_pending ==
               expected->gateway_ack_forward_pending &&
           actual->gateway_ack_confirm_pending ==
               expected->gateway_ack_confirm_pending;
}

static int setup_relay(struct mesh_relay *relay, uint32_t now_ms)
{
    mesh_relay_init(relay, MESH_RELAY_ROLE_ANCHOR, RELAY_ID, GATEWAY_ID,
                    ROUTE_EPOCH);
    if (mesh_relay_note_direct_gateway_route(relay, now_ms) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    relay->downlinks[0] = (struct mesh_downlink_entry) {
        .target_id = CHILD_ID,
        .next_hop_id = CHILD_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = now_ms,
        .hop_count = 1u,
        .quality = 95u,
        .valid = true,
    };
    return PROTO_OK;
}

static int build_operation(struct transit_operation *operation,
                           uint32_t session_id,
                           uint16_t seq)
{
    int ret;

    memset(operation, 0, sizeof(*operation));
    /*
     * Command results require collection identity and use the result
     * bundle/EACK path. These scenarios exercise ordinary report hop custody,
     * so use a valid diagnostic mesh-data packet from the same approved class.
     */
    operation->payload[0] = 0x5au;
    operation->payload_len = 1u;
    operation->packet.msg_type = MSG_MESH_DATA;
    operation->packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    operation->packet.src_id = CHILD_ID;
    operation->packet.dst_id = GATEWAY_ID;
    operation->packet.session_id = session_id;
    operation->packet.seq = seq;
    operation->packet.ttl = MESH_DEFAULT_TTL;
    operation->packet.payload_len = (uint8_t)operation->payload_len;
    operation->packet.message_age_ms = 0u;
    ret = mesh_append_requested_seq(operation->gateway_ack_payload,
                                    sizeof(operation->gateway_ack_payload),
                                    &operation->gateway_ack_payload_len,
                                    seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_append_ack_semantic_identity(
        operation->gateway_ack_payload,
        sizeof(operation->gateway_ack_payload),
        &operation->gateway_ack_payload_len,
        &operation->packet,
        operation->payload,
        operation->payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    return mesh_init_gateway_ack(&operation->gateway_ack,
                                 GATEWAY_ID,
                                 CHILD_ID,
                                 session_id,
                                 (uint16_t)(seq + UINT16_C(0x4000)),
                                 (uint8_t)operation->gateway_ack_payload_len);
}

static int start_transit(struct mesh_relay *relay,
                         struct transit_operation *operation,
                         uint32_t now_ms,
                         struct mesh_outbound *upstream_out)
{
    struct mesh_relay_result received = {0};
    struct mesh_outbound upstream = {0};
    int ret;

    ret = mesh_relay_handle_rx(relay,
                               &operation->packet,
                               operation->payload,
                               operation->payload_len,
                               CHILD_ID,
                               95u,
                               now_ms,
                               &received);
    if (ret != PROTO_OK || received.status != PROTO_OK ||
        !has_action(&received, MESH_RELAY_ACTION_FORWARD) ||
        received.forward.next_hop_id != GATEWAY_ID) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_relay_start_tx(relay,
                              &received.forward.packet,
                              received.forward.payload,
                              received.forward.payload_len,
                              now_ms + 1u,
                              &upstream);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_bind_transit_previous_hop(
        relay,
        &upstream,
        received.forward.ingress_previous_hop_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    mesh_relay_note_tx_sent(relay, &upstream, now_ms + 1u);
    if (upstream_out != NULL) {
        *upstream_out = upstream;
    }
    return relay->pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
                   relay->pending.packet.src_id == CHILD_ID &&
                   relay->pending.packet.session_id ==
                       operation->packet.session_id &&
                   relay->outbox_record.valid ?
               PROTO_OK :
               PROTO_ERR_MALFORMED;
}

static int receive_gateway_ack(struct mesh_relay *relay,
                               struct transit_operation *operation,
                               uint32_t now_ms,
                               uint32_t *actions)
{
    struct mesh_relay_result result = {0};
    int ret;

    ret = mesh_relay_handle_rx(relay,
                               &operation->gateway_ack,
                               operation->gateway_ack_payload,
                               operation->gateway_ack_payload_len,
                               GATEWAY_ID,
                               98u,
                               now_ms,
                               &result);
    if (actions != NULL) {
        *actions = result.actions;
    }
    return ret;
}

/*
 * MSG_GATEWAY_ACK_CONFIRM is retired: the gateway ACK is emitted on RAM/BLE
 * admission and is itself terminal. A burst of identical gateway ACKs must
 * therefore terminate the transit copy exactly once and leave no residual
 * custody, confirmation debt, or forward debt behind.
 */
static void test_duplicate_ack_burst_is_bounded(void)
{
    struct mesh_relay relay;
    struct transit_operation operation;
    uint32_t terminal_count = 0u;
    uint32_t actions = MESH_RELAY_ACTION_NONE;

    phase = "duplicate-gateway-ack-burst";
    CHECK(setup_relay(&relay, 1000u) == PROTO_OK, "relay setup failed");
    CHECK(build_operation(&operation, UINT32_C(0xd1000001), 11u) == PROTO_OK,
          "operation build failed");
    CHECK(start_transit(&relay, &operation, 1010u, NULL) == PROTO_OK,
          "transit setup failed");

    for (uint32_t i = 0u; i < DUPLICATE_ACK_BURST; i++) {
        actions = MESH_RELAY_ACTION_NONE;

        CHECK(receive_gateway_ack(&relay, &operation, 1020u + i, &actions) ==
                  PROTO_OK,
              "duplicate ACK %u was rejected", i);
        if ((actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) != 0u) {
            terminal_count++;
        }
        CHECK((actions & MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING) == 0u,
              "duplicate ACK %u created retired ACK_CONFIRM debt", i);
        CHECK(!relay.pending.gateway_ack_confirm_pending &&
                  !relay.pending.gateway_ack_forward_pending &&
                  relay.pending.state == MESH_RELAY_TX_IDLE &&
                  !mesh_relay_tx_active(&relay),
              "duplicate ACK %u corrupted custody state", i);
    }

    CHECK(terminal_count == 1u,
          "%u identical ACK receptions produced %u terminal owners; "
          "only the first may terminate the copy",
          DUPLICATE_ACK_BURST, terminal_count);
    CHECK(!relay.outbox_record.valid && relay.outbox_record.gateway_acked,
          "duplicate ACK burst left the persistent outbox live");
}

static void test_queueable_report_burst_never_creates_busy_control(void)
{
    struct mesh_relay relay;
    struct transit_operation active;
    struct transit_operation competing;
    struct transit_operation recovered;
    struct mesh_pending_tx active_pending;
    uint32_t busy_count = 0u;
    const uint32_t burst_count = 4u;

    phase = "queueable-report-burst";
    CHECK(setup_relay(&relay, 1500u) == PROTO_OK, "relay setup failed");
    CHECK(build_operation(&active, UINT32_C(0xd1500001), 15u) == PROTO_OK &&
              start_transit(&relay, &active, 1510u, NULL) == PROTO_OK,
          "active custody setup failed");
    CHECK(build_operation(&competing, UINT32_C(0xd1500002), 16u) == PROTO_OK,
          "competing operation build failed");
    CHECK(build_operation(&recovered, UINT32_C(0xd1500003), 17u) == PROTO_OK,
          "recovery operation build failed");
    active_pending = relay.pending;

    for (uint32_t burst = 0u; burst < burst_count; burst++) {
        uint32_t burst_start_ms = 1520u + burst * RELAY_BUSY_RETRY_MAX_MS;

        for (uint32_t i = 0u; i < DUPLICATE_ACK_BURST; i++) {
            struct mesh_relay_result result = {0};

            CHECK(mesh_relay_handle_rx(&relay,
                                       &competing.packet,
                                       competing.payload,
                                       competing.payload_len,
                                       CHILD_ID,
                                       95u,
                                       burst_start_ms + i,
                                       &result) == PROTO_OK,
                  "duplicate competing packet %u/%u failed", burst, i);
            if (has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY) ||
                has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY)) {
                busy_count++;
            }
            CHECK(pending_identity_matches(&relay.pending, &active_pending),
                  "duplicate competing packet %u/%u mutated active custody",
                  burst, i);
        }
    }
    CHECK(busy_count == 0u,
          "%u duplicate bursts produced %u obsolete busy controls for a "
          "queueable reliable report",
          burst_count, busy_count);

    mesh_relay_cancel_tx(&relay);
    {
        struct mesh_relay_result result = {0};
        uint32_t after_last_burst_ms = 1520u +
            (burst_count - 1u) * RELAY_BUSY_RETRY_MAX_MS +
            DUPLICATE_ACK_BURST;

        CHECK(mesh_relay_handle_rx(&relay,
                                   &recovered.packet,
                                   recovered.payload,
                                   recovered.payload_len,
                                   CHILD_ID,
                                   95u,
                                   after_last_burst_ms,
                                   &result) == PROTO_OK,
              "capacity recovery rejected a fresh queueable packet");
        CHECK(result.status == PROTO_OK &&
                  has_action(&result, MESH_RELAY_ACTION_FORWARD) &&
                  has_action(&result, MESH_RELAY_ACTION_SEND_HOP_ACK) &&
                  !has_action(&result, MESH_RELAY_ACTION_SEND_RELAY_BUSY) &&
                  !has_action(&result, MESH_RELAY_ACTION_SEND_RESULT_BUSY),
              "prior queue admission poisoned later packet admission");
    }
}

static void test_stale_ack_and_commit_cannot_cross_operations(void)
{
    struct mesh_relay relay;
    struct transit_operation old_operation;
    struct transit_operation new_operation;
    struct mesh_pending_tx new_pending;
    uint32_t actions = MESH_RELAY_ACTION_NONE;
    uint32_t terminal_count = 0u;

    phase = "stale-ack-operation-generation";
    CHECK(setup_relay(&relay, 2000u) == PROTO_OK, "relay setup failed");
    CHECK(build_operation(&old_operation, UINT32_C(0xd2000001), 21u) ==
              PROTO_OK &&
              start_transit(&relay, &old_operation, 2010u, NULL) == PROTO_OK,
          "old operation setup failed");
    /* One terminal gateway ACK completes the old operation outright. */
    CHECK(receive_gateway_ack(&relay, &old_operation, 2020u, &actions) ==
              PROTO_OK &&
              actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED &&
              relay.pending.state == MESH_RELAY_TX_IDLE,
          "old operation did not terminate on its exact gateway ACK");
    terminal_count++;
    /*
     * A second copy of the same gateway ACK is just an ordinary downstream
     * frame for the child origin now; it must never re-terminate the transit
     * copy or resurrect custody.
     */
    CHECK(receive_gateway_ack(&relay, &old_operation, 2021u, &actions) ==
              PROTO_OK &&
              (actions & (MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED |
                          MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING)) ==
                  0u &&
              relay.pending.state == MESH_RELAY_TX_IDLE &&
              !mesh_relay_tx_active(&relay),
          "a second ACK completed the old operation twice");

    CHECK(build_operation(&new_operation, UINT32_C(0xd2000002), 22u) ==
              PROTO_OK &&
              start_transit(&relay, &new_operation, 2030u, NULL) == PROTO_OK,
          "new operation setup failed");
    new_pending = relay.pending;

    CHECK(receive_gateway_ack(&relay, &old_operation, 2032u, &actions) ==
              PROTO_OK &&
              actions == MESH_RELAY_ACTION_NONE,
          "stale gateway ACK triggered new-operation traffic");
    CHECK(pending_identity_matches(&relay.pending, &new_pending),
          "stale gateway ACK mutated the new operation");

    CHECK(receive_gateway_ack(&relay, &new_operation, 2040u, &actions) ==
              PROTO_OK &&
              actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED,
          "new operation did not complete");
    terminal_count++;
    CHECK(terminal_count == 2u && !mesh_relay_tx_active(&relay) &&
              relay.pending.state == MESH_RELAY_TX_IDLE &&
              !relay.pending.gateway_ack_confirm_pending &&
              !relay.outbox_record.valid,
          "operations did not terminate once each without confirmation debt");
}

/*
 * The retired ACK_CONFIRM round trip used to own the post-operation tail.
 * With the gateway ACK terminal, what still has to hold is that a settled
 * operation stops generating work: no tick after the terminal ACK may emit
 * traffic or resurrect custody.
 */
static void test_post_settle_polling_is_bounded(void)
{
    struct mesh_relay relay;
    struct transit_operation operation;
    struct mesh_relay_result tick = {0};
    uint32_t actions = MESH_RELAY_ACTION_NONE;

    phase = "post-settle-polling-bounded";
    CHECK(setup_relay(&relay, 3000u) == PROTO_OK, "relay setup failed");
    CHECK(build_operation(&operation, UINT32_C(0xd3000001), 31u) == PROTO_OK &&
              start_transit(&relay, &operation, 3010u, NULL) == PROTO_OK,
          "operation setup failed");
    CHECK(receive_gateway_ack(&relay, &operation, 3020u, &actions) ==
              PROTO_OK &&
              actions == MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED &&
              relay.pending.state == MESH_RELAY_TX_IDLE,
          "operation did not settle on its terminal gateway ACK");

    for (uint32_t i = 0u; i < POST_SETTLE_POLL_BOUND; i++) {
        CHECK(mesh_relay_tick(&relay, 3021u + i, &tick) == PROTO_OK &&
                  tick.actions == MESH_RELAY_ACTION_NONE,
              "idle poll %u emitted post-operation traffic", i);
    }
    CHECK(relay.pending.state == MESH_RELAY_TX_IDLE &&
              !relay.pending.gateway_ack_confirm_pending &&
              !relay.pending.gateway_ack_forward_pending &&
              !relay.outbox_record.valid &&
              !mesh_relay_tx_active(&relay),
          "settled path retained custody or confirmation debt");
}

static void test_reset_drains_ack_forward_ownership(void)
{
    static struct mesh_sim_world world;
    struct mesh_sim_role_instance *node;
    struct mesh_sim_invariant_report report;
    struct transit_operation operation;
    struct mesh_outbound upstream = {0};
    uint8_t relay_index;

    phase = "reset-drains-ack-forward";
    mesh_sim_init(&world, UINT32_C(0xd4000001));
    CHECK(mesh_sim_add_role(&world,
                            MESH_SIM_ROLE_ANCHOR,
                            RELAY_ID,
                            GATEWAY_ID,
                            ROUTE_EPOCH,
                            &relay_index) == MESH_SIM_OK,
          "simulated relay setup failed");
    node = mesh_sim_role(&world, relay_index);
    CHECK(node != NULL, "simulated relay is unavailable");
    CHECK(setup_relay(&node->relay, 4000u) == PROTO_OK,
          "relay setup failed");
    CHECK(build_operation(&operation, UINT32_C(0xd4000001), 41u) == PROTO_OK &&
              start_transit(&node->relay, &operation, 4010u, &upstream) ==
                  PROTO_OK,
          "operation setup failed");
    /* There is no ACK_CONFIRM tail any more, so the delayed work the reset
     * boundary has to drain is the outstanding upstream report itself. */
    CHECK(node->relay.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
              !node->relay.pending.gateway_ack_confirm_pending,
          "operation did not enter upstream gateway-ACK custody");
    CHECK(mesh_sim_runtime_reserve_transit(
              &world, relay_index, &upstream,
              world.now_us + 10u) ==
              MESH_SIM_OK &&
              node->runtime.transit_reserved,
          "simulator did not own delayed transit work");

    CHECK(mesh_sim_reset_role(&world, relay_index) == MESH_SIM_OK,
          "reset boundary failed");
    CHECK(node->relay.pending.state == MESH_RELAY_TX_IDLE &&
              !node->relay.pending.gateway_ack_confirm_pending &&
              !node->relay.pending.gateway_ack_forward_pending &&
              !node->relay.outbox_record.valid,
          "reset left confirmation custody or its persistent outbox orphaned");
    CHECK(!node->runtime.transit_reserved &&
              node->runtime.radio_owner == MESH_RUNTIME_RADIO_NONE &&
              node->tx_queue_count == 0u &&
              node->radio_state == MESH_SIM_RADIO_SLEEP,
          "reset left runtime, queue, or radio ownership live");
    CHECK(mesh_sim_check_settled(&world, &report) == MESH_SIM_OK,
          "reset did not settle: invariant=%s detail=%llu",
          mesh_sim_invariant_name(report.code),
          (unsigned long long)report.detail);
}

int main(void)
{
    test_duplicate_ack_burst_is_bounded();
    test_queueable_report_burst_never_creates_busy_control();
    test_stale_ack_and_commit_cannot_cross_operations();
    test_post_settle_polling_is_bounded();
    test_reset_drains_ack_forward_ownership();

    if (failures != 0) {
        fprintf(stderr, "%d post-operation liveness scenario(s) failed\n",
                failures);
        return 1;
    }
    puts("mesh post-operation liveness scenarios passed");
    return 0;
}
