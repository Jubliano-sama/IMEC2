/* Execute actual HIA prearm and survey START application code. Assignment
 * policy, roster storage, identity matching and lifecycle use production code;
 * only Zephyr locks, work scheduling and physical upstream callbacks are fake. */
#include "app_discovery_assignment_policy.h"
#include "enumeration_response_lane.h"
#include "mesh_relay.h"
#include "protocol_rx_lifecycle.h"
#include "survey_protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ROLE_ANCHOR 1
#define DEVICE_ROLE ROLE_ANCHOR
#define DEVICE_ID UINT64_C(0xa100)
#define GATEWAY_ID UINT64_C(0x9000)
#define K_FOREVER 0
#define APP_SURVEY_ANCHOR_PREPARE_MS 100u
#define APP_SURVEY_START_EDGE_SLOP_MS SURVEY_RADIO_GUARD_MS
#define DWM3000_RECEIVE_ABORT_MESH_CONTROL 1
#define OLD_EPOCH UINT32_C(38797366)
#define NEW_EPOCH UINT32_C(21757995)
#define TABLE_SEQ UINT32_C(21758005)
typedef int k_spinlock_key_t;
static int anchor_enumeration_rx_lock, anchor_discovery_claim_mutex, survey_lock, anchor_work;
static unsigned lock_depth, claim_calls, scheduled, consumed;
static uint32_t now_ms;
static uint64_t scheduled_at;
static struct mesh_relay mesh_runtime;
static struct protocol_rx_lifecycle anchor_enumeration_rx_lifecycle, anchor_rx_lifecycle;
static bool anchor_enumeration_rx_prearmed, anchor_enumeration_rx_survey_follows;
static bool anchor_enumeration_rx_claim_identity_valid;
static uint32_t anchor_enumeration_rx_claim_session_id, anchor_enumeration_rx_claim_command_seq;
static struct app_discovery_assignment_policy assignment_policy;
struct anchor_enumeration_response_config {
    uint64_t start_ms, hia_deepest_source_start_ms, parent_id;
    uint32_t epoch;
    uint8_t hop_count, max_hop_count;
    bool hia_pipeline, active;
};
static struct anchor_enumeration_response_config anchor_enumeration_response_config;
enum app_survey_anchor_action { APP_SURVEY_ANCHOR_ACTION_NONE, APP_SURVEY_ANCHOR_ACTION_NEIGHBORS,
    APP_SURVEY_ANCHOR_ACTION_EXECUTE, APP_SURVEY_ANCHOR_ACTION_EXPIRE };
#include "hia_survey_state.inc"
static struct app_survey_anchor_state anchor_state;
static struct {
    int (*anchor_upstream)(uint64_t *,uint8_t *);
    int (*anchor_consume_enumeration_handoff)(uint32_t);
} survey_ops;

