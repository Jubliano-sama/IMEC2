/* Execute the production probe loop, semaphore backoff and rebroadcast
 * worker. Only kernel scheduling and complete RF operations are faked;
 * retry accounting, route installation and request/reply encoding are real. */
#include "app_mesh_direct_gateway_retry.h"
#include "app_mesh_route_request_policy.h"
#include "mesh_relay.h"
#include "uwb.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ROLE_ANCHOR 1
#define ROLE_GATEWAY 2
#define DEVICE_ROLE ROLE_ANCHOR
#define DEVICE_ID UINT64_C(0xa100)
#define CHILD_ID UINT64_C(0xa101)
#define GATEWAY_ID UINT64_C(0x9000)
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ 0
#define IS_ENABLED(config) (config)
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(ms) (ms)
#define ARG_UNUSED(value) ((void)(value))
#define C5_CONTACT_PURPOSE_ROUTE_SOLICIT 1
#define FW_C5_TX_INTENT_BACKGROUND 1
#define LOG_ERR(...) status_debug_printf(__VA_ARGS__)
#define LOG_WRN(...) status_debug_printf(__VA_ARGS__)

struct k_work { int unused; };
struct k_work_delayable { struct k_work work; };
struct test_mutex { bool held; };
typedef int k_spinlock_key_t;
static struct test_mutex mesh_direct_gateway_probe_scratch_lock;
static struct test_mutex mesh_route_request_action_scratch_lock;
static struct test_mutex mesh_send_scratch_lock;
static int mesh_rx_msgq, mesh_click_preempt_request_lock;
static unsigned mesh_click_preempt_route_wake;
static uint32_t now_ms;
static unsigned lock_count;
static bool radio_owned, transport_paused;

static bool uptime_deadline_reached(uint32_t now, uint32_t deadline)
{ return (int32_t)(now - deadline) >= 0; }
static uint32_t uptime_ms_until_deadline(uint32_t now, uint32_t deadline)
{ return uptime_deadline_reached(now, deadline) ? 1u : deadline - now; }
static bool mesh_id_is_unicast(uint64_t id) { return id != MESH_BROADCAST_ID; }
static uint32_t k_uptime_get_32(void) { return now_ms; }
static void status_debug_printf(const char *format, ...) { (void)format; }
static void status_debug_note(const char *note) { (void)note; }

#include "direct_probe_preempt_helpers.inc"

static struct mesh_click_preempt_request mesh_click_preempt_request;
static struct mesh_relay mesh_runtime;
static struct mesh_outbound mesh_direct_gateway_probe_scratch;
static struct mesh_outbound mesh_route_request_action_tx;
static struct mesh_outbound mesh_route_request_reply_tx;
static struct k_work_delayable mesh_route_request_action_work;
static uint64_t mesh_route_request_action_previous_hop_id;
static uint32_t mesh_route_request_action_reply_deadline_ms;
static bool mesh_route_request_action_pending;

/* A bounded script represents complete hardware calls, including failures
 * before RF. Time is never advanced by a zero-cost successful RF operation. */
static int send_results[128];
static uint8_t send_attempts[128];
static unsigned send_count, send_script_len, rf_count, sem_waits, plain_sleeps;
static unsigned service_count, reschedules, reply_count, wake_count, control_count;
static unsigned retry_notes, consumed_attempts;
static uint32_t scheduled_at_ms, serviced_at_ms;
static uint32_t click_at_ms, click_lifetime_ms;
static bool click_scheduled;
static uint16_t next_sequence;

static int k_mutex_lock(struct test_mutex *mutex, int timeout)
{
    if (mutex->held) {
        assert(timeout == K_NO_WAIT);
        return -EBUSY;
    }
    mutex->held = true;
    lock_count++;
    return 0;
}
static void k_mutex_unlock(struct test_mutex *mutex)
{
    assert(mutex->held && lock_count > 0u);
    mutex->held = false;
    lock_count--;
}
static k_spinlock_key_t k_spin_lock(int *lock) { assert(!*lock); *lock = 1; return 0; }
static void k_spin_unlock(int *lock, k_spinlock_key_t key)
{ (void)key; assert(*lock); *lock = 0; }
static unsigned k_msgq_num_used_get(const int *queue) { return (unsigned)*queue; }

