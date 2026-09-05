/* Execute the production C5 batch include. Only hardware, queue and work
 * boundaries are faked; packet encoding, identity, ACK admission and routes
 * use the real native core. RX requires complete frame airtime in its window. */
#include "mesh_relay.h"
#include "app_mesh_report_delivery_state.h"
#include "uwb.h"
#include "app_mesh_ch9_ack.h"
#include "app_gateway_ble_stream.h"
#include "firmware_state_machines.h"
#include "dwm3000_timing.h"
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
#define MESH_CLICK_SLOT_WAKE_LEAD_MS 30u
#define MESH_RX_QUEUE_DEPTH 4u
#define REPORT_TX_QUEUE_DEPTH 4u
#define REPORT_TX_RETRY_DELAY_MS 10u
#define BUILD_ASSERT _Static_assert
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define BIT(i) (1u << (i))
#define K_FOREVER 0
#define RADIO_GUARD_UWB_CLIENT_MESH_RX 1
#define RADIO_GUARD_UWB_CLIENT_MESH_TX 2
#define UWB_MESH_TX_TIMEOUT_MS 20u
#define C5_CONTACT_PURPOSE_UPLINK 0
#define APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY 0
#define APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA 0
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define IS_ENABLED(config) (config)
#define MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS 1u

struct radio_guard_uwb_lease { bool active; };
enum dwm3000_rx_failure {
    DWM3000_RX_FAILURE_NONE = 0,
    DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT = 1,
    DWM3000_RX_FAILURE_SFD_TIMEOUT = 2,
    DWM3000_RX_FAILURE_FRAME_TIMEOUT = 3,
    DWM3000_RX_FAILURE_CRC_OR_PHY = 4,
    DWM3000_RX_FAILURE_BAD_FRAME = 5,
};
bool app_wake_train_politeness_rx_activity(int ret, enum dwm3000_rx_failure failure);
enum mesh_standard_wake_probe_result {
    MESH_STANDARD_WAKE_PROBE_NONE,
    MESH_STANDARD_WAKE_PROBE_CLICK,
};
struct app_mesh_queue_head_token { bool active; };
struct app_mesh_rf_retry_key { uint16_t seq; };
struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;
    uint64_t previous_hop_id;
    uint8_t radio_channel;
};
struct fake_callbacks {
    int (*anchor_delivery_gateway_confirmed)(const struct proto_packet *, const uint8_t *);
};
static struct fake_callbacks *mesh_report_callbacks;
static struct mesh_relay mesh_runtime;
static struct mesh_report_delivery_state mesh_report_delivery;
static struct mesh_outbound report_tx_worker_scratch;
static struct app_mesh_ch9_ack_table mesh_ch9_ack_table;
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
static bool mesh_route_waiting_tx_valid;
static unsigned route_requests;
static int completion_error;
static uint64_t mesh_c5_rx_burst_peer;
static int mesh_rx_msgq, mesh_send_scratch_lock, report_tx_queue_overflow_lock;
static struct mesh_outbound queue[REPORT_TX_QUEUE_DEPTH], sent[4];
static unsigned queue_count, sent_count, retired_count, rx_count;
static unsigned claims, releases, parks, stops, starts, watchdog_stops, locks;
static uint32_t now_ms, scheduled_ms;
static int claim_error, configure_error, send_error, receive_error, park_error;
static uint8_t credit, final_mask;
static bool refuse, corrupt_digest, wrong_peer, stale_ack;
static bool duplicate_first_ack, follower_refusal;
static uint8_t response_depth;
static unsigned ack_reads;
static unsigned receive_calls, typed_phy_remaining, standard_probes, click_handoffs;
static unsigned click_contact_clears;
static unsigned rx_work_submissions;
static bool repeat_typed_phy, capture_click, radio_owned, fixed_ack_deadline;
static bool standard_probe_clipped;
static uint32_t first_ack_deadline_ms, click_observed_ms;
static uint32_t typed_phy_duration_ms, standard_probe_duration_ms;
static uint8_t click_quality;
static struct uwb_wake_claim_frame observed_click;
static struct mesh_outbound unrelated_rx, queued_rx;
static bool inject_unrelated_rx;
static uint16_t last_retry_seq;
static unsigned retry_round_calls;
static unsigned causal_send_calls, causal_send_allowed;
static int causal_send_error;
static struct mesh_outbound causal_sent_ack;
static bool handoff_held;
static unsigned handoff_begins, handoff_ends;
static int handoff_error;
static bool capture_mode;
static unsigned follower_index, follower_count;
static uint32_t follower_start_ms[3], follower_duration_ms;
static int mesh_report_rf_retry_bank;
static struct gateway_ble_stream_state gateway_ble_stream_state;
static int gateway_ble_stream_lock;
typedef int k_spinlock_key_t;
static int k_spin_lock(int *lock) { (void)lock; return 0; }
static void k_spin_unlock(int *lock, int key) { (void)lock; (void)key; }

/* CMake extracts these exact functions from app_mesh_report.c and
 * app_gateway_ble.c, and reconfigures when either source changes. */
#include "c5_batch_production_helpers.inc"

