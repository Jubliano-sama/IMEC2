#include "app_mesh_c5_priority.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_report.h"
#include "app_node_comm.h"
#include "app_radio_guard.h"
#include "app_radio_low_power_policy.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "mesh.h"
#include "protocol.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <stdarg.h>
#include <string.h>

LOG_MODULE_REGISTER(production_seam_click_handoff, LOG_LEVEL_INF);

/* Match the exact production ordering asserted by app_anchor.c. */
#define HARNESS_REPAIR_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(0)
#define HARNESS_CLICK_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(1)
#define HARNESS_REPAIR_WORKQUEUE_STACK_SIZE 8576u
#define HARNESS_CLICK_WORKQUEUE_STACK_SIZE 7168u
#define ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS 20u
#define ANCHOR_CLICK_RANGE_REPORT_FRAGMENT_CAPACITY 2u
#define ROLE_ANCHOR 2
#define APP_ANCHOR_CLICK_HANDOFF_SEAM_ONLY 1

BUILD_ASSERT(HARNESS_REPAIR_WORKQUEUE_PRIORITY <
                 HARNESS_CLICK_WORKQUEUE_PRIORITY,
             "digest repair must preempt the anchor click workqueue");
BUILD_ASSERT(APP_NODE_COMM_MAX_DELIVERIES == 6u,
             "production facade capacity changed");

/* This seam links app_node_comm.c without the mesh report owner; the real
 * Channel-9 cadence release lives there and has no surface to observe here. */
void app_mesh_report_close_channel9_idle_parent(const char *reason)
{
    (void)reason;
}

static const uint64_t clicker_id = UINT64_C(0x1111222233334444);
static const uint64_t anchor_id = UINT64_C(0x2222333344445555);
static const uint64_t gateway_id = UINT64_C(0x9999888877776666);
static const uint64_t repair_peer_id = UINT64_C(0x777788889999AAAA);
static const uint64_t repair_origin_id = UINT64_C(0x6666777788889999);
static const uint32_t click_event_seq = 0x10203040u;
static const uint8_t click_attempt_index = 2u;

static struct k_work_q anchor_uwb_scan_work_q;
static struct k_work_q repair_work_q;
K_THREAD_STACK_DEFINE(anchor_uwb_scan_work_q_stack,
                      HARNESS_CLICK_WORKQUEUE_STACK_SIZE);
K_THREAD_STACK_DEFINE(repair_work_q_stack,
                      HARNESS_REPAIR_WORKQUEUE_STACK_SIZE);

static struct k_work_delayable anchor_uwb_scan_work;
static struct k_work anchor_click_handoff_work;
static struct k_work click_queue_barrier_work;
static struct k_work_delayable repair_work;

K_SEM_DEFINE(click_queue_barrier_entered, 0, 1);
K_SEM_DEFINE(click_queue_barrier_release, 0, 1);
K_SEM_DEFINE(click_rf_entered, 0, 1);
K_SEM_DEFINE(click_rf_release, 0, 1);
K_SEM_DEFINE(click_work_completed, 0, 1);
K_SEM_DEFINE(repair_attempt_completed, 0, 1);

struct anchor_pending_click_handoff {
    struct uwb_wake_claim_frame claim;
    struct radio_guard_uwb_lease radio_lease;
    int64_t received_at_ms;
    uint8_t link_quality;
    bool active;
};

static struct anchor_pending_click_handoff anchor_pending_click_handoff;
static struct k_spinlock anchor_pending_click_handoff_lock;
static struct k_spinlock click_window_lock;
static struct uwb_anchor_session anchor_uwb_session;
static uint32_t anchor_uwb_scan_interval_ms = 100u;
static bool anchor_click_window_busy;
static bool anchor_scan_recovery_gap_requested;
static bool anchor_uwb_busy;

static struct app_node_comm_reservation_lease click_reservation;
static struct app_node_comm_reservation_lease filler_reservations[4];
static struct app_node_comm_reservation_lease protocol_reservation;
static struct mesh_outbound click_outbound;
static uint32_t click_delivery_handle;
static uint32_t click_delivery_generation;
static uint64_t click_owner_generation;
static uint64_t reserved_clicker_id;
static uint32_t reserved_event_seq;
static uint8_t reserved_attempt_index;
static bool click_reservation_committed;
static struct uwb_wake_claim_frame injected_competing_claim;
static bool inject_competing_claim;
static bool competing_claim_ignored;

