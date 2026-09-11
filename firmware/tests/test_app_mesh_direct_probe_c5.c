/* Run the actual cold-route builder, RF sender, exact ACK wait and gateway
 * C5 ACK policy. The fake gateway listens only on production C5; every frame
 * consumes its real complete airtime and an ACK must fit its receive window. */
#include "app_mesh_ch9_ack.h"
#include "app_mesh_direct_gateway_retry.h"
#include "app_mesh_direct_probe_diag.h"
#include "dwm3000_timing.h"
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
#define GATEWAY_ID UINT64_C(0x9000)
#define NETWORK_ID 1u
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ TEST_PROBE_FORCED
#define IS_ENABLED(config) (config)
#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ARG_UNUSED(value) ((void)(value))
#define BUILD_ASSERT _Static_assert
#define LOG_ERR(...) status_debug_printf(__VA_ARGS__)
#define LOG_WRN(...) status_debug_printf(__VA_ARGS__)
#define RADIO_GUARD_UWB_CLIENT_MESH_TX 1
#define MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP 128u
#define MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS APP_MESH_DIRECT_GATEWAY_ACK_RX_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_RX_SLICE_MS 60u

enum dwm3000_rx_failure {
    DWM3000_RX_FAILURE_NONE,
    DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT,
    DWM3000_RX_FAILURE_CRC_OR_PHY,
};
struct radio_guard_uwb_lease { bool active; };
struct test_rx { struct proto_packet packet; uint32_t received_at_ms; };
static struct mesh_relay mesh_runtime, gateway_runtime;
static struct mesh_outbound mesh_direct_gateway_probe_scratch;
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
static int mesh_send_scratch_lock, mesh_direct_gateway_probe_scratch_lock;
static int mesh_rx_msgq;
static struct mesh_event_diagnostics mesh_event_stats;
static uint64_t now_ms, ack_start_ms, ack_end_ms;
static uint8_t configured_channel, ack_channel;
static uint8_t ack_wire[UWB_MESH_MAX_FRAME_LEN];
static size_t ack_len;
static bool radio_owned, scan_stopped, pending_ack, drop_ack, wrong_ack;
static bool malformed_ack, late_ack, burst_collects, hard_rx_error;
static int configure_error, parking_error, ack_send_error;
static unsigned sends, receives, configurations, legacy_configurations;
static unsigned queue_calls, progress_calls, ack_sends, aggregate_calls;
static unsigned response_submits, followers, retry_owned_notes;
static uint16_t sequence;

static uint32_t k_uptime_get_32(void) { return (uint32_t)now_ms; }
static int64_t k_uptime_get(void) { return (int64_t)now_ms; }
static bool uptime_deadline_reached(uint32_t now, uint32_t deadline)
{ return (int32_t)(now-deadline) >= 0; }
static uint32_t uptime_ms_until_deadline(uint32_t now, uint32_t deadline)
{ return uptime_deadline_reached(now,deadline) ? 1u : deadline-now; }
static uint32_t airtime_ms(size_t length)
{
    return (uint32_t)((dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_MESH_CONTROL,length)+999u)/1000u);
}
static void status_debug_printf(const char *format, ...)
{
    if (strstr(format,"DBG_GATEWAY_ROUTE_PROBE_ACK_RETRY_OWNED")) retry_owned_notes++;
}
static void status_debug_note(const char *text) { (void)text; }
static void status_debug_tx_gateway_ack_rx_pulse(void) {}
void app_mesh_direct_probe_breadcrumb_note(enum app_mesh_direct_probe_phase phase,
                                          uint8_t attempt,uint16_t seq)
{ (void)phase;(void)attempt;(void)seq; }
static int k_mutex_lock(int *lock,int timeout)
{ (void)timeout;assert(!*lock);*lock=1;return 0; }
static void k_mutex_unlock(int *lock) { assert(*lock);*lock=0; }
static unsigned k_msgq_num_used_get(const int *queue) { return (unsigned)*queue; }
static void mesh_stop_role_scan(void) { assert(!scan_stopped);scan_stopped=true; }
static void mesh_restart_role_scan(void) { assert(!radio_owned);scan_stopped=false; }
static int mesh_transport_radio_claim(int client,const char *reason,
                                     struct radio_guard_uwb_lease *lease)
{ (void)client;(void)reason;assert(scan_stopped&&!radio_owned);radio_owned=lease->active=true;now_ms++;return 0; }
static int mesh_transport_radio_finish(struct radio_guard_uwb_lease *lease,int parking)
{
    assert(radio_owned&&lease->active);
    if(parking<0)return parking;
    radio_owned=lease->active=false;return 0;
}
static int mesh_release_radio_after_mesh_turn(bool stop,const char *reason)
{ (void)stop;(void)reason;assert(radio_owned);now_ms++;return parking_error; }
static int mesh_radio_idle_with_bounded_recovery(const char *reason)
{ return mesh_release_radio_after_mesh_turn(false,reason); }
static void mesh_report_note_anchor_uwb_awake_since(int64_t start,uint32_t flags)
{ (void)flags;assert(start>=0&&(uint64_t)start<=now_ms); }
static void app_watchdog_note_radio_progress(void)
{ assert(!radio_owned&&!mesh_send_scratch_lock);progress_calls++; }
static int mesh_route_wake_sniff_activity(const char *tag,uint64_t peer,
                                         const char *reason,uint8_t attempt,bool *active)
{ (void)tag;(void)peer;(void)reason;(void)attempt;assert(radio_owned);now_ms+=2;*active=false;return 0; }
static int dwm3000_driver_configure_wake_mesh_control_mode(void)
{ configurations++;configured_channel=UWB_CHANNEL_WAKE_CONTACT;now_ms+=2;return configure_error; }
/* Keep the legacy fake callable for the negative mutation of the production
 * sender. The gateway below will never hear a probe transmitted on C9. */
