/* Actual enumeration reservation and flood-burst code, with asynchronous
 * scan release at the radio boundary and real flood/activation state logic. */
#include "app_mesh_flood.h"
#include "app_mesh_report.h"
#include "discovery_assignment.h"
#include "firmware_state_machines.h"
#include "mesh_radio_timing.h"
#include "mesh_packet_age.h"
#include "protocol_rx_lifecycle.h"
#include "survey.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ROLE_ANCHOR 1
#define DEVICE_ROLE ROLE_ANCHOR
#define DEVICE_ID UINT64_C(0xa100)
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define IS_ENABLED(value) (value)
#define RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN 1
#define DWM3000_RECEIVE_ABORT_MESH_CONTROL 1
#define C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD 3u
#define WAKE_ADV_MS MESH_RADIO_WAKE_TRAIN_MS

struct app_mesh_command_orchestrator { int unused; };
enum mesh_radio_release_policy { MESH_RADIO_RELEASE_STANDBY };
struct app_mesh_c5_tx_authorization_token;
struct mesh_c5_flood_tx_context {
    bool *rf_started_out;
    struct app_mesh_tx_observation *observation;
    uint64_t absolute_deadline_ms;
    bool response_priority;
    bool atomic_gateway_control;
    uint8_t c5_tx_intent;
    const struct mesh_outbound *candidate;
};
static struct protocol_rx_downstream_activation mesh_enumeration_downstream_activation;
static bool mesh_enumeration_downstream_survey_follows;
static int mesh_c5_enumeration_relay_burst_count;
static uint32_t now_ms, release_at_ms, release_delay_ms;
static uint64_t uptime_epoch_ms;
static unsigned aborts, wakes, sends, idle_scan_ms, reserved_copy_gap_ms, reservations, releases;
static int radio_owner, wake_error, data_error;
static bool abort_requested, never_release, paused, defer_work;
static bool survey_control, survey_start;
static unsigned fail_copy, defer_after_copy;
static uint32_t send_handoff_ms;
static unsigned send_attempts;
static uint64_t copy_deadlines_ms[3];
static uint64_t copy_rf_ms[3];
static uint32_t copy_arrival_ms[3], copy_air_age_ms[3];

static bool uptime_deadline_reached(uint32_t now, uint32_t deadline)
{ return (int32_t)(now-deadline)>=0; }
static uint32_t k_uptime_get_32(void) { return now_ms; }
static uint64_t k_uptime_get(void) { return uptime_epoch_ms + now_ms; }
static bool mesh_transport_paused(void) { return paused; }
static int radio_guard_uwb_owner_client(void) { return radio_owner; }
static void atomic_inc(int *value) { assert(*value==0); (*value)++; reservations++; }
static void atomic_dec(int *value)
{ assert(*value==1); (*value)--; releases++; radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN; }
static int atomic_get(const int *value) { return *value; }
void dwm3000_driver_request_receive_abort(uint32_t reason)
{ assert(reason==DWM3000_RECEIVE_ABORT_MESH_CONTROL); aborts++; abort_requested=true; release_at_ms=now_ms+release_delay_ms; }
static void k_msleep(uint32_t ms)
{
    for (uint32_t i=0;i<ms;i++) {
        now_ms++;
        if (now_ms == 0u) uptime_epoch_ms += UINT64_C(1) << 32;
        if (mesh_c5_enumeration_relay_burst_count==0) {
            radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN;
            idle_scan_ms++;
        } else if (sends > 0u && sends < 3u) {
            assert(radio_owner!=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN);
            reserved_copy_gap_ms++;
        }
        if (abort_requested && !never_release && uptime_deadline_reached(now_ms,release_at_ms)) {
            radio_owner=0; abort_requested=false;
        }
    }
}
static void mesh_wait_until_ms(uint32_t due)
{ if (!uptime_deadline_reached(now_ms,due)) k_msleep(due-now_ms); }
void mesh_restart_role_scan(void)
{ if (mesh_c5_enumeration_relay_burst_count==0) radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN; }
static void status_debug_printf(const char *fmt, ...) { (void)fmt; }
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)

