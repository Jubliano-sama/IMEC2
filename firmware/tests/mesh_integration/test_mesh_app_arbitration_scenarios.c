#include "app_mesh_c5_priority.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_coordinator_runtime.h"
#include "app_mesh_arbitration_zephyr.h"
#include "app_gateway_command_ingress.h"
#include "app_mesh_gateway_command_priority.h"
#include "app_mesh_preemption.h"
#include "mesh_preemption.h"
#include "route.h"
#include "serial_frame.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ANCHOR_ID UINT64_C(0xA100)
#define GATEWAY_ID UINT64_C(0x9000)
#define TRANSIT_ID UINT64_C(0xB001)
#define ROUTE_EPOCH UINT32_C(17)

struct click_preempt_fixture {
    uint8_t save_calls;
    uint8_t clear_calls;
    uint8_t cancel_calls;
    uint8_t schedule_calls;
    uint8_t requeue_calls;
    int save_ret;
    int clear_ret;
    int cancel_ret;
    int schedule_ret;
    int requeue_ret;
    struct mesh_outbound requeued;
};

struct ingress_scenario_fixture {
    struct app_gateway_command_ingress_item queued;
    struct app_gateway_command_identity cancelled;
    struct proto_packet host_error;
    struct proto_packet anchor_result;
    struct k_work_delayable work;
    struct k_work_q priority_work_queue;
    uint8_t admission_count;
    uint8_t cancel_count;
    uint8_t error_count;
    uint8_t flood_count;
    uint8_t anchor_receive_count;
    bool queued_valid;
};

static uint8_t receive_abort_requests;
static uint8_t receive_abort_clears;

void dwm3000_driver_request_receive_abort(void)
{
    receive_abort_requests++;
}

void dwm3000_driver_clear_receive_abort(void)
{
    receive_abort_clears++;
}

static struct route_candidate direct_gateway_route(void)
{
    return (struct route_candidate) {
        .next_hop_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .hop_count = 1u,
        .link_quality = 90u,
        .route_cost = 110u,
        .last_seen_ms = 1000u,
        .last_success_ms = 1000u,
        .valid = true,
    };
}

static struct mesh_outbound outbound(uint8_t msg_type,
                                     uint64_t src_id,
                                     uint16_t seq)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = msg_type,
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = src_id,
            .dst_id = GATEWAY_ID,
            .session_id = ROUTE_EPOCH,
            .seq = seq,
            .ttl = MESH_GATEWAY_ACK_TTL,
        },
        .next_hop_id = GATEWAY_ID,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
    };
}

static void start_gateway_bound_tx(struct mesh_relay *relay,
                                   uint8_t msg_type,
                                   uint64_t src_id,
                                   uint16_t seq,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    struct proto_packet packet = outbound(msg_type, src_id, seq).packet;
    struct route_candidate route = direct_gateway_route();
    struct mesh_outbound started;

    mesh_relay_init(relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_ID,
                    GATEWAY_ID,
                    ROUTE_EPOCH);
    assert(route_upsert_candidate(&relay->upstream, &route) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(mesh_relay_start_tx(relay,
                               &packet,
                               payload,
                               payload_len,
                               1000u,
                               &started) == PROTO_OK);
    relay->pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
}

static struct mesh_outbound pending_hop_ack(void)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = MSG_MESH_HOP_ACK,
            .src_id = ANCHOR_ID,
            .dst_id = TRANSIT_ID,
            .session_id = ROUTE_EPOCH,
            .seq = 8u,
            .ttl = 1u,
        },
        .next_hop_id = TRANSIT_ID,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
    };
}

static int save_outbox(void *ctx)
{
    struct click_preempt_fixture *fixture = ctx;

    fixture->save_calls++;
    return fixture->save_ret;
}

static int clear_outbox(void *ctx)
{
    struct click_preempt_fixture *fixture = ctx;

    fixture->clear_calls++;
    return fixture->clear_ret;
}

static int cancel_timeout(void *ctx)
{
    struct click_preempt_fixture *fixture = ctx;

    fixture->cancel_calls++;
    return fixture->cancel_ret;
}