static uint32_t k_uptime_get_32(void) { return now_ms; }
static uint64_t k_uptime_get(void) { return now_ms; }
static void k_msleep(uint32_t ms) { now_ms += ms; }
static uint32_t sys_rand32_get(void) { return 0u; }
static bool uptime_deadline_reached(uint32_t now, uint32_t deadline) { return (int32_t)(now-deadline) >= 0; }
static uint32_t uptime_ms_until_deadline(uint32_t now, uint32_t deadline) { return deadline-now; }
static unsigned report_tx_queue_used(void) { return queue_count; }
static unsigned k_msgq_num_used_get(const int *q) { (void)q; return rx_count; }
static void k_mutex_lock(int *lock, int timeout) { (void)lock; (void)timeout; assert(locks++ == 0u); }
static void k_mutex_unlock(int *lock) { (void)lock; assert(locks-- == 1u); }
static void mesh_stop_role_scan(void) { stops++; }
static void mesh_restart_role_scan(void) { starts++; }
static int mesh_route_refresh_begin_radio_control(void *ctx)
{ (void)ctx; handoff_begins++; assert(!handoff_held); if (handoff_error) return handoff_error; handoff_held=true; return 0; }
static void mesh_route_refresh_end_radio_control(void *ctx)
{ (void)ctx; assert(handoff_held); handoff_held=false; handoff_ends++; }
static int mesh_transport_radio_claim(int client, const char *why, struct radio_guard_uwb_lease *lease)
{ (void)why; if (client==RADIO_GUARD_UWB_CLIENT_MESH_RX) assert(handoff_held); claims++; lease->active = claim_error == 0; radio_owned=lease->active; return claim_error; }
static int mesh_transport_radio_finish(struct radio_guard_uwb_lease *lease, int ret)
{ assert(lease->active && radio_owned); lease->active = false; radio_owned=false; releases++; return ret; }
static int mesh_radio_idle_with_bounded_recovery(const char *why) { (void)why; parks++; return park_error; }
static int dwm3000_driver_configure_wake_mesh_control_mode(void) { return configure_error; }
static void app_watchdog_stop_feeding(void) { watchdog_stops++; }
static void app_watchdog_note_radio_progress(void) { }
static void report_tx_schedule(uint32_t ms) { scheduled_ms = ms; }
static void report_tx_schedule_backoff(uint32_t ms) { report_tx_schedule(ms); }
static int report_tx_queue_begin_head(struct mesh_outbound *out, struct app_mesh_queue_head_token *token)
{ if (!queue_count) return -ENOENT; *out = queue[0]; token->active = true; return 0; }
static int report_tx_queue_abort_head(struct app_mesh_queue_head_token *token) { token->active = false; return 0; }
static int report_tx_queue_commit_head(struct app_mesh_queue_head_token *token, struct mesh_outbound *out)
{
    assert(token->active && queue_count);
    assert(mesh_report_delivery.count < MESH_REPORT_DELIVERY_CAPACITY);
    k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    struct mesh_report_delivery_entry *entry =
        &mesh_report_delivery.entries[mesh_report_delivery.count++];
    entry->outbound = *out;
    entry->acked = false;
    token->active = false;
    queue_count--;
    memmove(queue, queue+1, queue_count*sizeof(queue[0]));
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return 0;
}
static bool mesh_outbound_ready_for_tx(const struct mesh_outbound *out, uint32_t now)
{ return !out->earliest_tx_valid || uptime_deadline_reached(now, out->earliest_tx_ms); }
static void mesh_outbound_refresh_age(struct mesh_outbound *out, uint32_t now) { (void)out; (void)now; }
static int mesh_schedule_route_request(uint64_t peer, const char *reason)
{ (void)reason; assert(peer==GATEWAY_ID); route_requests++; return 0; }
static int mesh_send_outbound_preconfigured_ch9_locked(const struct mesh_outbound *out, const char *why, void *ctx)
{ (void)why; (void)ctx; if (send_error) return send_error; assert(sent_count<4); sent[sent_count++]=*out; now_ms++; return 0; }
static int mesh_anchor_range_report_note_gateway_confirmed(const struct proto_packet *packet, const uint8_t *digest)
{ (void)packet; (void)digest; return -ENOENT; }
static int app_node_comm_note_gateway_confirmed_digest_at(const struct proto_packet *packet, const uint8_t *digest, uint64_t now)
{ (void)packet; (void)digest; (void)now; assert(locks==0u); if (completion_error) return completion_error; retired_count++; return 0; }
static struct app_mesh_rf_retry_key mesh_rf_retry_packet_key(const struct proto_packet *packet, int operation)
{ (void)operation; return (struct app_mesh_rf_retry_key){packet->seq}; }
static uint32_t mesh_rf_retry_bank_next_delay_ms(int *bank, const struct app_mesh_rf_retry_key *key, int policy, const char *why)
{ (void)bank; (void)policy; (void)why; last_retry_seq=key->seq; retry_round_calls++; return 40u; }
static int mesh_probe_standard_wake_claim(uint8_t *frame, size_t cap,
    struct uwb_wake_claim_frame *claim, uint8_t *quality, uint32_t *observed,
    bool allow_relayed_gateway_control, int64_t deadline_ms)
{
    (void)frame; (void)allow_relayed_gateway_control;
    assert(cap != 0u && radio_owned && locks == 1u);
    assert(deadline_ms >= (int64_t)now_ms);
    if (fixed_ack_deadline) assert(deadline_ms == first_ack_deadline_ms);
    standard_probes++;
    uint32_t remaining = (uint32_t)(deadline_ms - now_ms);
    standard_probe_clipped |= remaining < standard_probe_duration_ms;
    now_ms += MIN(standard_probe_duration_ms, remaining);
    if (!capture_click) return MESH_STANDARD_WAKE_PROBE_NONE;
    *claim = observed_click;
    *quality = click_quality;
    *observed = click_observed_ms = now_ms;
    return MESH_STANDARD_WAKE_PROBE_CLICK;
}
static void mesh_c5_contact_clear(const char *reason)
{ (void)reason; assert(!radio_owned && locks == 0u); click_contact_clears++; }
static bool mesh_handoff_anchor_click_claim(const struct uwb_wake_claim_frame *claim,
    uint8_t quality, uint32_t observed)
{
    assert(!radio_owned && locks == 0u && releases == claims && parks == releases);
    assert(claim->clicker_id == observed_click.clicker_id);
    assert(claim->click_event_id == observed_click.click_event_id);
    assert(claim->nonce == observed_click.nonce);
    assert(quality == click_quality && observed == click_observed_ms);
    click_handoffs++;
    return true;
}
static void status_debug_printf(const char *format, ...) { (void)format; }
#define status_debug_note(...) ((void)0)
#define fw_delivery_loss_note_sent(...) ((void)0)
#define app_stack_workload_diag_relay_release(...) ((void)0)
#define app_mesh_report_click_participant(...) true
#define app_mesh_report_click_listen_active(...) true
#define mesh_send_route_wake_train_with_duration(...) 0
#define mesh_wait_for_c5_control_followup_turnaround(...) ((void)0)