static uint32_t mesh_c5_flood_now_ms(void *ctx) { (void)ctx; return now_ms; }
static void mesh_c5_flood_sleep_until_ms(uint32_t due,void *ctx) { (void)ctx; mesh_wait_until_ms(due); }
static bool mesh_c5_flood_defer_active_cb(void *ctx) { (void)ctx; return defer_work; }
static bool mesh_c5_flood_quiet_cb(uint32_t sniff,void *ctx) { (void)sniff; (void)ctx; return true; }
static uint32_t mesh_c5_flood_random_u32(void *ctx) { (void)ctx; return 0u; }
static int mesh_send_outbound_with_release_on_channel_until(
    const struct mesh_outbound *out, const char *reason,
    enum mesh_radio_release_policy release_policy, bool *rf_started_out,
    uint64_t absolute_deadline_ms, struct app_mesh_tx_observation *observation,
    uint8_t forced_radio_channel,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    enum fw_c5_tx_intent intent)
{
    (void)reason; (void)forced_radio_channel;
    (void)authorization; (void)intent;
    assert(release_policy == MESH_RADIO_RELEASE_STANDBY);
    assert(mesh_c5_enumeration_relay_burst_count==1);
    assert(radio_owner!=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN);
    if (observation != NULL) memset(observation, 0, sizeof(*observation));
    if (rf_started_out != NULL) *rf_started_out = false;
    assert(send_attempts < 3u);
    copy_deadlines_ms[send_attempts++] = absolute_deadline_ms;
    if (data_error || (fail_copy != 0u && sends + 1u == fail_copy))
        return data_error != 0 ? data_error : -EIO;
    assert(sends < 3u);
    k_msleep(send_handoff_ms);
    /* Model the lower sender's physical cutoff after its fallible handoff.
     * This seam verifies that production flood code passes that same cutoff
     * through every copy; the DWM3000 fixture owns delayed-start enforcement. */
    if (absolute_deadline_ms != 0u && k_uptime_get() >= absolute_deadline_ms)
        return -ETIMEDOUT;
    copy_rf_ms[sends] = k_uptime_get();
    copy_air_age_ms[sends] = mesh_packet_age_at_air_arrival(
        out->packet.message_age_ms, out->queued_at_ms,
        out->queued_at_valid, now_ms, 1000u);
    sends++; k_msleep(1u);
    copy_arrival_ms[sends - 1u] = now_ms;
    if (rf_started_out != NULL) *rf_started_out = true;
    if (observation != NULL) {
        observation->rf_started = true;
        observation->rf_started_at_ms = copy_rf_ms[sends - 1u];
        observation->tx_completed = true;
        observation->tx_completed_at_ms = k_uptime_get();
        observation->result_at_ms = k_uptime_get();
    }
    if (defer_after_copy == sends) defer_work = true;
    return 0;
}
static int mesh_send_route_wake_train_with_duration(uint64_t peer,void *a,void *b,
    uint8_t purpose,const char *why,const struct mesh_outbound *out,void *c,
    enum fw_c5_tx_intent intent,uint32_t duration,void *d,void *e)
{
    (void)peer; (void)a; (void)b; (void)purpose; (void)why; (void)out;
    (void)c; (void)intent; (void)d; (void)e;
    assert(mesh_c5_enumeration_relay_burst_count==1);
    assert(radio_owner!=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN);
    wakes++; if (wake_error) return wake_error;
    k_msleep(duration); return 0;
}
static void mesh_wait_for_c5_control_followup_turnaround(uint8_t msg,const char *why)
{ (void)msg; (void)why; assert(mesh_c5_enumeration_relay_burst_count==1); k_msleep(1u); }
static bool mesh_c5_flood_destination_valid(const struct mesh_outbound *out)
{ return out->next_hop_id==MESH_BROADCAST_ID && out->packet.dst_id==MESH_BROADCAST_ID; }
static bool mesh_c5_gateway_enumeration_quick_copy_burst(const struct mesh_outbound *out)
{ (void)out; return false; }
static bool mesh_c5_flood_enumeration_identity(const struct mesh_outbound *out,
    enum discovery_assignment_phase *phase,uint32_t *epoch,uint32_t *budget,bool *survey)
{ (void)out; *phase=DISCOVERY_ASSIGNMENT_PHASE_CLAIM; *epoch=7u; *budget=60000u; *survey=false; return !survey_control; }
static bool mesh_c5_compact_scheduled_control(const struct mesh_outbound *out) { (void)out; return survey_control; }
static bool mesh_c5_compact_scheduled_activation(const struct mesh_outbound *out) { (void)out; return survey_control && survey_start; }
static bool mesh_c5_survey_start_assignment_epoch(const struct mesh_outbound *out,uint32_t *epoch)
{ (void)out; *epoch = 7u; return survey_control && survey_start; }
static bool mesh_c5_gateway_enumeration_claim(const struct mesh_outbound *out) { (void)out; return false; }
static int app_mesh_command_orchestrator_serialize_flood(const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,struct app_mesh_flood_result *result)
{ return app_mesh_flood_send_bounded(out,ops,result); }
static int app_mesh_command_orchestrator_send_flood(const struct app_mesh_command_orchestrator *owner,
    const struct app_mesh_flood_ops *ops,struct app_mesh_flood_result *result)
{ (void)owner; return app_mesh_flood_send_bounded(((struct mesh_c5_flood_tx_context *)ops->ctx)->candidate,ops,result); }

