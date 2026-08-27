#include "mesh_sim.h"
#include "mesh_sim_invariants.h"

#include "mesh.h"
#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LEAF_ID UINT64_C(0x4100000000000001)
#define RELAY_A_ID UINT64_C(0x4100000000000002)
#define RELAY_B_ID UINT64_C(0x4100000000000003)
#define GATEWAY_ID UINT64_C(0x4100000000000004)
#define ROUTE_EPOCH UINT32_C(73)
#define RX_GUARD_US UINT64_C(100)
#define CONTROL_GAP_US UINT64_C(500)
#define CONTROL_PAYLOAD_CAPACITY 128u
#define MAX_SCENARIO_TRANSITIONS 256u

#define CHECK(expression) do {                                               \
    if (!(expression)) {                                                     \
        fprintf(stderr, "event-control phase=%s line=%d assertion=%s\n",  \
                phase, __LINE__, #expression);                               \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char *phase = "setup";

static struct mesh_event_params connection_params(uint32_t first_event_ms)
{
    return (struct mesh_event_params) {
        .first_event_time_ms = first_event_ms,
        .event_interval_ms = 250u,
        .event_window_ms = 80u,
        .guard_ms = 20u,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 1500u,
    };
}

static uint16_t sequence_after(uint16_t sequence, uint16_t delta)
{
    uint16_t next = (uint16_t)(sequence + delta);

    return next == 0u ? 1u : next;
}

static uint64_t endpoint_ready_us(const struct mesh_sim_role_instance *node)
{
    uint64_t ready_us = node->runtime.radio_busy_until_us;

    if (node->dwm3000.cpu_busy_until_us > ready_us) {
        ready_us = node->dwm3000.cpu_busy_until_us;
    }
    if (node->dwm3000.spi_busy_until_us > ready_us) {
        ready_us = node->dwm3000.spi_busy_until_us;
    }
    if (node->dwm3000.radio_busy_until_us > ready_us) {
        ready_us = node->dwm3000.radio_busy_until_us;
    }
    return ready_us;
}

static bool timing_equal(const struct mesh_event_timing *a,
                         const struct mesh_event_timing *b)
{
    return a->mesh_channel == b->mesh_channel &&
           a->event_interval_ms == b->event_interval_ms &&
           a->event_window_ms == b->event_window_ms &&
           a->next_event_time_ms == b->next_event_time_ms &&
           a->event_counter == b->event_counter &&
           a->guard_ms == b->guard_ms &&
           a->peer_clock_skew_estimate_ppm ==
               b->peer_clock_skew_estimate_ppm &&
           a->max_missed_events == b->max_missed_events &&
           a->missed_event_count == b->missed_event_count &&
           a->supervision_timeout_ms == b->supervision_timeout_ms &&
           a->last_successful_ch9_event_ms ==
               b->last_successful_ch9_event_ms &&
           a->local_tx_on_even_events == b->local_tx_on_even_events &&
           a->route_fresh == b->route_fresh &&
           a->timing_fresh == b->timing_fresh &&
           a->fallback_required == b->fallback_required;
}

static int run_scheduled_connection_exchange(struct mesh_sim_world *world,
                                             uint16_t connection_index)
{
    struct mesh_sim_connection_action action;
    int ret = mesh_sim_connection_next_action(world, connection_index,
                                              &action);

    if (ret != MESH_SIM_OK ||
        action.kind != MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR ||
        !action.already_scheduled) {
        return ret == MESH_SIM_OK ? MESH_SIM_ERR_EVENT_ORDER : ret;
    }
    return mesh_sim_run_until(world, action.end_us);
}

static int append_control_timing_payload(
    const struct mesh_sim_world *world,
    const struct mesh_event_timing *base,
    uint32_t event_counter,
    bool include_update_phase,
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len)
{
    struct mesh_event_timing timing = *base;
    uint32_t now_ms = (uint32_t)(world->now_us / 1000u);

    timing.event_counter = event_counter;
    timing.next_event_time_ms = now_ms + 1000u + event_counter;
    timing.missed_event_count = 0u;
    timing.last_successful_ch9_event_ms = now_ms;
    timing.route_fresh = true;
    timing.timing_fresh = true;
    timing.fallback_required = false;
    *payload_len = 0u;
    return include_update_phase ?
        mesh_append_event_update_tlvs_at(payload, payload_cap, payload_len,
                                         &timing, now_ms) :
        mesh_append_event_timing_tlvs_at(payload, payload_cap, payload_len,
                                         &timing, now_ms);
}

static int append_update_payload(const struct mesh_sim_world *world,
                                 const struct mesh_event_timing *base,
                                 uint32_t event_counter,
                                 uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *payload_len)
{
    return append_control_timing_payload(world,
                                         base,
                                         event_counter,
                                         true,
                                         payload,
                                         payload_cap,
                                         payload_len);
}

static int send_control_over_radio(struct mesh_sim_world *world,
                                   uint16_t connection_index,
                                   uint8_t sender_index,
                                   uint8_t message_type,
                                   uint32_t session_id,
                                   uint16_t sequence,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    const struct mesh_sim_connection *connection =
        &world->connections[connection_index];
    struct proto_packet packet;
    uint8_t receiver_index;
    uint64_t tx_start_us;
    uint64_t arrival_start_us;
    uint64_t arrival_end_us;
    uint32_t airtime_us;
    uint16_t transmission_index;
    int ret;

    if (sender_index == connection->node_a) {
        receiver_index = connection->node_b;
    } else if (sender_index == connection->node_b) {
        receiver_index = connection->node_a;
    } else {
        return MESH_SIM_ERR_ARG;
    }
    if (payload_len > UINT8_MAX) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_init_event_control(&packet, message_type,
                                  world->roles[sender_index].id,
                                  world->roles[receiver_index].id,
                                  session_id, sequence,
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        proto_packet_encoded_len(payload_len));
    if (airtime_us == 0u) {
        return MESH_SIM_ERR_FRAME_TOO_LONG;
    }
    tx_start_us = endpoint_ready_us(&world->roles[sender_index]);
    if (endpoint_ready_us(&world->roles[receiver_index]) > tx_start_us) {
        tx_start_us = endpoint_ready_us(&world->roles[receiver_index]);
    }
    if (world->now_us > tx_start_us) {
        tx_start_us = world->now_us;
    }
    tx_start_us += CONTROL_GAP_US;
    arrival_start_us = tx_start_us +
        world->propagation_us[sender_index][receiver_index];
    arrival_end_us = arrival_start_us + airtime_us;
    ret = mesh_sim_schedule_rx(world, receiver_index,
                               arrival_start_us - RX_GUARD_US,
                               arrival_end_us + RX_GUARD_US,
                               UWB_CHANNEL_WAKE_CONTACT,
                               MESH_SIM_PHY_CHANNEL5_MESH_CONTROL, NULL);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "control RX schedule ret=%d last=%d now=%llu\n",
                ret, world->last_error,
                (unsigned long long)world->now_us);
        return ret;
    }
    ret = mesh_sim_schedule_packet_tx(world, sender_index, tx_start_us,
                                      UWB_CHANNEL_WAKE_CONTACT,
                                      MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                      &packet, payload, payload_len,
                                      &transmission_index);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr, "control TX schedule ret=%d last=%d now=%llu msg=%u\n",
                ret, world->last_error,
                (unsigned long long)world->now_us, message_type);
        return ret;
    }
    ret = mesh_sim_run_until(
        world, world->transmissions[transmission_index].end_us +
                   world->propagation_us[sender_index][receiver_index] +
                   RX_GUARD_US + 1u);
    if (ret != MESH_SIM_OK) {
        fprintf(stderr,
                "control RF run ret=%d last=%d file=%s line=%u msg=%u\n",
                ret, world->last_error,
                world->last_error_file == NULL ? "?" : world->last_error_file,
                world->last_error_line, message_type);
    }
    return ret;
}