static void make_ack(struct mesh_outbound *ack, uint8_t mask)
{
    memset(ack,0,sizeof(*ack));
    uint64_t peer=mesh_report_delivery.next_hop_id;
    ack->packet=(struct proto_packet){.msg_type=peer==GATEWAY_ID ? MSG_GATEWAY_ACK : MSG_MESH_HOP_ACK,
        .flags=peer==GATEWAY_ID ? FLAG_GATEWAY_ACK : 0u,.src_id=peer,
        .dst_id=DEVICE_ID,.session_id=mesh_report_delivery.entries[0].outbound.packet.session_id,
        .seq=200u,.ttl=1u};
    size_t len=0;
    unsigned first=0;
    while (mask != 0u && (mask & BIT(first)) == 0u) first++;
    assert(mesh_append_requested_seq(ack->payload,sizeof(ack->payload),&len,
        mesh_report_delivery.entries[first].outbound.packet.seq)==PROTO_OK);
    struct mesh_ack_flow_control flow={.depth=response_depth,.credit=credit,
        .depth_valid=true,.credit_valid=true,.retry_after_valid=refuse,.retry_after_ms=50u};
    assert(mesh_append_ack_flow_control(ack->payload,sizeof(ack->payload),&len,&flow)==PROTO_OK);
    for (uint8_t i=0;i<mesh_report_delivery.count;i++) if (mask & BIT(i)) {
        struct mesh_outbound out=mesh_report_delivery.entries[i].outbound;
        if (corrupt_digest) out.packet.flags ^= FLAG_DIAGNOSTIC;
        if (stale_ack) out.packet.session_id++;
        assert(mesh_append_ack_semantic_identity(ack->payload,sizeof(ack->payload),&len,
            &out.packet,out.payload,out.payload_len)==PROTO_OK);
    }
    if ((mask & (mask-1u)) != 0u) {
        uint8_t sessions[16], seqs[8];
        size_t count=0;
        for (uint8_t i=0;i<mesh_report_delivery.count;i++) if (mask & BIT(i)) {
            proto_put_u32_le(sessions+count*4u,mesh_report_delivery.entries[i].outbound.packet.session_id);
            proto_put_u16_le(seqs+count*2u,mesh_report_delivery.entries[i].outbound.packet.seq);
            count++;
        }
        assert(tlv_append_bytes(ack->payload,sizeof(ack->payload),&len,TLV_MESH_ACK_SESSION_LIST,sessions,(uint8_t)(count*4u))==PROTO_OK);
        assert(tlv_append_bytes(ack->payload,sizeof(ack->payload),&len,TLV_MESH_ACK_SEQ_LIST,seqs,(uint8_t)(count*2u))==PROTO_OK);
    }
    ack->payload_len=(uint16_t)len; ack->packet.payload_len=(uint16_t)len;
    ack->radio_channel=UWB_CHANNEL_WAKE_CONTACT;
}
static int dwm3000_driver_receive_frame_continuous(uint32_t timeout, uint8_t *frame, size_t cap,
    size_t *len, uint8_t *quality, void *rsl, enum dwm3000_rx_failure *failure)
{
    (void)rsl;
    receive_calls++;
    assert(receive_calls < 100u);
    *failure = DWM3000_RX_FAILURE_NONE;
    *quality = 90u;
    if (fixed_ack_deadline) {
        if (receive_calls == 1u) first_ack_deadline_ms = now_ms + timeout;
        assert(now_ms + timeout == first_ack_deadline_ms);
    }
    if (typed_phy_remaining != 0u || repeat_typed_phy) {
        if (typed_phy_remaining != 0u) typed_phy_remaining--;
        *failure = DWM3000_RX_FAILURE_CRC_OR_PHY;
        now_ms += MIN(timeout, typed_phy_duration_ms);
        return -EIO;
    }
    if (inject_unrelated_rx) {
        inject_unrelated_rx = false;
        assert(uwb_mesh_frame_encode(NETWORK_ID, unrelated_rx.next_hop_id,
            DEVICE_ID, &unrelated_rx.packet, unrelated_rx.payload,
            frame, cap, len) == PROTO_OK);
        uint32_t duration = (uint32_t)((dwm3000_timing_airtime_us_ceil(
            DWM3000_TIMING_PHY_CH5_MESH_CONTROL, *len) + 999u) / 1000u);
        if (duration > timeout) { now_ms += timeout; return -ETIMEDOUT; }
        now_ms += duration;
        return 0;
    }
    if (receive_error || ack_reads >= (duplicate_first_ack ? 3u : 2u)) { now_ms+=timeout; return receive_error ? receive_error : -ETIMEDOUT; }
    struct mesh_outbound ack;
    if (follower_refusal && ack_reads==1u) {
        refuse=true; credit=0u; response_depth=MESH_ROUTE_DEPTH_UNREACHABLE;
    }
    make_ack(&ack, refuse ? 0u : ack_reads == 0u ||
        (duplicate_first_ack && ack_reads==1u) ? 1u : final_mask);
    if (follower_refusal && ack_reads==1u) {
        proto_put_u16_le(ack.payload+2u,mesh_report_delivery.entries[1].outbound.packet.seq);
    }
    ack_reads++;
    uint64_t peer=mesh_report_delivery.next_hop_id + (wrong_peer ? 1u : 0u);
    assert(uwb_mesh_frame_encode(NETWORK_ID,peer,DEVICE_ID,&ack.packet,ack.payload,frame,cap,len)==PROTO_OK);
    uint64_t airtime_us=dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_MESH_CONTROL,*len);
    if (airtime_us > (uint64_t)timeout*1000u) { now_ms+=timeout; return -ETIMEDOUT; }
    now_ms+=(uint32_t)((airtime_us+999u)/1000u);
    return 0;
}
static int dwm3000_driver_receive_frame_continuous_extend_on_activity(uint32_t timeout, uint32_t extend,
    uint8_t *frame,size_t cap,size_t *len,uint8_t *quality,void *rsl,enum dwm3000_rx_failure *failure)
{
    (void)extend;
    if (!capture_mode) return dwm3000_driver_receive_frame_continuous(timeout,frame,cap,len,quality,rsl,failure);
    if (receive_error || follower_index==follower_count) { now_ms+=timeout; return receive_error ? receive_error : -ETIMEDOUT; }
    struct mesh_outbound frame_out=queue[0];
    uint32_t start=follower_start_ms[follower_index++];
    assert(uwb_mesh_frame_encode(NETWORK_ID,mesh_c5_rx_burst_peer,DEVICE_ID,&frame_out.packet,
        frame_out.payload,frame,cap,len)==PROTO_OK);
    uint32_t duration=follower_duration_ms ? follower_duration_ms :
        (uint32_t)((dwm3000_timing_airtime_us_ceil(DWM3000_TIMING_PHY_CH5_MESH_CONTROL,*len)+999u)/1000u);
    if (start<now_ms || start+duration>now_ms+timeout) { now_ms+=timeout; return -ETIMEDOUT; }
    now_ms=start+duration;
    return 0;
}
static bool queue_captured_frame(const uint8_t *frame, size_t len, bool submit_work,
    bool *valid, uint64_t *previous)
{
    size_t payload_len;
    if (rx_count==MESH_RX_QUEUE_DEPTH || uwb_mesh_frame_decode(frame,len,NETWORK_ID,DEVICE_ID,
        previous,&queued_rx.packet,queued_rx.payload,sizeof(queued_rx.payload),&payload_len)!=PROTO_OK) return false;
    queued_rx.payload_len=(uint16_t)payload_len;
    queued_rx.ingress_previous_hop_id=*previous;
    *valid=true; rx_count++; rx_work_submissions+=submit_work; return true;
}
#define mesh_queue_from_frame_at_internal(frame,len,quality,rsl,rslvalid,channel,now,age,identity,idlen,late,valid,previous,ctx) \
    queue_captured_frame(frame,len,late,valid,previous)