static uint32_t k_uptime_get_32(void) { return now_ms; }
static uint64_t k_uptime_get(void) { return now_ms; }
static int k_spin_lock(int *lock) { (void)lock; assert(lock_depth++==0u); return 0; }
static void k_spin_unlock(int *lock,int key) { (void)lock; (void)key; assert(lock_depth--==1u); }
static void k_mutex_lock(int *lock,int timeout) { (void)timeout; (void)k_spin_lock(lock); }
static void k_mutex_unlock(int *lock) { k_spin_unlock(lock,0); }
static int k_work_cancel_delayable(int *work) { (void)work; return 0; }
static void dwm3000_driver_request_receive_abort(int reason) { (void)reason; }
static void status_debug_printf(const char *fmt,...) { (void)fmt; }
static int anchor_uwb_scan_schedule_ms(uint32_t ms) { (void)ms; return 0; }
static void anchor_enumeration_rx_clear_phase_identities(void)
{
    anchor_enumeration_rx_prearmed=false;
    anchor_enumeration_rx_survey_follows=false;
    anchor_enumeration_rx_claim_identity_valid=false;
    anchor_enumeration_rx_claim_session_id=anchor_enumeration_rx_claim_command_seq=0u;
}
static void anchor_compact_enumeration_deactivate(uint32_t epoch)
{ if (anchor_enumeration_response_config.epoch==epoch) anchor_enumeration_response_config.active=false; }
static bool anchor_enumeration_rx_terminate_epoch(uint32_t epoch,const char *reason)
{
    (void)reason;
    bool ended=protocol_rx_lifecycle_terminate(&anchor_enumeration_rx_lifecycle,PROTOCOL_RX_OPERATION_ENUMERATION,epoch);
    if (ended) { anchor_enumeration_rx_clear_phase_identities(); anchor_compact_enumeration_deactivate(epoch); }
    return ended;
}
static enum app_discovery_assignment_claim_decision local_anchor_discovery_assignment_note_claim(uint32_t epoch)
{ claim_calls++; return app_discovery_assignment_policy_note_claim(&assignment_policy,epoch); }
static bool local_anchor_discovery_assignment_identity_get(uint32_t *epoch,uint32_t *seq,
    struct discovery_assignment_table_commitment *commitment,uint8_t *slot,uint8_t *count)
{
    *epoch=assignment_policy.committed_epoch; *seq=assignment_policy.committed_table_seq;
    *commitment=assignment_policy.committed_table_commitment; *slot=0u; *count=2u;
    return assignment_policy.provisioned;
}
static int anchor_work_reschedule(uint64_t due) { scheduled++; scheduled_at=due; return 0; }
static int upstream(uint64_t *parent,uint8_t *hops) { *parent=GATEWAY_ID; *hops=1u; return 0; }
static int consume(uint32_t epoch)
{ assert(epoch==NEW_EPOCH); consumed++; return 0; }

#include "hia_prearm_production.inc"
#include "hia_survey_production.inc"

