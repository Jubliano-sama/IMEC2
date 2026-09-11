/* Exercise the production assignment admission, HIA callbacks and cleanup.
 * Only scheduling, radio submission, storage and host output are substituted;
 * policy decoding, deadlines and generation-bound ownership are real. */
#include "app_discovery_assignment_policy.h"
#include "app_gateway_assignment_publisher.h"
#include "app_gateway_control_sequence.h"
#include "app_gateway_operation_owner.h"
#include "app_node_comm.h"
#include "app_operation_policy.h"
#include "enumeration_response_lane.h"
#include "gateway_command.h"
#include "mesh_relay.h"
#include <zephyr/kernel.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "The assignment HIA production-seam test requires enabled assertions"
#endif

#define ROLE_GATEWAY 2
#define DEVICE_ROLE ROLE_GATEWAY
#define DEVICE_ID UINT64_C(0x9000)
#define BUILD_ASSERT _Static_assert
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define GATEWAY_CONTROL_VERBOSE_LOG(...) ((void)0)
#define GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS

#include "gateway_assignment_hia_state.inc"
static struct gateway_discovery_assignment_state gateway_discovery_assignment_state;
K_MUTEX_DEFINE(gateway_discovery_assignment_mutex);
static struct k_work_delayable gateway_discovery_assignment_finalize_work;
static struct k_work_delayable gateway_discovery_assignment_publish_work;
static struct mesh_relay mesh_runtime;
static struct mesh_gateway_ack_store ack_store;
static uint32_t gateway_discovery_assignment_generation;
static uint32_t gateway_route_refresh_result_token;
static uint64_t now_ms;
static unsigned stop_feeds, rx_starts, hia_requests, schedules, aborts, abandons;
static unsigned policy_admissions, sequence_allocations;
static unsigned ram_clears, durable_clears;
static int hia_return, rx_return, schedule_return, abort_return, abandon_return;
static bool sequence_available, publication_pending, publisher_active;
static uint32_t next_sequence, hia_budget;
static uint32_t dispatch_delay_ms;
static uint64_t delivery_deadline;
static enum node_comm_delivery_profile delivery_profile;
static struct mesh_outbound submitted;
static struct proto_packet hia_identity;
static void (*hia_hook)(void);
static struct {
    struct proto_packet host;
    enum command_id command;
    enum command_status status;
    uint8_t reason;
    uint32_t token;
    unsigned count;
} result;
static struct gateway_command_event terminal;
static unsigned terminals;

int64_t k_uptime_get(void) { return (int64_t)now_ms; }
static void status_debug_printf(const char *format, ...) { (void)format; }
static void status_debug_note(const char *message) { (void)message; }
static void app_watchdog_stop_feeding(void) { stop_feeds++; }
static int mesh_errno_from_proto(int ret) { (void)ret; return -EINVAL; }
#include "gateway_assignment_hia_owner.inc"