#include "enumeration_scan_production.inc"

static struct mesh_outbound reset_fixture(uint8_t msg)
{
    now_ms=1000u; release_delay_ms=5u; release_at_ms=0u;
    uptime_epoch_ms=0u;
    aborts=wakes=sends=idle_scan_ms=reserved_copy_gap_ms=reservations=releases=0u;
    radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN;
    wake_error=data_error=mesh_c5_enumeration_relay_burst_count=0;
    abort_requested=never_release=paused=defer_work=false;
    survey_control=survey_start=false;
    fail_copy=defer_after_copy=send_handoff_ms=0u;
    send_attempts=0u;
    memset(copy_deadlines_ms, 0, sizeof(copy_deadlines_ms));
    memset(copy_rf_ms, 0, sizeof(copy_rf_ms));
    memset(copy_arrival_ms, 0, sizeof(copy_arrival_ms));
    memset(copy_air_age_ms, 0, sizeof(copy_air_age_ms));
    protocol_rx_downstream_activation_init(&mesh_enumeration_downstream_activation);
    mesh_enumeration_downstream_survey_follows=false;
    return (struct mesh_outbound){.packet={.msg_type=msg,.src_id=DEVICE_ID,
        .dst_id=MESH_BROADCAST_ID,.ttl=FLOOD_EPOCH_GLOBAL_TTL,.session_id=7u,.seq=9u},
        .next_hop_id=MESH_BROADCAST_ID,.radio_channel=UWB_CHANNEL_WAKE_CONTACT,
        .route_wave_start_ms=now_ms,.flood_retry_count=2u};
}
static int run_flood(struct mesh_outbound *out,struct app_mesh_flood_result *result)
{ return mesh_send_c5_flood_now_until(out,1u,"harness",true,false,true,NULL,result,NULL,0u,NULL,FW_C5_TX_INTENT_BACKGROUND); }
static void assert_balanced(void)
{ assert(mesh_c5_enumeration_relay_burst_count==0); assert(reservations==releases); }