static int send_control_tx_into_existing_rx(
    struct mesh_sim_world *world,
    uint16_t connection_index,
    uint8_t sender_index,
    uint8_t message_type,
    uint32_t session_id,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_len)
{
    const struct mesh_sim_connection *connection =
        &world->connections[connection_index];
    struct proto_packet packet;
    uint8_t receiver_index;
    uint64_t tx_start_us;
    uint32_t airtime_us;
    uint16_t transmission_index;
    int ret;

    if (sender_index == connection->node_a) {
        receiver_index = connection->node_b;
    } else if (sender_index == connection->node_b) {
        receiver_index = connection->node_a;
    } else {
        return MESH_SIM_ERR_ARG;
    }
    if (payload_len > UINT8_MAX) {
        return MESH_SIM_ERR_ARG;
    }
    ret = mesh_init_event_control(&packet, message_type,
                                  world->roles[sender_index].id,
                                  world->roles[receiver_index].id,
                                  session_id, sequence,
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    airtime_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
        proto_packet_encoded_len(payload_len));
    if (airtime_us == 0u) {
        return MESH_SIM_ERR_FRAME_TOO_LONG;
    }
    /* The repair start has already armed the receiver's existing control
     * window. Send this probe inside that window instead of waiting for the
     * event owner to release its whole repair reservation. */
    tx_start_us = world->now_us + CONTROL_GAP_US;
    ret = mesh_sim_schedule_packet_tx(world, sender_index, tx_start_us,
                                      UWB_CHANNEL_WAKE_CONTACT,
                                      MESH_SIM_PHY_CHANNEL5_MESH_CONTROL,
                                      &packet, payload, payload_len,
                                      &transmission_index);
    if (ret != MESH_SIM_OK) {
        return ret;
    }
    return mesh_sim_run_until(
        world,
        world->transmissions[transmission_index].end_us +
            world->propagation_us[sender_index][receiver_index] +
            RX_GUARD_US + 1u);
}

static bool connection_owners_active(const struct mesh_sim_connection *connection)
{
    return connection->owner_a.active && connection->owner_b.active &&
           connection->owner_a.session_id == connection->owner_b.session_id;
}

static bool connection_owners_inactive(const struct mesh_sim_connection *connection)
{
    return !connection->owner_a.active && !connection->owner_b.active;
}

struct reciprocal_endpoint {
    struct mesh_event_owner owner;
    struct mesh_event_timing proposal_timing;
    struct mesh_event_timing installed_timing;
    struct proto_packet proposal;
    uint8_t proposal_payload[CONTROL_PAYLOAD_CAPACITY];
    size_t proposal_payload_len;
    uint64_t id;
    uint64_t peer_id;
    uint64_t boot_nonce;
    uint32_t predecessor_generation;
    bool proposal_pending;
    bool accept_pending;
    bool installed;
    bool reciprocal_window_open;
};