static uint32_t gateway_next_broadcast_command_seq(void)
{ sequence_allocations++; return next_sequence++; }
static uint16_t gateway_next_command_seq(void) { return (uint16_t)next_sequence++; }
static uint32_t gateway_command_result_get_dispatch_token(void) { return 0x1234u; }
bool app_gateway_control_sequence_admission_available(uint32_t minimum)
{ assert(minimum == APP_GATEWAY_CONTROL_SEQUENCE_ASSIGNMENT_ADMISSION_BUDGET); policy_admissions++; return sequence_available; }
static bool gateway_assignment_publication_pending(void) { return publication_pending; }
void app_gateway_assignment_publisher_get_diagnostics(struct app_gateway_assignment_publisher_diagnostics *out)
{ memset(out, 0, sizeof(*out)); out->active = publisher_active; }
static int gateway_clear_registered_membership_roster_ram_only(void) { ram_clears++; return 0; }
static void gateway_clear_registered_membership_roster(void) { durable_clears++; }
static int gateway_get_registered_membership_roster_with_slots(uint64_t *ids, uint8_t *slots,
    size_t cap, size_t *count, uint16_t *epoch)
{ (void)ids; (void)slots; (void)cap; (void)count; (void)epoch; return -ENOENT; }
static void gateway_emit_host_command_result_reserved(uint32_t token,
    const struct proto_packet *host, enum command_id command, enum command_status status, uint8_t reason)
{ result.reason=reason; result.host=*host; result.command=command; result.status=status; result.token=token; result.count++; }
static void gateway_emit_host_command_result(const struct proto_packet *host,
    enum command_id command, enum command_status status, uint8_t reason)
{ gateway_emit_host_command_result_reserved(0u,host,command,status,reason); }
static int gateway_commit_host_command_result_reserved(uint32_t token,
    const struct proto_packet *host, enum command_id command, enum command_status status, uint8_t reason)
{ gateway_emit_host_command_result_reserved(token,host,command,status,reason); return 0; }
static int gateway_observe_command_event(struct gateway_command_event *event, bool is_terminal)
{
    if (event->stage==GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING) now_ms+=dispatch_delay_ms;
    if (is_terminal) { terminal=*event; terminals++; }
    return 0;
}
bool app_gateway_assignment_publisher_abort_prepared_batch(const struct gateway_command_event *event)
{ (void)event; return false; }
static int gateway_abort_pending_assignment_publication_ram_only(const struct gateway_command_event *event)
{ (void)event; return 0; }
static int mesh_start_uwb_rx(const char *reason) { (void)reason; rx_starts++; return rx_return; }
static int mesh_route_owner_work_reschedule_timeout(struct k_work_delayable *work, k_timeout_t delay)
{ (void)work; (void)delay; schedules++; return schedule_return; }
int app_node_comm_abandon_delivery(uint32_t handle)
{ assert(handle != 0u); abandons++; return abandon_return; }
int app_node_comm_delivery_attempts_started(uint32_t handle, uint8_t *attempts)
{ (void)handle; *attempts=0u; return 0; }
static size_t gateway_discovery_assignment_current_claim_count_locked(void)
{ return (size_t)__builtin_popcountll(gateway_discovery_assignment_state.claim_response_mask); }
static uint8_t gateway_discovery_assignment_missing_ack_count_locked(void) { return 0u; }
static uint32_t gateway_discovery_assignment_window_ms_locked(void) { return 0u; }
int app_node_comm_submit_delivery(const struct mesh_outbound *outbound,
    enum node_comm_delivery_profile profile, uint64_t deadline, uint32_t owner, uint32_t *handle)
{
    assert(owner!=0u); aborts++; submitted=*outbound;
    delivery_deadline=deadline; delivery_profile=profile;
    if (abort_return==0) *handle=777u;
    return abort_return;
}
static struct gateway_command_event gateway_observability_event(enum gateway_command_event_kind,
    enum gateway_command_event_stage, enum command_id, const struct proto_packet *, uint32_t);
static void gateway_route_refresh_observe(const struct app_node_comm_route_refresh_event *);
static void gateway_discovery_assignment_fail_locked(enum command_status,uint8_t);
static void gateway_discovery_assignment_finish_failure_locked(enum command_status,uint8_t);
#include "gateway_assignment_hia_prearm.inc"
#include "gateway_assignment_hia_controls.inc"
#include "gateway_assignment_hia_cleanup.inc"
#include "gateway_assignment_hia_start.inc"
#include "gateway_assignment_hia_observe.inc"

int app_node_comm_request_route_refresh_correlated_bounded(uint32_t delay, const char *reason,
    const struct proto_packet *correlation, uint32_t timeout)
{
    assert(delay == 0u && strcmp(reason,"assignment-hia") == 0);
    hia_requests++; hia_identity=*correlation; hia_budget=timeout;
    if (hia_hook != NULL) hia_hook();
    return hia_return;
}