static void test_asynchronous_scan_release_and_reserved_copy_gaps(void)
{
    struct mesh_outbound out=reset_fixture(MSG_COMMAND);
    struct app_mesh_flood_result result;
    assert(run_flood(&out,&result)==0);
    assert(wakes==1u && sends==3u && result.sent_count==3u);
    assert(aborts==1u && reservations==1u); /* One handoff through all copies. */
    assert(idle_scan_ms==0u);
    assert(reserved_copy_gap_ms>=2u*MESH_ENUMERATION_RELAY_COPY_GUARD_MS);
    assert(mesh_enumeration_downstream_activation.activated);
    assert_balanced();
    k_msleep(10u); assert(idle_scan_ms==10u); /* Scan resumes after the burst. */
}
static void test_timeout_and_shutdown_never_send_or_leak_reservation(void)
{
    for (unsigned shutdown=0;shutdown<2;shutdown++) {
        struct mesh_outbound out=reset_fixture(MSG_COMMAND);
        struct app_mesh_flood_result result;
        never_release=true; paused=shutdown!=0;
        uint32_t start=now_ms;
        assert(run_flood(&out,&result)==(shutdown ? -ESHUTDOWN : -EBUSY));
        assert(now_ms-start==(shutdown ? 0u : MESH_CONTROL_RX_HANDOFF_TIMEOUT_MS));
        assert(wakes==0 && sends==0 && !mesh_enumeration_downstream_activation.activated);
        assert_balanced();
        k_msleep(10u); assert(idle_scan_ms==10u); /* Retry backoff leaves scan free. */
    }
}
static void test_physical_failure_balances_reservation(void)
{
    for (unsigned data=0;data<2;data++) {
        struct mesh_outbound out=reset_fixture(MSG_COMMAND);
        struct app_mesh_flood_result result;
        if (data) data_error=-EIO; else wake_error=-EIO;
        assert(run_flood(&out,&result)==-EIO);
        assert(sends==0u && !mesh_enumeration_downstream_activation.activated);
        assert_balanced();
    }
}
static void test_deferred_burst_releases_reservation_for_scan(void)
{
    struct mesh_outbound out=reset_fixture(MSG_COMMAND);
    struct app_mesh_flood_result result;
    defer_work=true;
    assert(run_flood(&out,&result)==-EAGAIN);
    assert(wakes==0u && sends==0u && reservations==1u);
    assert_balanced();
    k_msleep(10u); assert(idle_scan_ms==10u);
}
static void test_expired_first_wake_slot_cannot_send_later_activation_copies(void)
{
    struct mesh_outbound out=reset_fixture(MSG_GATEWAY_ROUTE_ADV);
    struct app_mesh_flood_result result;
    now_ms=out.route_wave_start_ms+MESH_GATEWAY_ROUTE_ACTIVATION_ENVELOPE_MS+
        MESH_GATEWAY_ROUTE_ADV_COPY_SPACING_MS;
    assert(run_flood(&out,&result)==-ETIMEDOUT);
    assert(wakes==0u && sends==0u && result.sent_count==0u);
    assert(!mesh_enumeration_downstream_activation.activated);
    assert_balanced();
}

static struct mesh_outbound survey_fixture(bool start)
{
    struct mesh_outbound out = reset_fixture(MSG_COMMAND);
    survey_control = true;
    survey_start = start;
    mesh_enumeration_downstream_survey_follows = true;
    assert(protocol_rx_downstream_activation_mark(
        &mesh_enumeration_downstream_activation,
        PROTOCOL_RX_OPERATION_ENUMERATION, 7u, now_ms, now_ms + 60000u));
    send_handoff_ms = 13u; /* Final radio arbitration/SPI work is nonzero. */
    return out;
}

static int survey_flood_until(const struct mesh_outbound *out,
                              struct app_mesh_flood_result *result,
                              struct app_mesh_tx_observation *observation,
                              uint64_t absolute_deadline_ms)
{
    return mesh_send_c5_flood_now_until(out,
        C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD, "survey-clock", false,
        false, true, NULL, result, NULL, absolute_deadline_ms, observation,
        FW_C5_TX_INTENT_BACKGROUND);
}

static int survey_flood(const struct mesh_outbound *out,
                        struct app_mesh_flood_result *result,
                        struct app_mesh_tx_observation *observation)
{
    return survey_flood_until(out, result, observation, 0u);
}

static void test_survey_first_rf_cutoff_survives_handoff_pauses(void)
{
    for (unsigned start = 0u; start < 2u; start++) {
        for (unsigned at_sender = 0u; at_sender < 2u; at_sender++) {
            struct mesh_outbound out = survey_fixture(start != 0u);
            struct app_mesh_flood_result result = {0};
            struct app_mesh_tx_observation observation = {0};
            const uint64_t deadline_ms = k_uptime_get() + 20u;

            if (at_sender != 0u) send_handoff_ms = 25u;
            else release_delay_ms = 25u;
            assert(survey_flood_until(&out, &result, &observation,
                                      deadline_ms) == -ETIMEDOUT);
            assert(wakes == 0u && sends == 0u && result.sent_count == 0u);
            assert(!observation.rf_started && !observation.tx_completed);
            assert(!observation.message_origin_valid && !out.queued_at_valid);
            for (unsigned copy = 0u; copy < send_attempts; copy++) {
                assert(copy_deadlines_ms[copy] == deadline_ms);
            }
            assert_balanced();
        }
    }
}