static void publish_click(void)
{
    click_scheduled = false;
    mesh_click_preempt_request.state = MESH_CLICK_PREEMPT_REQUEST_QUEUED;
    mesh_click_preempt_request.bridge_deadline_ms = now_ms + click_lifetime_ms;
    mesh_click_preempt_route_wake++;
}

static void advance_time(uint32_t duration_ms, bool interruptible)
{
    uint32_t finish_ms = now_ms + duration_ms;

    if (click_scheduled && (uint32_t)(click_at_ms - now_ms) <= duration_ms) {
        now_ms = click_at_ms;
        publish_click();
        if (interruptible) {
            return;
        }
    }
    now_ms = finish_ms;
}

static int k_sem_take(unsigned *sem, uint32_t timeout_ms)
{
    assert(sem == &mesh_click_preempt_route_wake && timeout_ms != 0u);
    assert(!radio_owned && !mesh_send_scratch_lock.held);
    sem_waits++;
    if (*sem == 0u) {
        advance_time(timeout_ms, true);
    }
    if (*sem != 0u) {
        (*sem)--;
        return 0;
    }
    return -EAGAIN;
}

static void k_msleep(uint32_t delay_ms)
{
    assert(!radio_owned && !mesh_send_scratch_lock.held);
    plain_sleeps++;
    advance_time(delay_ms, false);
}
static uint32_t sys_rand32_get(void) { return 0u; }
static uint32_t nonzero_uptime_session_id(void) { return now_ms == 0u ? 1u : now_ms; }
static uint16_t mesh_next_event_control_seq(void) { return ++next_sequence; }
static int mesh_errno_from_proto(int ret) { assert(ret != PROTO_OK); return -EINVAL; }
static bool mesh_transport_paused(void) { return transport_paused; }
static void mesh_restart_role_scan(void) { assert(!radio_owned); }

static int mesh_send_direct_gateway_probe_and_wait(const struct mesh_outbound *probe,
                                                   const char *reason,
                                                   uint8_t attempt)
{
    (void)reason;
    assert(mesh_direct_gateway_probe_scratch_lock.held && !radio_owned);
    assert(probe->packet.msg_type == MSG_GATEWAY_ROUTE_REQ);
    assert(probe->packet.src_id == DEVICE_ID && probe->packet.dst_id == GATEWAY_ID);
    assert(probe->packet.session_id != 0u && probe->next_hop_id == GATEWAY_ID);
    assert(send_count < sizeof(send_results) / sizeof(send_results[0]));
    int ret = send_count < send_script_len ? send_results[send_count] : 0;
    send_attempts[send_count++] = attempt;
    assert(k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER) == 0);
    if (ret != -EBUSY) {
        radio_owned = true;
        rf_count++;
    }
    advance_time(ret == -EBUSY ? 1u : 2u, false);
    radio_owned = false;
    k_mutex_unlock(&mesh_send_scratch_lock);
    return ret;
}

/* Observe the real retry policy's decisions without replacing that policy. */
static int tracked_retry_note(struct app_mesh_direct_gateway_retry_state *state,
                             enum app_mesh_direct_gateway_attempt_outcome outcome,
                             uint32_t random_value,
                             struct app_mesh_direct_gateway_retry_decision *decision)
{
    int ret = app_mesh_direct_gateway_retry_note(state, outcome, random_value, decision);
    assert(ret == 0);
    retry_notes++;
    consumed_attempts += decision->attempt_consumed ? 1u : 0u;
    return ret;
}
#define app_mesh_direct_gateway_retry_note tracked_retry_note
#include "direct_probe_preempt_loop.inc"
#undef app_mesh_direct_gateway_retry_note

static int mesh_reschedule_owned_work(struct k_work_delayable *work,
                                      uint32_t delay_ms, const char *reason)
{
    (void)reason;
    assert(work == &mesh_route_request_action_work);
    assert(mesh_route_request_action_pending);
    reschedules++;
    scheduled_at_ms = now_ms + delay_ms;
    return 0;
}

static bool mesh_click_preempt_service_queued_route_owned(void)
{
    /* This is the custody callback that the old route worker starved. It
     * may acquire any scratch itself, so no outer mutex may remain held. */
    assert(lock_count == 0u && !radio_owned);
    assert(!mesh_direct_gateway_probe_scratch_lock.held);
    assert(!mesh_route_request_action_scratch_lock.held);
    assert(mesh_click_preempt_boundary_requested());
    service_count++;
    serviced_at_ms = now_ms;
    mesh_click_preempt_request.state = MESH_CLICK_PREEMPT_REQUEST_COMPLETE;
    return true;
}

