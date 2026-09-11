/* Complete production route listeners with canonical RF decoding/validation.
 * Hardware boundaries consume nonzero time and require one matching full PHY
 * for the complete frame airtime. No route or direct-delivery fallback exists. */
#include "app_mesh_c5_priority.h"
#include "app_mesh_c5_repair_authorization.h"
#include "app_mesh_report.h"
#include "app_mesh_smoke_fast.h"
#include "app_mesh_route_reply_ack.h"
#include "app_mesh_rx_policy.h"
#include "app_wake_train_politeness.h"
#include "discovery_assignment.h"
#include "dwm3000_driver.h"
#include "dwm3000_timing.h"
#include "gateway_command.h"
#include "firmware_state_machines.h"
#include "mesh_relay.h"
#include "uwb.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "Production listener regressions require assertions enabled"
#endif
#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3
#define DEVICE_ROLE ROLE_ANCHOR
#define DEVICE_ID UINT64_C(0xb100)
#define ANCHOR_A UINT64_C(0xa100)
#define CLICKER_C UINT64_C(0xc100)
#define GATEWAY_ID UINT64_C(0x9000)
#define NETWORK_ID 1u
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER 0
#define CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL 0
#define CONFIG_IMEC_ML_ANCHOR 0
#define IS_ENABLED(config) (config)
#define K_NO_WAIT 0
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define LOG_INF(...) status_debug_printf(__VA_ARGS__)
#define LOG_WRN(...) status_debug_printf(__VA_ARGS__)
#define LOG_ERR(...) status_debug_printf(__VA_ARGS__)
#define RADIO_GUARD_UWB_CLIENT_MESH_RX 1
#define MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN (125u-UWB_RF_SCOPE_WIRE_LEN)
#define MESH_STANDARD_WAKE_PROBE_CLICK 1
#define MESH_STANDARD_WAKE_PROBE_RELAYED_GATEWAY_CONTROL 2
#define MESH_STANDARD_WAKE_PROBE_ROUTE_ACTIVATION 3

typedef unsigned atomic_val_t;
struct radio_guard_uwb_lease { bool active; };
static uint64_t now_ms;
static uint32_t k_uptime_get_32(void) { return (uint32_t)now_ms; }
static int64_t k_uptime_get(void) { return (int64_t)now_ms; }
#include "deferred_route_reply_time.inc"
#include "reactive_route_state.inc"
static struct mesh_relay mesh_runtime, origin_runtime, upstream_runtime;
static struct mesh_frame_parse_context mesh_route_reply_ack_parsed;
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
static uint8_t mesh_route_reply_ack_frame[UWB_MESH_MAX_FRAME_LEN];
static struct app_mesh_c5_control_route_history mesh_c5_control_route_history;
static struct c5_contact_context mesh_c5_contact;
static int mesh_c5_control_scratch_lock, mesh_route_reply_scratch_lock;
static struct mesh_outbound mesh_route_reply_backup_scratch;
static int mesh_route_reply_ack_scratch_lock, mesh_rx_work;
static unsigned mesh_route_ready_generation;
static unsigned receives, queue_count, yields, semantic_rejects, ack_captures;
static unsigned config_count, work_submissions;
static bool radio_owned, scan_stopped;
static int configure_error, parking_error;
static bool change_route_generation;
static int injected_rx_error;
static uint64_t receive_deadline, last_frame_end;
static enum dwm3000_timing_phy configured_phy;
struct air_frame {
    uint8_t wire[UWB_MESH_MAX_FRAME_LEN]; size_t length;
    uint64_t start, end; enum dwm3000_timing_phy phy;
};
static struct air_frame air[32];
static unsigned air_count, air_next;
#include "reactive_route_queue_capacity.inc"
_Static_assert(MESH_RX_QUEUE_DEPTH==4,"Use the actual connected-anchor queue capacity");
static struct mesh_rx_pending queued[MESH_RX_QUEUE_DEPTH];
static unsigned reply_transmissions, dropped_acks, wake_attempts, drop_ack_count;
static unsigned replace_contact_on_rx;
static uint64_t last_tx_end, last_tx_deadline;
static bool automatic_downstream_ack;