static int reciprocal_endpoint_prepare_proposal_at(
    struct reciprocal_endpoint *endpoint,
    uint32_t session_id,
    uint16_t sequence,
    uint32_t encoded_at_ms,
    uint32_t first_event_ms)
{
    struct mesh_event_params params = connection_params(first_event_ms);
    size_t payload_len = 0u;
    int ret;

    memset(&endpoint->proposal_timing, 0, sizeof(endpoint->proposal_timing));
    memset(&endpoint->proposal, 0, sizeof(endpoint->proposal));
    memset(endpoint->proposal_payload, 0, sizeof(endpoint->proposal_payload));
    ret = mesh_event_timing_negotiate(&endpoint->proposal_timing,
                                      &params,
                                      true);
    if (ret != PROTO_OK ||
        !mesh_event_timing_bind_proposal_session(
            &endpoint->proposal_timing, session_id)) {
        return ret == PROTO_OK ? PROTO_ERR_ARG : ret;
    }
    ret = mesh_append_event_timing_tlvs_at(
        endpoint->proposal_payload,
        sizeof(endpoint->proposal_payload),
        &payload_len,
        &endpoint->proposal_timing,
        encoded_at_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(endpoint->proposal_payload,
                         sizeof(endpoint->proposal_payload),
                         &payload_len,
                         TLV_MESH_EVENT_BOOT_NONCE,
                         endpoint->boot_nonce);
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return ret == PROTO_OK ? PROTO_ERR_NO_SPACE : ret;
    }
    ret = mesh_init_event_control(&endpoint->proposal,
                                  MSG_MESH_EVENT_PROPOSE,
                                  endpoint->id,
                                  endpoint->peer_id,
                                  session_id,
                                  sequence,
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    endpoint->proposal_payload_len = payload_len;
    endpoint->proposal_pending = true;
    return PROTO_OK;
}

static int reciprocal_endpoint_init(struct reciprocal_endpoint *endpoint,
                                    uint64_t id,
                                    uint64_t peer_id,
                                    uint64_t boot_nonce,
                                    uint32_t session_id,
                                    uint16_t sequence,
                                    uint32_t first_event_ms,
                                    bool installed)
{
    int ret;

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->id = id;
    endpoint->peer_id = peer_id;
    endpoint->boot_nonce = boot_nonce;
    endpoint->installed = installed;
    endpoint->reciprocal_window_open = installed;
    ret = reciprocal_endpoint_prepare_proposal_at(endpoint,
                                                  session_id,
                                                  sequence,
                                                  0u,
                                                  first_event_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (!installed) {
        return PROTO_OK;
    }
    endpoint->proposal_pending = false;
    endpoint->installed_timing = endpoint->proposal_timing;
    ret = mesh_event_owner_begin(&endpoint->owner,
                                 peer_id,
                                 session_id,
                                 sequence,
                                 false);
    return ret;
}

static int reciprocal_receive_proposal(
    struct reciprocal_endpoint *receiver,
    const struct reciprocal_endpoint *sender)
{
    const struct mesh_event_owner *owner =
        receiver->owner.generation == 0u ? NULL : &receiver->owner;
    const struct mesh_event_owner *arbitration_owner =
        receiver->reciprocal_window_open ? owner : NULL;
    bool reciprocal_local_proposal_proven =
        receiver->proposal_pending || receiver->reciprocal_window_open;
    enum mesh_event_proposal_arbitration arbitration =
        mesh_event_owner_arbitrate_reciprocal_proposal(
            receiver->id,
            sender->id,
            receiver->proposal_pending,
            receiver->peer_id,
            arbitration_owner);
    enum mesh_event_owner_decision decision;

    if (arbitration == MESH_EVENT_PROPOSAL_KEEP_LOCAL) {
        return MESH_SIM_OK;
    }
    if (arbitration == MESH_EVENT_PROPOSAL_ACCEPT_REMOTE &&
        reciprocal_local_proposal_proven) {
        decision = mesh_event_owner_classify_reciprocal_proposal(
            owner,
            receiver->id,
            sender->id,
            &sender->proposal,
            sender->proposal_payload,
            sender->proposal_payload_len);
    } else {
        decision = mesh_event_owner_classify_proposal(
            owner,
            receiver->id,
            sender->id,
            &sender->proposal,
            sender->proposal_payload,
            sender->proposal_payload_len);
    }
    if (decision != MESH_EVENT_OWNER_APPLY) {
        return MESH_SIM_OK;
    }

    receiver->predecessor_generation = receiver->owner.generation;
    receiver->accept_pending = true;
    receiver->proposal_pending = false;
    return MESH_SIM_OK;
}

static int reciprocal_complete_accept(
    struct mesh_sim_world *world,
    uint16_t connection,
    uint8_t responder_index,
    struct reciprocal_endpoint *responder,
    struct reciprocal_endpoint *proposer)
{
    struct mesh_event_timing responder_timing = proposer->proposal_timing;
    uint8_t payload[CONTROL_PAYLOAD_CAPACITY];
    size_t payload_len = 0u;
    int ret;

    if (!responder->accept_pending || responder->proposal_pending) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    if (responder->installed &&
        (!responder->owner.active ||
         responder->owner.generation != responder->predecessor_generation ||
         responder->owner.proposal_from_peer)) {
        return MESH_SIM_ERR_EVENT_ORDER;
    }
    mesh_event_timing_set_local_first_slot_tx(&responder_timing, false);
    ret = mesh_append_event_timing_tlvs_at(
        payload, sizeof(payload), &payload_len, &responder_timing,
        (uint32_t)(world->now_us / 1000u));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = send_control_over_radio(world,
                                  connection,
                                  responder_index,
                                  MSG_MESH_EVENT_ACCEPT,
                                  proposer->proposal.session_id,
                                  proposer->proposal.seq,
                                  payload,
                                  payload_len);
    if (ret != MESH_SIM_OK) {
        return ret;
    }

    /*
     * This is the sender-side TX-completion boundary used by the firmware:
     * failed or not-yet-started ACCEPT work leaves the predecessor untouched.
     */
    if (responder->installed) {
        if (!responder->owner.active ||
            responder->owner.generation != responder->predecessor_generation ||
            responder->owner.proposal_from_peer) {
            return MESH_SIM_ERR_EVENT_ORDER;
        }
        mesh_event_owner_abandon(&responder->owner);
    }
    ret = mesh_event_owner_begin_with_boot_nonce(
        &responder->owner,
        proposer->id,
        proposer->proposal.session_id,
        proposer->proposal.seq,
        true,
        proposer->boot_nonce);
    if (ret != PROTO_OK) {
        return ret;
    }
    responder->installed_timing = responder_timing;
    responder->installed = true;
    responder->accept_pending = false;
    responder->reciprocal_window_open = false;

    if (!proposer->installed) {
        ret = mesh_event_owner_begin(
            &proposer->owner,
            responder->id,
            proposer->proposal.session_id,
            proposer->proposal.seq,
            false);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    proposer->installed_timing = proposer->proposal_timing;
    proposer->installed = true;
    proposer->proposal_pending = false;
    return MESH_SIM_OK;
}

static size_t reciprocal_message_count(const struct mesh_sim_world *world,
                                       uint8_t message_type)
{
    size_t count = 0u;

    for (size_t i = 0u; i < world->transmission_count; i++) {
        if (world->transmissions[i].protocol_msg_type == message_type) {
            count++;
        }
    }
    return count;
}

static int run_reciprocal_proposal_case(bool higher_first,
                                        bool preinstalled_local_timing)
{
    static struct mesh_sim_world world;
    struct reciprocal_endpoint lower;
    struct reciprocal_endpoint higher;
    struct mesh_event_timing higher_timing_before_accept;
    struct mesh_event_owner higher_owner_before_accept;
    struct mesh_event_timing lower_final;
    struct mesh_event_timing higher_final;
    struct mesh_event_owner lower_owner_final;
    struct mesh_event_owner higher_owner_final;
    struct mesh_event_params params = connection_params(5000u);
    uint8_t lower_index;
    uint8_t higher_index;
    uint16_t connection;
    int ret;

    mesh_sim_init(&world,
                  higher_first ? UINT32_C(0x17170001) :
                                 UINT32_C(0x17170002));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &lower_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &higher_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, lower_index, higher_index, 100u, 4u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_connection(&world, lower_index, higher_index, &params,
                                  true, &connection) == MESH_SIM_OK);
    CHECK(reciprocal_endpoint_init(
              &lower, LEAF_ID, RELAY_A_ID,
              UINT64_C(0x1717000000000001), UINT32_C(0x17170001), 11u,
              6000u, preinstalled_local_timing) == PROTO_OK);
    CHECK(reciprocal_endpoint_init(
              &higher, RELAY_A_ID, LEAF_ID,
              UINT64_C(0x1717000000000002), UINT32_C(0x17170002), 12u,
              7000u, preinstalled_local_timing) == PROTO_OK);

    if (higher_first) {
        CHECK(send_control_over_radio(
                  &world, connection, higher_index, MSG_MESH_EVENT_PROPOSE,
                  higher.proposal.session_id, higher.proposal.seq,
                  higher.proposal_payload, higher.proposal_payload_len) ==
              MESH_SIM_OK);
        CHECK(reciprocal_receive_proposal(&lower, &higher) == MESH_SIM_OK);
        CHECK(!lower.accept_pending);
        CHECK(send_control_over_radio(
                  &world, connection, lower_index, MSG_MESH_EVENT_PROPOSE,
                  lower.proposal.session_id, lower.proposal.seq,
                  lower.proposal_payload, lower.proposal_payload_len) ==
              MESH_SIM_OK);
        CHECK(reciprocal_receive_proposal(&higher, &lower) == MESH_SIM_OK);
    } else {
        CHECK(send_control_over_radio(
                  &world, connection, lower_index, MSG_MESH_EVENT_PROPOSE,
                  lower.proposal.session_id, lower.proposal.seq,
                  lower.proposal_payload, lower.proposal_payload_len) ==
              MESH_SIM_OK);
        CHECK(reciprocal_receive_proposal(&higher, &lower) == MESH_SIM_OK);
        CHECK(send_control_over_radio(
                  &world, connection, higher_index, MSG_MESH_EVENT_PROPOSE,
                  higher.proposal.session_id, higher.proposal.seq,
                  higher.proposal_payload, higher.proposal_payload_len) ==
              MESH_SIM_OK);
        CHECK(reciprocal_receive_proposal(&lower, &higher) == MESH_SIM_OK);
        CHECK(!lower.accept_pending);
    }

    CHECK(higher.accept_pending);
    CHECK(!higher.proposal_pending);
    CHECK(lower.proposal_pending || preinstalled_local_timing);
    CHECK(reciprocal_message_count(&world, MSG_MESH_EVENT_PROPOSE) == 2u);
    CHECK(reciprocal_message_count(&world, MSG_MESH_EVENT_ACCEPT) == 0u);

    higher_timing_before_accept = higher.installed_timing;
    higher_owner_before_accept = higher.owner;
    if (preinstalled_local_timing) {
        /* Reservation and failed/pre-RF retry handling are state preserving. */
        CHECK(timing_equal(&higher.installed_timing,
                           &higher_timing_before_accept));
        CHECK(memcmp(&higher.owner, &higher_owner_before_accept,
                     sizeof(higher.owner)) == 0);
    }
    ret = reciprocal_complete_accept(&world,
                                     connection,
                                     higher_index,
                                     &higher,
                                     &lower);
    CHECK(ret == MESH_SIM_OK);
    CHECK(reciprocal_message_count(&world, MSG_MESH_EVENT_ACCEPT) == 1u);
    CHECK(!lower.proposal_pending);
    CHECK(!higher.proposal_pending);
    CHECK(!lower.accept_pending);
    CHECK(!higher.accept_pending);
    CHECK(lower.owner.active && higher.owner.active);
    CHECK(!lower.owner.proposal_from_peer);
    CHECK(higher.owner.proposal_from_peer);
    CHECK(lower.owner.session_id == lower.proposal.session_id);
    CHECK(higher.owner.session_id == lower.proposal.session_id);
    CHECK(lower.installed_timing.local_tx_on_even_events !=
          higher.installed_timing.local_tx_on_even_events);
    CHECK(lower.installed_timing.next_event_time_ms ==
          higher.installed_timing.next_event_time_ms);

    lower_final = lower.installed_timing;
    higher_final = higher.installed_timing;
    lower_owner_final = lower.owner;
    higher_owner_final = higher.owner;
    CHECK(reciprocal_receive_proposal(&lower, &higher) == MESH_SIM_OK);
    CHECK(timing_equal(&lower.installed_timing, &lower_final));
    CHECK(timing_equal(&higher.installed_timing, &higher_final));
    CHECK(memcmp(&lower.owner, &lower_owner_final, sizeof(lower.owner)) == 0);
    CHECK(memcmp(&higher.owner, &higher_owner_final,
                 sizeof(higher.owner)) == 0);
    CHECK(reciprocal_message_count(&world, MSG_MESH_EVENT_ACCEPT) == 1u);
    return 0;
}

static int run_expired_installed_proposal_replay_case(void)
{
    static struct mesh_sim_world world;
    struct reciprocal_endpoint lower;
    struct reciprocal_endpoint higher;
    struct mesh_event_timing higher_timing_before;
    struct mesh_event_owner higher_owner_before;
    struct mesh_event_params params = connection_params(5000u);
    uint8_t lower_index;
    uint8_t higher_index;
    uint16_t connection;

    mesh_sim_init(&world, UINT32_C(0x17170003));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &lower_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &higher_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, lower_index, higher_index, 100u, 4u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_connection(&world, lower_index, higher_index, &params,
                                  true, &connection) == MESH_SIM_OK);
    CHECK(reciprocal_endpoint_init(
              &lower, LEAF_ID, RELAY_A_ID,
              UINT64_C(0x1717000000000001), UINT32_C(0x17170001), 11u,
              6000u, false) == PROTO_OK);
    CHECK(reciprocal_endpoint_init(
              &higher, RELAY_A_ID, LEAF_ID,
              UINT64_C(0x1717000000000002), UINT32_C(0x17170002), 12u,
              7000u, true) == PROTO_OK);
    higher.reciprocal_window_open = false;
    higher_timing_before = higher.installed_timing;
    higher_owner_before = higher.owner;

    CHECK(send_control_over_radio(
              &world, connection, lower_index, MSG_MESH_EVENT_PROPOSE,
              lower.proposal.session_id, lower.proposal.seq,
              lower.proposal_payload, lower.proposal_payload_len) ==
          MESH_SIM_OK);
    CHECK(reciprocal_receive_proposal(&higher, &lower) == MESH_SIM_OK);
    CHECK(!higher.accept_pending);
    CHECK(timing_equal(&higher.installed_timing, &higher_timing_before));
    CHECK(memcmp(&higher.owner, &higher_owner_before,
                 sizeof(higher.owner)) == 0);
    CHECK(reciprocal_message_count(&world, MSG_MESH_EVENT_PROPOSE) == 1u);
    CHECK(reciprocal_message_count(&world, MSG_MESH_EVENT_ACCEPT) == 0u);
    return 0;
}

static int run_lost_accept_exact_proposal_retry_case(void)
{
    static struct mesh_sim_world world;
    struct reciprocal_endpoint proposer;
    struct mesh_event_owner responder_owner = {0};
    struct mesh_event_owner owner_after_first_accept;
    struct mesh_event_timing accepted_timing;
    struct mesh_event_timing timing_after_first_accept;
    struct mesh_relay_result relay_result;
    struct mesh_event_params params = connection_params(5000u);
    uint8_t accept_payload[CONTROL_PAYLOAD_CAPACITY];
    size_t accept_payload_len = 0u;
    size_t first_accept_tx = SIZE_MAX;
    size_t second_accept_tx = SIZE_MAX;
    size_t first_accept_rx = SIZE_MAX;
    size_t second_accept_rx = SIZE_MAX;
    uint64_t first_propose_tx_us = 0u;
    uint64_t retry_propose_tx_us = 0u;
    uint64_t proposer_boot_nonce;
    uint8_t proposer_index;
    uint8_t responder_index;
    uint16_t connection;
    uint16_t accept_sequence;

    phase = "lost_accept_exact_proposal_retry";
    mesh_sim_init(&world, UINT32_C(0x17170004));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &proposer_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &responder_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, proposer_index, responder_index, 100u, 4u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_connection(&world, proposer_index, responder_index,
                                  &params, true, &connection) == MESH_SIM_OK);
    CHECK(reciprocal_endpoint_init(
              &proposer, LEAF_ID, RELAY_A_ID,
              UINT64_C(0x1717000000000004), UINT32_C(0x17170004), 13u,
              6000u, false) == PROTO_OK);
    CHECK(mesh_event_owner_proposal_boot_nonce(
              proposer.proposal_payload,
              proposer.proposal_payload_len,
              &proposer_boot_nonce) == PROTO_OK);

    CHECK(send_control_over_radio(
              &world, connection, proposer_index, MSG_MESH_EVENT_PROPOSE,
              proposer.proposal.session_id, proposer.proposal.seq,
              proposer.proposal_payload, proposer.proposal_payload_len) ==
          MESH_SIM_OK);
    for (size_t i = 0u; i < world.transmission_count; i++) {
        if (world.transmissions[i].protocol_msg_type ==
            MSG_MESH_EVENT_PROPOSE) {
            first_propose_tx_us = world.transmissions[i].start_us;
            break;
        }
    }
    CHECK(first_propose_tx_us != 0u);
    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[responder_index].relay,
              &proposer.proposal,
              proposer.proposal_payload,
              proposer.proposal_payload_len,
              LEAF_ID,
              100u,
              (uint32_t)(world.now_us / 1000u),
              0u,
              &relay_result) == PROTO_OK);
    CHECK(relay_result.status == PROTO_OK);
    CHECK((relay_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    CHECK((relay_result.actions & MESH_RELAY_ACTION_DROP) == 0u);
    CHECK(mesh_event_owner_classify_proposal(
              NULL,
              RELAY_A_ID,
              LEAF_ID,
              &proposer.proposal,
              proposer.proposal_payload,
              proposer.proposal_payload_len) == MESH_EVENT_OWNER_APPLY);
    CHECK(mesh_event_timing_from_tlvs_at(
              &accepted_timing,
              proposer.proposal_payload,
              proposer.proposal_payload_len,
              (uint32_t)(world.now_us / 1000u),
              true) == PROTO_OK);
    mesh_event_timing_set_local_first_slot_tx(&accepted_timing, false);
    CHECK(mesh_append_event_timing_tlvs_at(
              accept_payload,
              sizeof(accept_payload),
              &accept_payload_len,
              &accepted_timing,
              (uint32_t)(world.now_us / 1000u)) == PROTO_OK);
    accept_sequence = proposer.proposal.seq;

    CHECK(mesh_sim_set_directed_rx_failures(
              &world,
              responder_index,
              proposer_index,
              1u,
              MESH_SIM_RX_FRAME_TIMEOUT) == MESH_SIM_OK);
    CHECK(send_control_over_radio(
              &world, connection, responder_index, MSG_MESH_EVENT_ACCEPT,
              proposer.proposal.session_id, accept_sequence,
              accept_payload, accept_payload_len) == MESH_SIM_OK);
    CHECK(mesh_event_owner_begin_with_boot_nonce(
              &responder_owner,
              LEAF_ID,
              proposer.proposal.session_id,
              proposer.proposal.seq,
              true,
              proposer_boot_nonce) == PROTO_OK);
    {
        uint8_t proposal_digest[SEMANTIC_DIGEST_SHA256_LEN];

        CHECK(semantic_digest_sha256(proposer.proposal_payload,
                                    proposer.proposal_payload_len,
                                    proposal_digest));
        CHECK(mesh_event_owner_bind_remote_proposal_digest(
                  &responder_owner,
                  proposer.proposal.session_id,
                  proposer.proposal.seq,
                  proposal_digest) == PROTO_OK);
    }
    owner_after_first_accept = responder_owner;
    timing_after_first_accept = accepted_timing;

    CHECK(mesh_sim_run_until(&world, world.now_us + UINT64_C(1000000)) ==
          MESH_SIM_OK);
    CHECK(send_control_over_radio(
              &world, connection, proposer_index, MSG_MESH_EVENT_PROPOSE,
              proposer.proposal.session_id, proposer.proposal.seq,
              proposer.proposal_payload, proposer.proposal_payload_len) ==
          MESH_SIM_OK);
    for (size_t i = 0u; i < world.transmission_count; i++) {
        if (world.transmissions[i].protocol_msg_type !=
            MSG_MESH_EVENT_PROPOSE ||
            world.transmissions[i].start_us == first_propose_tx_us) {
            continue;
        }
        retry_propose_tx_us = world.transmissions[i].start_us;
        break;
    }
    CHECK(retry_propose_tx_us > first_propose_tx_us);
    CHECK(retry_propose_tx_us - first_propose_tx_us <= UINT64_C(6000000));
    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[responder_index].relay,
              &proposer.proposal,
              proposer.proposal_payload,
              proposer.proposal_payload_len,
              LEAF_ID,
              100u,
              (uint32_t)(world.now_us / 1000u),
              0u,
              &relay_result) == PROTO_OK);
    CHECK(relay_result.status == PROTO_OK);
    CHECK((relay_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    CHECK((relay_result.actions & MESH_RELAY_ACTION_DROP) == 0u);
    CHECK(mesh_event_owner_classify_proposal(
              &responder_owner,
              RELAY_A_ID,
              LEAF_ID,
              &proposer.proposal,
              proposer.proposal_payload,
              proposer.proposal_payload_len) ==
          MESH_EVENT_OWNER_DUPLICATE);
    CHECK(memcmp(&responder_owner,
                 &owner_after_first_accept,
                 sizeof(responder_owner)) == 0);
    CHECK(timing_equal(&accepted_timing, &timing_after_first_accept));
    CHECK(send_control_over_radio(
              &world, connection, responder_index, MSG_MESH_EVENT_ACCEPT,
              proposer.proposal.session_id, accept_sequence,
              accept_payload, accept_payload_len) == MESH_SIM_OK);

    for (size_t i = 0u; i < world.transmission_count; i++) {
        if (world.transmissions[i].protocol_msg_type !=
            MSG_MESH_EVENT_ACCEPT) {
            continue;
        }
        if (first_accept_tx == SIZE_MAX) {
            first_accept_tx = i;
        } else {
            second_accept_tx = i;
        }
    }
    for (size_t i = 0u; i < world.reception_count; i++) {
        if (world.receptions[i].source_id != RELAY_A_ID ||
            world.receptions[i].receiver_id != LEAF_ID) {
            continue;
        }
        if (first_accept_rx == SIZE_MAX) {
            first_accept_rx = i;
        } else {
            second_accept_rx = i;
        }
    }
    CHECK(first_accept_tx != SIZE_MAX && second_accept_tx != SIZE_MAX);
    CHECK(first_accept_rx != SIZE_MAX && second_accept_rx != SIZE_MAX);
    CHECK(world.receptions[first_accept_rx].outcome ==
          MESH_SIM_RX_FRAME_TIMEOUT);
    CHECK(world.receptions[second_accept_rx].outcome == MESH_SIM_RX_DECODED);
    CHECK(world.transmissions[first_accept_tx].outbound.packet.msg_type ==
          world.transmissions[second_accept_tx].outbound.packet.msg_type);
    CHECK(world.transmissions[first_accept_tx].outbound.packet.src_id ==
          world.transmissions[second_accept_tx].outbound.packet.src_id);
    CHECK(world.transmissions[first_accept_tx].outbound.packet.dst_id ==
          world.transmissions[second_accept_tx].outbound.packet.dst_id);
    CHECK(world.transmissions[first_accept_tx].outbound.packet.session_id ==
          world.transmissions[second_accept_tx].outbound.packet.session_id);
    CHECK(world.transmissions[first_accept_tx].outbound.packet.seq ==
          world.transmissions[second_accept_tx].outbound.packet.seq);
    CHECK(world.transmissions[first_accept_tx].outbound.payload_len ==
          world.transmissions[second_accept_tx].outbound.payload_len);
    CHECK(memcmp(world.transmissions[first_accept_tx].outbound.payload,
                 world.transmissions[second_accept_tx].outbound.payload,
                 accept_payload_len) == 0);
    CHECK(memcmp(&responder_owner,
                 &owner_after_first_accept,
                 sizeof(responder_owner)) == 0);
    CHECK(timing_equal(&accepted_timing, &timing_after_first_accept));
    return 0;
}

static int run_lost_accept_fresh_proposal_retry_case(void)
{
    static struct mesh_sim_world world;
    struct reciprocal_endpoint proposer;
    struct mesh_event_owner responder_owner = {0};
    struct mesh_event_timing retry_received_timing = {0};
    struct mesh_event_timing stale_received_timing = {0};
    struct mesh_event_params params = connection_params(5000u);
    struct mesh_relay_result relay_result;
    struct proto_packet first_proposal;
    uint8_t first_proposal_payload[CONTROL_PAYLOAD_CAPACITY];
    size_t first_proposal_payload_len;
    const uint8_t *delay_value = NULL;
    uint8_t delay_len = 0u;
    uint8_t proposer_index;
    uint8_t responder_index;
    uint16_t connection;
    uint32_t first_session;
    uint32_t retry_encoded_at_ms;
    uint32_t retry_session;
    uint16_t first_sequence;
    uint16_t retry_sequence;

    phase = "lost_accept_fresh_proposal_retry";
    mesh_sim_init(&world, UINT32_C(0x17170005));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &proposer_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &responder_index) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, proposer_index, responder_index, 100u, 4u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_add_connection(&world, proposer_index, responder_index,
                                  &params, true, &connection) == MESH_SIM_OK);
    CHECK(reciprocal_endpoint_init(
              &proposer, LEAF_ID, RELAY_A_ID,
              UINT64_C(0x1717000000000005), UINT32_C(0x17170005), 15u,
              6000u, false) == PROTO_OK);
    first_session = proposer.proposal.session_id;
    first_sequence = proposer.proposal.seq;
    first_proposal = proposer.proposal;
    first_proposal_payload_len = proposer.proposal_payload_len;
    memcpy(first_proposal_payload,
           proposer.proposal_payload,
           first_proposal_payload_len);

    CHECK(send_control_over_radio(
              &world, connection, proposer_index, MSG_MESH_EVENT_PROPOSE,
              proposer.proposal.session_id, proposer.proposal.seq,
              proposer.proposal_payload, proposer.proposal_payload_len) ==
          MESH_SIM_OK);
    CHECK(mesh_event_owner_begin_with_boot_nonce(
              &responder_owner,
              LEAF_ID,
              first_session,
              first_sequence,
              true,
              proposer.boot_nonce) == PROTO_OK);
    {
        uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

        CHECK(semantic_digest_sha256(proposer.proposal_payload,
                                    proposer.proposal_payload_len,
                                    digest));
        CHECK(mesh_event_owner_bind_remote_proposal_digest(
                  &responder_owner,
                  first_session,
                  first_sequence,
                  digest) == PROTO_OK);
    }

    /* Model a lost ACCEPT and the later same-parent contact retry. */
    CHECK(mesh_sim_run_until(&world, world.now_us + UINT64_C(1000000)) ==
          MESH_SIM_OK);
    retry_encoded_at_ms = (uint32_t)(world.now_us / 1000u);
    retry_session = first_session + 1u;
    retry_sequence = sequence_after(first_sequence, 1u);
    CHECK(reciprocal_endpoint_prepare_proposal_at(
              &proposer,
              retry_session,
              retry_sequence,
              retry_encoded_at_ms,
              retry_encoded_at_ms + 600u) == PROTO_OK);
    CHECK(proposer.proposal.session_id != first_session);
    CHECK(proposer.proposal.seq != first_sequence);
    CHECK(tlv_find_unique(proposer.proposal_payload,
                          proposer.proposal_payload_len,
                          TLV_MESH_NEXT_EVENT_TIME_MS,
                          &delay_value,
                          &delay_len) == PROTO_OK);
    CHECK(delay_len == sizeof(uint32_t));
    CHECK(proto_get_u32_le(delay_value) == 600u);

    CHECK(send_control_over_radio(
              &world, connection, proposer_index, MSG_MESH_EVENT_PROPOSE,
              proposer.proposal.session_id, proposer.proposal.seq,
              proposer.proposal_payload, proposer.proposal_payload_len) ==
          MESH_SIM_OK);
    CHECK(mesh_relay_handle_rx_with_random(
              &world.roles[responder_index].relay,
              &proposer.proposal,
              proposer.proposal_payload,
              proposer.proposal_payload_len,
              LEAF_ID,
              100u,
              retry_encoded_at_ms,
              0u,
              &relay_result) == PROTO_OK);
    CHECK(relay_result.status == PROTO_OK);
    CHECK((relay_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    CHECK(mesh_event_owner_classify_proposal(
              &responder_owner,
              RELAY_A_ID,
              LEAF_ID,
              &proposer.proposal,
              proposer.proposal_payload,
              proposer.proposal_payload_len) == MESH_EVENT_OWNER_APPLY);
    CHECK(mesh_event_timing_from_tlvs_at(
              &retry_received_timing,
              proposer.proposal_payload,
              proposer.proposal_payload_len,
              retry_encoded_at_ms,
              true) == PROTO_OK);
    CHECK(retry_received_timing.next_event_time_ms ==
          proposer.proposal_timing.next_event_time_ms);
    CHECK(retry_received_timing.event_counter ==
          proposer.proposal_timing.event_counter);

    CHECK(mesh_event_owner_begin_with_boot_nonce(
              &responder_owner,
              LEAF_ID,
              retry_session,
              retry_sequence,
              true,
              proposer.boot_nonce) == PROTO_OK);
    CHECK(responder_owner.session_id == retry_session);
    CHECK(mesh_event_owner_retains_session(&responder_owner, first_session));

    CHECK(mesh_event_timing_from_tlvs_at(
              &stale_received_timing,
              first_proposal_payload,
              first_proposal_payload_len,
              retry_encoded_at_ms,
              true) == PROTO_OK);
    CHECK(stale_received_timing.next_event_time_ms !=
          retry_received_timing.next_event_time_ms);
    CHECK(mesh_event_owner_classify_proposal(
              &responder_owner,
              RELAY_A_ID,
              LEAF_ID,
              &first_proposal,
              first_proposal_payload,
              first_proposal_payload_len) ==
          MESH_EVENT_OWNER_STALE);
    return 0;
}

static int run_reciprocal_proposal_arbitration_scenario(void)
{
    phase = "reciprocal_pending_higher_arrives_first";
    CHECK(run_reciprocal_proposal_case(true, false) == 0);
    phase = "reciprocal_pending_lower_arrives_first";
    CHECK(run_reciprocal_proposal_case(false, false) == 0);
    phase = "reciprocal_installed_higher_arrives_first";
    CHECK(run_reciprocal_proposal_case(true, true) == 0);
    phase = "reciprocal_installed_lower_arrives_first";
    CHECK(run_reciprocal_proposal_case(false, true) == 0);
    phase = "expired_installed_proposal_replay";
    CHECK(run_expired_installed_proposal_replay_case() == 0);
    CHECK(run_lost_accept_exact_proposal_retry_case() == 0);
    return run_lost_accept_fresh_proposal_retry_case();
}

static int run_event_control_scenario(void)
{
    static struct mesh_sim_world world;
    struct mesh_sim_invariant_report report;
    struct mesh_sim_fault_config faults = {0};
    struct mesh_sim_fault_stats fault_stats;
    struct mesh_event_params params;
    struct mesh_event_timing snapshot_a;
    struct mesh_event_timing snapshot_b;
    struct mesh_event_timing peer_timing_before_reset;
    struct mesh_event_owner owner_before_bad_proposal;
    struct mesh_event_timing timing_before_bad_proposal;
    struct mesh_sim_connection_action repair_action;
    uint8_t valid_payload[CONTROL_PAYLOAD_CAPACITY];
    uint8_t zero_nonce_payload[CONTROL_PAYLOAD_CAPACITY];
    const uint8_t malformed_payload[] = {
        TLV_MESH_EVENT_INTERVAL_MS, 4u, 0x01u,
    };
    size_t valid_payload_len;
    size_t proposal_timing_payload_len;
    size_t zero_nonce_payload_len;
    uint64_t selected_next_hop = 0u;
    uint64_t expiry_us;
    uint32_t session_n;
    uint32_t session_n1;
    uint32_t session_before_reset;
    uint32_t malformed_proposal_session;
    uint32_t generation_n1_a;
    uint32_t generation_n1_b;
    uint16_t seq_n;
    uint16_t seq_n1;
    uint16_t seq_bc;
    uint16_t leaf_proposal_seq;
    uint16_t relay_proposal_seq;
    uint16_t malformed_proposal_seq;
    uint16_t connection_ab;
    uint16_t connection_bc;
    uint8_t expired_endpoints = 0u;
    uint8_t leaf;
    uint8_t relay_a;
    uint8_t relay_b;
    uint8_t gateway;

    mesh_sim_init(&world, UINT32_C(0x7ea4d91b));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &leaf) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay_a) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_B_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay_b) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_GATEWAY, GATEWAY_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &gateway) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, leaf, relay_a, 96u, 4u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_a, relay_b, 95u, 6u) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, relay_b, gateway, 97u, 3u) == MESH_SIM_OK);
    CHECK(!world.reachable[leaf][relay_b]);
    CHECK(!world.reachable[leaf][gateway]);
    CHECK(!world.reachable[relay_a][gateway]);
    CHECK(mesh_sim_install_route(&world, leaf, relay_a, 2u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&world, relay_a, relay_b, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_install_route(&world, relay_b, gateway, 0u,
                                 ROUTE_EPOCH) == PROTO_OK);

    phase = "forced_multihop_radio_negotiation";
    params = connection_params(5000u);
    CHECK(mesh_sim_add_connection_over_radio(&world, leaf, relay_a, &params,
                                             true, &connection_ab) ==
          MESH_SIM_OK);

    phase = "proposal_nonce_fail_closed_before_timing_mutation";
    CHECK(mesh_sim_connection_next_action(&world, connection_ab,
                                          &repair_action) == MESH_SIM_OK);
    CHECK(repair_action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR);
    CHECK(repair_action.already_scheduled);
    CHECK(mesh_sim_run_until(&world, repair_action.start_us) == MESH_SIM_OK);
    malformed_proposal_session =
        world.connections[connection_ab].repair_session_id;
    malformed_proposal_seq = world.connections[connection_ab].repair_seq;
    CHECK(append_control_timing_payload(
              &world, &world.connections[connection_ab].timing_a,
              malformed_proposal_session, false,
              valid_payload, sizeof(valid_payload),
              &proposal_timing_payload_len) ==
          PROTO_OK);
    memcpy(zero_nonce_payload, valid_payload, proposal_timing_payload_len);
    zero_nonce_payload_len = proposal_timing_payload_len;
    CHECK(tlv_append_u64(zero_nonce_payload, sizeof(zero_nonce_payload),
                         &zero_nonce_payload_len,
                         TLV_MESH_EVENT_BOOT_NONCE,
                         0u) == PROTO_OK);
    owner_before_bad_proposal = world.connections[connection_ab].owner_b;
    timing_before_bad_proposal = world.connections[connection_ab].timing_b;
    CHECK(send_control_tx_into_existing_rx(
              &world, connection_ab, leaf, MSG_MESH_EVENT_PROPOSE,
              malformed_proposal_session, malformed_proposal_seq,
              valid_payload, proposal_timing_payload_len) == MESH_SIM_OK);
    CHECK(memcmp(&owner_before_bad_proposal,
                 &world.connections[connection_ab].owner_b,
                 sizeof(owner_before_bad_proposal)) == 0);
    CHECK(timing_equal(&timing_before_bad_proposal,
                       &world.connections[connection_ab].timing_b));
    CHECK(!world.connections[connection_ab].repair_propose_decoded);
    CHECK(send_control_tx_into_existing_rx(
              &world, connection_ab, leaf, MSG_MESH_EVENT_PROPOSE,
              malformed_proposal_session, malformed_proposal_seq,
              zero_nonce_payload, zero_nonce_payload_len) == MESH_SIM_OK);
    CHECK(memcmp(&owner_before_bad_proposal,
                 &world.connections[connection_ab].owner_b,
                 sizeof(owner_before_bad_proposal)) == 0);
    CHECK(timing_equal(&timing_before_bad_proposal,
                       &world.connections[connection_ab].timing_b));
    CHECK(!world.connections[connection_ab].repair_propose_decoded);

    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].repair_requester_timing.event_counter ==
          world.connections[connection_ab].repair_session_id);
    CHECK(world.connections[connection_ab].repair_peer_timing.event_counter ==
          world.connections[connection_ab].repair_session_id);
    CHECK(mesh_event_timing_local_tx_slot(
              &world.connections[connection_ab].repair_requester_timing));
    CHECK(mesh_event_timing_local_rx_slot(
              &world.connections[connection_ab].repair_peer_timing));
    leaf_proposal_seq = world.connections[connection_ab].repair_seq;
    CHECK(leaf_proposal_seq != 0u);
    CHECK(world.roles[leaf].event_control_seq == leaf_proposal_seq);
    CHECK(world.transmission_count == 4u);
    params = connection_params((uint32_t)(world.now_us / 1000u) + 5000u);
    CHECK(mesh_sim_add_connection_over_radio(&world, relay_a, relay_b, &params,
                                             true, &connection_bc) ==
          MESH_SIM_OK);
    CHECK(run_scheduled_connection_exchange(&world, connection_bc) ==
          MESH_SIM_OK);
    relay_proposal_seq = world.connections[connection_bc].repair_seq;
    CHECK(relay_proposal_seq == leaf_proposal_seq);
    CHECK(world.roles[relay_a].event_control_seq == relay_proposal_seq);
    CHECK(world.transmission_count == 6u);
    CHECK(connection_owners_active(&world.connections[connection_ab]));
    CHECK(connection_owners_active(&world.connections[connection_bc]));
    CHECK(world.connections[connection_ab].repair_propose_decoded);
    CHECK(world.connections[connection_ab].repair_accept_decoded);
    CHECK(world.connections[connection_bc].repair_propose_decoded);
    CHECK(world.connections[connection_bc].repair_accept_decoded);

    phase = "independent_sequence_domains_and_duplicate";
    session_n = world.connections[connection_ab].owner_a.session_id;
    seq_n = sequence_after(world.connections[connection_ab].repair_seq,
                           UINT16_C(0x4000));
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                101u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n, seq_n,
                                  valid_payload, valid_payload_len) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.local_control_seen);
    CHECK(world.connections[connection_ab].owner_b.remote_control_seen);
    CHECK(world.connections[connection_ab].owner_a.local_sequence == seq_n);
    CHECK(world.connections[connection_ab].owner_b.remote_sequence == seq_n);
    CHECK(mesh_event_timing_local_tx_slot(
              &world.connections[connection_ab].timing_a) !=
          mesh_event_timing_local_tx_slot(
              &world.connections[connection_ab].timing_b));
    snapshot_a = world.connections[connection_ab].timing_a;
    snapshot_b = world.connections[connection_ab].timing_b;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n, seq_n,
                                  valid_payload, valid_payload_len) ==
          MESH_SIM_OK);
    CHECK(timing_equal(&snapshot_a,
                       &world.connections[connection_ab].timing_a));
    CHECK(timing_equal(&snapshot_b,
                       &world.connections[connection_ab].timing_b));

    phase = "reordered_and_malformed_controls";
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                103u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 3u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    snapshot_a = world.connections[connection_ab].timing_a;
    snapshot_b = world.connections[connection_ab].timing_b;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 2u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    CHECK(timing_equal(&snapshot_a,
                       &world.connections[connection_ab].timing_a));
    CHECK(timing_equal(&snapshot_b,
                       &world.connections[connection_ab].timing_b));
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 4u),
                                  malformed_payload,
                                  sizeof(malformed_payload)) == MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.local_sequence ==
          sequence_after(seq_n, 3u));
    CHECK(world.connections[connection_ab].owner_b.remote_sequence ==
          sequence_after(seq_n, 3u));
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                104u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 4u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.local_sequence ==
          sequence_after(seq_n, 4u));
    CHECK(world.connections[connection_ab].owner_b.remote_sequence ==
          sequence_after(seq_n, 4u));

    phase = "route_change_and_operation_n_plus_one";
    CHECK(mesh_sim_set_link(&world, leaf, relay_b, 99u, 5u) == MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world, leaf, relay_b, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_relay_select_next_hop(&world.roles[leaf].relay, GATEWAY_ID,
                                     &selected_next_hop) == PROTO_OK);
    CHECK(selected_next_hop == RELAY_B_ID);
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, leaf) == MESH_SIM_OK);
    leaf_proposal_seq = sequence_after(leaf_proposal_seq, 1u);
    CHECK(world.connections[connection_ab].repair_seq == leaf_proposal_seq);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    session_n1 = world.connections[connection_ab].owner_a.session_id;
    CHECK(session_n1 != session_n);
    CHECK(world.connections[connection_ab].owner_b.session_id == session_n1);
    generation_n1_a = world.connections[connection_ab].owner_a.generation;
    generation_n1_b = world.connections[connection_ab].owner_b.generation;
    snapshot_a = world.connections[connection_ab].timing_a;
    snapshot_b = world.connections[connection_ab].timing_b;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n,
                                  sequence_after(seq_n, 5u), valid_payload,
                                  valid_payload_len) == MESH_SIM_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n,
                                  sequence_after(seq_n, 6u), NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(connection_owners_active(&world.connections[connection_ab]));
    CHECK(world.connections[connection_ab].owner_a.generation ==
          generation_n1_a);
    CHECK(world.connections[connection_ab].owner_b.generation ==
          generation_n1_b);
    CHECK(timing_equal(&snapshot_a,
                       &world.connections[connection_ab].timing_a));
    CHECK(timing_equal(&snapshot_b,
                       &world.connections[connection_ab].timing_b));

    phase = "matching_end_is_single_terminal_transition";
    seq_n1 = sequence_after(world.connections[connection_ab].repair_seq,
                            UINT16_C(0x4000));
    CHECK(append_update_payload(&world,
                                &world.connections[connection_ab].timing_a,
                                201u, valid_payload,
                                sizeof(valid_payload),
                                &valid_payload_len) == PROTO_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_UPDATE, session_n1, seq_n1,
                                  valid_payload, valid_payload_len) ==
          MESH_SIM_OK);
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n1,
                                  sequence_after(seq_n1, 1u), NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    CHECK(world.connections[connection_ab].owner_a.terminal);
    CHECK(world.connections[connection_ab].owner_b.terminal);
    generation_n1_a = world.connections[connection_ab].owner_a.generation;
    generation_n1_b = world.connections[connection_ab].owner_b.generation;
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n1,
                                  sequence_after(seq_n1, 1u), NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.generation ==
          generation_n1_a);
    CHECK(world.connections[connection_ab].owner_b.generation ==
          generation_n1_b);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));

    phase = "opposite_requester_uses_its_own_sequence_domain";
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, relay_a) == MESH_SIM_OK);
    relay_proposal_seq = sequence_after(relay_proposal_seq, 1u);
    CHECK(world.connections[connection_ab].repair_seq == relay_proposal_seq);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.proposal_from_peer);
    CHECK(!world.connections[connection_ab].owner_b.proposal_from_peer);
    CHECK(connection_owners_active(&world.connections[connection_ab]));

    phase = "reset_recovery_preserves_peer_owner";
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, leaf) == MESH_SIM_OK);
    leaf_proposal_seq = sequence_after(leaf_proposal_seq, 1u);
    CHECK(world.connections[connection_ab].repair_seq == leaf_proposal_seq);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    session_before_reset = world.connections[connection_ab].owner_a.session_id;
    peer_timing_before_reset = world.connections[connection_ab].timing_b;
    CHECK(mesh_sim_reset_role(&world, leaf) == MESH_SIM_OK);
    CHECK(world.roles[leaf].event_control_seq == 0u);
    CHECK(!world.connections[connection_ab].owner_a.active);
    CHECK(world.connections[connection_ab].owner_a.terminal);
    CHECK(world.connections[connection_ab].owner_b.active);
    CHECK(world.connections[connection_ab].owner_b.session_id ==
          session_before_reset);
    CHECK(!mesh_event_timing_usable(
              &world.connections[connection_ab].timing_a,
              (uint32_t)(world.now_us / 1000u)));
    CHECK(mesh_event_timing_usable(
              &world.connections[connection_ab].timing_b,
              (uint32_t)(world.now_us / 1000u)));
    CHECK(timing_equal(&peer_timing_before_reset,
                       &world.connections[connection_ab].timing_b));
    CHECK(mesh_sim_check_invariants(&world, &report) == MESH_SIM_OK);

    phase = "reset_recovery_waits_for_peer_supervision";
    expiry_us = ((uint64_t)peer_timing_before_reset.last_successful_ch9_event_ms +
                 peer_timing_before_reset.supervision_timeout_ms + 1u) *
                1000u;
    if (expiry_us > world.now_us) {
        CHECK(mesh_sim_run_until(&world, expiry_us) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_expire_connection_ownership(
              &world, connection_ab, &expired_endpoints) == MESH_SIM_OK);
    CHECK(expired_endpoints == 1u);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection_ab, leaf) == MESH_SIM_OK);
    CHECK(world.connections[connection_ab].repair_seq == 1u);
    CHECK(run_scheduled_connection_exchange(&world, connection_ab) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection_ab].owner_a.session_id !=
          session_before_reset);

    phase = "lost_end_expires_orphan_within_supervision_bound";
    faults.seed = UINT32_C(0x51a1e0d5);
    faults.frame_loss_permyriad = MESH_SIM_FAULT_RATE_SCALE;
    CHECK(mesh_sim_set_fault_config(&world, &faults) == MESH_SIM_OK);
    session_n1 = world.connections[connection_ab].owner_a.session_id;
    seq_n1 = sequence_after(world.connections[connection_ab].repair_seq,
                            UINT16_C(0x4000));
    CHECK(send_control_over_radio(&world, connection_ab, leaf,
                                  MSG_MESH_EVENT_END, session_n1, seq_n1,
                                  NULL, 0u) == MESH_SIM_OK);
    CHECK(!world.connections[connection_ab].owner_a.active);
    CHECK(world.connections[connection_ab].owner_a.terminal);
    CHECK(world.connections[connection_ab].owner_b.active);
    if (mesh_sim_check_invariants(&world, &report) != MESH_SIM_OK) {
        fprintf(stderr, "lost-END invariant=%s detail=%llu description=%s\n",
                mesh_sim_invariant_name(report.code),
                (unsigned long long)report.detail,
                report.description == NULL ? "?" : report.description);
        return 1;
    }
    CHECK(mesh_sim_get_fault_stats(&world, &fault_stats) == MESH_SIM_OK);
    CHECK(fault_stats.frame_losses == 1u);
    expiry_us = ((uint64_t)
        world.connections[connection_ab].timing_b.last_successful_ch9_event_ms +
        world.connections[connection_ab].timing_b.supervision_timeout_ms + 1u) *
        1000u;
    if (expiry_us > world.now_us) {
        CHECK(mesh_sim_run_until(&world, expiry_us) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_expire_connection_ownership(
              &world, connection_ab, &expired_endpoints) == MESH_SIM_OK);
    CHECK(expired_endpoints == 1u);
    CHECK(connection_owners_inactive(&world.connections[connection_ab]));
    memset(&faults, 0, sizeof(faults));
    CHECK(mesh_sim_set_fault_config(&world, &faults) == MESH_SIM_OK);

    phase = "second_hop_teardown_and_bounded_idle";
    seq_bc = sequence_after(world.connections[connection_bc].repair_seq,
                            UINT16_C(0x4000));
    CHECK(send_control_over_radio(
              &world, connection_bc, relay_a, MSG_MESH_EVENT_END,
              world.connections[connection_bc].owner_a.session_id,
              seq_bc, NULL, 0u) == MESH_SIM_OK);
    CHECK(connection_owners_inactive(&world.connections[connection_bc]));
    {
        unsigned int propose_count = 0u;
        unsigned int accept_count = 0u;
        unsigned int update_count = 0u;
        unsigned int end_count = 0u;

        for (size_t i = 0u; i < world.transmission_count; i++) {
            switch (world.transmissions[i].protocol_msg_type) {
            case MSG_MESH_EVENT_PROPOSE:
                propose_count++;
                break;
            case MSG_MESH_EVENT_ACCEPT:
                accept_count++;
                break;
            case MSG_MESH_EVENT_UPDATE:
                update_count++;
                break;
            case MSG_MESH_EVENT_END:
                end_count++;
                break;
            default:
                break;
            }
        }
        CHECK(propose_count == 8u);
        CHECK(accept_count == 6u);
        CHECK(update_count == 8u);
        CHECK(end_count == 5u);
        CHECK(world.transmission_count ==
              (size_t)propose_count + accept_count + update_count + end_count);
    }
    CHECK(world.transition_count <= MAX_SCENARIO_TRANSITIONS);
    CHECK(mesh_sim_count_transitions(&world,
              MESH_SIM_TRANSITION_ROUTE_REQUIRED, 0u) == 0u);
    CHECK(mesh_sim_count_transitions(&world,
              MESH_SIM_TRANSITION_RETRY_READY, 0u) == 0u);
    for (size_t i = 0u; i < world.role_count; i++) {
        CHECK(world.roles[i].tx_queue_count == 0u);
        CHECK(world.roles[i].watchdog.workers_active == 0u);
        CHECK(world.roles[i].radio_state == MESH_SIM_RADIO_SLEEP);
    }
    CHECK(mesh_sim_check_settled(&world, &report) == MESH_SIM_OK);
    CHECK(report.code == MESH_SIM_INVARIANT_NONE);
    return 0;
}