static int schedule_timeout(void *ctx)
{
    struct click_preempt_fixture *fixture = ctx;

    fixture->schedule_calls++;
    return fixture->schedule_ret;
}

static int requeue_click_report(void *ctx, const struct mesh_outbound *outbound)
{
    struct click_preempt_fixture *fixture = ctx;

    fixture->requeue_calls++;
    fixture->requeued = *outbound;
    return fixture->requeue_ret;
}

static int ingress_admit(void *ctx, struct app_gateway_command_ingress_item *item)
{
    struct ingress_scenario_fixture *fixture = ctx;

    assert(!fixture->queued_valid);
    item->admission_id = 1u;
    fixture->queued = *item;
    fixture->queued_valid = true;
    fixture->admission_count++;
    return 0;
}

static int ingress_submit_priority(void *ctx)
{
    struct ingress_scenario_fixture *fixture = ctx;
    const struct app_mesh_arbitration_zephyr_gateway_ops ops = {
        .gateway_role = true,
        .priority_work_queue = &fixture->priority_work_queue,
    };

    return app_mesh_arbitration_zephyr_gateway_command_submit(&ops,
                                                               &fixture->work);
}

static int ingress_cancel(void *ctx,
                          const struct app_gateway_command_identity *identity)
{
    struct ingress_scenario_fixture *fixture = ctx;

    fixture->cancelled = *identity;
    fixture->cancel_count++;
    return 0;
}

static void ingress_emit_error(void *ctx,
                               const struct proto_packet *command,
                               enum command_id command_id,
                               enum command_status status,
                               uint8_t reason)
{
    struct ingress_scenario_fixture *fixture = ctx;

    (void)command_id;
    (void)status;
    (void)reason;
    fixture->host_error = *command;
    fixture->error_count++;
}

static void simulated_gateway_command_flood(
    struct ingress_scenario_fixture *fixture,
    const struct app_gateway_command_ingress_item *item)
{
    fixture->flood_count++;
    assert(item->packet.msg_type == MSG_COMMAND);
    assert(item->packet.seq == 61u);
}

static void simulated_anchor_receive_and_result(
    struct ingress_scenario_fixture *fixture,
    const struct app_gateway_command_ingress_item *item)
{
    fixture->anchor_receive_count++;
    fixture->anchor_result = (struct proto_packet) {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID,
        .dst_id = GATEWAY_ID,
        .session_id = item->packet.session_id,
        .seq = item->packet.seq,
    };
}

static void run_scheduled_gateway_command(struct ingress_scenario_fixture *fixture)
{
    assert(fixture->work.reschedule_calls == 1u);
    assert(fixture->queued_valid);
    simulated_gateway_command_flood(fixture, &fixture->queued);
    simulated_anchor_receive_and_result(fixture, &fixture->queued);
    fixture->queued_valid = false;
}

static uint32_t lost_packet_tlv_count(const struct mesh_outbound *outbound)
{
    size_t offset = 0u;
    uint32_t count = 0u;

    while (offset < outbound->payload_len) {
        uint8_t len;

        assert((size_t)outbound->payload_len - offset >= 2u);
        len = outbound->payload[offset + 1u];
        assert((size_t)outbound->payload_len - offset - 2u >= len);
        if (outbound->payload[offset] == TLV_MESH_LOST_PACKET_COUNT) {
            count++;
        }
        offset += 2u + len;
    }
    return count;
}

static uint32_t lost_packet_tlv_value(const struct mesh_outbound *outbound)
{
    const uint8_t *value = NULL;
    uint8_t len = 0u;

    assert(tlv_find(outbound->payload,
                    outbound->payload_len,
                    TLV_MESH_LOST_PACKET_COUNT,
                    &value,
                    &len) == PROTO_OK);
    assert(len == 4u);
    return proto_get_u32_le(value);
}