static bool mesh_send_route_reply_outbound_action(const struct mesh_outbound *reply,
                                                 bool backup, uint64_t backup_hop,
                                                 const char *reason,
                                                 const uint32_t *owner_deadline_ms)
{
    (void)reason;
    assert(owner_deadline_ms != NULL &&
           !uptime_deadline_reached(now_ms, *owner_deadline_ms));
    assert(!backup && backup_hop == 0u);
    assert(reply->packet.msg_type == MSG_ROUTE_REPLY && reply->next_hop_id == CHILD_ID);
    assert(!mesh_click_preempt_boundary_requested());
    reply_count++;
    advance_time(2u, false);
    return true;
}
static int mesh_send_route_wake_train(uint64_t target, const struct mesh_outbound *request,
                                      bool *embedded, ...)
{
    assert(target == MESH_BROADCAST_ID && request == &mesh_route_request_action_tx);
    assert(!mesh_click_preempt_boundary_requested());
    wake_count++;
    *embedded = false;
    advance_time(5u, false);
    return 0;
}
static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    (void)reason;
    assert(out == &mesh_route_request_action_tx);
    assert(!mesh_click_preempt_boundary_requested());
    control_count++;
    advance_time(2u, false);
    return 0;
}
static int mesh_listen_for_route_reply(uint64_t target, const char *reason,
                                      uint32_t window_ms,
                                      const struct mesh_route_capture_identity *identity,
                                      bool *captured)
{
    (void)reason;
    assert(target == GATEWAY_ID && identity->session_id != 0u && window_ms > 0u);
    advance_time(window_ms, false);
    *captured = false;
    return -ETIMEDOUT;
}
#include "direct_probe_preempt_worker.inc"

static void reset_fixture(void)
{
    assert(lock_count == 0u && !radio_owned);
    now_ms = 1000u;
    mesh_rx_msgq = 0;
    mesh_click_preempt_route_wake = 0u;
    memset(&mesh_click_preempt_request, 0, sizeof(mesh_click_preempt_request));
    memset(send_results, 0, sizeof(send_results));
    memset(send_attempts, 0, sizeof(send_attempts));
    send_count = send_script_len = rf_count = sem_waits = plain_sleeps = 0u;
    service_count = reschedules = reply_count = wake_count = control_count = 0u;
    retry_notes = consumed_attempts = 0u;
    scheduled_at_ms = serviced_at_ms = 0u;
    click_scheduled = transport_paused = false;
    click_lifetime_ms = 245u;
    mesh_route_request_action_pending = false;
    next_sequence = 0u;
    mesh_relay_init(&mesh_runtime, MESH_RELAY_ROLE_ANCHOR, DEVICE_ID, GATEWAY_ID, 1u);
}

static void busy_streak(unsigned count)
{
    assert(count < sizeof(send_results) / sizeof(send_results[0]));
    send_script_len = count;
    for (unsigned index = 0u; index < count; index++) {
        send_results[index] = -EBUSY;
    }
}
static void click_after(uint32_t delay_ms)
{ click_at_ms = now_ms + delay_ms; click_scheduled = true; }
static int probe(bool install)
{
    int ret = mesh_try_direct_gateway_route_probe(GATEWAY_ID, "test", install,
        APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE, 17u);
    assert(lock_count == 0u && !radio_owned);
    return ret;
}

static void test_no_click_preserves_real_attempt_accounting(void)
{
    reset_fixture();
    busy_streak(64u);
    assert(probe(true) == 0);
    assert(send_count == 65u && rf_count == 1u);
    assert(consumed_attempts == 1u && retry_notes == 65u);
    for (unsigned index = 0u; index < send_count; index++) {
        assert(send_attempts[index] == 1u);
    }
    assert(now_ms == 1000u + 64u * 31u + 2u);
    assert(sem_waits == 64u && plain_sleeps == 0u && service_count == 0u);
    assert(route_selected(&mesh_runtime.upstream) != NULL);

    reset_fixture();
    busy_streak(5u);
    send_results[5] = -ETIMEDOUT;
    send_results[6] = -EBUSY;
    send_results[7] = -ETIMEDOUT;
    send_results[8] = -ETIMEDOUT;
    send_script_len = 9u;
    assert(probe(true) == -ETIMEDOUT);
    assert(rf_count == 3u && consumed_attempts == 3u && send_count == 9u);
    assert(send_attempts[5] == 1u && send_attempts[6] == 2u);
    assert(send_attempts[7] == 2u && send_attempts[8] == 3u);
    assert(route_selected(&mesh_runtime.upstream) == NULL);
}