static int mesh_ch9_ack_batch_queue(const struct mesh_outbound *ack, const struct mesh_rx_pending *rx)
{
    (void)rx;
    struct mesh_ack_semantic_identity identity;
    if (mesh_ack_semantic_identity_at(ack->payload,ack->payload_len,0u,&identity)!=PROTO_OK) return -EBADMSG;
    struct app_mesh_ch9_ack_batch_entry entry={.session_id=identity.session_id,.seq=identity.seq};
    return app_mesh_ch9_ack_table_queue(&mesh_ch9_ack_table,ack,&entry,NULL);
}
static int mesh_ch9_ack_batch_build(uint64_t peer, struct mesh_outbound *out)
{ return app_mesh_ch9_ack_table_build_peer(&mesh_ch9_ack_table,peer,out); }
static int mesh_ch9_ack_note_send_failure(uint64_t peer,uint32_t now,uint32_t random,uint32_t *delay,const char *why)
{ (void)why; return app_mesh_ch9_ack_table_note_send_failure(&mesh_ch9_ack_table,peer,now,random,delay); }
static void mesh_note_channel9_local_tx(uint64_t peer,uint32_t now) { (void)peer; (void)now; }
static int mesh_send_causal_channel9_response(const struct mesh_outbound *ack,const char *why,void *ctx)
{
    (void)why; (void)ctx;
    struct fw_radio_activity_capture capture={.rx_queue_used=rx_count,
        .report_queue_used=queue_count,.relay_tx_active=mesh_relay_tx_active(&mesh_runtime),
        .ch9_ack_wait_active=false,
        .ch9_ack_send_pending=app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table),
        .c5_tx_intent=FW_C5_TX_INTENT_CAUSAL_RESPONSE};
    struct fw_radio_activity_decision decision;
    assert(fw_radio_activity_decide(&capture,NULL,&decision,NULL)==0);
    causal_send_calls++;
    if (!decision.c5_tx_allowed) return -EBUSY;
    causal_send_allowed++;
    causal_sent_ack=*ack;
    return causal_send_error;
}
#include "c5_batch_production_ack_sender.inc"

#include "../app/src/app_mesh_report_c5_batch.inc"

/* Compile the actual gateway receiver/credit branch as well. */
#undef DEVICE_ROLE
#define DEVICE_ROLE ROLE_GATEWAY
#define mesh_c5_update_credit gateway_c5_update_credit
#define mesh_c5_capture_followers gateway_c5_capture_followers
#define mesh_c5_collect_ack gateway_c5_collect_ack
#define mesh_c5_flush_batch_ack gateway_c5_flush_batch_ack
#define mesh_report_delivery_step gateway_c5_try_batch
#define mesh_report_delivery_active gateway_delivery_active
#include "../app/src/app_mesh_report_c5_batch.inc"
#undef mesh_c5_update_credit
#undef mesh_c5_capture_followers
#undef mesh_c5_collect_ack
#undef mesh_c5_flush_batch_ack
#undef mesh_report_delivery_step
#undef mesh_report_delivery_active
#undef DEVICE_ROLE
#define DEVICE_ROLE ROLE_ANCHOR