int dwm3000_driver_configure_mesh_payload_mode(void)
{ legacy_configurations++;configured_channel=UWB_CHANNEL_MESH_PAYLOAD;now_ms+=2;return configure_error; }

static bool mesh_c5_collect_ack(const struct mesh_outbound *ack,const struct test_rx *rx)
{ (void)ack;(void)rx;aggregate_calls++;return burst_collects; }
static int mesh_send_causal_channel9_response(const struct mesh_outbound *ack,
                                             const char *reason,void *unused)
{
    (void)reason;(void)unused;ack_sends++;
    assert(ack->radio_channel==UWB_CHANNEL_WAKE_CONTACT);
    if(ack_send_error)return ack_send_error;
    struct mesh_outbound response=*ack;
    if(wrong_ack)response.packet.session_id++;
    assert(uwb_mesh_frame_encode(NETWORK_ID,GATEWAY_ID,DEVICE_ID,&response.packet,
                                response.payload,ack_wire,sizeof(ack_wire),&ack_len)==PROTO_OK);
    if(malformed_ack)ack_wire[0]^=0xffu;
    ack_channel=ack->radio_channel;
    ack_start_ms=now_ms+2u+(late_ack?MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS:0u);
    ack_end_ms=ack_start_ms+airtime_ms(ack_len);
    pending_ack=!drop_ack;
    return 0;
}
static void mesh_c5_capture_followers(const struct mesh_outbound *ack,const struct test_rx *rx)
{ (void)ack;(void)rx;followers++; }
static int app_node_comm_submit_control_response(const struct mesh_outbound *ack,
                                                 uint64_t deadline,uint16_t token)
{ (void)ack;(void)deadline;(void)token;response_submits++;return 0; }
static void gateway_ack_policy(struct mesh_outbound *gateway_ack,const struct test_rx *rx,
                               bool *gateway_ack_handed_off)
{
    const uint8_t received_radio_channel=UWB_CHANNEL_WAKE_CONTACT;
    int ret=-EAGAIN;
#define mesh_runtime gateway_runtime
#include "direct_probe_c5_gateway_ack.inc"
#undef mesh_runtime
    assert(false);
after_gateway_ack:;
}