static void test_live_click_cancels_before_rf_and_after_atomic_send(void)
{
    reset_fixture();
    publish_click();
    assert(probe(true) == -ECANCELED);
    assert(send_count == 0u && retry_notes == 0u && now_ms == 1000u);

    const int outcomes[] = {-EBUSY, -ETIMEDOUT, 0};
    for (size_t index = 0u; index < sizeof(outcomes) / sizeof(outcomes[0]); index++) {
        reset_fixture();
        send_results[0] = outcomes[index];
        send_script_len = 1u;
        click_after(1u);
        assert(probe(true) == -ECANCELED);
        assert(send_count == 1u && retry_notes == 0u && sem_waits == 0u);
        assert(rf_count == (outcomes[index] == -EBUSY ? 0u : 1u));
        assert(route_selected(&mesh_runtime.upstream) == NULL);
        assert(service_count == 0u); /* The caller still owns custody. */
    }
}

static void test_backoff_interrupts_at_click_edge_including_uptime_wrap(void)
{
    const uint32_t starts[] = {1000u, UINT32_MAX - 20u};
    for (size_t index = 0u; index < sizeof(starts) / sizeof(starts[0]); index++) {
        for (unsigned preceding_busy = 0u; preceding_busy <= 64u; preceding_busy += 64u) {
            for (uint32_t click_delay = 2u; click_delay < 31u; click_delay++) {
                reset_fixture();
                now_ms = starts[index];
                busy_streak(100u);
                click_after(preceding_busy * 31u + click_delay);
                assert(probe(true) == -ECANCELED);
                assert(now_ms == starts[index] + preceding_busy * 31u + click_delay);
                assert(send_count == preceding_busy + 1u && rf_count == 0u);
                assert(consumed_attempts == 0u && sem_waits == preceding_busy + 1u);
                assert(plain_sleeps == 0u);
                assert(mesh_click_preempt_boundary_requested());
            }
        }
    }
}

static void test_stale_wake_and_expired_click_do_not_shorten_backoff(void)
{
    for (unsigned expired = 0u; expired < 2u; expired++) {
        reset_fixture();
        busy_streak(1u);
        if (expired) {
            click_lifetime_ms = 0u;
            publish_click();
        } else {
            mesh_click_preempt_route_wake = 1u;
        }
        assert(probe(false) == 0);
        assert(now_ms == 1033u && send_count == 2u && rf_count == 1u);
        assert(consumed_attempts == 1u && sem_waits == 2u && plain_sleeps == 0u);
        assert(route_selected(&mesh_runtime.upstream) == NULL);
    }
}

static void prepare_rebroadcast(uint32_t lifetime_ms)
{
    static struct mesh_relay child;
    static struct mesh_relay_result received;
    struct mesh_outbound request;
    mesh_relay_init(&child, MESH_RELAY_ROLE_ANCHOR, CHILD_ID, GATEWAY_ID, 1u);
    assert(mesh_relay_prepare_route_request(&child, GATEWAY_ID, now_ms,
        17u, &request) == PROTO_OK);
    now_ms = child.route_discovery.next_request_ms;
    assert(mesh_relay_prepare_route_request(&child, GATEWAY_ID, now_ms,
        17u, &request) == PROTO_OK);
    assert(mesh_relay_handle_rx(&mesh_runtime, &request.packet,
        request.payload, request.payload_len, CHILD_ID, 90u, now_ms,
        &received) == PROTO_OK);
    assert(received.actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ);
    mesh_route_request_action_tx = received.route_request;
    mesh_route_request_action_tx.earliest_tx_ms = now_ms;
    mesh_route_request_action_tx.earliest_tx_valid = true;
    mesh_route_request_action_tx.packet.message_age_ms = 19u;
    mesh_route_request_action_previous_hop_id = CHILD_ID;
    mesh_route_request_action_reply_deadline_ms = now_ms + lifetime_ms;
    mesh_route_request_action_pending = true;
}