static void reset_fixture(unsigned count)
{
    memset(queue,0,sizeof(queue)); memset(sent,0,sizeof(sent)); memset(&mesh_report_delivery,0,sizeof(mesh_report_delivery));
    queue_count=count; sent_count=retired_count=rx_count=0;
    claims=releases=parks=stops=starts=watchdog_stops=locks=0;
    causal_send_calls=causal_send_allowed=0; causal_send_error=0;
    handoff_held=false; handoff_begins=handoff_ends=0; handoff_error=0;
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    now_ms=1000; scheduled_ms=0; ack_reads=0;
    receive_calls=typed_phy_remaining=standard_probes=click_handoffs=rx_work_submissions=0;
    click_contact_clears=0;
    repeat_typed_phy=capture_click=radio_owned=fixed_ack_deadline=inject_unrelated_rx=false;
    standard_probe_clipped=false; typed_phy_duration_ms=standard_probe_duration_ms=5u;
    first_ack_deadline_ms=click_observed_ms=0; click_quality=83u;
    last_retry_seq=0; retry_round_calls=0;
    memset(&unrelated_rx,0,sizeof(unrelated_rx)); memset(&queued_rx,0,sizeof(queued_rx));
    observed_click=(struct uwb_wake_claim_frame){.network_id=NETWORK_ID,
        .clicker_id=UINT64_C(0xc100),.click_event_id=51u,.nonce=UINT64_C(0x12340056)};
    claim_error=configure_error=send_error=receive_error=park_error=0;
    credit=3; final_mask=0x0e; response_depth=0;
    refuse=corrupt_digest=wrong_peer=stale_ack=false;
    duplicate_first_ack=follower_refusal=false;
    capture_mode=false; follower_index=follower_count=follower_duration_ms=0;
    gateway_ble_stream_init(&gateway_ble_stream_state);
    mesh_route_waiting_tx_valid=false; route_requests=0; completion_error=0; mesh_c5_rx_burst_peer=0;
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_ANCHOR,DEVICE_ID,GATEWAY_ID,1u);
    struct route_candidate route={.next_hop_id=GATEWAY_ID,.gateway_id=GATEWAY_ID,
        .route_epoch=1u,.last_seen_ms=now_ms,.hop_count=0u,.link_quality=90u,.valid=true};
    assert(route_upsert_candidate(&mesh_runtime.upstream,&route)==PROTO_OK);
    for (unsigned i=0;i<count;i++) queue[i]=(struct mesh_outbound){
        .packet={.msg_type=MSG_MESH_DATA,.flags=FLAG_GATEWAY_ACK_REQUIRED|FLAG_DIAGNOSTIC,
            .src_id=DEVICE_ID,.dst_id=GATEWAY_ID,.session_id=77u,.seq=(uint16_t)(i+1),
            .ttl=MESH_DEFAULT_TTL,.payload_len=3u},
        .payload={TLV_MESH_TEST_PADDING,1u,(uint8_t)i},.payload_len=3u,.queued_at_ms=now_ms};
}
static void test_credit_and_partial_ack(void)
{
    for (uint8_t grant=0;grant<=3;grant++) {
        reset_fixture(4); credit=grant; final_mask=(uint8_t)((1u<<(grant+1u))-2u);
        assert(mesh_report_delivery_step()==0);
        assert(sent_count==grant+1u); assert(retired_count==grant+1u);
        assert(queue_count==0 && mesh_report_delivery.count==3u-grant);
        assert(claims==1 && releases==1 && parks==1 && locks==0);
    }
    reset_fixture(4); credit=1; final_mask=0x0e; /* ACK also names unsent frames. */
    assert(mesh_report_delivery_step()==0); assert(retired_count==2); assert(mesh_report_delivery.count==2 && queue_count==0);
    assert(mesh_report_delivery.entries[0].outbound.packet.seq==3 && mesh_report_delivery.entries[1].outbound.packet.seq==4);
    reset_fixture(4); final_mask=BIT(2);
    assert(mesh_report_delivery_step()==0); assert(retired_count==2 && mesh_report_delivery.count==2 && queue_count==0);
    assert(mesh_report_delivery.entries[0].outbound.packet.seq==2 && mesh_report_delivery.entries[1].outbound.packet.seq==4);
    reset_fixture(4); duplicate_first_ack=true;
    assert(mesh_report_delivery_step()==0); assert(ack_reads==3 && retired_count==4 && queue_count==0);
}

static void test_forwarded_report_queue_is_independent_of_route_and_activity(void)
{
    const uint64_t parents[] = {GATEWAY_ID, UINT64_C(0xa101), 0u};
    const uint8_t reports[] = {MSG_CLICK_REPORT, MSG_SELF_TEST_REPORT, MSG_MESH_DATA};
    for (unsigned active = 0u; active < 2u; active++) {
        for (unsigned parent = 0u; parent < sizeof(parents)/sizeof(parents[0]); parent++) {
            reset_fixture(1);
            if (active) {
                struct mesh_outbound tx;
                assert(mesh_relay_start_tx(&mesh_runtime, &queue[0].packet,
                    queue[0].payload, queue[0].payload_len, now_ms, &tx) == PROTO_OK);
            }
            assert(mesh_relay_tx_active(&mesh_runtime) == (active != 0u));
            route_table_init(&mesh_runtime.upstream, 1u);
            if (parents[parent] != 0u) {
                struct route_candidate route = {
                    .next_hop_id = parents[parent], .gateway_id = GATEWAY_ID,
                    .route_epoch = 1u, .last_seen_ms = now_ms,
                    .hop_count = parents[parent] == GATEWAY_ID ? 0u : 1u,
                    .link_quality = 90u,
                };
                assert(route_upsert_candidate(&mesh_runtime.upstream, &route) == PROTO_OK);
            }
            struct mesh_outbound forwarded = queue[0];
            forwarded.packet.src_id = UINT64_C(0xb100);
            forwarded.next_hop_id = parents[parent];
            for (unsigned report = 0u; report < sizeof(reports)/sizeof(reports[0]); report++) {
                forwarded.packet.msg_type = reports[report];
                assert(mesh_forward_uses_gateway_batch_queue(&forwarded));
            }
        }
    }

    reset_fixture(1);
    assert(!mesh_forward_uses_gateway_batch_queue(NULL));
    const uint8_t controls[] = {MSG_ROUTE_REQ, MSG_COMMAND, MSG_GATEWAY_ACK};
    for (unsigned i = 0u; i < sizeof(controls)/sizeof(controls[0]); i++) {
        struct mesh_outbound control = queue[0];
        control.packet.msg_type = controls[i];
        assert(!mesh_forward_uses_gateway_batch_queue(&control));
    }
    struct mesh_outbound invalid = queue[0];
    invalid.packet.flags = 0u;
    assert(!mesh_forward_uses_gateway_batch_queue(&invalid));
    invalid = queue[0];
    invalid.packet.dst_id = UINT64_C(0xb100);
    assert(!mesh_forward_uses_gateway_batch_queue(&invalid));
    mesh_relay_init(&mesh_runtime, MESH_RELAY_ROLE_GATEWAY, GATEWAY_ID, GATEWAY_ID, 1u);
    assert(!mesh_forward_uses_gateway_batch_queue(&queue[0]));
}
static void test_single_candidate_and_retained_full_queue(void)
{
    reset_fixture(2); queue[1].packet.src_id++;
    assert(mesh_report_delivery_step()==0); assert(sent_count==1 && retired_count==1 && queue_count==1);
    reset_fixture(1);
    assert(mesh_report_delivery_step()==0);
    assert(sent_count==1 && retired_count==1 && queue_count==0);
    assert(!mesh_report_delivery_active());
    reset_fixture(4);
    mesh_report_delivery.entries[0].outbound=queue[0];
    mesh_report_delivery.entries[0].outbound.packet.seq=99u;
    mesh_report_delivery.count=1;
    struct fw_radio_activity_capture capture={.report_queue_used=queue_count};
    struct fw_radio_activity_decision decision;
    assert(fw_radio_activity_decide(&capture,NULL,&decision,NULL)==0);
    assert(decision.report_tx_allowed);
    assert(mesh_report_delivery_step()==0);
    assert(sent_count==1 && sent[0].packet.seq==99u && retired_count==1);
    assert(queue_count==4 && !mesh_report_delivery_active());
    /* No transfer to the full queue or another transport is needed to drain. */
    sent_count=ack_reads=0;
    assert(mesh_report_delivery_step()==0);
    assert(sent_count==4 && retired_count==5 && queue_count==0);
    assert(!mesh_report_delivery_active());

}
static void test_route_wait_and_terminal_completion(void)
{
    reset_fixture(1);
    route_table_init(&mesh_runtime.upstream,1u);
    assert(mesh_report_delivery_step()==0);
    assert(route_requests==1 && sent_count==0 && queue_count==0);
    assert(mesh_report_delivery.count==1);
    assert(route_selected(&mesh_runtime.upstream)==NULL);
    struct route_candidate parent={.next_hop_id=0xa101u,.gateway_id=GATEWAY_ID,
        .route_epoch=1u,.last_seen_ms=now_ms,.hop_count=1u,.link_quality=90u};
    assert(route_upsert_candidate(&mesh_runtime.upstream,&parent)==PROTO_OK);
    response_depth=1u;
    assert(mesh_report_delivery_step()==0);
    assert(sent_count==1 && retired_count==1 && !mesh_report_delivery_active());

    reset_fixture(1); completion_error=-EBUSY;
    assert(mesh_report_delivery_step()==0);
    assert(sent_count==1 && retired_count==0 && mesh_report_delivery.entries[0].acked);
    assert(mesh_report_delivery_step()==0);
    assert(sent_count==1 && retired_count==0); /* ACKed bytes never retransmit. */
    completion_error=0;
    assert(mesh_report_delivery_step()==-ENOENT);
    assert(sent_count==1 && retired_count==1 && !mesh_report_delivery_active());
}