static int mesh_send_outbound_preconfigured_ch9_locked(const struct mesh_outbound *probe,
                                                       const char *reason,void *unused)
{
    uint8_t wire[UWB_MESH_MAX_FRAME_LEN],payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t length=0u,payload_len=0u;
    struct proto_packet packet;
    uint64_t previous=0u;
    static struct mesh_relay_result result;
    (void)reason;(void)unused;
    assert(radio_owned&&mesh_send_scratch_lock&&probe->packet.msg_type==MSG_GATEWAY_ROUTE_REQ);
    sends++;
    assert(uwb_mesh_frame_encode(NETWORK_ID,DEVICE_ID,GATEWAY_ID,&probe->packet,
                                probe->payload,wire,sizeof(wire),&length)==PROTO_OK);
    now_ms+=airtime_ms(length);
    /* The gateway production receiver is continuously C5 extended control.
     * Matching only the outbound's metadata cannot establish reception. */
    if(configured_channel!=UWB_CHANNEL_WAKE_CONTACT || probe->radio_channel!=configured_channel)return 0;
    assert(uwb_mesh_frame_decode(wire,length,NETWORK_ID,GATEWAY_ID,&previous,
                                &packet,payload,sizeof(payload),&payload_len)==PROTO_OK);
    assert(mesh_packet_rx_envelope_validate(&packet,payload,payload_len,previous,
        GATEWAY_ID,GATEWAY_ID,configured_channel,false)==PROTO_OK);
    assert(mesh_relay_handle_rx(&gateway_runtime,&packet,payload,payload_len,
        previous,90u,k_uptime_get_32(),&result)==PROTO_OK);
    assert(result.actions&MESH_RELAY_ACTION_SEND_GATEWAY_ACK);
    struct test_rx rx={.packet=packet,.received_at_ms=k_uptime_get_32()};
    bool handed=false;
    gateway_ack_policy(&result.gateway_ack,&rx,&handed);
    assert(handed==(ack_send_error==0));
    return 0;
}
static int dwm3000_driver_receive_frame_continuous(uint32_t timeout,uint8_t *frame,
    size_t cap,size_t *len,uint8_t *quality,void *rsl,enum dwm3000_rx_failure *failure)
{
    (void)rsl;assert(radio_owned);receives++;*failure=DWM3000_RX_FAILURE_NONE;
    if(hard_rx_error){now_ms++;return -EIO;}
    if(pending_ack){
        pending_ack=false;
        if(ack_channel==configured_channel&&ack_start_ms>=now_ms&&ack_end_ms<=now_ms+timeout){
            assert(ack_len<=cap);memcpy(frame,ack_wire,ack_len);*len=ack_len;*quality=90u;
            now_ms=ack_end_ms;return 0;
        }
    }
    now_ms+=timeout;*len=0u;*failure=DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;return -ETIMEDOUT;
}
static bool mesh_queue_from_frame_at(const uint8_t *frame,size_t len,uint8_t quality,
    uint8_t channel,uint32_t at,uint32_t flags,void *extra,uint32_t extra_len,
    bool *valid,uint64_t *previous)
{
    struct proto_packet packet;uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];size_t payload_len=0u;
    (void)quality;(void)at;(void)flags;(void)extra;(void)extra_len;
    queue_calls++;assert(channel==configured_channel);
    *valid=uwb_mesh_frame_decode(frame,len,NETWORK_ID,DEVICE_ID,previous,&packet,
                               payload,sizeof(payload),&payload_len)==PROTO_OK;
    if(*valid){mesh_rx_msgq++;return true;}return false;
}
#include "direct_probe_c5_receiver.inc"

static bool mesh_click_preempt_boundary_requested(void) { return false; }
static bool mesh_click_preempt_wait_route_backoff(uint32_t delay)
{ assert(!radio_owned&&!mesh_send_scratch_lock);now_ms+=delay;return false; }
static uint32_t sys_rand32_get(void) { return 0u; }
static uint32_t nonzero_uptime_session_id(void) { return k_uptime_get_32()?k_uptime_get_32():1u; }
static uint16_t mesh_next_event_control_seq(void) { return ++sequence; }
static int mesh_errno_from_proto(int ret) { assert(ret!=PROTO_OK);return -EINVAL; }
#include "direct_probe_c5_sender.inc"