static struct mesh_pending_tx repair_pending;
static struct app_mesh_ch9_ack_table repair_ack_table;
static struct app_mesh_c5_tx_authorization_token repair_authorization;
static struct mesh_outbound repair_candidate;
static uint32_t repair_work_runs;
static uint32_t repair_deferrals;
static uint32_t repair_rf_starts;
static uint32_t repair_authorization_rejections;
static int repair_last_result;
static uint32_t watchdog_stop_calls;
static uint32_t node_comm_reschedules;
static uint32_t click_scan_reschedules;
static uint32_t click_phase_claims;

void status_debug_printf(const char *fmt, ...);

static bool anchor_claim_collection_candidate_allowed(
    const struct uwb_anchor_epoch *epoch,
    const struct uwb_wake_claim_frame *claim,
    bool admitted_handoff_identity_frozen,
    bool *same_epoch);

static bool anchor_click_window_active(void)
{
    k_spinlock_key_t key = k_spin_lock(&click_window_lock);
    bool active = anchor_click_window_busy;

    k_spin_unlock(&click_window_lock, key);
    return active;
}

static void anchor_click_window_set_active(bool active)
{
    k_spinlock_key_t key = k_spin_lock(&click_window_lock);

    anchor_click_window_busy = active;
    k_spin_unlock(&click_window_lock, key);
}

static void anchor_set_uwb_busy(bool busy)
{
    anchor_uwb_busy = busy;
}

static void anchor_note_uwb_awake_since(int64_t start_ms,
                                        uint32_t already_counted_us)
{
    ARG_UNUSED(start_ms);
    ARG_UNUSED(already_counted_us);
}

static void anchor_click_event_abort_if_needed(const char *reason)
{
    ARG_UNUSED(reason);
}

int app_anchor_click_event_runtime_claim(
    const struct uwb_wake_claim_frame *claim,
    uint32_t now_ms,
    struct fw_transition *transition)
{
    ARG_UNUSED(now_ms);
    ARG_UNUSED(transition);
    zassert_not_null(claim);
    click_phase_claims++;
    return 0;
}

static int anchor_enter_low_power(enum app_radio_low_power_mode mode,
                                  const char *reason)
{
    ARG_UNUSED(mode);
    ARG_UNUSED(reason);
    return 0;
}

static int anchor_uwb_scan_schedule_ms(uint32_t delay_ms)
{
    ARG_UNUSED(delay_ms);
    click_scan_reschedules++;
    k_sem_give(&click_work_completed);
    return 1;
}

static void click_report_build(const struct uwb_wake_claim_frame *claim)
{
    memset(&click_outbound, 0, sizeof(click_outbound));
    click_outbound.packet = (struct proto_packet) {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = anchor_id,
        .dst_id = gateway_id,
        .session_id = proto_click_report_session_id(claim->clicker_id,
                                                    claim->click_event_id),
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 0u,
    };
    click_outbound.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    click_outbound.next_hop_id = gateway_id;
}

