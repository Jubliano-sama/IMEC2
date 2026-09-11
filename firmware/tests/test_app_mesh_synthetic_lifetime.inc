/* Included only by the synthetic-role communication adapter fixture. The
 * producer, admission retry, shared expiry policy and custody service are real;
 * its existing radio/backend callbacks provide the controlled obstruction. */
#include "app_mesh_smoke_fast.h"
#ifdef NDEBUG
#error "Synthetic lifetime regressions require assertions enabled"
#endif
#define DEVICE_ID UINT64_C(0x3333333333333301)
#define GATEWAY_ID UINT64_C(0x1111111111111111)
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS 1000u
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT 4u
#define CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES 900u
#define CONFIG_MESH_SMOKE_FAST_TX 1
#define IS_ENABLED(value) (value)
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#define LOG_WRN(...) status_debug_printf(__VA_ARGS__)
#include "synthetic_source_state.inc"

static struct mesh_relay mesh_runtime;
static uint32_t mesh_test_next_packet_id, mesh_test_drop_count;
static uint16_t mesh_test_attempt, mesh_test_seq;
static uint8_t mesh_test_wait_log_ticks;
static uint32_t mesh_test_first_queue_ms, mesh_test_first_queue_id;
static uint16_t mesh_test_first_queue_seq;
static uint32_t mesh_test_queued_total;
static struct mesh_test_delivery_tracking mesh_test_deliveries[MESH_TEST_SOURCE_DELIVERY_CAPACITY];
static struct mesh_outbound mesh_test_pending_admission;
static uint64_t mesh_test_pending_admission_deadline_ms;
static uint32_t mesh_test_pending_admission_packet_id;
static uint16_t mesh_test_pending_admission_attempt;
static bool mesh_test_pending_admission_valid;
static unsigned synthetic_delivered_notes, synthetic_failed_notes;

static void status_debug_note(const char *message)
{
    if(strcmp(message,"DBG_MESH_TEST_DELIVERED\n")==0)synthetic_delivered_notes++;
    if(strcmp(message,"DBG_MESH_TEST_DELIVERY_FAILED\n")==0)synthetic_failed_notes++;
}
static void mesh_test_route_periodic_summary(void) {}
#include "synthetic_source_production.inc"

static void synthetic_reset_at(uint64_t now)
{
    assert(now<=INT64_MAX);
    reset_fixture();atomic_store(&fake_now_ms,(int64_t)now);
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_ANCHOR,DEVICE_ID,GATEWAY_ID,1u);
    mesh_test_next_packet_id=1u;mesh_test_attempt=mesh_test_seq=0u;
    mesh_test_drop_count=mesh_test_queued_total=0u;mesh_test_wait_log_ticks=0u;
    mesh_test_first_queue_ms=mesh_test_first_queue_id=0u;mesh_test_first_queue_seq=0u;
    memset(mesh_test_deliveries,0,sizeof(mesh_test_deliveries));
    memset(&mesh_test_pending_admission,0,sizeof(mesh_test_pending_admission));
    mesh_test_pending_admission_deadline_ms=0u;mesh_test_pending_admission_packet_id=0u;
    mesh_test_pending_admission_attempt=0u;mesh_test_pending_admission_valid=false;
    synthetic_delivered_notes=synthetic_failed_notes=0u;
}
static void synthetic_build_packet(struct mesh_outbound *out,uint32_t id)
{
    assert(mesh_test_build_packet(out,id,1u)==0);
    assert(out->payload_len==902u);
    assert(mesh_relay_outbox_expiry_s_for_packet(&out->packet,out->payload,out->payload_len)==86400u);
}
static void synthetic_assert_same(const struct mesh_outbound *actual,const struct mesh_outbound *expected)
{
    assert_same_frozen_control(actual,expected);
    assert(actual->packet.flags==expected->packet.flags);
    assert(actual->packet.ttl==expected->packet.ttl);
    assert(actual->packet.message_age_ms==expected->packet.message_age_ms);
}
static void test_synthetic_deadline_uses_shared_policy_and_remaining_age(void)
{
    const uint64_t starts[]={0u,1000u,UINT64_C(25)*86400000u,
        UINT64_C(49)*86400000u,(UINT64_C(1)<<32)-25u};
    for(size_t i=0u;i<sizeof(starts)/sizeof(starts[0]);i++) {
        struct mesh_outbound out;
        synthetic_reset_at(starts[i]);synthetic_build_packet(&out,1u);
        assert(mesh_test_delivery_deadline_ms(&out)==starts[i]+86400000u);
        out.packet.message_age_ms=12345u;
        atomic_store(&fake_now_ms,(int64_t)(starts[i]+70000u));
        assert(mesh_test_delivery_deadline_ms(&out)==starts[i]+86400000u-12345u);
        /* An absent queued timestamp must not subtract an arbitrary stale value. */
        out.queued_at_valid=false;out.queued_at_ms=UINT32_MAX;
        assert(mesh_test_delivery_deadline_ms(&out)==starts[i]+70000u+86400000u-12345u);
        out.packet.message_age_ms=86400000u;
        assert(mesh_test_delivery_deadline_ms(&out)==starts[i]+70000u);
        out.queued_at_valid=true;out.queued_at_ms=(uint32_t)starts[i];
        out.packet.message_age_ms=UINT32_MAX-20u;
        assert(mesh_test_delivery_deadline_ms(&out)==starts[i]+70000u);
    }
}