static void status_debug_printf(const char *format,...)
{
    if(strstr(format,"DBG_ROUTE_REPLY_LISTEN_YIELD_REQUEST"))yields++;
    if(strstr(format,"DBG_ROUTE_REQ_SEMANTIC_REJECT"))semantic_rejects++;
}
static void status_debug_note(const char *text)
{ if(strstr(text,"DBG_ROUTE_REPLY_ACK_RX"))ack_captures++; }
#include "reactive_route_contact.inc"
static atomic_val_t atomic_get(const unsigned *value) { return *value; }
static int k_mutex_lock(int *lock,int timeout)
{ (void)timeout;assert(!*lock);*lock=1;return 0; }
static void k_mutex_unlock(int *lock) { assert(*lock);*lock=0; }
static bool mesh_coordinator_mesh_work_allowed(const char *reason)
{ (void)reason;return true; }
void mesh_stop_role_scan(void) { assert(!scan_stopped);scan_stopped=true; }
void mesh_restart_role_scan(void) { assert(!radio_owned);scan_stopped=false; }
static int mesh_transport_radio_claim(int client,const char *reason,struct radio_guard_uwb_lease *lease)
{ (void)client;(void)reason;assert(scan_stopped&&!radio_owned);radio_owned=lease->active=true;now_ms++;return 0; }
static int mesh_transport_radio_finish(struct radio_guard_uwb_lease *lease,int parking)
{ assert(radio_owned&&lease->active);if(parking<0)return parking;radio_owned=lease->active=false;return 0; }
static int mesh_radio_standby_with_bounded_recovery(const char *reason)
{ (void)reason;now_ms++;return parking_error; }
static void mesh_report_note_anchor_uwb_awake_since(int64_t start,uint32_t flags)
{ (void)flags;assert(start>=0&&(uint64_t)start<=now_ms); }
uint32_t mesh_rx_pending_count(void) { return queue_count; }
static int mesh_submit_owned_work(int *work,const char *reason)
{ (void)reason;assert(work==&mesh_rx_work&&!radio_owned);work_submissions++;return 0; }
static int mesh_schedule_uwb_rx(uint32_t delay) { (void)delay;assert(false);return 0; }
static bool mesh_next_channel9_receive_prepare_delay_ms(uint32_t now,uint32_t *delay)
{ (void)now;(void)delay;return false; }
uint8_t app_mesh_report_selected_gateway_hop_count(void) { return 0u; }
static bool mesh_frame_requires_anchor_click_handoff(const uint8_t *frame,size_t length,struct uwb_wake_claim_frame *claim)
{ (void)frame;(void)length;(void)claim;return false; }
static bool mesh_decode_channel5_wake_claim(const uint8_t *frame,size_t length,struct uwb_wake_claim_frame *claim,void *extra)
{ (void)frame;(void)length;(void)claim;(void)extra;return false; }
static bool mesh_handle_channel5_wake_claim(const uint8_t *frame,size_t length,uint8_t quality,bool *embedded,bool *click)
{ (void)frame;(void)length;(void)quality;(void)embedded;(void)click;return false; }
static bool mesh_handoff_anchor_click_claim(const struct uwb_wake_claim_frame *claim,uint8_t quality,uint32_t observed)
{ (void)claim;(void)quality;(void)observed;assert(false);return false; }
static int mesh_probe_standard_wake_claim(uint8_t *frame,size_t cap,struct uwb_wake_claim_frame *claim,uint8_t *quality,uint32_t *observed,bool allowed,int64_t deadline)
{ (void)frame;(void)cap;(void)claim;(void)quality;(void)observed;(void)allowed;(void)deadline;now_ms++;return 0; }
static bool mesh_handle_event_control(const struct proto_packet *packet,const uint8_t *payload,size_t len,uint64_t previous,uint32_t at)
{ (void)packet;(void)payload;(void)len;(void)previous;(void)at;assert(false);return false; }
static bool mesh_packet_is_event_control_type(uint8_t type)
{ return type==MSG_MESH_EVENT_PROPOSE||type==MSG_MESH_EVENT_ACCEPT||type==MSG_MESH_EVENT_UPDATE||type==MSG_MESH_EVENT_END; }
static int configure(enum dwm3000_timing_phy phy)
{ assert(radio_owned);configured_phy=phy;config_count++;now_ms+=2u;return configure_error; }
int dwm3000_driver_configure_wake_mode(void) { return configure(DWM3000_TIMING_PHY_CH5_WAKE); }
int dwm3000_driver_configure_wake_mesh_control_mode(void) { return configure(DWM3000_TIMING_PHY_CH5_MESH_CONTROL); }
int dwm3000_driver_configure_mesh_payload_mode(void) { return configure(DWM3000_TIMING_PHY_CH9_MESH); }
int dwm3000_driver_last_rx_host_uptime(uint32_t *at) { *at=(uint32_t)last_frame_end;return 0; }
void dwm3000_driver_stats_get(struct dwm3000_driver_stats *stats) { memset(stats,0,sizeof(*stats)); }
void dwm3000_driver_last_rx_debug_get(struct dwm3000_rx_debug_snapshot *snapshot) { memset(snapshot,0,sizeof(*snapshot)); }
int dwm3000_driver_receive_frame_continuous(uint32_t timeout,uint8_t *frame,size_t cap,size_t *length,uint8_t *quality,int8_t *rsl,enum dwm3000_rx_failure *failure)
{
    assert(radio_owned&&timeout);receives++;
    const uint64_t end=now_ms+timeout;
    if(receive_deadline&&end>receive_deadline)assert(false);
    *failure=DWM3000_RX_FAILURE_NONE;*length=0u;
    if(replace_contact_on_rx) {
        const unsigned replacement=replace_contact_on_rx;
        replace_contact_on_rx=0u;
        mesh_c5_contact_accept(replacement==1u?ANCHOR_A:CLICKER_C,
            replacement==1u?C5_CONTACT_PURPOSE_ROUTE_REPLY:C5_CONTACT_PURPOSE_UPLINK,
            k_uptime_get_32()+5000u,"foreign-owner");
    }
    if(change_route_generation){change_route_generation=false;mesh_route_ready_generation++;now_ms=end;return -ETIMEDOUT;}
    if(injected_rx_error){now_ms=end;return injected_rx_error;}
    if(air_next<air_count&&air[air_next].end<=end){
        const struct air_frame *candidate=&air[air_next++];
        /* Matching channel alone is insufficient: PHR, preamble and every
         * shared production profile field come from the exact PHY identity. */
        if(candidate->start>=now_ms&&candidate->phy==configured_phy){
            assert(candidate->length<=cap);memcpy(frame,candidate->wire,candidate->length);
            *length=candidate->length;*quality=90u;if(rsl)*rsl=-60;
            now_ms=last_frame_end=candidate->end;return 0;
        }
    }
    now_ms=end;*failure=DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;return -ETIMEDOUT;
}
static int mesh_rx_msgq;
static unsigned queue_put_attempts, queue_put_rejections;
static struct uwb_anchor_session anchor_uwb_session;

static int k_msgq_put(int *queue, const void *item, int timeout)
{
    assert(queue == &mesh_rx_msgq && timeout == K_NO_WAIT && item != NULL);
    ++queue_put_attempts;
    if (queue_count == sizeof(queued) / sizeof(queued[0])) {
        ++queue_put_rejections;
        return -ENOMSG;
    }
    assert(queue_count < sizeof(queued) / sizeof(queued[0]));
    memcpy(&queued[queue_count++], item, sizeof(queued[0]));
    return 0;
}
static unsigned k_msgq_num_used_get(const int *queue)
{ assert(queue == &mesh_rx_msgq); return queue_count; }
static bool mesh_gateway_route_test_role(void) { return false; }
static bool permit_table_in_inactive_survey;
static bool app_survey_anchor_command_allowed(const struct proto_packet *packet,
                                              const uint8_t *payload, size_t len)
{
    enum discovery_assignment_phase phase;
    uint32_t epoch;

    return permit_table_in_inactive_survey && packet != NULL &&
        packet->msg_type == MSG_COMMAND && packet->src_id == GATEWAY_ID &&
        discovery_assignment_extract_control_tlvs(payload, len, &phase, &epoch) == PROTO_OK &&
        phase == DISCOVERY_ASSIGNMENT_PHASE_TABLE && epoch != 0u &&
        mesh_relay_command_packet_envelope_valid(&mesh_runtime, packet,
                                                  payload, len, GATEWAY_ID);
}
static bool app_survey_anchor_active(void) { return false; }
static bool mesh_route_activation_allows_route_adv(uint64_t previous,
                                                   uint32_t session,
                                                   uint8_t ttl, uint32_t received)
{ (void)previous; (void)session; (void)ttl; (void)received; return false; }
static void app_mesh_test_note_wake_event(const struct proto_packet *packet,
                                         uint64_t previous, uint8_t quality,
                                         uint8_t channel)
{ (void)packet; (void)previous; (void)quality; assert(channel == UWB_CHANNEL_WAKE_CONTACT); }
static void status_debug_uwb_rx_channel_pulse(uint8_t channel)
{ assert(channel == UWB_CHANNEL_WAKE_CONTACT); }
static void status_debug_tx_gateway_ack_rx_pulse(void) {}
static int gateway_command_result_validation_reserve(const struct proto_packet *packet,
                                                      uint64_t received,
                                                      uint32_t *token)
{ (void)packet; (void)received; (void)token; assert(false); return -EINVAL; }
static void gateway_command_result_validation_release_reserved(uint32_t token)
{ assert(token == 0u); }
static const char *role_name(void) { return "anchor"; }

/* Exact production timestamp expansion and full queue admission, not a
 * simplified decode shim. uwb_anchor_note_mesh_packet uses the actual core. */
#include "reactive_route_rx_queue.inc"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter" /* retained production attempt argument */
#include "reactive_route_listeners.inc"
#pragma GCC diagnostic pop