static bool anchor_handle_uwb_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms,
    bool admitted_handoff_identity_frozen,
    uint32_t *retained_sleep_us,
    bool *deferred_mesh_rx_queued)
{
    int ret;

    ARG_UNUSED(link_quality);
    ARG_UNUSED(received_at_ms);
    zassert_not_null(claim);
    zassert_true(radio_guard_uwb_busy(),
                 "click worker reached RF without its transferred lease");
    zassert_equal(reserved_clicker_id, claim->clicker_id);
    zassert_equal(reserved_event_seq, claim->click_event_id);
    zassert_equal(reserved_attempt_index, claim->attempt_index);
    zassert_not_equal(click_reservation.token, 0u);
    zassert_false(click_reservation_committed);

    anchor_uwb_session.epoch = (struct uwb_anchor_epoch) {
        .active = true,
        .network_id = claim->network_id,
        .clicker_id = claim->clicker_id,
        .click_event_id = claim->click_event_id,
        .attempt_index = claim->attempt_index,
        .priority_id = claim->priority_id,
        .nonce = claim->nonce,
        .flags = claim->flags,
    };
    if (inject_competing_claim) {
        bool same_epoch = true;

        zassert_true(admitted_handoff_identity_frozen,
                     "handoff did not freeze its reserved click identity");
        zassert_true(injected_competing_claim.priority_id > claim->priority_id,
                     "test competitor is not higher priority");
        zassert_false(anchor_claim_collection_candidate_allowed(
                          &anchor_uwb_session.epoch,
                          &injected_competing_claim,
                          admitted_handoff_identity_frozen,
                          &same_epoch),
                      "higher-priority claim replaced reserved handoff identity");
        zassert_false(same_epoch,
                      "higher-priority test claim unexpectedly matched handoff");
        competing_claim_ignored = true;
    }

    click_report_build(claim);
    ret = app_node_comm_commit_reliable_uplink_reservation(
        &click_reservation,
        &click_outbound,
        (uint64_t)k_uptime_get() + 60000u,
        claim->click_event_id,
        &click_delivery_handle);
    zassert_ok(ret, "composed six-slot click commit failed: %d", ret);
    click_reservation_committed = true;
    zassert_not_equal(click_delivery_handle, 0u);
    zassert_ok(app_node_comm_delivery_generation(
        click_delivery_handle, &click_delivery_generation));
    zassert_not_equal(click_delivery_generation, 0u);

    if (retained_sleep_us != NULL) {
        *retained_sleep_us = 0u;
    }
    if (deferred_mesh_rx_queued != NULL) {
        *deferred_mesh_rx_queued = false;
    }
    k_sem_give(&click_rf_entered);
    zassert_ok(k_sem_take(&click_rf_release, K_SECONDS(2)),
               "test did not release the click RF barrier");
    return true;
}

/* Compile and invoke the exact production handoff/worker implementation. */
#include "app_anchor_radio.inc"

void status_debug_printf(const char *fmt, ...)
{
    ARG_UNUSED(fmt);
}

void app_watchdog_stop_feeding(void)
{
    watchdog_stop_calls++;
}

int dwm3000_driver_configure_wake_mode(void)
{
    return 0;
}

void uwb_anchor_abort_epoch(struct uwb_anchor_session *session)
{
    ARG_UNUSED(session);
}

bool uwb_anchor_epoch_matches(const struct uwb_anchor_epoch *epoch,
                              uint32_t network_id,
                              uint64_t candidate_clicker_id,
                              uint32_t click_event_id,
                              uint8_t attempt_index,
                              uint64_t nonce)
{
    return epoch != NULL && epoch->active &&
           epoch->network_id == network_id &&
           epoch->clicker_id == candidate_clicker_id &&
           epoch->click_event_id == click_event_id &&
           epoch->attempt_index == attempt_index &&
           epoch->nonce == nonce;
}

bool mesh_anchor_connected_radio_active(void)
{
    return false;
}

void mesh_stop_role_scan(void)
{
}

void mesh_submit_queued_rx(void)
{
}

void report_tx_schedule(uint32_t delay_ms)
{
    ARG_UNUSED(delay_ms);
}

int mesh_range_report_batch_reserve(uint64_t requested_clicker_id,
                                    uint32_t event_seq,
                                    uint8_t attempt_index)
{
    int ret;

    zassert_equal(click_reservation.token, 0u,
                  "click handoff replaced an existing reservation");
    click_owner_generation = requested_clicker_id ^
        ((uint64_t)event_seq << 8) ^ attempt_index;
    if (click_owner_generation == 0u) {
        click_owner_generation = 1u;
    }
    /*
     * The monolithic production range-batch implementation is deliberately
     * outside this small link.  Adapt its real callback boundary to the real
     * six-slot facade so the test can compose both owners without copying
     * either policy.  README.md records this remaining production-link gap.
     */
    ret = app_node_comm_reserve_reliable_uplinks(
        click_owner_generation, 1u, &click_reservation, 1u);
    if (ret == 0) {
        reserved_clicker_id = requested_clicker_id;
        reserved_event_seq = event_seq;
        reserved_attempt_index = attempt_index;
    }
    return ret;
}

