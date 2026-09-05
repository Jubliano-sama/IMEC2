/* Actual enumeration reservation and flood-burst code, with asynchronous
 * scan release at the radio boundary and real flood/activation state logic. */
#include "app_mesh_flood.h"
#include "discovery_assignment.h"
#include "firmware_state_machines.h"
#include "mesh_radio_timing.h"
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
struct app_mesh_tx_observation { bool sent; };
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
static unsigned aborts, wakes, sends, idle_scan_ms, reserved_copy_gap_ms, reservations, releases;
static int radio_owner, wake_error, data_error;
static bool abort_requested, never_release, paused, defer_work;

static bool uptime_deadline_reached(uint32_t now, uint32_t deadline)
{ return (int32_t)(now-deadline)>=0; }
static uint32_t k_uptime_get_32(void) { return now_ms; }
static uint64_t k_uptime_get(void) { return now_ms; }
static bool mesh_transport_paused(void) { return paused; }
static int radio_guard_uwb_owner_client(void) { return radio_owner; }
static void atomic_inc(int *value) { assert(*value==0); (*value)++; reservations++; }
static void atomic_dec(int *value)
{ assert(*value==1); (*value)--; releases++; radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN; }
static int atomic_get(const int *value) { return *value; }
static void dwm3000_driver_request_receive_abort(int reason)
{ assert(reason==DWM3000_RECEIVE_ABORT_MESH_CONTROL); aborts++; abort_requested=true; release_at_ms=now_ms+release_delay_ms; }
static void k_msleep(uint32_t ms)
{
    for (uint32_t i=0;i<ms;i++) {
        now_ms++;
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
static void mesh_restart_role_scan(void)
{ if (mesh_c5_enumeration_relay_burst_count==0) radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN; }
static void status_debug_printf(const char *fmt, ...) { (void)fmt; }
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)

static uint32_t mesh_c5_flood_now_ms(void *ctx) { (void)ctx; return now_ms; }
static void mesh_c5_flood_sleep_until_ms(uint32_t due,void *ctx) { (void)ctx; mesh_wait_until_ms(due); }
static bool mesh_c5_flood_defer_active_cb(void *ctx) { (void)ctx; return defer_work; }
static bool mesh_c5_flood_quiet_cb(uint32_t sniff,void *ctx) { (void)sniff; (void)ctx; return true; }
static uint32_t mesh_c5_flood_random_u32(void *ctx) { (void)ctx; return 0u; }
static int mesh_c5_flood_send_cb(const struct mesh_outbound *out,void *ctx)
{
    struct mesh_c5_flood_tx_context *flood=ctx;
    (void)out;
    assert(mesh_c5_enumeration_relay_burst_count==1);
    assert(radio_owner!=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN);
    if (data_error) return data_error;
    sends++; k_msleep(1u);
    if (flood->rf_started_out) *flood->rf_started_out=true;
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
{ (void)out; *phase=DISCOVERY_ASSIGNMENT_PHASE_CLAIM; *epoch=7u; *budget=60000u; *survey=false; return true; }
static bool mesh_c5_compact_scheduled_control(const struct mesh_outbound *out) { (void)out; return false; }
static bool mesh_c5_compact_scheduled_activation(const struct mesh_outbound *out) { (void)out; return false; }
static bool mesh_c5_survey_start_assignment_epoch(const struct mesh_outbound *out,uint32_t *epoch)
{ (void)out; (void)epoch; return false; }
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
    aborts=wakes=sends=idle_scan_ms=reserved_copy_gap_ms=reservations=releases=0u;
    radio_owner=RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN;
    wake_error=data_error=mesh_c5_enumeration_relay_burst_count=0;
    abort_requested=never_release=paused=defer_work=false;
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
int main(void)
{
    test_asynchronous_scan_release_and_reserved_copy_gaps();
    test_timeout_and_shutdown_never_send_or_leak_reservation();
    test_physical_failure_balances_reservation();
    test_deferred_burst_releases_reservation_for_scan();
    test_expired_first_wake_slot_cannot_send_later_activation_copies();
    puts("production enumeration scan handoff harness passed");
    return 0;
}