static void test_ack_rejection_and_refusal(void)
{
    for (unsigned mode=0;mode<3;mode++) {
        reset_fixture(2); wrong_peer=mode==0; stale_ack=mode==1; corrupt_digest=mode==2;
        assert(mesh_report_delivery_step()==0); assert(sent_count==1 && retired_count==0 && mesh_report_delivery.count==2 && queue_count==0);
    }
    reset_fixture(2); refuse=true; credit=0;
    assert(mesh_report_delivery_step()==0); assert(retired_count==0 && mesh_report_delivery.count==2 && queue_count==0 && scheduled_ms==50u);
    assert(mesh_runtime.upstream.candidates[0].failure_count==0);
    reset_fixture(2); follower_refusal=true;
    mesh_runtime.upstream.candidates[0].next_hop_id=UINT64_C(0xa101);
    mesh_runtime.upstream.candidates[0].hop_count=1u;
    response_depth=1u;
    assert(mesh_report_delivery_step()==0);
    assert(retired_count==1 && queue_count==0 && mesh_report_delivery.count==1 && mesh_report_delivery.entries[0].outbound.packet.seq==2u);
    assert(mesh_runtime.upstream.candidates[0].hold_down_valid);
    assert(mesh_runtime.upstream.candidates[0].failure_count==0 && scheduled_ms==50u);
}
static void test_radio_failure_cleanup(void)
{
    for (unsigned mode=0;mode<5;mode++) {
        reset_fixture(2);
        if (mode==0) claim_error=-EBUSY;
        if (mode==1) configure_error=-EIO;
        if (mode==2) send_error=-EIO;
        if (mode==3) receive_error=-EIO;
        if (mode==4) park_error=-EIO;
        assert(mesh_report_delivery_step()==0); assert(locks==0 && starts==1);
        assert(releases==(mode==0 ? 0u : 1u)); assert(parks==releases);
        if (mode<4) assert(retired_count==0 && queue_count==0 && mesh_report_delivery.count==2);
        if (mode==4) assert(watchdog_stops==1);
    }
}

static void test_ack_wait_queues_unrelated_data_and_foreign_ack(void)
{
    for (unsigned foreign_ack = 0u; foreign_ack < 2u; foreign_ack++) {
        reset_fixture(1);
        unrelated_rx = queue[0];
        unrelated_rx.next_hop_id = UINT64_C(0xb100);
        unrelated_rx.packet.src_id = unrelated_rx.next_hop_id;
        unrelated_rx.packet.session_id += 100u;
        unrelated_rx.packet.seq += 100u;
        if (foreign_ack) {
            struct mesh_outbound other = unrelated_rx;
            size_t len = 0u;
            unrelated_rx.packet = (struct proto_packet){
                .msg_type = MSG_GATEWAY_ACK, .flags = FLAG_GATEWAY_ACK,
                .src_id = GATEWAY_ID, .dst_id = DEVICE_ID,
                .session_id = other.packet.session_id, .seq = 200u, .ttl = 1u,
            };
            unrelated_rx.next_hop_id = GATEWAY_ID;
            assert(mesh_append_requested_seq(unrelated_rx.payload,
                sizeof(unrelated_rx.payload), &len, other.packet.seq) == PROTO_OK);
            assert(mesh_append_ack_semantic_identity(unrelated_rx.payload,
                sizeof(unrelated_rx.payload), &len, &other.packet,
                other.payload, other.payload_len) == PROTO_OK);
            unrelated_rx.packet.payload_len = unrelated_rx.payload_len = (uint16_t)len;
        }
        inject_unrelated_rx = true;
        assert(mesh_report_delivery_step() == 0);
        assert(sent_count == 1u && retired_count == 0u && ack_reads == 0u);
        assert(mesh_report_delivery.count == 1u && !mesh_report_delivery.entries[0].acked);
        assert(rx_count == 1u && rx_work_submissions == 1u);
        assert(queued_rx.packet.msg_type == unrelated_rx.packet.msg_type);
        assert(queued_rx.packet.src_id == unrelated_rx.packet.src_id);
        assert(queued_rx.packet.session_id == unrelated_rx.packet.session_id);
        assert(queued_rx.ingress_previous_hop_id == unrelated_rx.next_hop_id);
        assert(queued_rx.payload_len == unrelated_rx.payload_len);
        assert(memcmp(queued_rx.payload, unrelated_rx.payload, queued_rx.payload_len) == 0);
        assert(!radio_owned && releases == 1u && locks == 0u);
        assert(mesh_runtime.upstream.candidates[0].failure_count == 0u);
    }
}