int mesh_range_report_batch_reserve_capacity(uint64_t requested_clicker_id,
                                             uint32_t event_seq,
                                             uint8_t attempt_index,
                                             uint8_t fragment_capacity)
{
    ARG_UNUSED(fragment_capacity);
    return mesh_range_report_batch_reserve(requested_clicker_id,
                                           event_seq,
                                           attempt_index);
}

void mesh_range_report_batch_abort(uint64_t requested_clicker_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index)
{
    zassert_equal(reserved_clicker_id, requested_clicker_id);
    zassert_equal(reserved_event_seq, event_seq);
    zassert_equal(reserved_attempt_index, attempt_index);
    if (!click_reservation_committed && click_reservation.token != 0u) {
        zassert_ok(app_node_comm_cancel_reliable_uplink_reservation(
            &click_reservation));
        memset(&click_reservation, 0, sizeof(click_reservation));
    }
}

bool mesh_id_is_unicast(uint64_t node_id)
{
    return node_id != 0u && node_id != MESH_BROADCAST_ID;
}

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks)
{
    ARG_UNUSED(callbacks);
    return 0;
}

int mesh_route_work_reschedule(struct k_work_delayable *work,
                               uint32_t delay_ms)
{
    ARG_UNUSED(work);
    ARG_UNUSED(delay_ms);
    node_comm_reschedules++;
    /* Keep the committed facade record owned but do not run an RF backend. */
    return 1;
}

bool mesh_transport_quiesced(void)
{
    return true;
}

bool mesh_rx_response_active(void)
{
    return false;
}

bool dwm3000_driver_receive_abort_pending(void)
{
    return false;
}

void dwm3000_driver_request_receive_abort(uint32_t owner_mask)
{
    ARG_UNUSED(owner_mask);
}

int mesh_transport_pause_preserving_queued(void)
{
    return 0;
}

void mesh_transport_resume(void)
{
}

void mesh_restart_role_scan(void)
{
}

int mesh_try_send_c5_flood_view(const struct app_mesh_outbound_view *view,
                                uint8_t purpose,
                                const char *reason,
                                bool send_wake_train,
                                struct app_mesh_tx_observation *observation,
                                uint32_t *scheduled_retry_delay_ms)
{
    ARG_UNUSED(view);
    ARG_UNUSED(purpose);
    ARG_UNUSED(reason);
    ARG_UNUSED(send_wake_train);
    if (scheduled_retry_delay_ms != NULL) {
        *scheduled_retry_delay_ms = 0u;
    }
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    return -EAGAIN;
}

int mesh_try_send_control_response_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation)
{
    ARG_UNUSED(view);
    ARG_UNUSED(reason);
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    return -EAGAIN;
}

int mesh_try_send_reliable_uplink_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    struct app_mesh_tx_observation *observation,
    uint32_t *scheduled_retry_delay_ms)
{
    ARG_UNUSED(view);
    ARG_UNUSED(reason);
    if (observation != NULL) {
        memset(observation, 0, sizeof(*observation));
    }
    if (scheduled_retry_delay_ms != NULL) {
        *scheduled_retry_delay_ms = 0u;
    }
    return -EAGAIN;
}

int mesh_request_reliable_uplink_cancel(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t delivery_handle,
    uint32_t delivery_generation,
    uint32_t request_token)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(semantic_digest);
    ARG_UNUSED(delivery_handle);
    ARG_UNUSED(delivery_generation);
    ARG_UNUSED(request_token);
    return -EAGAIN;
}

int mesh_take_reliable_uplink_cancel_result(uint32_t delivery_handle,
                                            uint32_t request_token,
                                            int *cancel_result)
{
    ARG_UNUSED(delivery_handle);
    ARG_UNUSED(request_token);
    ARG_UNUSED(cancel_result);
    return -ENOENT;
}

void app_node_comm_gateway_route_refresh_pause(uint32_t now_ms)
{
    ARG_UNUSED(now_ms);
}

void app_node_comm_gateway_route_refresh_resume(uint32_t now_ms)
{
    ARG_UNUSED(now_ms);
}