static void test_gateway_command_aborts_receive_before_priority_scheduling(void)
{
    struct mesh_relay relay;
    struct app_mesh_ch9_ack_table ack_table;
    struct app_mesh_ch9_ack_batch_entry ack_entry = {
        .session_id = ROUTE_EPOCH,
        .packet_id = 0x44u,
        .seq = 7u,
        .has_packet_id = true,
    };
    struct mesh_outbound ack = pending_hop_ack();
    struct k_work_delayable command_work = {0};
    struct k_work_q priority_work_queue = {0};
    const struct app_mesh_arbitration_zephyr_gateway_ops ops = {
        .gateway_role = true,
        .priority_work_queue = &priority_work_queue,
    };
    struct app_mesh_coordinator_runtime_capture capture = {0};
    struct app_mesh_coordinator_runtime_state runtime_state;
    struct app_mesh_coordinator_decision decision;
    enum app_mesh_ch9_ack_queue_result queue_result;
    bool state_changed;
    receive_abort_requests = 0u;
    receive_abort_clears = 0u;

    start_gateway_bound_tx(&relay, MSG_MESH_DATA, TRANSIT_ID, 7u, NULL, 0u);
    app_mesh_ch9_ack_table_init(&ack_table);
    assert(app_mesh_ch9_ack_table_queue(&ack_table,
                                        &ack,
                                        &ack_entry,
                                        &queue_result) == PROTO_OK);
    assert(queue_result == APP_MESH_CH9_ACK_QUEUE_ADDED);
    assert(app_mesh_ch9_ack_table_any_pending(&ack_table));

    capture.relay_tx_active = mesh_relay_tx_active(&relay);
    capture.route_waiting_tx_active = true;
    capture.ch9_ack_wait_active =
        app_mesh_ch9_core_ack_wait_active(&relay.pending,
                                          mesh_relay_tx_active(&relay));
    capture.ch9_ack_send_pending =
        app_mesh_ch9_ack_table_any_pending(&ack_table);
    capture.report_queue_used = 1u;
    app_mesh_coordinator_runtime_reset(&runtime_state);
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_MESH_RX);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);

    assert(app_mesh_arbitration_zephyr_gateway_command_submit(
               &ops, &command_work) == 0);
    assert(receive_abort_requests == 1u);
    assert(command_work.reschedule_calls == 0u);
    assert(app_mesh_arbitration_zephyr_gateway_receive_abort_observed() == 0);
    assert(command_work.reschedule_calls == 1u);
    assert(command_work.last_queue == &priority_work_queue);
    assert(receive_abort_clears == 0u);
}

static void test_gateway_ble_ingress_waits_for_safe_boundary_then_preserves_result_identity(void)
{
    const uint8_t payload[] = {
        TLV_COMMAND_ID, 2u,
        (uint8_t)CMD_FORCE_REDISCOVERY,
        (uint8_t)(CMD_FORCE_REDISCOVERY >> 8),
    };
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1234),
        .dst_id = ANCHOR_ID,
        .session_id = ROUTE_EPOCH,
        .seq = 61u,
    };
    struct app_gateway_command_ingress_ops ingress_ops = {
        .gateway_role = true,
        .admit = ingress_admit,
        .submit_priority = ingress_submit_priority,
        .cancel_admitted = ingress_cancel,
        .emit_result = ingress_emit_error,
    };
    struct ingress_scenario_fixture fixture = {0};
    struct app_gateway_command_ingress_item decoded;
    struct mesh_pending_tx result_ack = {
        .state = MESH_RELAY_TX_WAIT_GATEWAY_ACK,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = GATEWAY_ID,
    };
    bool command_handled;
    bool anchor_ranging_active = true;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;

    receive_abort_requests = 0u;
    receive_abort_clears = 0u;
    ingress_ops.ctx = &fixture;
    command.payload_len = sizeof(payload);
    assert(serial_frame_encode_packet(&command, payload, frame, sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_gateway_command_ingress_handle_frame(&ingress_ops, frame,
                                                    frame_len, &decoded,
                                                    &command_handled) == 0);
    assert(command_handled);
    assert(fixture.admission_count == 1u);
    assert(receive_abort_requests == 1u);
    assert(fixture.work.reschedule_calls == 0u);

    /* The model deliberately withholds the RX-safe callback during DS-TWR. */
    assert(anchor_ranging_active);
    assert(fixture.work.reschedule_calls == 0u);
    anchor_ranging_active = false;
    assert(!anchor_ranging_active);
    assert(app_mesh_arbitration_zephyr_gateway_receive_abort_observed() == 0);
    assert(fixture.work.reschedule_calls == 1u);
    assert(fixture.work.last_queue == &fixture.priority_work_queue);

    run_scheduled_gateway_command(&fixture);
    assert(fixture.flood_count == 1u);
    assert(fixture.anchor_receive_count == 1u);
    assert(fixture.anchor_result.msg_type == MSG_COMMAND_RESULT);
    assert(fixture.anchor_result.dst_id == GATEWAY_ID);
    assert(fixture.anchor_result.session_id == command.session_id);
    assert(fixture.anchor_result.seq == command.seq);
    assert(app_mesh_ch9_core_ack_wait_active(&result_ack, true));
    assert(fixture.error_count == 0u);
}