static void test_typed_phy_activity_keeps_original_ack_deadline(void)
{
    reset_fixture(1);
    fixed_ack_deadline = true;
    typed_phy_remaining = 1u;
    assert(mesh_report_delivery_step() == 0);
    assert(receive_calls == 2u && standard_probes == 1u && retired_count == 1u);
    assert(now_ms < first_ack_deadline_ms && !mesh_report_delivery_active());
    assert(releases == 1u && !radio_owned && click_handoffs == 0u);

    reset_fixture(1);
    fixed_ack_deadline = repeat_typed_phy = true;
    typed_phy_duration_ms = 7u;
    standard_probe_duration_ms = 11u;
    assert(mesh_report_delivery_step() == 0);
    assert(receive_calls == 14u && standard_probe_clipped);
    assert(standard_probes == receive_calls && now_ms == first_ack_deadline_ms);
    assert(sent_count == 1u && retired_count == 0u && mesh_report_delivery.count == 1u);
    assert(mesh_runtime.upstream.candidates[0].failure_count == 1u);
    assert(releases == 1u && !radio_owned && locks == 0u);

    reset_fixture(1);
    receive_error = -EIO;
    assert(mesh_report_delivery_step() == 0);
    assert(receive_calls == 1u && standard_probes == 0u);
    assert(retired_count == 0u && mesh_report_delivery.count == 1u);
    assert(mesh_runtime.upstream.candidates[0].failure_count == 0u);
}

static void test_click_during_ack_wait_handoffs_only_after_radio_release(void)
{
    for (unsigned release_failure = 0u; release_failure < 2u; release_failure++) {
        reset_fixture(2);
        typed_phy_remaining = 1u;
        capture_click = true;
        if (release_failure) park_error = -EIO;
        assert(mesh_report_delivery_step() == 0);
        assert(standard_probes == 1u && receive_calls == 1u);
        assert(click_handoffs == (release_failure ? 0u : 1u));
        assert(click_contact_clears == click_handoffs);
        assert(watchdog_stops == release_failure);
        assert(sent_count == 1u && retired_count == 0u && mesh_report_delivery.count == 2u);
        assert(releases == 1u && parks == 1u && !radio_owned && locks == 0u);
        assert(mesh_runtime.upstream.candidates[0].failure_count == 0u);
    }
}

static void test_partial_ack_retry_key_belongs_to_unacknowledged_member(void)
{
    reset_fixture(4);
    final_mask = BIT(2);
    assert(mesh_report_delivery_step() == 0);
    assert(retired_count == 2u && mesh_report_delivery.count == 2u);
    assert(mesh_report_delivery.entries[0].outbound.packet.seq == 2u);
    assert(last_retry_seq == 2u && retry_round_calls == 1u);
}