static struct proto_packet host;
static struct operation_policy_assignment policy;
static uint8_t payload[128];
static size_t payload_len;
static void encode_policy(void)
{
    payload_len=0u;
    assert(tlv_append_u16(payload,sizeof(payload),&payload_len,TLV_COMMAND_ID,CMD_ASSIGN_DISCOVERY_SLOTS)==PROTO_OK);
    assert(tlv_append_u8(payload,sizeof(payload),&payload_len,TLV_COMMAND_SCOPE,CMD_SCOPE_SINGLE_NODE)==PROTO_OK);
    assert(tlv_append_u8(payload,sizeof(payload),&payload_len,TLV_COMMAND_RESPONSE_MODE,CMD_RESPONSE_SMALL_RESULT)==PROTO_OK);
    struct operation_policy wire={.family=OPERATION_POLICY_FAMILY_ASSIGNMENT,.value.assignment=policy};
    assert(operation_policy_append_tlv(payload,sizeof(payload),&payload_len,&wire)==PROTO_OK);
    host.payload_len=(uint16_t)payload_len;
}
static void reset_fixture(void)
{
    memset(&gateway_discovery_assignment_state,0,sizeof(gateway_discovery_assignment_state));
    memset(&gateway_operation_owner,0,sizeof(gateway_operation_owner));
    memset(&gateway_assignment_operation_lease,0,sizeof(gateway_assignment_operation_lease));
    memset(&gateway_discovery_assignment_finalize_work,0,sizeof(gateway_discovery_assignment_finalize_work));
    memset(&gateway_discovery_assignment_publish_work,0,sizeof(gateway_discovery_assignment_publish_work));
    memset(&result,0,sizeof(result)); memset(&terminal,0,sizeof(terminal));
    gateway_assignment_hia_pending=false; gateway_assignment_hia_error=0;
    memset(&gateway_route_refresh_host_command,0,sizeof(gateway_route_refresh_host_command));
    gateway_discovery_assignment_generation=0u; gateway_route_refresh_result_token=0u;
    stop_feeds=rx_starts=hia_requests=schedules=aborts=abandons=terminals=0u;
    policy_admissions=sequence_allocations=0u; hia_return=rx_return=schedule_return=abort_return=abandon_return=0;
    ram_clears=durable_clears=0u;
    sequence_available=true; publication_pending=publisher_active=false;
    now_ms=1000u; next_sequence=100u; hia_budget=dispatch_delay_ms=0u; hia_hook=NULL;
    delivery_deadline=0u;
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_GATEWAY,DEVICE_ID,DEVICE_ID,1u);
    mesh_gateway_ack_store_init(&ack_store);
    assert(mesh_relay_attach_gateway_ack_store(&mesh_runtime,&ack_store)==PROTO_OK);
    app_operation_policy_reset_defaults();
    operation_policy_assignment_defaults(&policy);
    policy.expected_anchor_count=3u; policy.ram_only_iteration=true;
    policy.operation_budget_ms=DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;
    host=(struct proto_packet){.msg_type=MSG_COMMAND,.src_id=0x42u,.dst_id=DEVICE_ID,
        .session_id=0x11223344u,.seq=17u,.ttl=1u};
    encode_policy();
}
static int start(void)
{
    int ret=gateway_start_discovery_assignment(&host,payload,payload_len);
    if (ret < 0) fprintf(stderr,"assignment admission ret=%d status=%u reason=%u\n",ret,result.status,result.reason);
    return ret;
}
static void refresh(enum app_node_comm_route_refresh_event_kind kind, int status, uint8_t sent)
{
    struct app_node_comm_route_refresh_event event={.kind=kind,.correlation=hia_identity,
        .gateway_sequence=999u,.attempt=1u,.sent_count=sent,.result=status,.correlated=true};
    gateway_route_refresh_observe(&event);
}
static void pipeline(void)
{ gateway_enumeration_pipeline_start(gateway_discovery_assignment_state.epoch,(uint32_t)now_ms); }
static void assert_owner(void)
{
    assert(gateway_discovery_assignment_state.active);
    assert(gateway_operation_owner_matches(APP_GATEWAY_OPERATION_OWNER_ASSIGNMENT,&gateway_assignment_operation_lease));
    assert(gateway_discovery_assignment_state.host_command.session_id==host.session_id);
    assert(gateway_discovery_assignment_state.host_command.seq==host.seq);
    assert(gateway_discovery_assignment_state.result_reservation_token==0x1234u);
}
static void test_cold_assignment_requests_hia_and_keeps_absolute_budget(void)
{
    reset_fixture(); uint64_t deadline=now_ms+policy.operation_budget_ms;
    assert(start()==0); assert(hia_requests==1u); assert_owner();
    assert(gateway_assignment_hia_pending && result.count==0u);
    assert(hia_identity.session_id==host.session_id && hia_identity.seq==host.seq);
    assert(hia_budget==MIN(policy.operation_budget_ms,APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS));
    uint32_t epoch=0u,hold=0u; bool survey=false; struct operation_policy_set snapshot={0};
    assert(gateway_route_refresh_prearm_snapshot(&epoch,&hold,&snapshot,&survey));
    assert(epoch==gateway_discovery_assignment_state.epoch);
    assert(snapshot.assignment.expected_anchor_count==policy.expected_anchor_count);
    assert(snapshot.assignment.operation_budget_ms==policy.operation_budget_ms);
    assert(snapshot.assignment.ram_only_iteration);
    pipeline(); assert(rx_starts==1u);
    /* These are already admitted compact identities, not fabricated delivery
     * evidence: this test asks whether callback ownership preserves its RAM. */
    gateway_discovery_assignment_state.anchor_ids[0]=0xa100u;
    gateway_discovery_assignment_state.anchor_previous_hop_ids[0]=0xa101u;
    gateway_discovery_assignment_state.anchor_hop_counts[0]=2u;
    gateway_discovery_assignment_state.claim_count=1u;
    gateway_discovery_assignment_state.claim_response_mask=1u;
    now_ms+=5000u; refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    assert(!gateway_assignment_hia_pending); assert_owner();
    assert(gateway_discovery_assignment_state.operation_deadline_ms==deadline);
    assert(gateway_discovery_assignment_state.anchor_ids[0]==0xa100u);
    assert(gateway_discovery_assignment_state.anchor_previous_hop_ids[0]==0xa101u);
    assert(gateway_discovery_assignment_state.anchor_hop_counts[0]==2u);
    assert(gateway_discovery_assignment_state.claim_count==1u);
    assert(gateway_discovery_assignment_state.claim_response_mask==1u);
    assert(gateway_discovery_assignment_state.claim_delivery_succeeded);
    assert(gateway_discovery_assignment_state.claim_round==DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS);
    assert(result.count==0u && hia_requests==1u);
}
static void test_matching_prearm_is_consumed_without_second_hia(void)
{
    reset_fixture(); assert(gateway_enumeration_prearm_prepare(&policy,true)==0);
    uint32_t epoch=gateway_discovery_assignment_state.epoch;
    pipeline(); gateway_discovery_assignment_state.anchor_ids[0]=0xa100u;
    gateway_discovery_assignment_state.claim_count=1u;
    uint64_t lane_start=gateway_discovery_assignment_state.response_lane_start_ms;
    uint64_t collection_deadline=gateway_discovery_assignment_state.claim_collection_deadline_ms;
    now_ms+=500u;
    uint64_t assignment_deadline=now_ms+policy.operation_budget_ms;
    assert(start()==0); assert_owner(); assert(hia_requests==0u);
    assert(gateway_discovery_assignment_state.epoch==epoch);
    assert(gateway_discovery_assignment_state.anchor_ids[0]==0xa100u);
    assert(gateway_discovery_assignment_state.response_lane_start_ms==lane_start);
    assert(gateway_discovery_assignment_state.claim_collection_deadline_ms==collection_deadline);
    assert(gateway_discovery_assignment_state.survey_follows);
    /* The earlier HIA reserved an epoch under a separate host operation.
     * Assignment owns its budget from this admission; lane origin survives. */
    assert(gateway_discovery_assignment_state.operation_deadline_ms==assignment_deadline);
    assert(gateway_discovery_assignment_state.claim_round==DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS);
}
static void assert_assignment_terminal(enum command_status status)
{
    assert(result.count==1u && result.command==CMD_ASSIGN_DISCOVERY_SLOTS);
    assert(result.host.session_id==host.session_id && result.host.seq==host.seq);
    assert(result.token==0x1234u && result.status==status);
    assert(terminals==1u && terminal.command_id==CMD_ASSIGN_DISCOVERY_SLOTS);
    assert(!gateway_discovery_assignment_state.active && !gateway_assignment_hia_pending);
    assert(!app_gateway_operation_lease_valid(&gateway_assignment_operation_lease));
    assert(!app_gateway_operation_lease_valid(&gateway_operation_owner.active));
    assert(gateway_discovery_assignment_finalize_work.cancel_calls==1u);
    assert(gateway_discovery_assignment_publish_work.cancel_calls==1u);
}
static void finish_abort(void)
{
    assert(gateway_discovery_assignment_state.stage==GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY);
    assert(aborts==1u && result.count==0u);
    gateway_discovery_assignment_finish_failure_locked(
        gateway_discovery_assignment_state.pending_failure_status,
        gateway_discovery_assignment_state.pending_failure_reason);
}
static void inline_hia_complete(void)
{ pipeline(); refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u); }
static void test_inline_completion_keeps_exact_host_owner(void)
{
    reset_fixture(); hia_hook=inline_hia_complete;
    assert(start()==0); assert_owner(); assert(hia_requests==1u);
    assert(!gateway_assignment_hia_pending && result.count==0u);
    assert(gateway_discovery_assignment_state.claim_delivery_succeeded);
    assert(schedules==1u);
}
static void test_submission_failures_are_local_radio_terminals_not_no_anchors(void)
{
    const int errors[]={-EBUSY,-EIO,-EINVAL,-ETIMEDOUT};
    for (size_t i=0u;i<ARRAY_SIZE(errors);i++) {
        reset_fixture(); hia_return=errors[i]; assert(start()==0);
        assert(hia_requests==1u && rx_starts==0u && aborts==0u);
        assert_assignment_terminal(COMMAND_RADIO_ERROR);
        assert(terminal.reason==GATEWAY_COMMAND_EVENT_REASON_RADIO);
    }
}
static void test_admission_backpressure_has_no_hia_or_new_identity(void)
{
    for (unsigned which=0u;which<5u;which++) {
        reset_fixture();
        struct app_gateway_operation_lease other={0};
        if (which==0u) gateway_route_refresh_result_token=55u;
        if (which==1u) sequence_available=false;
        if (which==2u) publication_pending=true;
        if (which==3u) publisher_active=true;
        if (which==4u) assert(gateway_operation_owner_claim(APP_GATEWAY_OPERATION_OWNER_SURVEY,&other)==0);
        assert(start()<0); assert(hia_requests==0u && sequence_allocations==0u);
        assert(!gateway_discovery_assignment_state.active && result.count==1u);
        assert(result.status!=COMMAND_OK);
        if (which==4u) assert(gateway_operation_owner_matches(APP_GATEWAY_OPERATION_OWNER_SURVEY,&other));
    }
}
static void test_pending_hia_rejects_replacement_assignment_and_prearm(void)
{
    reset_fixture(); assert(start()==0);
    uint32_t epoch=gateway_discovery_assignment_state.epoch;
    uint32_t lease=gateway_assignment_operation_lease.generation;
    host.session_id++; assert(start()==-EBUSY);
    assert(gateway_enumeration_prearm_prepare(&policy,false)==-EBUSY);
    assert(gateway_discovery_assignment_state.epoch==epoch);
    assert(gateway_assignment_operation_lease.generation==lease && gateway_assignment_hia_pending);
    assert(hia_requests==1u && sequence_allocations==1u);
}
static void test_rx_start_failure_keeps_owner_until_actual_hia_terminal(void)
{
    reset_fixture(); assert(start()==0); rx_return=-EIO; pipeline();
    assert_owner(); assert(gateway_assignment_hia_pending && gateway_assignment_hia_error==-EIO);
    assert(!gateway_discovery_assignment_state.response_lane_active && result.count==0u);
    /* Even a later successful RX start cannot erase a failed owner boundary. */
    rx_return=0; pipeline(); refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    assert(!gateway_assignment_hia_pending && gateway_discovery_assignment_state.active);
    assert(!gateway_discovery_assignment_state.claim_delivery_succeeded);
    finish_abort(); assert_assignment_terminal(COMMAND_RADIO_ERROR);
}
static void test_hia_failures_cleanup_when_remote_preparation_is_possible(void)
{
    for (unsigned prepared=0u;prepared<2u;prepared++) {
        for (unsigned sent=0u;sent<2u;sent++) {
            reset_fixture(); assert(start()==0);
            if (prepared) pipeline();
            refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,-EIO,(uint8_t)sent);
            assert(gateway_discovery_assignment_state.claim_rf_started==(sent!=0u));
            if (sent || prepared) { assert_owner(); finish_abort(); assert(abandons==1u); }
            else assert(aborts==0u);
            assert_assignment_terminal(COMMAND_RADIO_ERROR);
            assert(terminal.reason==GATEWAY_COMMAND_EVENT_REASON_RADIO);
        }
    }
}
static void test_expired_hia_keeps_original_deadline_through_abort(void)
{
    reset_fixture(); assert(start()==0); pipeline();
    uint64_t deadline=gateway_discovery_assignment_state.operation_deadline_ms;
    now_ms=deadline; refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    assert(gateway_discovery_assignment_state.operation_deadline_ms==deadline);
    assert(delivery_deadline==deadline+GATEWAY_DISCOVERY_ASSIGNMENT_ABORT_DELIVERY_BUDGET_MS);
    assert(delivery_profile==NODE_COMM_PROFILE_SINGLE_CONTROL_ORIGIN);
    enum discovery_assignment_phase phase; uint32_t epoch;
    struct discovery_assignment_abort_identity identity;
    assert(discovery_assignment_extract_control_tlvs(submitted.payload,submitted.payload_len,&phase,&epoch)==PROTO_OK);
    assert(phase==DISCOVERY_ASSIGNMENT_PHASE_ABORT && epoch==gateway_discovery_assignment_state.epoch);
    assert(discovery_assignment_extract_abort_identity(submitted.payload,submitted.payload_len,&identity)==PROTO_OK);
    assert(identity.epoch==epoch && identity.claim_session_id==epoch && identity.claim_command_seq==epoch);
    assert_owner(); finish_abort(); assert_assignment_terminal(COMMAND_TIMEOUT);
    assert(terminal.reason==GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
    assert(gateway_discovery_assignment_submit_control_flood_locked(&submitted,
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM)==-ETIMEDOUT);
    assert(gateway_discovery_assignment_submit_control_flood_locked(&submitted,
        GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE)==-ETIMEDOUT);
    assert(aborts==1u);
}
static void test_start_budget_is_not_restarted_after_slow_admission(void)
{
    reset_fixture(); uint64_t deadline=now_ms+policy.operation_budget_ms;
    dispatch_delay_ms=policy.operation_budget_ms-1000u;
    assert(start()==0); assert(hia_budget==1000u);
    assert(gateway_discovery_assignment_state.operation_deadline_ms==deadline);
    reset_fixture(); dispatch_delay_ms=policy.operation_budget_ms;
    assert(start()==0); assert(hia_requests==0u); assert_assignment_terminal(COMMAND_RADIO_ERROR);
}
static void test_clock_crossing_uint32_wrap_preserves_64bit_owner_budget(void)
{
    reset_fixture(); now_ms=UINT32_MAX-100u; assert(start()==0); pipeline();
    uint64_t deadline=gateway_discovery_assignment_state.operation_deadline_ms;
    now_ms+=5000u; refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    assert_owner(); assert(gateway_discovery_assignment_state.operation_deadline_ms==deadline);
    assert(gateway_discovery_assignment_state.claim_delivery_succeeded);
}
static void test_stale_or_foreign_hia_callback_cannot_touch_pending_successor(void)
{
    for (unsigned field=0u;field<6u;field++) {
        reset_fixture(); assert(start()==0); uint64_t deadline=gateway_discovery_assignment_state.operation_deadline_ms;
        uint32_t epoch=gateway_discovery_assignment_state.epoch;
        gateway_route_refresh_result_token=0x999u;
        struct app_node_comm_route_refresh_event stale={.kind=APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,
            .correlation=hia_identity,.sent_count=3u,.result=-EIO,.correlated=true};
        if (field==0u) stale.correlation.src_id++;
        if (field==1u) stale.correlation.dst_id++;
        if (field==2u) stale.correlation.session_id++;
        if (field==3u) stale.correlation.seq++;
        if (field==4u) stale.correlation.msg_type++;
        if (field==5u) stale.correlated=false;
        gateway_route_refresh_observe(&stale);
        assert_owner(); assert(gateway_assignment_hia_pending);
        assert(gateway_discovery_assignment_state.epoch==epoch);
        assert(gateway_discovery_assignment_state.operation_deadline_ms==deadline);
        if (gateway_route_refresh_result_token!=0x999u)
            fprintf(stderr,"stale callback field=%u spent generic token, command=%u terminals=%u\n",field,result.command,terminals);
        assert(gateway_route_refresh_result_token==0x999u);
        assert(result.count==0u && terminals==0u && aborts==0u);
    }
}
static void test_expired_or_different_policy_prearm_cannot_seed_successor_records(void)
{
    for (unsigned expired=0u;expired<2u;expired++) {
        reset_fixture(); struct operation_policy_assignment previous=policy;
        if (!expired) previous.expected_anchor_count--;
        assert(gateway_enumeration_prearm_prepare(&previous,false)==0);
        uint32_t epoch=gateway_discovery_assignment_state.epoch;
        gateway_discovery_assignment_state.anchor_ids[0]=0xdead;
        gateway_discovery_assignment_state.claim_count=1u;
        if (expired) now_ms=gateway_discovery_assignment_state.operation_deadline_ms;
        assert(start()==0); assert(hia_requests==1u && gateway_assignment_hia_pending);
        assert(gateway_discovery_assignment_state.epoch!=epoch);
        assert(gateway_discovery_assignment_state.claim_count==0u);
    }
}
static void test_cleanup_failure_retains_owner_and_stops_watchdog(void)
{
    reset_fixture(); assert(start()==0); pipeline();
    refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,-EIO,1u);
    abandon_return=-EIO; finish_abort();
    assert_owner(); assert(stop_feeds==1u && result.count==0u);
    abandon_return=0; gateway_discovery_assignment_finish_failure_locked(COMMAND_RADIO_ERROR,EIO);
    assert_assignment_terminal(COMMAND_RADIO_ERROR);
}
static void test_abort_admission_retries_are_bounded_and_retain_exact_cleanup_identity(void)
{
    reset_fixture(); assert(start()==0); pipeline(); abort_return=-EAGAIN;
    uint64_t deadline=gateway_discovery_assignment_state.operation_deadline_ms;
    refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,-EIO,1u);
    uint32_t abort_identity=gateway_discovery_assignment_state.abort_command_seq;
    uint16_t abort_sequence=gateway_discovery_assignment_state.abort_packet_seq;
    for (unsigned retry=1u;retry<10u;retry++) {
        assert_owner(); assert(result.count==0u && aborts==retry);
        assert(gateway_discovery_assignment_state.delivery_handle==0u);
        assert(gateway_discovery_assignment_state.operation_deadline_ms==deadline);
        assert(gateway_discovery_assignment_state.abort_command_seq==abort_identity);
        assert(gateway_discovery_assignment_state.abort_packet_seq==abort_sequence);
        now_ms+=100u;
        gateway_discovery_assignment_fail_locked(COMMAND_RADIO_ERROR,EIO);
    }
    assert(aborts==10u); assert_assignment_terminal(COMMAND_RADIO_ERROR);
}
static void test_completion_schedule_failure_keeps_owner_and_stops_watchdog(void)
{
    reset_fixture(); assert(start()==0); pipeline(); schedule_return=-EIO;
    refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    assert_owner(); assert(stop_feeds==1u && result.count==0u && aborts==0u);
    assert(!gateway_assignment_hia_pending && gateway_discovery_assignment_state.claim_delivery_succeeded);
}
static void test_durable_assignment_uses_same_hia_owner_without_ram_clear(void)
{
    reset_fixture(); policy.ram_only_iteration=false; encode_policy();
    assert(start()==0); assert(durable_clears==1u && ram_clears==0u);
    struct operation_policy_set installed;
    app_operation_policy_snapshot(&installed);
    assert(!installed.assignment.ram_only_iteration);
    pipeline(); refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    assert_owner(); assert(hia_requests==1u && result.count==0u);
    assert(!gateway_discovery_assignment_state.ram_only_iteration);
}
static void test_duplicate_automatic_completion_cannot_spend_later_generic_token(void)
{
    reset_fixture(); assert(start()==0); pipeline();
    refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,0,3u);
    uint64_t lane=gateway_discovery_assignment_state.response_lane_start_ms;
    gateway_route_refresh_host_command=host;
    gateway_route_refresh_host_command.seq++;
    gateway_route_refresh_result_token=0x9988u;
    refresh(APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,-EIO,3u);
    assert_owner(); assert(result.count==0u && terminals==0u && aborts==0u);
    assert(gateway_route_refresh_result_token==0x9988u);
    assert(gateway_discovery_assignment_state.response_lane_start_ms==lane);
}
static void test_exact_generic_hia_owner_still_gets_its_own_terminal(void)
{
    for (unsigned failure=0u;failure<2u;failure++) {
        reset_fixture(); assert(gateway_enumeration_prearm_prepare(&policy,false)==0);
        gateway_route_refresh_host_command=host;
        gateway_route_refresh_result_token=0x9988u;
        struct app_node_comm_route_refresh_event event={.kind=APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT,
            .correlation=host,.sent_count=1u,.correlated=true};
        gateway_route_refresh_observe(&event);
        assert(result.count==0u && gateway_route_refresh_result_token==0x9988u);
        event.kind=APP_NODE_COMM_ROUTE_REFRESH_COMPLETE;
        event.result=failure ? -ETIMEDOUT : 0;
        gateway_route_refresh_observe(&event);
        assert(result.count==1u && terminals==1u && result.command==CMD_FORCE_REDISCOVERY);
        assert(result.token==0x9988u && gateway_route_refresh_result_token==0u);
        assert(result.host.session_id==host.session_id && result.host.seq==host.seq);
        assert(result.status==(failure ? COMMAND_TIMEOUT : COMMAND_OK));
        assert(gateway_enumeration_prearm_valid_locked()==!failure);
        gateway_route_refresh_observe(&event);
        assert(result.count==1u && terminals==1u);
    }
}
int main(void)
{
    test_cold_assignment_requests_hia_and_keeps_absolute_budget();
    test_matching_prearm_is_consumed_without_second_hia();
    test_inline_completion_keeps_exact_host_owner();
    test_submission_failures_are_local_radio_terminals_not_no_anchors();
    test_admission_backpressure_has_no_hia_or_new_identity();
    test_pending_hia_rejects_replacement_assignment_and_prearm();
    test_rx_start_failure_keeps_owner_until_actual_hia_terminal();
    test_hia_failures_cleanup_when_remote_preparation_is_possible();
    test_expired_hia_keeps_original_deadline_through_abort();
    test_start_budget_is_not_restarted_after_slow_admission();
    test_clock_crossing_uint32_wrap_preserves_64bit_owner_budget();
    test_stale_or_foreign_hia_callback_cannot_touch_pending_successor();
    test_expired_or_different_policy_prearm_cannot_seed_successor_records();
    test_cleanup_failure_retains_owner_and_stops_watchdog();
    test_abort_admission_retries_are_bounded_and_retain_exact_cleanup_identity();
    test_completion_schedule_failure_keeps_owner_and_stops_watchdog();
    test_durable_assignment_uses_same_hia_owner_without_ram_clear();
    test_duplicate_automatic_completion_cannot_spend_later_generic_token();
    test_exact_generic_hia_owner_still_gets_its_own_terminal();
    puts("gateway assignment HIA production seams: PASS");
    return 0;
}