static void test_click_claim_requeues_one_local_report_without_corruption(void)
{
    const uint8_t payload[] = {0xdeu, 0xadu, 0xbeu, 0xefu};
    struct mesh_relay relay;
    struct mesh_click_preempt_plan plan;
    struct mesh_click_preempt_plan after_preempt;
    struct click_preempt_fixture fixture = {0};
    const struct app_mesh_click_preempt_ops ops = {
        .save_outbox = save_outbox,
        .clear_outbox = clear_outbox,
        .cancel_timeout = cancel_timeout,
        .schedule_timeout = schedule_timeout,
        .requeue_click_report = requeue_click_report,
        .ctx = &fixture,
    };
    struct app_mesh_click_preempt_result result;
    struct app_mesh_coordinator_runtime_capture capture = {0};
    struct app_mesh_coordinator_runtime_state runtime_state;
    struct app_mesh_coordinator_decision decision;
    bool state_changed;

    start_gateway_bound_tx(&relay,
                           MSG_CLICK_REPORT,
                           ANCHOR_ID,
                           9u,
                           payload,
                           sizeof(payload));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_COUNT_AS_CLICK));
    assert(app_mesh_c5_wake_claim_requires_anchor_handoff(
        FLAG_COUNT_AS_CLICK, true));
    assert(app_mesh_c5_connected_gap_rx_action(true, false) ==
           APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK);

    capture.click_active = true;
    capture.relay_tx_active = mesh_relay_tx_active(&relay);
    capture.report_queue_used = 1u;
    app_mesh_coordinator_runtime_reset(&runtime_state);
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_CLICK);
    assert(!decision.mesh_work_allowed);
    assert(!decision.report_tx_allowed);

    assert(mesh_prepare_click_preemption(&relay,
                                         ANCHOR_ID,
                                         1010u,
                                         &plan) == PROTO_OK);
    assert(plan.requeue_click_report);
    assert(plan.clear_outbox);
    assert(plan.cancel_timeout);
    assert(!plan.save_outbox);
    assert(!plan.schedule_timeout);
    assert(!mesh_relay_tx_active(&relay));
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == 0);
    assert(result.outbox_cleared);
    assert(result.timeout_cancelled);
    assert(result.click_report_requeued);
    assert(!result.click_report_requeue_failed);
    assert(fixture.clear_calls == 1u);
    assert(fixture.cancel_calls == 1u);
    assert(fixture.requeue_calls == 1u);
    assert(fixture.requeued.packet.msg_type == MSG_CLICK_REPORT);
    assert(fixture.requeued.packet.src_id == ANCHOR_ID);
    assert(fixture.requeued.packet.seq == 9u);
    assert(fixture.requeued.payload_len == sizeof(payload));
    assert(memcmp(fixture.requeued.payload, payload, sizeof(payload)) == 0);

    assert(mesh_prepare_click_preemption(&relay,
                                         ANCHOR_ID,
                                         1020u,
                                         &after_preempt) == PROTO_OK);
    assert(!after_preempt.requeue_click_report);
    assert(app_mesh_apply_click_preempt_plan(&after_preempt, &ops, &result) == 0);
    assert(fixture.requeue_calls == 1u);
}