static void synthetic_assert_terminal(
    uint32_t handle, uint32_t packet_id,
    enum node_comm_terminal_reason reason, uint8_t attempts,
    uint64_t terminal_at_ms)
{
    struct node_comm_terminal_event event;

    /* The producer owns the handle; peek lets its actual reaper consume it. */
    assert(app_node_comm_peek_delivery_event_for(handle, &event));
    assert(event.handle == handle);
    assert(event.client_token == packet_id);
    assert(event.reason == reason);
    assert(event.attempts_started == attempts);
    assert(event.terminal_at_ms == terminal_at_ms);
    assert(event.proof == (reason == NODE_COMM_TERMINAL_DELIVERED ?
        NODE_COMM_TERMINAL_PROOF_SEMANTIC : NODE_COMM_TERMINAL_PROOF_NONE));
}

static void test_synthetic_queued_burst_survives_60s_and_requires_exact_receipts(void)
{
    const uint64_t admitted_at = 1234u;
    const uint64_t held_until = admitted_at + 120001u;
    const uint64_t deadline = admitted_at + UINT64_C(86400000);
    struct mesh_test_delivery_tracking admitted[4u];
    struct node_comm_terminal_event event;
    struct mesh_outbound wrong;
    uint8_t attempts;

    synthetic_reset_at(admitted_at);
    assert(CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT == 4u);
    (void)mesh_test_tx_once();
    assert(mesh_test_queued_total == 4u);
    assert(mesh_test_drop_count == 0u);
    assert(!mesh_test_pending_admission_valid);
    memcpy(admitted, mesh_test_deliveries, sizeof(admitted));
    assert(app_node_comm_pending_delivery_count() == 4u);
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(try_uplink_envelopes[0].packet.msg_type == MSG_MESH_DATA);
    assert(try_uplink_envelopes[0].payload_len == 902u);
    assert(try_uplink_envelopes[0].radio_channel == UWB_CHANNEL_MESH_PAYLOAD);

    /* One live backend owns the first packet. Later packets really enter the
     * core's resource wait, and keep zero RF attempts beyond the old cutoff. */
    for (size_t i = 1u; i < 4u; i++) {
        assert(app_node_comm_service_deliveries() == -EAGAIN);
    }
    atomic_store(&fake_now_ms, (int64_t)held_until);
    assert(app_node_comm_pending_delivery_count() == 4u);
    for (size_t i = 0u; i < 4u; i++) {
        assert(app_node_comm_service_deliveries() == -EAGAIN);
        assert(app_node_comm_peek_delivery_attempts_started(
                   admitted[i].handle, &attempts) == 0);
        assert(attempts == (i == 0u ? 1u : 0u));
        assert(!app_node_comm_peek_delivery_event_for(admitted[i].handle,
                                                     &event));
    }
    assert(try_uplink_calls == 1u);
    assert(app_node_comm_pending_delivery_count() == 4u);
    assert(try_uplink_deadlines_ms[0] == deadline);

    wrong = try_uplink_envelopes[0];
    wrong.packet.seq ^= UINT16_C(0x4000);
    assert(note_gateway_confirmed_envelope(&wrong) == -ENOENT);
    wrong = try_uplink_envelopes[0];
    wrong.payload[wrong.payload_len - 1u] ^= 1u;
    assert(note_gateway_confirmed_envelope(&wrong) == -EBADMSG);
    assert(!app_node_comm_peek_delivery_event_for(admitted[0].handle, &event));
    assert(app_node_comm_pending_delivery_count() == 4u);

    for (size_t i = 0u; i < 4u; i++) {
        struct mesh_smoke_fast_payload decoded;
        if (i != 0u) {
            assert(app_node_comm_service_deliveries() == 0);
        }
        assert(try_uplink_calls == i + 1u);
        assert(!try_uplink_confirmed[i]);
        assert(try_uplink_envelopes[i].packet.seq == admitted[i].seq);
        assert(try_uplink_envelopes[i].payload_len == 902u);
        assert(mesh_smoke_fast_payload_decode(try_uplink_envelopes[i].payload,
                   try_uplink_envelopes[i].payload_len,&decoded)==PROTO_OK);
        assert(decoded.packet_id==admitted[i].packet_id);
        assert(decoded.build_uptime_ms==admitted_at);
        assert(decoded.origin_id==DEVICE_ID&&decoded.target_id==GATEWAY_ID);
        assert(try_uplink_envelopes[i].queued_at_ms == admitted_at);
        assert(try_uplink_deadlines_ms[i] == deadline);
        assert(!app_node_comm_peek_delivery_event_for(admitted[i].handle,
                                                     &event));
        assert(note_gateway_confirmed_envelope(&try_uplink_envelopes[i]) == 0);
        synthetic_assert_terminal(admitted[i].handle, admitted[i].packet_id,
                                  NODE_COMM_TERMINAL_DELIVERED, 1u, held_until);
    }
    mesh_test_reap_terminal_deliveries();
    for (size_t i = 0u; i < MESH_TEST_SOURCE_DELIVERY_CAPACITY; i++) {
        assert(mesh_test_deliveries[i].handle == 0u);
    }
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(mesh_test_drop_count == 0u);
    assert(mesh_test_queued_total == 4u);
    assert(synthetic_delivered_notes==4u&&synthetic_failed_notes==0u);
}