static int run_event_control_sequence_wrap_scenario(void)
{
    static struct mesh_sim_world world;
    struct mesh_event_params params = connection_params(5000u);
    uint16_t connection;
    uint8_t leaf;
    uint8_t relay;

    phase = "event_control_sequence_wrap_setup";
    mesh_sim_init(&world, UINT32_C(0x5e91a7e1));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_TRANSMITTER, LEAF_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &leaf) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR, RELAY_A_ID,
                            GATEWAY_ID, ROUTE_EPOCH, &relay) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, leaf, relay, 100u, 4u) == MESH_SIM_OK);

    /* add_connection_over_radio first builds, then replaces, its local model. */
    world.roles[leaf].event_control_seq = UINT16_MAX - 2u;
    CHECK(mesh_sim_add_connection_over_radio(&world, leaf, relay, &params,
                                             true, &connection) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection].repair_seq == UINT16_MAX);
    CHECK(run_scheduled_connection_exchange(&world, connection) ==
          MESH_SIM_OK);
    CHECK(world.connections[connection].owner_b.remote_proposal_sequence ==
          UINT16_MAX);

    phase = "event_control_sequence_wrap_skips_zero";
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection, leaf) == MESH_SIM_OK);
    CHECK(world.connections[connection].repair_seq == 1u);
    CHECK(run_scheduled_connection_exchange(&world, connection) ==
          MESH_SIM_OK);
    CHECK(world.roles[leaf].event_control_seq == 1u);
    CHECK(world.connections[connection].owner_b.remote_proposal_sequence == 1u);
    CHECK(connection_owners_active(&world.connections[connection]));
    return 0;
}