static void test_click_preemption_custody_failures_are_explicit(void)
{
    struct mesh_relay relay;
    struct mesh_click_preempt_plan plan;
    struct app_mesh_click_preempt_result result;
    struct click_preempt_fixture fixture;
    struct app_mesh_click_preempt_ops ops = {
        .save_outbox = save_outbox,
        .clear_outbox = clear_outbox,
        .cancel_timeout = cancel_timeout,
        .schedule_timeout = schedule_timeout,
        .requeue_click_report = requeue_click_report,
        .ctx = &fixture,
    };

    memset(&fixture, 0, sizeof(fixture));
    fixture.save_ret = -EIO;
    plan = (struct mesh_click_preempt_plan) {
        .save_outbox = true,
        .schedule_timeout = true,
    };
    assert(plan.save_outbox && plan.schedule_timeout);
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -EIO);
    assert(!result.outbox_saved);
    assert(result.timeout_scheduled);

    memset(&fixture, 0, sizeof(fixture));
    fixture.schedule_ret = -EIO;
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -EIO);
    assert(result.outbox_saved);
    assert(!result.timeout_scheduled);

    memset(&fixture, 0, sizeof(fixture));
    fixture.requeue_ret = -ENOSPC;
    start_gateway_bound_tx(&relay, MSG_CLICK_REPORT, ANCHOR_ID, 73u, NULL, 0u);
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_ID, 2020u, &plan) ==
           PROTO_OK);
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -ENOSPC);
    assert(result.click_report_requeue_failed);
    assert(fixture.cancel_calls == 0u);
    assert(fixture.clear_calls == 0u);

    memset(&fixture, 0, sizeof(fixture));
    fixture.cancel_ret = -EIO;
    start_gateway_bound_tx(&relay, MSG_CLICK_REPORT, ANCHOR_ID, 74u, NULL, 0u);
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_ID, 2030u, &plan) ==
           PROTO_OK);
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -EIO);
    assert(result.click_report_requeued);
    assert(!result.timeout_cancelled);
    assert(fixture.clear_calls == 0u);

    memset(&fixture, 0, sizeof(fixture));
    fixture.clear_ret = -EIO;
    start_gateway_bound_tx(&relay, MSG_CLICK_REPORT, ANCHOR_ID, 75u, NULL, 0u);
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_ID, 2040u, &plan) ==
           PROTO_OK);
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -EIO);
    assert(result.click_report_requeued);
    assert(result.timeout_cancelled);
    assert(!result.outbox_cleared);
}