static void test_synthetic_shared_policy_expires_unattempted_queue_truthfully(void)
{
    const uint64_t admitted_at = 7777u;
    const uint64_t deadline = admitted_at + UINT64_C(86400000);
    struct mesh_test_delivery_tracking admitted[4u];
    struct node_comm_terminal_event event;
    uint8_t attempts;

    synthetic_reset_at(admitted_at);
    (void)mesh_test_tx_once();
    assert(mesh_test_queued_total == 4u);
    memcpy(admitted, mesh_test_deliveries, sizeof(admitted));
    assert(app_node_comm_service_deliveries() == 0);
    assert(try_uplink_calls == 1u);
    assert(try_uplink_deadlines_ms[0] == deadline);
    for (size_t i = 1u; i < 4u; i++) {
        assert(app_node_comm_service_deliveries() == -EAGAIN);
    }

    atomic_store(&fake_now_ms, (int64_t)(deadline - 1u));
    assert(app_node_comm_pending_delivery_count() == 4u);
    for (size_t i = 0u; i < 4u; i++) {
        assert(app_node_comm_peek_delivery_attempts_started(
                   admitted[i].handle, &attempts) == 0);
        assert(attempts == (i == 0u ? 1u : 0u));
        assert(!app_node_comm_peek_delivery_event_for(admitted[i].handle,
                                                     &event));
    }
    assert(try_uplink_calls == 1u);
    atomic_store(&fake_now_ms, (int64_t)deadline);
    assert(app_node_comm_service_deliveries() == -EAGAIN);
    assert(try_uplink_calls == 1u);
    for (size_t i = 0u; i < 4u; i++) {
        synthetic_assert_terminal(admitted[i].handle, admitted[i].packet_id,
                                  NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
                                  i == 0u ? 1u : 0u, deadline);
    }
    /* An exact but late receipt cannot relabel expiry as delivery. */
    assert(note_gateway_confirmed_envelope(&try_uplink_envelopes[0]) == -ESTALE);
    mesh_test_reap_terminal_deliveries();
    assert(mesh_test_drop_count == 4u);
    assert(synthetic_delivered_notes==0u&&synthetic_failed_notes==4u);
    assert(mesh_test_queued_total == 4u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    for (size_t i = 0u; i < MESH_TEST_SOURCE_DELIVERY_CAPACITY; i++) {
        assert(mesh_test_deliveries[i].handle == 0u);
    }
}

static void test_synthetic_deferred_admission_keeps_original_packet_and_deadline(void)
{
    const uint64_t created_at = 9876u;
    const uint64_t resumed_at = created_at + 90001u;
    const uint64_t original_deadline = created_at + UINT64_C(86400000);
    struct mesh_outbound saved;
    struct mesh_test_delivery_tracking admitted[4u];
    uint32_t saved_packet_id;
    uint16_t saved_attempt;

    synthetic_reset_at(created_at);
    assert(app_node_comm_stop_preserving_queued() == 0);
    assert(mesh_test_tx_once() == MESH_TEST_RETRY_ADMISSION_MS);
    assert(mesh_test_pending_admission_valid);
    assert(mesh_test_pending_admission_deadline_ms == original_deadline);
    assert(mesh_test_queued_total == 0u);
    assert(mesh_test_drop_count == 0u);
    assert(app_node_comm_pending_delivery_count() == 0u);
    saved = mesh_test_pending_admission;
    saved_packet_id = mesh_test_pending_admission_packet_id;
    saved_attempt = mesh_test_pending_admission_attempt;

    atomic_store(&fake_now_ms, (int64_t)resumed_at);
    assert(mesh_test_tx_once() == MESH_TEST_RETRY_ADMISSION_MS);
    assert(mesh_test_pending_admission_valid);
    synthetic_assert_same(&mesh_test_pending_admission, &saved);
    assert(mesh_test_pending_admission_deadline_ms == original_deadline);
    assert(mesh_test_pending_admission_packet_id == saved_packet_id);
    assert(mesh_test_pending_admission_attempt == saved_attempt);
    assert(mesh_test_next_packet_id == saved_packet_id);
    assert(mesh_test_drop_count == 0u);
    assert(try_uplink_calls == 0u);

    assert(app_node_comm_start() == 0);
    (void)mesh_test_tx_once();
    assert(!mesh_test_pending_admission_valid);
    assert(mesh_test_queued_total == 4u);
    assert(mesh_test_drop_count == 0u);
    memcpy(admitted, mesh_test_deliveries, sizeof(admitted));
    assert(admitted[0].packet_id == saved_packet_id);
    assert(admitted[0].seq == saved.packet.seq);
    for (size_t i = 0u; i < 4u; i++) {
        assert(app_node_comm_service_deliveries() == 0);
        assert(try_uplink_calls == i + 1u);
        assert(try_uplink_envelopes[i].packet.seq == admitted[i].seq);
        assert(try_uplink_deadlines_ms[i] == (i == 0u ? original_deadline :
            resumed_at + UINT64_C(86400000)));
        if (i == 0u) {
            synthetic_assert_same(&try_uplink_envelopes[i], &saved);
            assert(try_uplink_envelopes[i].payload_len == saved.payload_len);
            assert(memcmp(try_uplink_envelopes[i].payload, saved.payload,
                          saved.payload_len) == 0);
            assert(try_uplink_envelopes[i].queued_at_valid == saved.queued_at_valid);
            assert(try_uplink_envelopes[i].queued_at_ms == saved.queued_at_ms);
        }
        assert(note_gateway_confirmed_envelope(&try_uplink_envelopes[i]) == 0);
        synthetic_assert_terminal(admitted[i].handle, admitted[i].packet_id,
                                  NODE_COMM_TERMINAL_DELIVERED, 1u, resumed_at);
    }
    mesh_test_reap_terminal_deliveries();
    assert(app_node_comm_pending_delivery_count() == 0u);
    assert(mesh_test_drop_count == 0u);

    /* A deferred source reaching its original end must be rejected on resume,
     * rather than receiving a new day because it never obtained a handle. */
    synthetic_reset_at(created_at);
    assert(app_node_comm_stop_preserving_queued()==0);
    assert(mesh_test_tx_once()==MESH_TEST_RETRY_ADMISSION_MS);
    atomic_store(&fake_now_ms,(int64_t)original_deadline);
    assert(app_node_comm_start()==0);
    (void)mesh_test_tx_once();
    assert(!mesh_test_pending_admission_valid);
    assert(mesh_test_drop_count==1u&&mesh_test_queued_total==0u);
    assert(mesh_test_next_packet_id==2u&&app_node_comm_pending_delivery_count()==0u);
    assert(try_uplink_calls==0u);
}

static void test_synthetic_lifetime_regressions(void)
{
    test_synthetic_queued_burst_survives_60s_and_requires_exact_receipts();
    test_synthetic_shared_policy_expires_unattempted_queue_truthfully();
    test_synthetic_deferred_admission_keeps_original_packet_and_deadline();
    test_synthetic_deadline_uses_shared_policy_and_remaining_age();
}