static void test_survey_later_copies_keep_original_cutoff_and_clock(void)
{
    for (unsigned start = 0u; start < 2u; start++) {
        struct mesh_outbound out = survey_fixture(start != 0u);
        struct app_mesh_flood_result result = {0};
        struct app_mesh_tx_observation observation = {0};
        const uint64_t deadline_ms = k_uptime_get() + 25u;

        assert(survey_flood_until(&out, &result, &observation,
                                  deadline_ms) == -ETIMEDOUT);
        assert(wakes == 0u && sends == 1u && result.sent_count == 1u);
        assert(send_attempts == 2u && k_uptime_get() >= deadline_ms);
        assert(copy_deadlines_ms[0] == deadline_ms);
        assert(copy_deadlines_ms[1] == deadline_ms);
        assert(copy_rf_ms[0] < deadline_ms);
        assert(observation.rf_started && observation.tx_completed);
        assert(observation.rf_started_at_ms == copy_rf_ms[0]);
        assert(observation.tx_completed_at_ms == copy_arrival_ms[0]);
        assert(observation.message_origin_valid);
        assert(observation.message_origin_at_ms < deadline_ms);
        assert(copy_arrival_ms[0] - copy_air_age_ms[0] ==
               observation.message_origin_at_ms);
        assert(!out.queued_at_valid);
        assert_balanced();
    }
}

static void test_survey_pre_rf_deferral_keeps_original_cutoff(void)
{
    for (unsigned start = 0u; start < 2u; start++) {
        struct mesh_outbound out = survey_fixture(start != 0u);
        struct app_mesh_flood_result result = {0};
        struct app_mesh_tx_observation observation = {0};
        const uint64_t deadline_ms = k_uptime_get() + 20u;

        data_error = -EAGAIN;
        assert(survey_flood_until(&out, &result, &observation,
                                  deadline_ms) == -EAGAIN);
        assert(send_attempts == 1u && copy_deadlines_ms[0] == deadline_ms);
        assert(sends == 0u && !observation.message_origin_valid);
        assert(!observation.rf_started && !out.queued_at_valid);
        assert_balanced();

        data_error = 0;
        k_msleep(25u);
        assert(survey_flood_until(&out, &result, &observation,
                                  deadline_ms) == -ETIMEDOUT);
        assert(send_attempts == 1u && sends == 0u);
        assert(!observation.rf_started && !observation.message_origin_valid);
        assert(!out.queued_at_valid);
        assert_balanced();
    }
}

static void test_survey_copies_keep_one_wire_clock_and_first_rf_observation(void)
{
    for (unsigned start = 0u; start < 2u; start++) {
        for (unsigned retained_origin = 0u; retained_origin < 2u; retained_origin++) {
            struct mesh_outbound out = survey_fixture(start != 0u);
            struct app_mesh_flood_result result = {0};
            struct app_mesh_tx_observation observation = {0};
            uint32_t origin_ms = now_ms;
            if (retained_origin != 0u) {
                /* A relay or a retried source already owns an older origin. */
                out.queued_at_valid = true;
                out.queued_at_ms = now_ms - 311u;
                out.packet.message_age_ms = 7u;
                origin_ms = out.queued_at_ms - out.packet.message_age_ms;
            }
            const struct mesh_outbound before = out;
            assert(survey_flood(&out, &result, &observation) == 0);
            assert(wakes == 0u && sends == 3u && result.sent_count == 3u);
            assert(observation.rf_started && observation.tx_completed);
            assert(observation.rf_started_at_ms == copy_rf_ms[0]);
            assert(observation.tx_completed_at_ms == copy_arrival_ms[2]);
            assert(copy_rf_ms[2] > copy_rf_ms[0] + SURVEY_RADIO_GUARD_MS);
            if (retained_origin == 0u) origin_ms = (uint32_t)copy_rf_ms[0] - send_handoff_ms;
            assert(observation.message_origin_valid);
            assert(observation.message_origin_at_ms == origin_ms);
            for (unsigned copy = 0u; copy < sends; copy++) {
                assert(copy_arrival_ms[copy] - copy_air_age_ms[copy] == origin_ms);
            }
            assert(memcmp(&out, &before, sizeof(out)) == 0);
            assert_balanced();
        }
    }
}