static void test_ch9_ack_wait_and_send_keep_receive_open(void)
{
    struct mesh_pending_tx pending = {
        .state = MESH_RELAY_TX_WAIT_GATEWAY_ACK,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = GATEWAY_ID,
    };
    struct app_mesh_ch9_ack_table ack_table;
    struct app_mesh_ch9_ack_batch_entry ack_entry = {
        .session_id = ROUTE_EPOCH,
        .seq = 12u,
    };
    struct mesh_outbound ack = pending_hop_ack();
    struct app_mesh_coordinator_runtime_capture capture = {0};
    struct app_mesh_coordinator_runtime_state runtime_state;
    struct app_mesh_coordinator_decision decision;
    enum app_mesh_ch9_ack_queue_result queue_result;
    bool state_changed;

    assert(app_mesh_ch9_core_ack_wait_active(&pending, true));
    assert(app_mesh_ch9_core_pending_allows_rx(&pending, true));
    app_mesh_ch9_ack_table_init(&ack_table);
    assert(app_mesh_ch9_ack_table_queue(&ack_table,
                                        &ack,
                                        &ack_entry,
                                        &queue_result) == PROTO_OK);
    assert(app_mesh_ch9_ack_table_any_pending(&ack_table));

    capture.ch9_ack_wait_active = true;
    capture.ch9_ack_send_pending = true;
    capture.route_waiting_tx_active = true;
    capture.report_queue_used = 1u;
    app_mesh_coordinator_runtime_reset(&runtime_state);
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(decision.uwb_rx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-send") == 0);

    assert(app_mesh_ch9_ack_table_clear_peer(&ack_table, TRANSIT_ID));
    capture.ch9_ack_send_pending = false;
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(!state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_MESH_RX);
    assert(decision.uwb_rx_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-wait") == 0);
}

static void test_paused_delivery_attaches_one_loss_tlv_until_sent(void)
{
    struct app_mesh_paused_delivery_state state;
    struct app_mesh_paused_delivery_store_result store_result;
    struct app_mesh_paused_delivery_attach_result attach_result;
    struct mesh_outbound first = outbound(MSG_MESH_DATA, TRANSIT_ID, 1u);
    struct mesh_outbound second = outbound(MSG_CLICK_REPORT, ANCHOR_ID, 2u);

    app_mesh_paused_delivery_reset(&state);
    app_mesh_paused_delivery_note_store(&state,
                                        false,
                                        NULL,
                                        &first,
                                        &store_result);
    assert(store_result.lost_count == 0u);
    app_mesh_paused_delivery_note_store(&state,
                                        true,
                                        &first,
                                        &second,
                                        &store_result);
    assert(store_result.replaced_existing);
    assert(store_result.lost_count == 1u);

    assert(app_mesh_paused_delivery_attach_loss(&state,
                                                &second,
                                                &attach_result) == PROTO_OK);
    assert(attach_result.tlv_attached);
    assert(lost_packet_tlv_count(&second) == 1u);
    assert(lost_packet_tlv_value(&second) == 1u);
    assert(app_mesh_paused_delivery_lost_count(&state) == 1u);

    assert(app_mesh_paused_delivery_attach_loss(&state,
                                                &second,
                                                &attach_result) == PROTO_OK);
    assert(attach_result.tlv_updated);
    assert(lost_packet_tlv_count(&second) == 1u);
    app_mesh_paused_delivery_note_sent(&state, &first);
    assert(app_mesh_paused_delivery_lost_count(&state) == 1u);
    app_mesh_paused_delivery_note_sent(&state, &second);
    assert(app_mesh_paused_delivery_lost_count(&state) == 0u);
}

static void test_click_survey_and_transit_order_defers_command_during_ranging(void)
{
    struct app_mesh_coordinator_runtime_capture capture = {0};
    struct app_mesh_coordinator_runtime_state runtime_state;
    struct app_mesh_coordinator_decision decision;
    struct app_mesh_c5_flood_priority_state flood_state = {
        .response_priority = true,
        .anchor_busy = true,
        .survey_busy = false,
        .gateway_ch5_preempt = true,
    };
    bool state_changed;

    capture.click_active = true;
    capture.survey_pending = true;
    capture.relay_tx_active = true;
    app_mesh_coordinator_runtime_reset(&runtime_state);
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_CLICK);

    capture.click_active = false;
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_SURVEY);
    assert(!decision.mesh_work_allowed);

    capture.survey_pending = false;
    assert(app_mesh_coordinator_runtime_decide(&capture,
                                               &runtime_state,
                                               &decision,
                                               &state_changed) == 0);
    assert(state_changed);
    assert(decision.state == APP_MESH_COORDINATOR_MESH_TX);
    assert(!decision.report_tx_allowed);

    assert(app_mesh_c5_flood_should_defer(&flood_state));
    assert(!app_mesh_c5_gateway_rx_should_yield_to_response(&flood_state));
}

int main(void)
{
    test_gateway_command_aborts_receive_before_priority_scheduling();
    test_gateway_ble_ingress_waits_for_safe_boundary_then_preserves_result_identity();
    test_click_claim_requeues_one_local_report_without_corruption();
    test_click_preemption_custody_failures_are_explicit();
    test_ch9_ack_wait_and_send_keep_receive_open();
    test_paused_delivery_attaches_one_loss_tlv_until_sent();
    test_click_survey_and_transit_order_defers_command_during_ranging();
    return 0;
}