static int run_reset_stale_peer_recovery_bound(void)
{
    static struct mesh_sim_world world;
    struct mesh_event_params params = connection_params(5000u);
    struct mesh_sim_connection_action action;
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = LEAF_ID + UINT64_C(0x100),
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x71000001),
        .seq = 1u,
        .ttl = 1u,
        .payload_len = 0u,
    };
    uint8_t leaf;
    uint8_t peer;
    uint16_t connection;
    uint32_t peer_session;
    uint64_t boot_nonce_before_reset;
    uint64_t boot_nonce_after_reset;
    uint64_t expiry_us;
    uint8_t expired_endpoints = 0u;
    struct mesh_event_timing peer_timing_before_reset;
    int ret;

    phase = "reset_stale_peer_setup";
    mesh_sim_init(&world, UINT32_C(0x7100a55a));
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            packet.src_id, GATEWAY_ID, ROUTE_EPOCH,
                            &leaf) == MESH_SIM_OK);
    CHECK(mesh_sim_add_role(&world, MESH_SIM_ROLE_ANCHOR,
                            RELAY_A_ID + UINT64_C(0x100), GATEWAY_ID,
                            ROUTE_EPOCH, &peer) == MESH_SIM_OK);
    CHECK(mesh_sim_set_link(&world, leaf, peer, 98u, 6u) == MESH_SIM_OK);
    CHECK(mesh_sim_install_route(&world, leaf, peer, 1u,
                                 ROUTE_EPOCH) == PROTO_OK);
    CHECK(mesh_sim_add_connection_over_radio(&world, leaf, peer, &params,
                                             true, &connection) == MESH_SIM_OK);
    CHECK(run_scheduled_connection_exchange(&world, connection) ==
          MESH_SIM_OK);
    peer_session = world.connections[connection].owner_b.session_id;
    boot_nonce_before_reset = world.roles[leaf].event_boot_nonce;
    peer_timing_before_reset = world.connections[connection].timing_b;

    phase = "reset_stale_peer_preserves_peer_owner";
    CHECK(mesh_sim_reset_role(&world, leaf) == MESH_SIM_OK);
    boot_nonce_after_reset = world.roles[leaf].event_boot_nonce;
    CHECK(boot_nonce_after_reset != 0u);
    CHECK(boot_nonce_after_reset != boot_nonce_before_reset);
    CHECK(world.connections[connection].owner_b.active);
    CHECK(world.connections[connection].owner_b.session_id == peer_session);
    CHECK(mesh_sim_queue_originated(&world, leaf, &packet, NULL, 0u) ==
          MESH_SIM_OK);
    CHECK(mesh_sim_connection_next_action(&world, connection, &action) ==
          MESH_SIM_OK);
    CHECK(action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL5_REPAIR);
    CHECK(mesh_sim_schedule_next_connection_event(&world, connection, false) ==
          MESH_SIM_OK);

    /*
     * The peer cannot order random boot nonces, so the first repair remains
     * inert while its pre-reset owner is live. This prevents an old replay
     * from using the same replacement path.
     */
    phase = "reset_recovery_rejects_fresh_incarnation_while_old_owner_live";
    ret = mesh_sim_run_until(&world, action.end_us);
    CHECK(ret == MESH_SIM_OK);
    CHECK(!world.connections[connection].owner_a.active);
    CHECK(world.connections[connection].owner_b.active);
    CHECK(world.connections[connection].owner_b.session_id == peer_session);

    phase = "reset_recovery_accepts_fresh_incarnation_after_supervision";
    expiry_us =
        ((uint64_t)peer_timing_before_reset.last_successful_ch9_event_ms +
         peer_timing_before_reset.supervision_timeout_ms + 1u) * 1000u;
    if (expiry_us > world.now_us) {
        CHECK(mesh_sim_run_until(&world, expiry_us) == MESH_SIM_OK);
    }
    CHECK(mesh_sim_expire_connection_ownership(
              &world, connection, &expired_endpoints) == MESH_SIM_OK);
    CHECK(expired_endpoints == 1u);
    CHECK(connection_owners_inactive(&world.connections[connection]));
    CHECK(mesh_sim_renegotiate_connection_over_radio(
              &world, connection, leaf) == MESH_SIM_OK);
    CHECK(run_scheduled_connection_exchange(&world, connection) ==
          MESH_SIM_OK);
    CHECK(connection_owners_active(&world.connections[connection]));
    CHECK(world.connections[connection].owner_a.session_id != peer_session);
    CHECK(world.connections[connection].owner_b.active);
    CHECK(world.connections[connection].owner_b.session_id != peer_session);
    CHECK(world.connections[connection].owner_b.remote_boot_nonce ==
          boot_nonce_after_reset);
    return 0;
}

int main(void)
{
    int ret = run_reciprocal_proposal_arbitration_scenario();

    if (ret != 0) {
        return ret;
    }
    ret = run_event_control_scenario();
    if (ret != 0) {
        return ret;
    }
    ret = run_reset_stale_peer_recovery_bound();
    return ret == 0 ? run_event_control_sequence_wrap_scenario() : ret;
}