static enum dwm3000_timing_phy ack_tx_phy(const struct mesh_outbound *ack,size_t frame_len)
{
    const uint8_t radio_channel=ack->radio_channel;
    const struct proto_packet tx_packet=ack->packet;
    bool channel5_extended_control;
    int ret;
    assert(!radio_owned);radio_owned=true;
#include "reactive_route_ack_tx_phy.inc"
    assert(ret==0);radio_owned=false;
    return configured_phy;
}
static void reset_fixture(uint64_t start)
{
    assert(!radio_owned&&!mesh_route_reply_ack_scratch_lock&&
        !mesh_c5_control_scratch_lock&&!mesh_route_reply_scratch_lock);
    permit_table_in_inactive_survey=false;
    now_ms=start;receives=queue_count=yields=semantic_rejects=ack_captures=0u;
    config_count=work_submissions=air_count=air_next=0u;scan_stopped=false;
    configure_error=parking_error=injected_rx_error=0;change_route_generation=false;
    receive_deadline=last_frame_end=0u;
    reply_transmissions=dropped_acks=wake_attempts=drop_ack_count=0u;
    replace_contact_on_rx=0u;automatic_downstream_ack=false;
    last_tx_end=last_tx_deadline=0u;
    memset(&mesh_c5_contact,0,sizeof(mesh_c5_contact));
    mesh_route_ready_generation=0u;memset(&mesh_c5_control_route_history,0,sizeof(mesh_c5_control_route_history));
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_ANCHOR,DEVICE_ID,GATEWAY_ID,1u);
    mesh_relay_init(&origin_runtime,MESH_RELAY_ROLE_CLICKER,CLICKER_C,GATEWAY_ID,1u);
    mesh_relay_init(&upstream_runtime,MESH_RELAY_ROLE_ANCHOR,ANCHOR_A,GATEWAY_ID,1u);
}
static void schedule(const struct mesh_outbound *packet,uint64_t previous,uint32_t after,enum dwm3000_timing_phy phy)
{
    assert(air_count<32u);struct air_frame *candidate=&air[air_count++];
    int ret=uwb_mesh_frame_encode(NETWORK_ID,previous,packet->next_hop_id,&packet->packet,packet->payload,candidate->wire,sizeof(candidate->wire),&candidate->length);
    if(ret!=PROTO_OK)fprintf(stderr,"encode ret=%d type=%u src=%llx dst=%llx ttl=%u prev=%llx\n",ret,packet->packet.msg_type,(unsigned long long)packet->packet.src_id,(unsigned long long)packet->packet.dst_id,packet->packet.ttl,(unsigned long long)previous);
    assert(ret==PROTO_OK);
    candidate->phy=phy;candidate->start=now_ms+after;
    candidate->end=candidate->start+(dwm3000_timing_airtime_us_ceil(phy,candidate->length)+999u)/1000u;
}

#include "reactive_route_release_policy.inc"
static int mesh_send_route_wake_train(uint64_t peer,const struct mesh_outbound *embedded,
    bool *sent,uint8_t purpose,const char *reason,const struct mesh_outbound *candidate,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    enum fw_c5_tx_intent intent,bool *started,uint64_t *started_at)
{
    (void)peer;(void)embedded;(void)sent;(void)purpose;(void)reason;(void)candidate;
    (void)authorization;(void)intent;(void)started;(void)started_at;
    wake_attempts++;assert(false);return -EIO;
}
static void mesh_wait_for_c5_control_followup_turnaround(uint8_t type,const char *reason)
{ (void)type;(void)reason;assert(false); }
static int mesh_event_propose_prepare_immediate_send(const struct mesh_outbound *out)
{ (void)out;assert(false);return -EIO; }

/* Only the physical TX boundary is scripted: real encoded bytes cross to C,
 * whose production relay constructs the exact ACK. Configuration/SPI costs
 * 3 ms and TX consumes its complete actual extended-C5 airtime. The production
 * admission/lease refresh above this boundary is deliberately shared with RX. */
static int mesh_send_outbound_with_release_on_channel_until(
    const struct mesh_outbound *out,const char *reason,
    enum mesh_radio_release_policy release,bool *deferred,uint64_t deadline,
    struct app_mesh_tx_observation *observation,uint8_t channel,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    enum fw_c5_tx_intent intent)
{
    uint8_t wire[UWB_MESH_MAX_FRAME_LEN],payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t length=0u,payload_length=0u;uint64_t previous=0u;
    struct proto_packet packet;static struct mesh_relay_result result;
    (void)reason;(void)deferred;(void)observation;
    assert(release==MESH_RADIO_RELEASE_STANDBY&&authorization==NULL);
    assert(intent==FW_C5_TX_INTENT_CAUSAL_RESPONSE&&channel==UWB_CHANNEL_WAKE_CONTACT);
    assert(out->packet.msg_type==MSG_ROUTE_REPLY&&out->next_hop_id==CLICKER_C);
    assert(!radio_owned&&!scan_stopped&&automatic_downstream_ack);
    assert(mesh_c5_contact_active(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,k_uptime_get_32()));
    now_ms+=3u;
    if(deadline&&now_ms>=deadline)return -ETIMEDOUT;
    assert(uwb_mesh_frame_encode(NETWORK_ID,DEVICE_ID,CLICKER_C,&out->packet,
        out->payload,wire,sizeof(wire),&length)==PROTO_OK);
    now_ms+=(dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_MESH_CONTROL,length)+999u)/1000u;
    last_tx_end=now_ms;last_tx_deadline=deadline;reply_transmissions++;
    assert(uwb_mesh_frame_decode(wire,length,NETWORK_ID,CLICKER_C,&previous,&packet,
        payload,sizeof(payload),&payload_length)==PROTO_OK);
    assert(previous==DEVICE_ID&&payload_length==out->payload_len);
    assert(mesh_relay_handle_rx(&origin_runtime,&packet,payload,payload_length,
        previous,90u,k_uptime_get_32(),&result)==PROTO_OK);
    assert(result.actions&MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK);
    if(dropped_acks<drop_ack_count)dropped_acks++;
    else schedule(&result.route_reply_ack,CLICKER_C,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    return 0;
}
#include "reactive_route_control_send.inc"
static void k_msleep(uint32_t duration) { now_ms+=duration; }
#include "deferred_route_reply_train.inc"
static void mesh_route_embedded_wait_before_reply(const struct mesh_outbound *out) { (void)out; }
static void mesh_wait_until_ms(uint32_t deadline)
{ if(!uptime_deadline_reached(k_uptime_get_32(),deadline))now_ms+=(uint32_t)(deadline-k_uptime_get_32()); }
static int mesh_propose_event_after_channel5_contact(uint64_t peer,const char *reason)
{ (void)peer;(void)reason;assert(false);return -EIO; }
#include "deferred_route_reply_sender.inc"
static void test_empty_original_deadline(void)
{
    reset_fixture(1000u);bool captured=true;receive_deadline=now_ms+3u+100u;
    assert(mesh_listen_for_route_reply(GATEWAY_ID,"test",100u,NULL,&captured)==-ETIMEDOUT);
    assert(!captured&&queue_count==0u&&now_ms==receive_deadline+1u);
    assert(receives==4u&&!radio_owned&&!scan_stopped);
}