int app_node_comm_gateway_route_refresh_request(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation)
{
    ARG_UNUSED(delay_ms);
    ARG_UNUSED(reason);
    ARG_UNUSED(forced);
    ARG_UNUSED(correlation);
    return 0;
}

int app_node_comm_gateway_route_refresh_request_bounded(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation,
    uint32_t timeout_ms)
{
    ARG_UNUSED(timeout_ms);
    return app_node_comm_gateway_route_refresh_request(
        delay_ms, reason, forced, correlation);
}

static void inert_scan_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
}

static void click_queue_barrier_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    k_sem_give(&click_queue_barrier_entered);
    zassert_ok(k_sem_take(&click_queue_barrier_release, K_SECONDS(2)),
               "test did not release the click queue barrier");
}

static void repair_work_handler(struct k_work *work)
{
    const struct app_mesh_ch9_ack_batch *batch;
    struct radio_guard_uwb_lease lease = {0};
    int ret;

    ARG_UNUSED(work);
    repair_work_runs++;
    batch = app_mesh_ch9_ack_table_get_peer(&repair_ack_table,
                                             repair_peer_id);
    if (!app_mesh_ch9_c5_repair_allowed(&repair_authorization,
                                         &repair_pending,
                                         true,
                                         batch,
                                         &repair_candidate)) {
        repair_authorization_rejections++;
        repair_last_result = -ESTALE;
        k_sem_give(&repair_attempt_completed);
        return;
    }

    ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_MESH_TX,
                                "production seam forwarded ACK repair",
                                &lease);
    repair_last_result = ret;
    if (ret == -EBUSY) {
        repair_deferrals++;
    } else {
        zassert_ok(ret, "repair radio claim failed unexpectedly: %d", ret);
        repair_rf_starts++;
        repair_authorization.valid = false;
        zassert_ok(radio_guard_uwb_release_begin(&lease));
        zassert_ok(radio_guard_uwb_release_finish(&lease, 0));
    }
    k_sem_give(&repair_attempt_completed);
}

static void forwarded_ack_repair_token_build(void)
{
    struct app_mesh_ch9_ack_batch *batch = &repair_ack_table.batches[0];

    memset(&repair_pending, 0, sizeof(repair_pending));
    memset(&repair_ack_table, 0, sizeof(repair_ack_table));
    memset(&repair_authorization, 0, sizeof(repair_authorization));
    memset(&repair_candidate, 0, sizeof(repair_candidate));

    repair_pending.packet = (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = repair_origin_id,
        .dst_id = gateway_id,
        .session_id = 0xA100u,
        .seq = 0x42u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 3u,
    };
    repair_pending.payload[0] = 0xA1u;
    repair_pending.payload[1] = 0xB2u;
    repair_pending.payload[2] = 0xC3u;
    repair_pending.payload_len = 3u;
    repair_pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    repair_pending.next_hop_id = gateway_id;
    repair_pending.state = MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD;
    repair_pending.gateway_ack_forward_pending = true;
    repair_pending.gateway_ack_forward_next_hop_id = repair_peer_id;

    batch->valid = true;
    batch->preserve_payload = true;
    batch->count = 1u;
    batch->owner = APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE;
    batch->peer_id = repair_peer_id;
    batch->template_ack.packet = (struct proto_packet) {
        .msg_type = MSG_GATEWAY_ACK,
        .flags = FLAG_GATEWAY_ACK,
        .src_id = gateway_id,
        .dst_id = repair_origin_id,
        .session_id = 0xB200u,
        .seq = 0x51u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = 2u,
    };
    batch->template_ack.payload[0] = 0xD4u;
    batch->template_ack.payload[1] = 0xE5u;
    batch->template_ack.payload_len = 2u;
    batch->template_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    batch->template_ack.next_hop_id = repair_peer_id;

    zassert_true(app_mesh_ch9_c5_repair_authorization_capture(
        &repair_authorization,
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR,
        &repair_pending,
        true,
        batch,
        repair_peer_id),
        "could not mint the multi-hop forwarded-ACK repair capability");

    repair_candidate.packet = (struct proto_packet) {
        .msg_type = MSG_MESH_EVENT_ACCEPT,
        .src_id = anchor_id,
        .dst_id = repair_peer_id,
        .session_id = 0xC300u,
        .seq = 0x61u,
        .ttl = 1u,
        .payload_len = 0u,
    };
    repair_candidate.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    repair_candidate.next_hop_id = repair_peer_id;
}