static void assert_same_request(const struct mesh_outbound *before)
{
    const struct mesh_outbound *after = &mesh_route_request_action_tx;
#define SAME(field) assert(after->field == before->field)
    SAME(packet.src_id); SAME(packet.dst_id); SAME(packet.session_id);
    SAME(packet.message_age_ms); SAME(packet.seq); SAME(packet.flags);
    SAME(packet.ttl); SAME(packet.msg_type); SAME(packet.payload_len);
    SAME(payload_len); SAME(next_hop_id); SAME(ingress_previous_hop_id);
    SAME(queued_at_ms); SAME(queued_at_valid); SAME(earliest_tx_ms);
    SAME(earliest_tx_valid); SAME(flood_retry_count); SAME(radio_channel);
    SAME(handoff_owner_generation);
#undef SAME
    assert(memcmp(after->payload, before->payload, before->payload_len) == 0);
    assert(mesh_route_request_action_previous_hop_id == CHILD_ID);
}

static void test_worker_releases_scratch_retains_exact_action_and_resumes(void)
{
    reset_fixture();
    prepare_rebroadcast(10000u);
    const struct mesh_outbound before = mesh_route_request_action_tx;
    const uint32_t deadline = mesh_route_request_action_reply_deadline_ms;
    const uint32_t started = now_ms;
    busy_streak(100u);
    click_after(17u);
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(lock_count == 0u && !radio_owned && service_count == 1u);
    assert(serviced_at_ms == started + 17u && send_count == 1u && rf_count == 0u);
    assert(mesh_route_request_action_pending && reschedules == 1u);
    assert(scheduled_at_ms == serviced_at_ms + REPORT_TX_RETRY_DELAY_MS);
    assert(mesh_route_request_action_reply_deadline_ms == deadline);
    assert_same_request(&before);
    assert(wake_count == 0u && control_count == 0u && reply_count == 0u);

    now_ms = scheduled_at_ms;
    send_script_len = send_count; /* The busy owner is gone. */
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(!mesh_route_request_action_pending && reply_count == 1u);
    assert(send_count == 2u && send_attempts[1] == 1u && rf_count == 1u);
    assert(consumed_attempts == 1u && service_count == 1u && lock_count == 0u);
    assert_same_request(&before);
    assert(mesh_route_request_action_reply_deadline_ms == deadline);
}

static void test_retained_action_expires_without_new_rf_or_deadline_extension(void)
{
    reset_fixture();
    prepare_rebroadcast(100u);
    const struct mesh_outbound before = mesh_route_request_action_tx;
    const uint32_t deadline = mesh_route_request_action_reply_deadline_ms;
    busy_streak(100u);
    click_after(17u);
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(mesh_route_request_action_pending && service_count == 1u);
    now_ms = deadline;
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(!mesh_route_request_action_pending && send_count == 1u);
    assert(rf_count == 0u && wake_count == 0u && reply_count == 0u);
    assert_same_request(&before);
    assert(mesh_route_request_action_reply_deadline_ms == deadline);
}

static void test_worker_no_click_and_paused_paths_keep_existing_behavior(void)
{
    reset_fixture();
    prepare_rebroadcast(10000u);
    transport_paused = true;
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(mesh_route_request_action_pending && send_count == 0u && lock_count == 0u);
    transport_paused = false;
    mesh_route_request_action_tx.earliest_tx_ms = now_ms + 50u;
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(mesh_route_request_action_pending && scheduled_at_ms == now_ms + 50u);
    now_ms = scheduled_at_ms;
    mesh_route_request_action_work_handler(&mesh_route_request_action_work.work);
    assert(!mesh_route_request_action_pending && send_count == 1u);
    assert(reply_count == 1u && service_count == 0u && lock_count == 0u);
}

int main(void)
{
    test_no_click_preserves_real_attempt_accounting();
    test_live_click_cancels_before_rf_and_after_atomic_send();
    test_backoff_interrupts_at_click_edge_including_uptime_wrap();
    test_stale_wake_and_expired_click_do_not_shorten_backoff();
    test_worker_releases_scratch_retains_exact_action_and_resumes();
    test_retained_action_expires_without_new_rf_or_deadline_extension();
    test_worker_no_click_and_paused_paths_keep_existing_behavior();
    puts("production direct-probe click preemption: 7 scenarios passed");
    return 0;
}