static struct discovery_assignment_table_commitment table_commitment;
static struct discovery_assignment_entry entries[] = {
    {.anchor_id=DEVICE_ID,.slot=0u}, {.anchor_id=UINT64_C(0xa101),.slot=1u},
};
static void reset_fixture(void)
{
    now_ms=1000u; lock_depth=claim_calls=scheduled=consumed=0u;
    scheduled_at=0u;
    memset(&anchor_state,0,sizeof(anchor_state));
    memset(&anchor_enumeration_response_config,0,sizeof(anchor_enumeration_response_config));
    protocol_rx_lifecycle_init(&anchor_enumeration_rx_lifecycle);
    protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
    anchor_enumeration_rx_clear_phase_identities();
    for (size_t i=0u;i<2u;i++) entries[i].hash=discovery_assignment_hash(entries[i].anchor_id);
    assert(discovery_assignment_table_commitment(entries,2u,2u,&table_commitment));
    app_discovery_assignment_policy_init(&assignment_policy,true,true,true,
        OLD_EPOCH,OLD_EPOCH+10u,&table_commitment);
    mesh_relay_init(&mesh_runtime,MESH_RELAY_ROLE_ANCHOR,DEVICE_ID,GATEWAY_ID,1u);
    struct route_candidate route={.next_hop_id=GATEWAY_ID,.gateway_id=GATEWAY_ID,
        .route_epoch=1u,.last_seen_ms=now_ms,.hop_count=0u,.link_quality=90u,.valid=true};
    assert(route_upsert_candidate(&mesh_runtime.upstream,&route)==PROTO_OK);
    survey_ops.anchor_upstream=upstream; survey_ops.anchor_consume_enumeration_handoff=consume;
}
static int prearm(uint32_t epoch)
{ return anchor_enumeration_rx_prearm(epoch,5000u,60000u,true,1000u,1u); }
static enum app_discovery_assignment_table_decision apply_table(uint32_t epoch)
{
    struct app_discovery_assignment_policy preview=assignment_policy;
    enum app_discovery_assignment_table_decision decision=app_discovery_assignment_policy_note_table(
        &preview,epoch,TABLE_SEQ,&table_commitment);
    if (decision!=APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY) return decision;
    assert(app_survey_anchor_note_ram_roster(entries,2u,2u,epoch,TABLE_SEQ,&table_commitment)==0);
    assert(app_discovery_assignment_policy_note_table(&assignment_policy,epoch,TABLE_SEQ,&table_commitment)==decision);
    assert(app_discovery_assignment_policy_commit(&assignment_policy,epoch,TABLE_SEQ,&table_commitment));
    return decision;
}
static struct survey_control start_control(void)
{
    return (struct survey_control){.phase=SURVEY_PHASE_NEIGHBOR_START,
        .identity={.generation=55u,.assignment={.assignment_epoch=NEW_EPOCH,
            .table_command_seq=TABLE_SEQ,.table_commitment=table_commitment,
            .slot_span=2u,.max_hop_count=2u}},
        .start_delay_present=true,.start_delay_ms=500u,
        .self_stop_delay_present=true,.self_stop_delay_ms=10000u};
}
static void test_lower_hia_authorizes_table_and_survey_without_separate_claim(void)
{
    reset_fixture();
    assert(apply_table(NEW_EPOCH)==APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(!anchor_state.roster_valid);
    assert(prearm(NEW_EPOCH)==0);
    assert(claim_calls==1u && assignment_policy.joining_epoch==NEW_EPOCH && assignment_policy.claim_observed);
    assert(anchor_enumeration_rx_claim_identity_valid);
    assert(anchor_enumeration_rx_claim_session_id==NEW_EPOCH && anchor_enumeration_rx_claim_command_seq==NEW_EPOCH);
    uint32_t deadline=anchor_enumeration_rx_lifecycle.deadline_ms;
    assert(deadline==6000u && anchor_enumeration_rx_prearmed);
    assert(apply_table(NEW_EPOCH+1u)==APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(apply_table(NEW_EPOCH)==APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    assert(anchor_state.roster_valid && anchor_state.roster_assignment_epoch==NEW_EPOCH);
    now_ms+=100u;
    assert(prearm(NEW_EPOCH)==0);
    assert(claim_calls==1u && assignment_policy.joining_epoch==0u);
    assert(anchor_enumeration_rx_lifecycle.deadline_ms==deadline);
    assert(anchor_state.roster_valid && anchor_state.node_ids_by_slot[1]==entries[1].anchor_id);
    struct survey_control control=start_control();
    struct proto_packet packet={.msg_type=MSG_COMMAND,.src_id=GATEWAY_ID};
    assert(app_survey_anchor_apply_control(&packet,&control)==0);
    assert(anchor_state.active && anchor_state.action==APP_SURVEY_ANCHOR_ACTION_NEIGHBORS);
    assert(scheduled==1u && consumed==1u && scheduled_at==now_ms+400u);
    assert(lock_depth==0u);
}
static void test_invalid_or_conflicting_hia_does_not_rebase_policy(void)
{
    reset_fixture();
    assert(prearm(0u)==-EINVAL && claim_calls==0u);
    anchor_enumeration_response_config=(struct anchor_enumeration_response_config){
        .active=true,.epoch=NEW_EPOCH,.hia_pipeline=true,.hia_deepest_source_start_ms=1u};
    assert(prearm(NEW_EPOCH)==-ESTALE);
    assert(claim_calls==0u && assignment_policy.committed_epoch==OLD_EPOCH);
    assert(apply_table(NEW_EPOCH)==APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE);
    assert(!anchor_state.roster_valid && lock_depth==0u);
}
static void test_survey_requires_matching_accepted_roster(void)
{
    reset_fixture();
    struct survey_control control=start_control();
    struct proto_packet packet={.msg_type=MSG_COMMAND,.src_id=GATEWAY_ID};
    assert(app_survey_anchor_apply_control(&packet,&control)==-ESTALE);
    assert(prearm(NEW_EPOCH)==0 && apply_table(NEW_EPOCH)==APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY);
    control.identity.assignment.table_command_seq++;
    assert(app_survey_anchor_apply_control(&packet,&control)==-ESTALE);
    assert(!anchor_state.active && scheduled==0u && consumed==0u);
}
int main(void)
{
    test_lower_hia_authorizes_table_and_survey_without_separate_claim();
    test_invalid_or_conflicting_hia_does_not_rebase_policy();
    test_survey_requires_matching_accepted_roster();
    puts("production HIA TABLE survey harness passed");
    return 0;
}