static void reset_fixture(uint64_t start)
{
    now_ms=start;sequence=0u;mesh_rx_msgq=0;
    memset(&mesh_event_stats,0,sizeof(mesh_event_stats));
    memset(&mesh_direct_gateway_probe_scratch,0,sizeof(mesh_direct_gateway_probe_scratch));
    mesh_send_scratch_lock=mesh_direct_gateway_probe_scratch_lock=0;
    configured_channel=ack_channel=0u;
    radio_owned=scan_stopped=pending_ack=drop_ack=wrong_ack=false;
    malformed_ack=late_ack=burst_collects=hard_rx_error=false;
    configure_error=parking_error=ack_send_error=0;
    sends=receives=configurations=legacy_configurations=0u;
    queue_calls=progress_calls=ack_sends=aggregate_calls=0u;
    response_submits=followers=retry_owned_notes=0u;
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_ANCHOR,DEVICE_ID,GATEWAY_ID,1u);
    mesh_relay_init(&gateway_runtime,MESH_RELAY_ROLE_GATEWAY,GATEWAY_ID,GATEWAY_ID,1u);
    assert(route_selected(&mesh_runtime.upstream)==NULL);
}
static int probe(bool install)
{
    int ret=mesh_try_direct_gateway_route_probe(GATEWAY_ID,"cold-route",install,
        APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE,17u);
    assert(!mesh_send_scratch_lock&&!mesh_direct_gateway_probe_scratch_lock);
    if(!parking_error)assert(!radio_owned&&!scan_stopped);
    return ret;
}
static void test_cold_route_requires_actual_c5_ack(void)
{
    const uint64_t starts[]={1000u,UINT32_MAX-10u};
    for(size_t i=0;i<sizeof(starts)/sizeof(starts[0]);i++){
        reset_fixture(starts[i]);burst_collects=true;
        assert(probe(true)==0);
        const struct route_candidate *route=route_selected(&mesh_runtime.upstream);
        if(TEST_PROBE_FORCED)assert(route==NULL);
        else assert(route!=NULL&&route->next_hop_id==GATEWAY_ID);
        assert(sends==1u&&configurations==1u&&legacy_configurations==0u);
        assert(ack_sends==1u&&aggregate_calls==0u&&response_submits==0u);
        assert(queue_calls==0u&&progress_calls==1u&&now_ms>starts[i]);
    }
    reset_fixture(1000u);
    assert(probe(false)==0&&route_selected(&mesh_runtime.upstream)==NULL);
}
static void test_no_false_route_for_missing_wrong_or_incomplete_ack(void)
{
    for(unsigned fault=0u;fault<4u;fault++){
        reset_fixture(1000u);
        drop_ack=fault==0u;wrong_ack=fault==1u;malformed_ack=fault==2u;late_ack=fault==3u;
        assert(probe(true)<0&&route_selected(&mesh_runtime.upstream)==NULL);
        assert(sends==(wrong_ack?1u:APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS));
        assert(queue_calls==(wrong_ack?1u:malformed_ack?APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS:0u));
        assert(response_submits==0u&&legacy_configurations==0u);
    }
}
static void test_ack_failure_keeps_probe_retry_owned(void)
{
    reset_fixture(1000u);ack_send_error=-EBUSY;burst_collects=true;
    assert(probe(true)==-ETIMEDOUT&&route_selected(&mesh_runtime.upstream)==NULL);
    assert(ack_sends==APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS);
    assert(retry_owned_notes==ack_sends&&aggregate_calls==0u&&response_submits==0u);
    assert(followers==0u);
}
static void test_failed_phy_or_release_cannot_install_or_feed(void)
{
    reset_fixture(1000u);configure_error=-EIO;
    assert(probe(true)==-EIO&&sends==0u&&progress_calls==0u);
    assert(route_selected(&mesh_runtime.upstream)==NULL);
    reset_fixture(1000u);hard_rx_error=true;
    assert(probe(true)==-EIO&&progress_calls==0u);
    assert(route_selected(&mesh_runtime.upstream)==NULL);
    reset_fixture(1000u);parking_error=-EIO;
    /* Test the atomic sender: after failed parking its caller must retain
     * the owner for recovery, so this fixture must not retry another claim. */
    mesh_direct_gateway_probe_scratch=(struct mesh_outbound){
        .packet={.msg_type=MSG_GATEWAY_ROUTE_REQ,.flags=FLAG_GATEWAY_ACK_REQUIRED,
                 .src_id=DEVICE_ID,.dst_id=GATEWAY_ID,.session_id=10u,
                 .seq=20u,.ttl=MESH_DEFAULT_TTL},
        .radio_channel=UWB_CHANNEL_WAKE_CONTACT,.next_hop_id=GATEWAY_ID,
    };
    assert(mesh_send_direct_gateway_probe_and_wait(&mesh_direct_gateway_probe_scratch,"fail",1u)==-EIO);
    assert(sends==1u&&ack_sends==1u&&receives==1u&&progress_calls==0u&&radio_owned);
    assert(route_selected(&mesh_runtime.upstream)==NULL);
}
static void test_legacy_channel_rejected_before_ownership(void)
{
    reset_fixture(1000u);
    struct mesh_outbound out={.radio_channel=UWB_CHANNEL_MESH_PAYLOAD,.next_hop_id=GATEWAY_ID};
    assert(mesh_send_direct_gateway_probe_and_wait(&out,"legacy",1u)==-EINVAL);
    assert(!radio_owned&&!scan_stopped&&sends==0u&&configurations==0u);
}
int main(void)
{
    test_cold_route_requires_actual_c5_ack();
    test_no_false_route_for_missing_wrong_or_incomplete_ack();
    test_ack_failure_keeps_probe_retry_owned();
    test_failed_phy_or_release_cannot_install_or_feed();
    test_legacy_channel_rejected_before_ownership();
    puts("production C5 direct-route bootstrap passed");return 0;
}