static void repair_attempt_run(void)
{
    struct k_work_sync sync;
    int ret = k_work_reschedule_for_queue(&repair_work_q,
                                           &repair_work,
                                           K_NO_WAIT);

    zassert_true(ret >= 0, "repair reschedule rejected: %d", ret);
    zassert_ok(k_sem_take(&repair_attempt_completed, K_SECONDS(2)),
               "repair work did not complete");
    zassert_true(k_work_flush_delayable(&repair_work, &sync),
                 "repair work was not submitted");
}

static void queues_start(void)
{
    static const struct k_work_queue_config repair_config = {
        .name = "imec2_repair_q",
        .no_yield = false,
    };
    static const struct k_work_queue_config click_config = {
        .name = "imec2_click_q",
        .no_yield = false,
    };

    k_work_queue_start(&repair_work_q,
                       repair_work_q_stack,
                       K_THREAD_STACK_SIZEOF(repair_work_q_stack),
                       HARNESS_REPAIR_WORKQUEUE_PRIORITY,
                       &repair_config);
    k_work_queue_start(&anchor_uwb_scan_work_q,
                       anchor_uwb_scan_work_q_stack,
                       K_THREAD_STACK_SIZEOF(anchor_uwb_scan_work_q_stack),
                       HARNESS_CLICK_WORKQUEUE_PRIORITY,
                       &click_config);
    zassert_equal(k_thread_priority_get(&repair_work_q.thread),
                  HARNESS_REPAIR_WORKQUEUE_PRIORITY);
    zassert_equal(k_thread_priority_get(&anchor_uwb_scan_work_q.thread),
                  HARNESS_CLICK_WORKQUEUE_PRIORITY);
}

static void fixture_reset(void)
{
    memset(&anchor_pending_click_handoff, 0,
           sizeof(anchor_pending_click_handoff));
    memset(&click_reservation, 0, sizeof(click_reservation));
    memset(filler_reservations, 0, sizeof(filler_reservations));
    memset(&protocol_reservation, 0, sizeof(protocol_reservation));
    memset(&click_outbound, 0, sizeof(click_outbound));
    click_delivery_handle = 0u;
    click_delivery_generation = 0u;
    click_owner_generation = 0u;
    reserved_clicker_id = 0u;
    reserved_event_seq = 0u;
    reserved_attempt_index = 0u;
    click_reservation_committed = false;
    memset(&injected_competing_claim, 0, sizeof(injected_competing_claim));
    inject_competing_claim = false;
    competing_claim_ignored = false;
    anchor_click_window_busy = false;
    anchor_scan_recovery_gap_requested = false;
    anchor_uwb_busy = false;
    repair_work_runs = 0u;
    repair_deferrals = 0u;
    repair_rf_starts = 0u;
    repair_authorization_rejections = 0u;
    repair_last_result = 0;
    watchdog_stop_calls = 0u;
    node_comm_reschedules = 0u;
    click_scan_reschedules = 0u;
    click_phase_claims = 0u;
    k_sem_reset(&click_queue_barrier_entered);
    k_sem_reset(&click_queue_barrier_release);
    k_sem_reset(&click_rf_entered);
    k_sem_reset(&click_rf_release);
    k_sem_reset(&click_work_completed);
    k_sem_reset(&repair_attempt_completed);
    k_work_init_delayable(&anchor_uwb_scan_work, inert_scan_work_handler);
    k_work_init(&anchor_click_handoff_work,
                anchor_click_handoff_work_handler);
    k_work_init(&click_queue_barrier_work, click_queue_barrier_handler);
    k_work_init_delayable(&repair_work, repair_work_handler);
    zassert_ok(app_node_comm_init(NULL));
}