static void test_later_failed_or_deferred_copy_retains_earlier_rf_evidence(void)
{
    for (unsigned defer = 0u; defer < 2u; defer++) {
        for (unsigned completed = 1u; completed < 3u; completed++) {
            struct mesh_outbound out = survey_fixture(false);
            struct app_mesh_flood_result result = {0};
            struct app_mesh_tx_observation observation = {0};
            if (defer != 0u) defer_after_copy = completed;
            else fail_copy = completed + 1u;
            assert(survey_flood(&out, &result, &observation) ==
                (defer != 0u ? -EAGAIN : -EIO));
            assert(sends == completed && result.sent_count == completed);
            assert(observation.rf_started && observation.tx_completed);
            assert(observation.rf_started_at_ms == copy_rf_ms[0]);
            assert(observation.tx_completed_at_ms == copy_arrival_ms[completed - 1u]);
            assert_balanced();
        }
    }
}

static void test_pre_rf_deferral_does_not_freeze_the_origin(void)
{
    struct mesh_outbound out = survey_fixture(true);
    struct app_mesh_flood_result result = {0};
    struct app_mesh_tx_observation observation = {0};
    defer_work = true;
    assert(survey_flood(&out, &result, &observation) == -EAGAIN);
    assert(!observation.rf_started && !observation.message_origin_valid);
    assert(sends == 0u && !out.queued_at_valid);
    assert_balanced();

    now_ms += 500u;
    defer_work = false;
    /* Clock retention must work even when this relay's caller does not ask
     * for transport telemetry. Its downstream still needs aligned copies. */
    assert(survey_flood(&out, &result, NULL) == 0);
    assert(sends == 3u);
    uint32_t origin_ms = (uint32_t)copy_rf_ms[0] - send_handoff_ms;
    assert(origin_ms >= 1500u);
    for (unsigned copy = 0u; copy < sends; copy++) {
        assert(copy_arrival_ms[copy] - copy_air_age_ms[copy] == origin_ms);
    }
    assert_balanced();
}

static void test_message_origin_expands_queue_stamp_after_uptime_rollover(void)
{
    for (unsigned retained = 0u; retained < 2u; retained++) {
        struct mesh_outbound out = survey_fixture(false);
        struct app_mesh_flood_result result = {0};
        struct app_mesh_tx_observation observation = {0};
        uptime_epoch_ms = UINT64_C(1) << 32;
        now_ms = 100u;
        uint64_t origin_ms = uptime_epoch_ms + now_ms + release_delay_ms;
        if (retained != 0u) {
            out.queued_at_valid = true;
            out.queued_at_ms = UINT32_MAX - 99u;
            out.packet.message_age_ms = 7u;
            origin_ms = uptime_epoch_ms - 107u;
        }
        assert(survey_flood(&out, &result, &observation) == 0);
        assert(sends == 3u && observation.message_origin_valid);
        assert(observation.message_origin_at_ms == origin_ms);
        assert(observation.rf_started_at_ms == copy_rf_ms[0]);
        assert(observation.tx_completed_at_ms == uptime_epoch_ms + copy_arrival_ms[2]);
        for (unsigned copy = 0u; copy < sends; copy++) {
            assert(copy_arrival_ms[copy] - copy_air_age_ms[copy] == (uint32_t)origin_ms);
        }
        assert_balanced();
    }
}
int main(void)
{
    test_asynchronous_scan_release_and_reserved_copy_gaps();
    test_timeout_and_shutdown_never_send_or_leak_reservation();
    test_physical_failure_balances_reservation();
    test_deferred_burst_releases_reservation_for_scan();
    test_expired_first_wake_slot_cannot_send_later_activation_copies();
    test_survey_first_rf_cutoff_survives_handoff_pauses();
    test_survey_later_copies_keep_original_cutoff_and_clock();
    test_survey_pre_rf_deferral_keeps_original_cutoff();
    test_survey_copies_keep_one_wire_clock_and_first_rf_observation();
    test_later_failed_or_deferred_copy_retains_earlier_rf_evidence();
    test_pre_rf_deferral_does_not_freeze_the_origin();
    test_message_origin_expands_queue_stamp_after_uptime_rollover();
    puts("production enumeration scan handoff harness passed");
    return 0;
}