static void test_retained_bank_dedup_uses_semantics_until_completion(void)
{
    reset_fixture(1);
    struct mesh_outbound retry = queue[0];
    receive_error = -EIO;
    assert(mesh_report_delivery_step() == 0);
    assert(queue_count == 0u && mesh_report_delivery.count == 1u);
    k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    assert(mesh_report_delivery_contains(&retry));
    retry.packet.message_age_ms += 9000u;
    retry.packet.ttl--;
    retry.queued_at_ms++;
    size_t len = retry.payload_len;
    assert(mesh_append_batch_pending(retry.payload, sizeof(retry.payload), &len, 3u) == PROTO_OK);
    retry.packet.payload_len = retry.payload_len = (uint16_t)len;
    assert(mesh_report_delivery_contains(&retry));
    retry.payload[2] ^= 1u;
    assert(!mesh_report_delivery_contains(&retry));
    retry.payload[2] ^= 1u;
    k_mutex_unlock(&report_tx_queue_overflow_lock);

    receive_error = 0;
    completion_error = -EBUSY;
    sent_count = ack_reads = 0u;
    assert(mesh_report_delivery_step() == 0);
    assert(mesh_report_delivery.entries[0].acked);
    k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    assert(mesh_report_delivery_contains(&retry));
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    completion_error = 0;
    assert(mesh_report_delivery_step() == -ENOENT);
    k_mutex_lock(&report_tx_queue_overflow_lock, K_FOREVER);
    assert(!mesh_report_delivery_contains(&retry));
    k_mutex_unlock(&report_tx_queue_overflow_lock);
}
static void test_receiver_grant_and_cleanup(void)
{
    for (unsigned gateway=0;gateway<2;gateway++) {
    reset_fixture(1); mesh_report_delivery.entries[0].outbound=queue[0];
    mesh_report_delivery.count=1; mesh_report_delivery.next_hop_id=GATEWAY_ID;
    struct mesh_outbound ack; make_ack(&ack,1u);
    ack.next_hop_id=GATEWAY_ID;
    struct mesh_rx_pending rx={.previous_hop_id=GATEWAY_ID,.radio_channel=UWB_CHANNEL_WAKE_CONTACT};
    size_t len=0; assert(mesh_append_batch_pending(rx.payload,sizeof(rx.payload),&len,3u)==PROTO_OK); rx.payload_len=len;
    mesh_c5_update_credit();
    capture_mode=true; follower_count=3;
    follower_start_ms[0]=now_ms+30u; follower_start_ms[1]=now_ms+40u; follower_start_ms[2]=now_ms+50u;
    if (gateway) gateway_c5_capture_followers(&ack,&rx); else mesh_c5_capture_followers(&ack,&rx);
    assert(mesh_c5_rx_burst_peer==GATEWAY_ID && claims==1 && releases==1 && locks==0);
    assert(rx_count==3);
    assert(gateway ? gateway_c5_collect_ack(&ack,&rx) : mesh_c5_collect_ack(&ack,&rx));
    if (gateway) gateway_c5_flush_batch_ack(); else mesh_c5_flush_batch_ack();
    assert(mesh_c5_rx_burst_peer==0 && starts==1);
    assert(causal_send_calls==1 && causal_send_allowed==1);
    assert(handoff_begins==1 && handoff_ends==1 && !handoff_held);
    /* A frame beginning inside RX but finishing outside must not be queued. */
    rx_count=0; follower_index=0; follower_count=1; follower_start_ms[0]=now_ms+49u; follower_duration_ms=2u;
    mesh_c5_capture_followers(&ack,&rx);
    assert(rx_count==0 && releases==2);
    mesh_c5_flush_batch_ack();
    }
}
static void test_receiver_handoff_failure_balance(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        reset_fixture(1);
        mesh_report_delivery.entries[0].outbound=queue[0];
        mesh_report_delivery.count=1; mesh_report_delivery.next_hop_id=GATEWAY_ID;
        struct mesh_outbound ack; make_ack(&ack,1u);
        struct mesh_rx_pending rx={.previous_hop_id=GATEWAY_ID,.radio_channel=UWB_CHANNEL_WAKE_CONTACT};
        size_t len=0;
        assert(mesh_append_batch_pending(rx.payload,sizeof(rx.payload),&len,1u)==PROTO_OK);
        rx.payload_len=len;
        if (mode==0) handoff_error=-EBUSY;
        if (mode==1) claim_error=-EBUSY;
        if (mode==2) configure_error=-EIO;
        if (mode==3) receive_error=-EIO;
        gateway_c5_capture_followers(&ack,&rx);
        assert(!handoff_held && locks==0);
        assert(handoff_begins==1 && handoff_ends==(mode==0 ? 0u : 1u));
        assert(claims==(mode==0 ? 0u : 1u));
        assert(releases==(mode<2 ? 0u : 1u));
        gateway_c5_flush_batch_ack();
        assert(mesh_c5_rx_burst_peer==0u);
    }
}
static void queue_generated_pair(uint64_t child)
{
    mesh_report_delivery.next_hop_id=DEVICE_ID;
    mesh_report_delivery.count=2;
    for (unsigned i=0;i<2;i++) {
        queue[i].packet.src_id=child;
        mesh_report_delivery.entries[i].outbound=queue[i];
    }
    for (unsigned i=0;i<2;i++) {
        struct mesh_outbound ack;
        make_ack(&ack,(uint8_t)BIT(i));
        ack.packet.dst_id=child; ack.next_hop_id=child;
        assert(mesh_ch9_ack_batch_queue(&ack,NULL)==PROTO_OK);
    }
}
static void test_generated_ack_transfers_owner_before_causal_send(void)
{
    const uint64_t child=UINT64_C(0xa200);
    for (unsigned failure=0;failure<2;failure++) {
        reset_fixture(2); queue_generated_pair(child);
        assert(app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table,child)->count==2u);
        causal_send_error=failure ? -EIO : 0;
        assert(mesh_send_current_ch9_ack_batch(child,"harness")==causal_send_error);
        assert(causal_send_calls==1 && causal_send_allowed==1);
        assert(!app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table));
        assert(queue_count==2 && retired_count==0); /* Report custody is independent. */
        for (unsigned i=0;i<2;i++) {
            bool matched=false;
            assert(mesh_ack_payload_contains_packet(&causal_sent_ack.packet,
                causal_sent_ack.payload,causal_sent_ack.payload_len,&queue[i].packet,
                queue[i].payload,queue[i].payload_len,&matched)==PROTO_OK && matched);
        }
        /* Lost generated ACKs can be reconstructed by exact sender retries. */
        queue_generated_pair(child); causal_send_error=0;
        assert(mesh_send_current_ch9_ack_batch(child,"retry")==0);
        assert(causal_send_allowed==2 && !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table));
    }
    reset_fixture(1);
    mesh_report_delivery.entries[0].outbound=queue[0];
    mesh_report_delivery.count=1; mesh_report_delivery.next_hop_id=GATEWAY_ID;
    struct mesh_outbound forwarded; make_ack(&forwarded,1u);
    forwarded.next_hop_id=child;
    assert(app_mesh_ch9_ack_table_queue_forwarded(&mesh_ch9_ack_table,&forwarded,NULL)==PROTO_OK);
    assert(mesh_send_current_ch9_ack_batch(child,"legacy") == -EBUSY);
    const struct app_mesh_ch9_ack_batch *retained=app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table,child);
    assert(retained && retained->preserve_payload && retained->retry_deferred);
    assert(causal_send_calls==1 && causal_send_allowed==0);
    assert(retained->template_ack.payload_len==forwarded.payload_len);
    assert(memcmp(retained->template_ack.payload,forwarded.payload,forwarded.payload_len)==0);
}
static void test_gateway_credit_uses_real_stream_occupancy(void)
{
    reset_fixture(1);
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_GATEWAY,GATEWAY_ID,GATEWAY_ID,1u);
    gateway_c5_update_credit();
    assert(mesh_relay_local_credit(&mesh_runtime)==3u);
    queue[0].packet.msg_type=MSG_CLICK_REPORT;
    queue[0].packet.flags=FLAG_GATEWAY_ACK_REQUIRED;
    for (unsigned i=0;i<GATEWAY_BLE_STREAM_QUEUE_DEPTH;i++) {
        queue[0].packet.seq=(uint16_t)(i+1u);
        assert(gateway_ble_stream_enqueue_packet(&gateway_ble_stream_state,&queue[0].packet,
            queue[0].payload,queue[0].payload_len,now_ms,now_ms,true)==1);
    }
    gateway_c5_update_credit(); assert(mesh_relay_local_credit(&mesh_runtime)==0u);
    assert(gateway_c5_try_batch()==-ENOTSUP);
}
int main(void)
{
    test_forwarded_report_queue_is_independent_of_route_and_activity();
    test_ack_wait_queues_unrelated_data_and_foreign_ack();
    test_typed_phy_activity_keeps_original_ack_deadline();
    test_click_during_ack_wait_handoffs_only_after_radio_release();
    test_partial_ack_retry_key_belongs_to_unacknowledged_member();
    test_retained_bank_dedup_uses_semantics_until_completion();
    test_credit_and_partial_ack(); test_single_candidate_and_retained_full_queue();
    test_route_wait_and_terminal_completion();
    test_ack_rejection_and_refusal(); test_radio_failure_cleanup(); test_receiver_grant_and_cleanup();
    test_gateway_credit_uses_real_stream_occupancy();
    test_receiver_handoff_failure_balance();
    test_generated_ack_transfers_owner_before_causal_send();
    puts("production C5 batch harness passed");
    return 0;
}