ZTEST(production_seam_click_handoff,
      test_digest_repair_cannot_cross_transferred_click_lease)
{
    struct app_node_comm_reservation_lease overflow = {0};
    struct uwb_wake_claim_frame claim = {
        .network_id = 0x12345678u,
        .clicker_id = clicker_id,
        .click_event_id = click_event_seq,
        .attempt_index = click_attempt_index,
        .priority_id = UINT64_C(0x0102030405060708),
        .nonce = UINT64_C(0x1122334455667788),
        .flags = FLAG_COUNT_AS_CLICK,
    };
    struct k_work_sync sync;
    size_t click_unused = 0u;
    size_t repair_unused = 0u;
    uint64_t started_cycles;
    uint64_t elapsed_ns;
    int ret;

    fixture_reset();
    queues_start();
    started_cycles = k_cycle_get_64();
    forwarded_ack_repair_token_build();

    /* Four ordinary owners leave one composed click slot plus the reserve. */
    zassert_ok(app_node_comm_reserve_reliable_uplinks(
        UINT64_C(0xF100), ARRAY_SIZE(filler_reservations),
        filler_reservations, ARRAY_SIZE(filler_reservations)));
    zassert_ok(app_node_comm_reserve_protocol_response(
        UINT64_C(0xF200), &protocol_reservation));

    zassert_equal(k_work_submit_to_queue(&anchor_uwb_scan_work_q,
                                         &click_queue_barrier_work),
                  1);
    zassert_ok(k_sem_take(&click_queue_barrier_entered, K_SECONDS(2)),
               "click queue barrier did not start");
    ret = k_work_reschedule_for_queue(&anchor_uwb_scan_work_q,
                                      &anchor_uwb_scan_work,
                                      K_SECONDS(10));
    zassert_true(ret >= 0, "scan scheduling failed: %d", ret);
    zassert_true(k_work_delayable_is_pending(&anchor_uwb_scan_work));

    /* mesh_handoff_anchor_click_claim() publishes this outer preemption bit
     * immediately before it invokes the real anchor callback below. */
    anchor_click_window_set_active(true);
    zassert_true(anchor_handle_mesh_click_wake_claim(
        &claim, 211u, k_uptime_get()),
        "real anchor click handoff rejected the claim");
    zassert_equal(click_phase_claims, 1u,
                  "handoff did not claim its phase before capacity");
    zassert_true(anchor_click_handoff_pending());
    zassert_true(anchor_click_window_active());
    zassert_true(radio_guard_uwb_busy());
    zassert_equal(radio_guard_uwb_phase(), RADIO_GUARD_UWB_ACTIVE);
    zassert_equal(anchor_pending_click_handoff.radio_lease.client,
                  RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK);
    zassert_not_equal(anchor_pending_click_handoff.radio_lease.generation,
                      0u);
    zassert_not_equal(click_reservation.token, 0u);
    zassert_false(k_work_delayable_is_pending(&anchor_uwb_scan_work),
                  "handoff did not cancel the lower-priority scan");
    zassert_equal(app_node_comm_reserve_reliable_uplinks(
        UINT64_C(0xF300), 1u, &overflow, 1u), -ENOSPC,
        "ordinary work consumed the protocol-reserved sixth slot");

    /* This claim wins ordinary arbitration, but arrives after the first
     * identity has already reserved durable click-report custody. */
    injected_competing_claim = claim;
    injected_competing_claim.clicker_id = UINT64_C(0x5555666677778888);
    injected_competing_claim.click_event_id = click_event_seq + 1u;
    injected_competing_claim.attempt_index = click_attempt_index + 1u;
    injected_competing_claim.priority_id = claim.priority_id + 1u;
    injected_competing_claim.nonce = UINT64_C(0x8877665544332211);
    inject_competing_claim = true;

    /* The priority-0 repair is runnable, but the transferred click lease wins. */
    repair_attempt_run();
    zassert_equal(repair_last_result, -EBUSY);
    zassert_equal(repair_deferrals, 1u);
    zassert_equal(repair_rf_starts, 0u);
    zassert_true(repair_authorization.valid,
                 "busy repair consumed its digest capability");

    k_sem_give(&click_queue_barrier_release);
    zassert_true(k_work_flush(&click_queue_barrier_work, &sync));
    zassert_ok(k_sem_take(&click_rf_entered, K_SECONDS(2)),
               "click worker did not execute the transferred lease");
    zassert_true(anchor_uwb_busy);
    zassert_true(radio_guard_uwb_busy());
    zassert_true(competing_claim_ignored,
                 "post-handoff higher-priority claim was not rejected");
    zassert_true(click_reservation_committed);
    zassert_equal(app_node_comm_delivery_handle_state(click_delivery_handle),
                  1);
    zassert_equal(app_node_comm_pending_delivery_count(), 1u);
    zassert_equal(reserved_clicker_id, claim.clicker_id);
    zassert_equal(reserved_event_seq, claim.click_event_id);
    zassert_equal(reserved_attempt_index, claim.attempt_index);
    zassert_equal(click_outbound.packet.session_id,
                  proto_click_report_session_id(claim.clicker_id,
                                               claim.click_event_id));

    /* It remains blocked for the complete physical click interval. */
    repair_attempt_run();
    zassert_equal(repair_last_result, -EBUSY);
    zassert_equal(repair_deferrals, 2u);
    zassert_equal(repair_rf_starts, 0u);
    zassert_true(repair_authorization.valid);
    zassert_equal(app_node_comm_delivery_handle_state(click_delivery_handle),
                  1,
                  "repair disturbed click custody");

    k_sem_give(&click_rf_release);
    zassert_ok(k_sem_take(&click_work_completed, K_SECONDS(2)),
               "click worker did not release and rearm");
    zassert_true(k_work_flush(&anchor_click_handoff_work, &sync));
    zassert_false(anchor_click_handoff_pending());
    zassert_false(anchor_click_window_active());
    zassert_false(radio_guard_uwb_busy());
    zassert_equal(radio_guard_uwb_phase(), RADIO_GUARD_UWB_IDLE);

    repair_attempt_run();
    zassert_ok(repair_last_result);
    zassert_equal(repair_rf_starts, 1u,
                  "repair must touch RF exactly once after click release");
    zassert_false(repair_authorization.valid,
                  "successful repair did not consume its exact capability");
    zassert_false(radio_guard_uwb_busy());

    /* A queued duplicate observes the consumed digest capability and cannot
     * start a second physical repair after ownership becomes available. */
    repair_attempt_run();
    zassert_equal(repair_last_result, -ESTALE);
    zassert_equal(repair_authorization_rejections, 1u);
    zassert_equal(repair_rf_starts, 1u,
                  "consumed repair capability started RF twice");
    zassert_equal(watchdog_stop_calls, 0u,
                  "a guard/parking failure stopped watchdog feeding");
    zassert_equal(app_node_comm_delivery_handle_state(click_delivery_handle),
                  1,
                  "click identity was lost after repair");
    zassert_not_equal(click_delivery_generation, 0u);
    zassert_equal(app_node_comm_cancel_reliable_uplink_reservation(
                      &click_reservation),
                  -ESTALE,
                  "a consumed click capability remained cancellable");
    zassert_ok(app_node_comm_cancel_protocol_response_reservation(
        &protocol_reservation));
    for (size_t i = 0u; i < ARRAY_SIZE(filler_reservations); i++) {
        zassert_ok(app_node_comm_cancel_reliable_uplink_reservation(
            &filler_reservations[i]));
    }

    elapsed_ns = k_cyc_to_ns_floor64(k_cycle_get_64() - started_cycles);
    zassert_ok(k_thread_stack_space_get(&anchor_uwb_scan_work_q.thread,
                                        &click_unused));
    zassert_ok(k_thread_stack_space_get(&repair_work_q.thread,
                                        &repair_unused));
    printk("PRODUCTION_SEAM_COST elapsed_us=%llu click_stack_used=%u/%u repair_stack_used=%u/%u node_comm_reschedules=%u repair_runs=%u\n",
           (unsigned long long)(elapsed_ns / 1000u),
           (unsigned int)(HARNESS_CLICK_WORKQUEUE_STACK_SIZE - click_unused),
           (unsigned int)HARNESS_CLICK_WORKQUEUE_STACK_SIZE,
           (unsigned int)(HARNESS_REPAIR_WORKQUEUE_STACK_SIZE - repair_unused),
           (unsigned int)HARNESS_REPAIR_WORKQUEUE_STACK_SIZE,
           node_comm_reschedules,
           repair_work_runs);
}

ZTEST_SUITE(production_seam_click_handoff,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL);