struct exchange {
    struct mesh_outbound source_request, forwarded_request, reflected_request, reply;
    struct mesh_route_capture_identity identity;
};
static void prepare_exchange(struct exchange *exchange)
{
    static struct mesh_relay_result result;
    for(unsigned attempt=0u;attempt<3u;attempt++) {
        assert(mesh_relay_prepare_route_request(&origin_runtime,GATEWAY_ID,
            k_uptime_get_32(),0u,&exchange->source_request)==PROTO_OK);
        if(exchange->source_request.packet.ttl>=4u)break;
        now_ms+=(uint32_t)(origin_runtime.route_discovery.next_request_ms-k_uptime_get_32());
    }
    assert(exchange->source_request.packet.ttl>=4u);
    assert(mesh_relay_handle_rx(&mesh_runtime,&exchange->source_request.packet,
        exchange->source_request.payload,exchange->source_request.payload_len,
        CLICKER_C,90u,k_uptime_get_32(),&result)==PROTO_OK);
    assert(result.actions&MESH_RELAY_ACTION_SEND_ROUTE_REQ);
    exchange->forwarded_request=result.route_request;
    assert(mesh_route_capture_identity_from_request(&exchange->forwarded_request,
        GATEWAY_ID,&exchange->identity));
    assert(exchange->identity.origin_id==CLICKER_C);
    now_ms+=10u;
    assert(mesh_relay_handle_rx(&upstream_runtime,&exchange->forwarded_request.packet,
        exchange->forwarded_request.payload,exchange->forwarded_request.payload_len,
        DEVICE_ID,90u,k_uptime_get_32(),&result)==PROTO_OK);
    assert(result.actions&MESH_RELAY_ACTION_SEND_ROUTE_REQ);
    exchange->reflected_request=result.route_request;
    assert(mesh_relay_note_direct_gateway_route(&upstream_runtime,k_uptime_get_32())==PROTO_OK);
    assert(mesh_relay_build_route_reply_for_forwarded_request(&upstream_runtime,
        &exchange->reflected_request,DEVICE_ID,k_uptime_get_32()+10000u,
        k_uptime_get_32(),0u,&exchange->reply)==PROTO_OK);
    assert(exchange->reply.packet.dst_id==CLICKER_C&&exchange->reply.next_hop_id==DEVICE_ID);
}
static uint8_t *field(struct mesh_outbound *out,uint8_t type,uint8_t expected)
{
    const uint8_t *value=NULL;uint8_t length=0u;
    assert(tlv_find_unique(out->payload,out->payload_len,type,&value,&length)==PROTO_OK);
    assert(length==expected);
    return out->payload+(value-out->payload);
}
static void test_transit_reply_uses_original_requester(void)
{
    reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
    bool captured=false;
    schedule(&exchange.reply,ANCHOR_A,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    receive_deadline=now_ms+3u+100u;
    assert(mesh_listen_for_route_reply(GATEWAY_ID,"transit",100u,&exchange.identity,&captured)==0);
    assert(captured&&queue_count==1u&&queued[0].packet.dst_id==CLICKER_C);
    assert(queued[0].previous_hop_id==ANCHOR_A&&yields==0u&&work_submissions==1u);
    assert(!radio_owned&&!scan_stopped);
}
static void mutate_reply(struct mesh_outbound *bad,unsigned mutation)
{
    switch(mutation) {
    case 0: bad->packet.src_id=ANCHOR_A;break;
    case 1: bad->packet.dst_id=UINT64_C(0xd100);break;
    case 2: bad->packet.session_id++;break;
    case 3: field(bad,TLV_FLOOD_EPOCH_ID,4u)[0]^=1u;break;
    case 4: field(bad,TLV_REPLY_NONCE,2u)[0]^=1u;break;
    case 5: memset(field(bad,TLV_ROUTE_EPOCH,4u),0,4u);break;
    case 6: bad->packet.ttl=0u;break;
    case 7: bad->payload[1]=0xffu;break;
    default: assert(false);
    }
}
static void test_foreign_replies_do_not_end_or_extend_wait(void)
{
    for(unsigned mutation=0u;mutation<8u;mutation++) {
        reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
        struct mesh_outbound bad=exchange.reply;mutate_reply(&bad,mutation);
        schedule(&bad,ANCHOR_A,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        const uint64_t start=now_ms;
        receive_deadline=start+3u+100u;bool captured=true;
        int ret=mesh_listen_for_route_reply(GATEWAY_ID,"foreign",100u,&exchange.identity,&captured);
        if(ret!=-ETIMEDOUT)fprintf(stderr,"foreign mutation=%u ret=%d queues=%u captured=%u\n",mutation,ret,queue_count,captured);
        assert(ret==-ETIMEDOUT);
        assert(!captured&&queue_count==0u&&yields==0u&&work_submissions==0u);
        assert(now_ms==receive_deadline+1u);
    }
}
static void test_foreign_reply_then_exact_reply_preserves_owner(void)
{
    for(unsigned mutation=0u;mutation<8u;mutation++) {
        reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
        struct mesh_outbound bad=exchange.reply;mutate_reply(&bad,mutation);
        schedule(&bad,ANCHOR_A,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        schedule(&exchange.reply,ANCHOR_A,42u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        receive_deadline=now_ms+3u+100u;bool captured=false;
        assert(mesh_listen_for_route_reply(GATEWAY_ID,"retry",100u,&exchange.identity,&captured)==0);
        assert(captured&&queue_count==1u&&yields==0u&&now_ms<receive_deadline);
    }
}
static void mutate_request(struct mesh_outbound *bad,unsigned mutation)
{
    switch(mutation) {
    case 0: break; /* valid wire reflection has this receiver in its path */
    case 1: bad->packet.src_id=DEVICE_ID;break;
    case 2: bad->packet.src_id=ANCHOR_A;break;
    case 3: bad->packet.ttl=0u;break;
    case 4: bad->payload[1]=0xffu;break;
    default: assert(false);
    }
}
static void test_reflections_and_malformed_requests_preserve_original_deadline(void)
{
    for(unsigned mutation=0u;mutation<5u;mutation++) {
        reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
        struct mesh_outbound bad=exchange.reflected_request;mutate_request(&bad,mutation);
        schedule(&bad,ANCHOR_A,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        receive_deadline=now_ms+3u+100u;bool captured=true;
        assert(mesh_listen_for_route_reply(GATEWAY_ID,"noise",100u,&exchange.identity,&captured)==-ETIMEDOUT);
        assert(!captured&&queue_count==0u&&yields==0u&&now_ms==receive_deadline+1u);
        if(mutation==0u)assert(semantic_rejects==1u);
    }
}
static void test_reflection_then_valid_reply_is_not_lost(void)
{
    reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
    schedule(&exchange.reflected_request,ANCHOR_A,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    schedule(&exchange.reply,ANCHOR_A,42u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    receive_deadline=now_ms+3u+100u;bool captured=false;
    assert(mesh_listen_for_route_reply(GATEWAY_ID,"reflected",100u,&exchange.identity,&captured)==0);
    assert(captured&&queue_count==1u&&semantic_rejects==1u&&yields==0u);
}
static void test_legitimate_competing_request_still_yields(void)
{
    reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
    schedule(&exchange.source_request,CLICKER_C,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    receive_deadline=now_ms+3u+100u;bool captured=true;
    assert(mesh_listen_for_route_reply(GATEWAY_ID,"competitor",100u,&exchange.identity,&captured)==-EAGAIN);
    assert(!captured&&queue_count==1u&&yields==1u&&now_ms<receive_deadline);
}
static void prepare_downstream_ack(struct mesh_outbound *reply,struct mesh_outbound *ack)
{
    struct exchange exchange;static struct mesh_relay_result receive;
    prepare_exchange(&exchange);
    assert(mesh_relay_handle_rx(&mesh_runtime,&exchange.reply.packet,
        exchange.reply.payload,exchange.reply.payload_len,ANCHOR_A,90u,
        k_uptime_get_32(),&receive)==PROTO_OK);
    assert(receive.actions&MESH_RELAY_ACTION_SEND_ROUTE_REPLY);
    *reply=receive.route_reply;
    mesh_relay_note_tx_sent(&mesh_runtime,reply,k_uptime_get_32());
    assert(mesh_runtime.route_reply_ack_expectation.active);
    assert(mesh_relay_handle_rx(&origin_runtime,&reply->packet,reply->payload,
        reply->payload_len,DEVICE_ID,90u,k_uptime_get_32(),&receive)==PROTO_OK);
    assert(receive.actions&MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK);
    *ack=receive.route_reply_ack;
}
static void test_downstream_ack_uses_complete_matching_control_phy(void)
{
    reset_fixture(1000u);struct mesh_outbound reply,ack;prepare_downstream_ack(&reply,&ack);
    uint8_t wire[UWB_MESH_MAX_FRAME_LEN];size_t length=0u;
    assert(uwb_mesh_frame_encode(NETWORK_ID,CLICKER_C,DEVICE_ID,&ack.packet,ack.payload,
        wire,sizeof(wire),&length)==PROTO_OK);
    enum dwm3000_timing_phy tx=ack_tx_phy(&ack,length);
    assert(tx==DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    schedule(&ack,CLICKER_C,10u,tx);receive_deadline=now_ms+3u+RREP_ACK_TIMEOUT_MS;
    assert(mesh_listen_for_route_reply_ack(&reply,0u)==0);
    assert(configured_phy==tx&&ack_captures==1u&&!mesh_runtime.route_reply_ack_expectation.active);
    assert(now_ms<receive_deadline&&!radio_owned);
}
static void test_wrong_ack_identity_keeps_waiting_for_exact_ack(void)
{
    for(unsigned mutation=0u;mutation<5u;mutation++) {
        reset_fixture(1000u);struct mesh_outbound reply,ack;prepare_downstream_ack(&reply,&ack);
        struct mesh_outbound bad=ack;
        switch(mutation){
        case 0:bad.packet.src_id=ANCHOR_A;break;
        case 1:bad.packet.dst_id=ANCHOR_A;break;
        case 2:bad.packet.session_id++;break;
        case 3:bad.payload[1]=0xffu;break;
        case 4:bad.payload[bad.payload_len-1]^=1u;break;
        }
        schedule(&bad,CLICKER_C,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        schedule(&ack,CLICKER_C,42u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        receive_deadline=now_ms+3u+RREP_ACK_TIMEOUT_MS;
        assert(mesh_listen_for_route_reply_ack(&reply,0u)==0);
        assert(ack_captures==1u&&!mesh_runtime.route_reply_ack_expectation.active&&receives==2u);
    }
}
static void test_ack_partial_airtime_and_wrong_phy_are_not_delivery(void)
{
    for(unsigned variant=0u;variant<2u;variant++) {
        reset_fixture(1000u);struct mesh_outbound reply,ack;prepare_downstream_ack(&reply,&ack);
        enum dwm3000_timing_phy tx=variant==0u?DWM3000_TIMING_PHY_CH5_WAKE:DWM3000_TIMING_PHY_CH5_MESH_CONTROL;
        uint32_t after=variant==0u?10u:RREP_ACK_TIMEOUT_MS+2u;
        schedule(&ack,CLICKER_C,after,tx);receive_deadline=now_ms+3u+RREP_ACK_TIMEOUT_MS;
        assert(mesh_listen_for_route_reply_ack(&reply,0u)==-ETIMEDOUT);
        assert(ack_captures==0u&&mesh_runtime.route_reply_ack_expectation.active);
        assert(now_ms==receive_deadline+1u);
    }
}

static void end_frame_at_deadline(void)
{
    assert(air_count==1u);
    const uint64_t duration=air[0].end-air[0].start;
    air[0].end=receive_deadline;air[0].start=receive_deadline-duration;
}
static void test_nonmatching_ack_at_deadline_is_never_success(void)
{
    for(unsigned variant=0u;variant<2u;variant++) {
        reset_fixture(1000u);struct mesh_outbound reply,ack;prepare_downstream_ack(&reply,&ack);
        if(variant==0u)ack.packet.session_id++;
        schedule(&ack,CLICKER_C,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        if(variant==1u)air[0].wire[0]^=0xffu;
        receive_deadline=now_ms+3u+RREP_ACK_TIMEOUT_MS;end_frame_at_deadline();
        assert(mesh_listen_for_route_reply_ack(&reply,0u)==-ETIMEDOUT);
        assert(ack_captures==0u&&mesh_runtime.route_reply_ack_expectation.active);
        assert(now_ms==receive_deadline+1u&&receives==1u);
    }
}
static void test_nonmatching_route_frame_at_deadline_is_never_success(void)
{
    for(unsigned variant=0u;variant<3u;variant++) {
        reset_fixture(1000u);struct exchange exchange;prepare_exchange(&exchange);
        struct mesh_outbound bad=variant==1u?exchange.reflected_request:exchange.reply;
        if(variant==0u)bad.packet.session_id++;
        schedule(&bad,ANCHOR_A,10u,DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        if(variant==2u)air[0].wire[0]^=0xffu;
        receive_deadline=now_ms+3u+100u;end_frame_at_deadline();bool captured=true;
        assert(mesh_listen_for_route_reply(GATEWAY_ID,"last-reject",100u,&exchange.identity,&captured)==-ETIMEDOUT);
        assert(!captured&&queue_count==0u&&yields==0u&&now_ms==receive_deadline+1u);
    }
}
static void test_route_ready_generation_without_queued_frame_succeeds(void)
{
    reset_fixture(1000u);change_route_generation=true;
    receive_deadline=now_ms+3u+100u;bool captured=false;
    assert(mesh_listen_for_route_reply(GATEWAY_ID,"ready",100u,NULL,&captured)==0);
    assert(captured&&queue_count==0u&&receives==1u&&now_ms<receive_deadline);
}
static void test_hard_receive_error_and_wrapping_deadline_stay_truthful(void)
{
    for(unsigned wrap=0u;wrap<2u;wrap++) {
        reset_fixture(wrap?UINT32_MAX-70u:1000u);
        injected_rx_error=-EIO;receive_deadline=now_ms+3u+100u;bool captured=true;
        assert(mesh_listen_for_route_reply(GATEWAY_ID,"hard-error",100u,NULL,&captured)==-EIO);
        assert(!captured&&queue_count==0u&&now_ms==receive_deadline+1u);
    }
    reset_fixture(1000u);struct mesh_outbound reply,ack;prepare_downstream_ack(&reply,&ack);
    injected_rx_error=-EIO;receive_deadline=now_ms+3u+RREP_ACK_TIMEOUT_MS;
    assert(mesh_listen_for_route_reply_ack(&reply,0u)==-EIO);
    assert(ack_captures==0u&&mesh_runtime.route_reply_ack_expectation.active);
}

static void prepare_combined_at(uint64_t at,struct mesh_outbound *reply)
{
    struct mesh_outbound ack;
    /* Derive the same real cold discovery lead-in on each clock. Keep the
     * 64-bit host clock monotonic while production deadlines wrap at 32 bits. */
    reset_fixture(1000u);prepare_downstream_ack(reply,&ack);
    if(reply->earliest_tx_valid)mesh_wait_until_ms(reply->earliest_tx_ms);
    const uint64_t lead_in=now_ms-1000u;
    assert(at>=lead_in);
    reset_fixture(at-lead_in);prepare_downstream_ack(reply,&ack);
    if(reply->earliest_tx_valid)mesh_wait_until_ms(reply->earliest_tx_ms);
    assert(now_ms==at);automatic_downstream_ack=true;
}
static void test_lost_ack_keeps_shared_contact_for_exact_retry(void)
{
    const uint64_t starts[]={20000u,(uint64_t)UINT32_MAX-80u,
        (uint64_t)UINT32_MAX+1u-MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS};
    for(unsigned i=0u;i<sizeof(starts)/sizeof(starts[0]);i++) {
        struct mesh_outbound reply;prepare_combined_at(starts[i],&reply);
        drop_ack_count=1u;
        const uint32_t owner_end=k_uptime_get_32()+1000u;
        assert(mesh_send_route_reply_outbound_action(&reply,false,0u,"lost-ack",&owner_end));
        assert(reply_transmissions==2u&&dropped_acks==1u&&ack_captures==1u);
        assert(wake_attempts==0u&&receives==2u&&last_tx_end<=starts[i]+1000u);
        assert(last_tx_deadline>starts[i]&&last_tx_deadline<starts[i]+1000u);
        assert(!mesh_runtime.route_reply_ack_expectation.active);
        assert(mesh_c5_contact.state==C5_CONTACT_NONE&&!radio_owned&&!scan_stopped);
    }
}
static void test_unbounded_owner_keeps_existing_retry_contact(void)
{
    struct mesh_outbound reply;prepare_combined_at(20000u,&reply);drop_ack_count=1u;
    assert(mesh_send_route_reply_outbound_action(&reply,false,0u,"ordinary",NULL));
    assert(reply_transmissions==2u&&ack_captures==1u&&wake_attempts==0u&&last_tx_deadline==0u);
}
static void test_original_owner_end_still_bounds_retry_with_live_contact(void)
{
    const uint64_t starts[]={20000u,(uint64_t)UINT32_MAX+1u-90u};
    for(unsigned i=0u;i<sizeof(starts)/sizeof(starts[0]);i++) {
        struct mesh_outbound reply;prepare_combined_at(starts[i],&reply);drop_ack_count=1u;
        const uint32_t owner_end=k_uptime_get_32()+90u;
        if(i==1u)assert(owner_end==0u);
        mesh_c5_contact_exchange(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,
            mesh_c5_exchange_expires_at(C5_CONTACT_PURPOSE_ROUTE_REPLY),"owner-shorter");
        assert(mesh_send_route_reply_train_to_hop(&reply,&owner_end)==-ETIMEDOUT);
        assert(reply_transmissions==1u&&dropped_acks==1u&&ack_captures==0u&&wake_attempts==0u);
        assert(last_tx_end<starts[i]+90u&&now_ms>starts[i]+90u);
        /* The separate ACK wait may finish after the owner end, but a second
         * TX must not start even though the longer peer contact is live. */
        assert(mesh_c5_contact_active(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,k_uptime_get_32()));
        assert(!radio_owned&&!scan_stopped);
    }
}
static void test_foreign_contact_replacement_cannot_authorize_retry(void)
{
    for(unsigned replacement=1u;replacement<=2u;replacement++) {
        struct mesh_outbound reply;prepare_combined_at(20000u,&reply);drop_ack_count=1u;
        const uint32_t owner_end=k_uptime_get_32()+1000u;
        mesh_c5_contact_exchange(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,
            mesh_c5_exchange_expires_at(C5_CONTACT_PURPOSE_ROUTE_REPLY),"initial");
        replace_contact_on_rx=replacement;
        assert(mesh_send_route_reply_train_to_hop(&reply,&owner_end)==-ENOTCONN);
        assert(reply_transmissions==1u&&dropped_acks==1u&&ack_captures==0u&&wake_attempts==0u);
        assert(mesh_c5_contact.peer_id==(replacement==1u?ANCHOR_A:CLICKER_C));
        assert(mesh_c5_contact.purpose==(replacement==1u?C5_CONTACT_PURPOSE_ROUTE_REPLY:C5_CONTACT_PURPOSE_UPLINK));
        assert(mesh_c5_contact_peer_active(mesh_c5_contact.peer_id,k_uptime_get_32()));
        mesh_c5_contact_clear_matching(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,"stale-clear");
        assert(mesh_c5_contact.state!=C5_CONTACT_NONE);
        mesh_c5_contact_clear_matching(mesh_c5_contact.peer_id,mesh_c5_contact.purpose,"owner-clear");
        assert(mesh_c5_contact.state==C5_CONTACT_NONE);
    }
}
static void test_expired_or_unaccepted_contact_never_reopens_for_retry(void)
{
    for(unsigned variant=0u;variant<3u;variant++) {
        struct mesh_outbound reply;prepare_combined_at((uint64_t)UINT32_MAX,&reply);
        if(variant==0u) {
            mesh_c5_contact_accept(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,0u,"wrap-zero");
            assert(mesh_c5_contact_active(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,k_uptime_get_32()));
            now_ms++;assert(k_uptime_get_32()==0u);
        } else if(variant==1u) {
            mesh_c5_contact_open(CLICKER_C,C5_CONTACT_PURPOSE_ROUTE_REPLY,12u,true,
                k_uptime_get_32()+1000u,"unaccepted");
        }
        assert(mesh_send_c5_control_attempt(&reply,C5_CONTACT_PURPOSE_ROUTE_REPLY,
            MESH_C5_CONTROL_ACCEPTED_EXCHANGE,"closed",now_ms+100u,NULL,NULL,
            FW_C5_TX_INTENT_CAUSAL_RESPONSE,false)==-ENOTCONN);
        assert(reply_transmissions==0u&&wake_attempts==0u&&receives==0u);
        if(variant!=1u)assert(mesh_c5_contact.state==C5_CONTACT_NONE);
        else assert(mesh_c5_contact.state==C5_CONTACT_WAKE_PENDING&&!mesh_c5_contact.accepted);
    }
}
static void reset_full_payload_fixture(uint64_t start)
{
    reset_fixture(start);
    queue_put_attempts = queue_put_rejections = 0u;
    memset(queued, 0, sizeof(queued));
    memset(&anchor_uwb_session, 0, sizeof(anchor_uwb_session));
}

static void build_full_payload(struct mesh_outbound *out, size_t length)
{
    const struct mesh_smoke_fast_payload_input input = {
        .packet_id = 71u, .build_uptime_ms = k_uptime_get_32(),
        .origin_id = CLICKER_C, .target_id = GATEWAY_ID,
        .selected_parent_id = DEVICE_ID, .attempt = 1u,
        .device_role = ROLE_ANCHOR, .mesh_channel = UWB_CHANNEL_WAKE_CONTACT,
        .flags = 7u,
    };
    struct mesh_smoke_fast_payload decoded;
    size_t produced = 0u;

    memset(out, 0, sizeof(*out));
    assert(length >= 255u && length <= UWB_MESH_MAX_PAYLOAD_LEN);
    /* The current production padding helper adds its final two-byte TLV
     * header beyond target. Target900 is the real transmitter's902-byte body. */
    assert(mesh_smoke_fast_payload_append(out->payload, sizeof(out->payload),
               &produced, &input, length - 2u) == PROTO_OK);
    assert(produced == length);
    assert(mesh_smoke_fast_payload_decode(out->payload, produced, &decoded) == PROTO_OK);
    assert(decoded.packet_id == input.packet_id);
    out->packet = (struct proto_packet) {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = CLICKER_C, .dst_id = GATEWAY_ID,
        .session_id = 73u, .seq = 79u, .ttl = MESH_DEFAULT_TTL,
        .message_age_ms = 37u, .payload_len = (uint16_t)produced,
    };
    out->payload_len = (uint16_t)produced;
    out->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    out->next_hop_id = DEVICE_ID;
    assert(mesh_packet_rx_envelope_validate(&out->packet, out->payload,
               out->payload_len, CLICKER_C, DEVICE_ID, GATEWAY_ID,
               UWB_CHANNEL_WAKE_CONTACT, true) == PROTO_OK);
}

static void test_route_listener_preserves_full_extended_payloads(void)
{
    const size_t lengths[] = {255u, 256u, 902u, UWB_MESH_MAX_PAYLOAD_LEN};

    assert(UWB_MESH_MAX_PAYLOAD_LEN == 957u);
    for (size_t i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        struct mesh_outbound packet;
        bool captured = true;

        reset_full_payload_fixture(1000u);
        build_full_payload(&packet, lengths[i]);
        receive_deadline = now_ms + 3u + 100u;
        schedule(&packet, CLICKER_C, 10u, DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        assert(mesh_listen_for_route_reply(CLICKER_C, "full-payload", 100u,
                                           NULL, &captured) == 0);
        assert(!captured); /* Uplink admission does not complete route discovery. */
        assert(queue_count == 1u && queue_put_attempts == 1u);
        assert(queue_put_rejections == 0u && work_submissions == 1u);
        assert(queued[0].payload_len == lengths[i]);
        assert(queued[0].packet.payload_len == lengths[i]);
        assert(memcmp(queued[0].payload, packet.payload, lengths[i]) == 0);
        assert(queued[0].packet.msg_type == MSG_MESH_DATA);
        assert(queued[0].packet.src_id == CLICKER_C);
        assert(queued[0].packet.dst_id == GATEWAY_ID);
        assert(queued[0].packet.seq == packet.packet.seq);
        assert(queued[0].packet.session_id == packet.packet.session_id);
        assert(queued[0].packet.message_age_ms == 37u);
        assert(queued[0].previous_hop_id == CLICKER_C);
        assert(queued[0].radio_channel == UWB_CHANNEL_WAKE_CONTACT);
        assert(queued[0].received_at_valid && !queued[0].current_channel9_plan_valid);
        assert(queued[0].received_at_ms == (uint32_t)last_frame_end);
        assert(queued[0].first_received_at_ms == last_frame_end);
        assert(now_ms < receive_deadline);
        assert(last_tx_end == 0u && ack_captures == 0u);
        assert(!radio_owned && !scan_stopped);
    }
}

static void test_full_payload_rejections_preserve_queue_and_deadline(void)
{
    for (unsigned mutation = 0u; mutation < 4u; ++mutation) {
        struct mesh_outbound packet;
        struct mesh_rx_pending before[sizeof(queued) / sizeof(queued[0])];
        bool captured = true;

        reset_full_payload_fixture(1000u);
        build_full_payload(&packet, UWB_MESH_MAX_PAYLOAD_LEN);
        if (mutation == 0u) {
            packet.packet.flags &= (uint8_t)~FLAG_GATEWAY_ACK_REQUIRED;
        } else if (mutation == 1u) {
            packet.packet.dst_id = ANCHOR_A;
        } else if (mutation == 2u) {
            /* MESH_DATA is opaque at this boundary. A report requires real
             * TLVs: use an oversized CLICK_REPORT with a malformed first TLV,
             * retaining an otherwise valid gateway-uplink header. */
            packet.packet.msg_type = MSG_CLICK_REPORT;
            packet.packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK;
            packet.payload[0] = TLV_CLICKER_ID;
            packet.payload[1] = 7u; /* mandatory64-bit identity has wrong width */
        } else {
            queue_count = sizeof(queued) / sizeof(queued[0]);
            memset(queued, 0x5a, sizeof(queued));
        }
        memcpy(before, queued, sizeof(before));
        receive_deadline = now_ms + 3u + 100u;
        schedule(&packet, CLICKER_C, 10u, DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
        assert(mesh_listen_for_route_reply(CLICKER_C, "long-rejected", 100u,
                                           NULL, &captured) == -ETIMEDOUT);
        assert(!captured && now_ms == receive_deadline + 1u);
        assert(memcmp(queued, before, sizeof(before)) == 0);
        assert(last_tx_end == 0u && ack_captures == 0u);
        assert(!radio_owned && !scan_stopped);
        if (mutation == 3u) {
            assert(queue_count == sizeof(queued) / sizeof(queued[0]));
            assert(queue_put_attempts == 1u && queue_put_rejections == 1u);
            assert(work_submissions == 1u); /* Drain the already-full queue. */
        } else {
            assert(queue_count == 0u && queue_put_attempts == 0u);
            assert(queue_put_rejections == 0u && work_submissions == 0u);
        }
    }
}

static void test_actual_queue_revalidates_long_envelopes_and_reports_full_queue(void)
{
    struct mesh_outbound packet;
    struct mesh_rx_pending before[sizeof(queued) / sizeof(queued[0])];
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t length;
    bool valid;

    reset_full_payload_fixture((UINT64_C(1) << 32) + 100u);
    build_full_payload(&packet, 256u);
    packet.packet.flags &= (uint8_t)~FLAG_GATEWAY_ACK_REQUIRED;
    assert(uwb_mesh_frame_encode(NETWORK_ID, CLICKER_C, DEVICE_ID,
               &packet.packet, packet.payload, frame, sizeof(frame), &length) == PROTO_OK);
    valid = true;
    assert(!mesh_queue_from_frame_at_internal(frame, length, 90u, -60, true,
               UWB_CHANNEL_WAKE_CONTACT, (uint32_t)now_ms, 0u, NULL, 0u,
               true, &valid, NULL, NULL));
    assert(!valid && queue_count == 0u && work_submissions == 0u);
    build_full_payload(&packet, 256u);
    assert(uwb_mesh_frame_encode(NETWORK_ID, CLICKER_C, DEVICE_ID,
               &packet.packet, packet.payload, frame, sizeof(frame), &length) == PROTO_OK);
    queue_count = sizeof(queued) / sizeof(queued[0]);
    memset(queued, 0x5a, sizeof(queued));
    memcpy(before, queued, sizeof(before));
    valid = false;
    assert(!mesh_queue_from_frame_at_internal(frame, length, 90u, -60, true,
               UWB_CHANNEL_WAKE_CONTACT, (uint32_t)now_ms, 0u, NULL, 0u,
               true, &valid, NULL, NULL));
    assert(valid); /* Parsed validity is distinct from successful admission. */
    assert(memcmp(queued, before, sizeof(before)) == 0);
    assert(queue_put_rejections == 1u && work_submissions == 0u);
}
static void test_gateway_table_over255_uses_actual_listener_and_queue(void)
{
    struct mesh_outbound table = {0};
    struct discovery_assignment_entry entries[16];
    struct discovery_assignment_entry decoded[16];
    struct operation_policy policy = {
        .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
        .value.assignment = {
            .expected_anchor_count = 16u,
            .operation_budget_ms = OPERATION_POLICY_ASSIGNMENT_DEFAULT_BUDGET_MS,
            .response_spread_ms = DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
            .ram_only_iteration = true,
        },
    };
    size_t payload_len = 0u, decoded_count = 0u;
    uint8_t slot_count = 0u;
    bool captured = true;

    reset_full_payload_fixture(1000u);
    for (size_t i = 0u; i < 16u; ++i) {
        entries[i].anchor_id = DEVICE_ID + i;
        entries[i].hash = discovery_assignment_hash(entries[i].anchor_id);
        entries[i].slot = (uint8_t)i;
    }
    assert(mesh_append_command_id(table.payload, sizeof(table.payload),
               &payload_len, CMD_ASSIGN_DISCOVERY_SLOTS) == PROTO_OK);
    assert(tlv_append_u8(table.payload, sizeof(table.payload), &payload_len,
               TLV_COMMAND_SCOPE, CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(table.payload, sizeof(table.payload), &payload_len,
               TLV_COMMAND_RESPONSE_MODE, CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(table.payload, sizeof(table.payload), &payload_len,
               TLV_COMMAND_SEQ, 101u) == PROTO_OK);
    assert(tlv_append_u32(table.payload, sizeof(table.payload), &payload_len,
               TLV_FLOOD_EPOCH_ID, 102u) == PROTO_OK);
    assert(tlv_append_u32(table.payload, sizeof(table.payload), &payload_len,
               TLV_COMMAND_EXPIRY_S, DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S) == PROTO_OK);
    assert(discovery_assignment_append_control_tlvs(table.payload,
               sizeof(table.payload), &payload_len,
               DISCOVERY_ASSIGNMENT_PHASE_TABLE, 7u) == PROTO_OK);
    assert(operation_policy_append_tlv(table.payload, sizeof(table.payload),
               &payload_len, &policy) == PROTO_OK);
    table.packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND, .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID, .session_id = 101u, .seq = 103u,
    };
    table.packet.flags = FLAG_DIAGNOSTIC;
    table.packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    table.packet.payload_len = table.payload_len = (uint16_t)payload_len;
    table.next_hop_id = MESH_BROADCAST_ID;
    table.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    assert(gateway_command_append_default_flood_controls(&table) == PROTO_OK);
    payload_len = table.payload_len;
    assert(discovery_assignment_append_table_tlvs(table.payload,
               sizeof(table.payload), &payload_len, entries, 16u) == PROTO_OK);
    table.packet.payload_len = table.payload_len = (uint16_t)payload_len;
    assert(payload_len > UINT8_MAX && payload_len <= UWB_MESH_MAX_PAYLOAD_LEN);
    assert(mesh_relay_command_packet_envelope_valid(&mesh_runtime, &table.packet,
               table.payload, table.payload_len, GATEWAY_ID));
    permit_table_in_inactive_survey = true;
    receive_deadline = now_ms + 3u + 100u;
    schedule(&table, GATEWAY_ID, 10u, DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    assert(mesh_listen_for_route_reply(GATEWAY_ID, "gateway-command-wake-followup",
                                       100u, NULL, &captured) == -EAGAIN);
    permit_table_in_inactive_survey = false;
    assert(!captured && queue_count == 1u && work_submissions == 1u);
    assert(queue_put_attempts == 1u && queue_put_rejections == 0u);
    assert(queued[0].payload_len == payload_len);
    assert(queued[0].packet.payload_len == payload_len);
    assert(queued[0].packet.src_id == GATEWAY_ID);
    assert(queued[0].packet.msg_type == MSG_COMMAND);
    assert(queued[0].previous_hop_id == GATEWAY_ID);
    assert(memcmp(queued[0].payload, table.payload, payload_len) == 0);
    assert(discovery_assignment_parse_table_tlvs(queued[0].payload,
               queued[0].payload_len, decoded, 16u, &decoded_count,
               &slot_count) == PROTO_OK);
    assert(decoded_count == 16u);
    for (size_t i = 0u; i < decoded_count; ++i) {
        assert(decoded[i].anchor_id == entries[i].anchor_id);
        assert(decoded[i].hash == entries[i].hash && decoded[i].slot == entries[i].slot);
    }
    assert(now_ms < receive_deadline && last_tx_end == 0u);
    assert(!radio_owned && !scan_stopped);
}

static void test_oversized_and_truncated_full_frames_are_rejected(void)
{
    struct mesh_outbound packet;
    struct mesh_frame_parse_context parsed;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN + 1u];
    size_t length;
    bool valid = true, captured = true;

    reset_full_payload_fixture(1000u);
    build_full_payload(&packet, UWB_MESH_MAX_PAYLOAD_LEN);
    assert(uwb_mesh_frame_encode(NETWORK_ID, CLICKER_C, DEVICE_ID,
               &packet.packet, packet.payload, frame, sizeof(frame), &length) == PROTO_OK);
    assert(length == UWB_MESH_MAX_FRAME_LEN);
    frame[length++] = 0xccu;
    assert(uwb_mesh_frame_decode(frame, length, NETWORK_ID, DEVICE_ID,
               &parsed.previous_hop_id, &parsed.packet, parsed.payload,
               sizeof(parsed.payload), &parsed.payload_len) != PROTO_OK);
    assert(!mesh_queue_from_frame_at_internal(frame, length, 90u, -60, true,
               UWB_CHANNEL_WAKE_CONTACT, (uint32_t)now_ms, 0u, NULL, 0u,
               true, &valid, NULL, NULL));
    assert(!valid && queue_count == 0u && queue_put_attempts == 0u);
    assert(work_submissions == 0u && last_tx_end == 0u);

    reset_full_payload_fixture(1000u);
    build_full_payload(&packet, UWB_MESH_MAX_PAYLOAD_LEN);
    receive_deadline = now_ms + 3u + 100u;
    schedule(&packet, CLICKER_C, 10u, DWM3000_TIMING_PHY_CH5_MESH_CONTROL);
    --air[0].length; /* Actual decoder sees a short frame, never a fake rejection. */
    assert(mesh_listen_for_route_reply(CLICKER_C, "truncated-uplink", 100u,
                                       NULL, &captured) == -ETIMEDOUT);
    assert(!captured && queue_count == 0u && queue_put_attempts == 0u);
    assert(now_ms == receive_deadline + 1u);
    assert(work_submissions == 0u && last_tx_end == 0u);
    assert(!radio_owned && !scan_stopped);
}
int main(void)
{
    test_empty_original_deadline();
    test_transit_reply_uses_original_requester();
    test_foreign_replies_do_not_end_or_extend_wait();
    test_foreign_reply_then_exact_reply_preserves_owner();
    test_reflections_and_malformed_requests_preserve_original_deadline();
    test_reflection_then_valid_reply_is_not_lost();
    test_legitimate_competing_request_still_yields();
    test_downstream_ack_uses_complete_matching_control_phy();
    test_wrong_ack_identity_keeps_waiting_for_exact_ack();
    test_ack_partial_airtime_and_wrong_phy_are_not_delivery();
    test_nonmatching_ack_at_deadline_is_never_success();
    test_nonmatching_route_frame_at_deadline_is_never_success();
    test_route_ready_generation_without_queued_frame_succeeds();
    test_hard_receive_error_and_wrapping_deadline_stay_truthful();
    test_lost_ack_keeps_shared_contact_for_exact_retry();
    test_unbounded_owner_keeps_existing_retry_contact();
    test_original_owner_end_still_bounds_retry_with_live_contact();
    test_foreign_contact_replacement_cannot_authorize_retry();
    test_expired_or_unaccepted_contact_never_reopens_for_retry();
    test_route_listener_preserves_full_extended_payloads();
    test_full_payload_rejections_preserve_queue_and_deadline();
    test_actual_queue_revalidates_long_envelopes_and_reports_full_queue();
    test_gateway_table_over255_uses_actual_listener_and_queue();
    test_oversized_and_truncated_full_frames_are_rejected();
    puts("reactive route listener regressions passed");return 0;
}
