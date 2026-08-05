#include "app_mesh_c5_priority.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_coordinator_runtime.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_arbitration_zephyr.h"
#include "app_mesh_flood.h"
#include "app_mesh_gateway_command_flow.h"
#include "app_gateway_command_ingress.h"
#include "app_mesh_gateway_command_priority.h"
#include "app_mesh_preemption.h"
#include "dwm3000_driver.h"
#include "mesh_preemption.h"
#include "route.h"
#include "serial_frame.h"
#include "survey_round_control.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ANCHOR_ID UINT64_C(0xA100)
#define GATEWAY_ID UINT64_C(0x9000)
#define TRANSIT_ID UINT64_C(0xB001)
#define ROUTE_EPOCH UINT32_C(17)

struct click_preempt_fixture {
    struct mesh_relay *relay;
    uint8_t save_calls;
    uint8_t clear_calls;
    uint8_t cancel_calls;
    uint8_t schedule_calls;
    uint8_t requeue_calls;
    uint8_t stage_calls;
    uint8_t commit_calls;
    uint8_t rollback_calls;
    int save_ret;
    int clear_ret;
    int cancel_ret;
    int schedule_ret;
    int requeue_ret;
    int stage_ret;
    int commit_ret;
    int rollback_ret;
    struct mesh_outbound requeued;
};

struct ingress_scenario_fixture {
    struct app_gateway_command_ingress_item queued;
    struct app_gateway_command_identity cancelled;
    struct k_work_delayable work;
    struct k_work_q priority_work_queue;
    struct app_mesh_command_orchestrator orchestrator;
    struct mesh_outbound anchor_result;
    uint8_t admission_count;
    uint8_t cancel_count;
    uint8_t error_count;
    uint32_t flood_count;
    uint32_t anchor_receive_count;
    uint32_t gatt_result_count;
    uint32_t now_ms;
    bool ranging_active;
    bool queued_valid;
};

struct priority_failure_fixture {
    uint32_t generation;
    uint32_t admission_cutoff;
    uint8_t count;
    int error;
};

static uint8_t receive_abort_requests;
static uint8_t receive_abort_clears;
static struct k_work_delayable *shim_priority_target_work;
static int shim_retry_work_reschedule_result;

void zephyr_shim_note_work_reschedule(struct k_work_delayable *work,
                                      int timeout)
{
    (void)timeout;
    if (shim_priority_target_work != NULL &&
        work != shim_priority_target_work) {
        work->reschedule_result = shim_retry_work_reschedule_result;
    }
}

void dwm3000_driver_request_receive_abort(uint32_t owner_mask)
{
    assert(owner_mask == DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    receive_abort_requests++;
}

void dwm3000_driver_clear_receive_abort(uint32_t owner_mask)
{
    assert(owner_mask == DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    receive_abort_clears++;
}

static void gateway_priority_schedule_failed(void *ctx,
                                             int error,
                                             uint32_t generation,
                                             uint32_t admission_cutoff)
{
    struct priority_failure_fixture *fixture = ctx;

    fixture->count++;
    fixture->error = error;
    fixture->generation = generation;
    fixture->admission_cutoff = admission_cutoff;
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

static struct mesh_outbound pending_hop_ack(uint16_t acknowledged_seq)
{
    const struct proto_packet acknowledged = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = TRANSIT_ID,
        .dst_id = GATEWAY_ID,
        .session_id = ROUTE_EPOCH,
        .seq = acknowledged_seq,
        .ttl = MESH_GATEWAY_ACK_TTL,
    };
    struct mesh_outbound ack = {
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
    size_t payload_len = 0u;

    assert(mesh_append_requested_seq(ack.payload,
                                     sizeof(ack.payload),
                                     &payload_len,
                                     acknowledged.seq) == PROTO_OK);
    assert(mesh_append_ack_semantic_identity(ack.payload,
                                             sizeof(ack.payload),
                                             &payload_len,
                                             &acknowledged,
                                             NULL,
                                             0u) == PROTO_OK);
    ack.payload_len = (uint16_t)payload_len;
    ack.packet.payload_len = (uint16_t)payload_len;
    return ack;
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

static int stage_click_handoff(void *ctx, const struct mesh_outbound *outbound)
{
    struct click_preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->stage_calls++;
    return fixture->stage_ret;
}

static int commit_click_handoff(void *ctx, const struct mesh_outbound *outbound)
{
    struct click_preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->commit_calls++;
    return fixture->commit_ret;
}

static int rollback_click_handoff(void *ctx, const struct mesh_outbound *outbound)
{
    struct click_preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    fixture->rollback_calls++;
    return fixture->rollback_ret;
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

static int discard_requeued_click_report(void *ctx,
                                         const struct mesh_outbound *outbound)
{
    struct click_preempt_fixture *fixture = ctx;

    assert(outbound != NULL);
    memset(&fixture->requeued, 0, sizeof(fixture->requeued));
    return 0;
}

static int cancel_active_tx(void *ctx)
{
    struct click_preempt_fixture *fixture = ctx;

    if (fixture->relay == NULL) {
        return -EINVAL;
    }
    mesh_relay_cancel_tx(fixture->relay);
    return 0;
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

static int ingress_submit_priority(void *ctx, uint32_t admission_cutoff)
{
    struct ingress_scenario_fixture *fixture = ctx;
    const struct app_mesh_arbitration_zephyr_gateway_ops ops = {
        .gateway_role = true,
        .priority_work_queue = &fixture->priority_work_queue,
    };
    int ret;

    ret = app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
        &fixture->work, admission_cutoff);
    if (ret < 0) {
        return ret;
    }
    return app_mesh_arbitration_zephyr_gateway_command_submit(
        &ops, &fixture->work);
}

static int ingress_submit_noop(void *ctx, uint32_t admission_cutoff)
{
    (void)ctx;
    (void)admission_cutoff;
    return 0;
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
    (void)command;
    fixture->error_count++;
}

static uint32_t command_flow_now(void *ctx)
{
    return ((struct ingress_scenario_fixture *)ctx)->now_ms;
}

static void command_flow_sleep_until(uint32_t due_ms, void *ctx)
{
    ((struct ingress_scenario_fixture *)ctx)->now_ms = due_ms;
}

static bool command_flow_not_deferred(void *ctx)
{
    (void)ctx;
    return false;
}

static bool command_flow_c5_quiet(uint32_t sniff_ms, void *ctx)
{
    (void)sniff_ms;
    (void)ctx;
    return true;
}

static uint32_t command_flow_random(void *ctx)
{
    (void)ctx;
    return 0u;
}

/* This is the final DWM channel-5 transmit boundary, not an app substitute. */
static int dwm_c5_transmit_boundary(const struct mesh_outbound *out, void *ctx)
{
    struct ingress_scenario_fixture *fixture = ctx;
    struct gateway_command_options options;
    enum command_id command_id;
    bool broadcast;
    bool expired;
    bool duplicate;
    size_t result_len = 0u;

    assert(!fixture->ranging_active);
    fixture->flood_count++;
    assert(app_mesh_command_orchestrator_anchor_receive(
               &fixture->orchestrator,
               &out->packet,
               out->payload,
               out->payload_len,
               GATEWAY_ID,
               fixture->now_ms,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(broadcast && !expired);
    if (duplicate) {
        return 0;
    }
    fixture->anchor_receive_count++;
    assert(mesh_append_command_result(fixture->anchor_result.payload,
                                      sizeof(fixture->anchor_result.payload),
                                      &result_len,
                                      command_id,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    fixture->anchor_result.payload_len = (uint16_t)result_len;
    assert(app_mesh_command_orchestrator_anchor_result(&out->packet,
                                                        ANCHOR_ID,
                                                        GATEWAY_ID,
                                                        false,
                                                        &fixture->anchor_result) == PROTO_OK);
    app_mesh_command_orchestrator_anchor_commit(&fixture->orchestrator,
                                                 &out->packet,
                                                 &options,
                                                 fixture->now_ms);
    return 0;
}

/* This is the final GATT notification boundary after production result decoding. */
static int gatt_result_boundary(void *ctx,
                                const struct proto_packet *command,
                                enum command_id command_id,
                                enum command_status status,
                                uint8_t reason)
{
    struct ingress_scenario_fixture *fixture = ctx;

    assert(command == &fixture->orchestrator.gateway_flow.outbound.packet);
    assert(command_id == fixture->orchestrator.gateway_flow.command_id);
    assert(status == COMMAND_OK);
    assert(reason == 0u);
    fixture->gatt_result_count++;
    return 0;
}

static int gateway_safe_boundary_schedule(void *ctx)
{
    (void)ctx;
    return app_mesh_arbitration_zephyr_gateway_receive_abort_observed();
}

static void run_scheduled_gateway_command(struct ingress_scenario_fixture *fixture)
{
    const struct app_mesh_flood_ops flood_ops = {
        .now_ms = command_flow_now,
        .sleep_until_ms = command_flow_sleep_until,
        .defer_active = command_flow_not_deferred,
        .c5_quiet = command_flow_c5_quiet,
        .random_u32 = command_flow_random,
        .send = dwm_c5_transmit_boundary,
        .ctx = fixture,
    };
    struct app_mesh_flood_result flood_result;

    assert(fixture->work.reschedule_calls == 1u);
    assert(fixture->queued_valid);
    assert(app_mesh_command_orchestrator_activate(&fixture->orchestrator,
                                                  &fixture->queued) == 0);
    app_mesh_command_orchestrator_mark_safe_boundary(
        &fixture->orchestrator);
    assert(app_mesh_command_orchestrator_prepare_flood(
               &fixture->orchestrator,
               GATEWAY_ID,
               fixture->now_ms,
               fixture->queued.packet.seq) == PROTO_OK);
    assert(!fixture->orchestrator.anchor.duplicate_cache.initialized);
    assert(app_mesh_command_orchestrator_send_flood(&fixture->orchestrator,
                                                    &flood_ops,
                                                    &flood_result) == 0);
    assert(flood_result.sent_count > 0u);
    assert(app_mesh_command_orchestrator_gateway_deliver(
               &fixture->orchestrator.gateway_flow.outbound.packet,
               fixture->orchestrator.gateway_flow.command_id,
               &fixture->anchor_result.packet,
               fixture->anchor_result.payload,
               fixture->anchor_result.payload_len,
               gatt_result_boundary,
               fixture) == 0);
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
    struct mesh_outbound ack = pending_hop_ack(ack_entry.seq);
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

static void test_gateway_priority_contention_retires_only_frozen_generation(void)
{
    struct k_work_delayable command_work = {
        .reschedule_result = -EBUSY,
    };
    struct k_work_q priority_work_queue = {0};
    const struct app_mesh_arbitration_zephyr_gateway_ops ops = {
        .gateway_role = true,
        .priority_work_queue = &priority_work_queue,
    };
    struct priority_failure_fixture failure = {0};

    receive_abort_requests = 0u;
    receive_abort_clears = 0u;
    app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
        gateway_priority_schedule_failed, &failure);
    assert(app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
               &command_work, 123u) == 0);
    assert(app_mesh_arbitration_zephyr_gateway_command_submit(
               &ops, &command_work) == 0);
    assert(receive_abort_requests == 1u);

    assert(app_mesh_arbitration_zephyr_gateway_receive_abort_observed() ==
           -EBUSY);
    assert(failure.count == 0u);
    /*
     * Admission 124 arrives after the first rejected scheduling attempt. It
     * stays queued behind the frozen cutoff and must not be named by this
     * generation's eventual terminal callback.
     */
    assert(app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
               &command_work, 124u) == 0);
    assert(app_mesh_arbitration_zephyr_gateway_command_submit(
               &ops, &command_work) == 0);
    for (uint8_t attempt = 1u;
         attempt < APP_MESH_GATEWAY_COMMAND_PRIORITY_MAX_SCHEDULE_ATTEMPTS;
         attempt++) {
        assert(app_mesh_arbitration_zephyr_gateway_receive_abort_observed() ==
               -EBUSY);
    }
    assert(failure.count == 1u);
    assert(failure.error == -EBUSY);
    assert(failure.generation != 0u);
    assert(failure.admission_cutoff == 123u);
    assert(receive_abort_clears == 1u);

    command_work.reschedule_result = 0;
    assert(app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
               &command_work, 124u) == 0);
    assert(app_mesh_arbitration_zephyr_gateway_command_submit(
               &ops, &command_work) == 0);
    assert(receive_abort_requests == 2u);
    assert(app_mesh_arbitration_zephyr_gateway_receive_abort_observed() == 0);
    assert(failure.count == 1u);
    app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
        NULL, NULL);
}

static void test_gateway_priority_retry_owner_rejection_cannot_orphan_generation(void)
{
    struct k_work_delayable command_work = {
        .reschedule_result = -EBUSY,
    };
    struct k_work_q priority_work_queue = {0};
    const struct app_mesh_arbitration_zephyr_gateway_ops ops = {
        .gateway_role = true,
        .priority_work_queue = &priority_work_queue,
    };
    struct priority_failure_fixture failure = {0};

    receive_abort_requests = 0u;
    receive_abort_clears = 0u;
    shim_priority_target_work = &command_work;
    shim_retry_work_reschedule_result = -ENOSPC;
    app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
        gateway_priority_schedule_failed, &failure);
    assert(app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
               &command_work, 201u) == 0);
    assert(app_mesh_arbitration_zephyr_gateway_command_submit(
               &ops, &command_work) == 0);
    assert(app_mesh_arbitration_zephyr_gateway_receive_abort_observed() ==
           -EBUSY);
    assert(command_work.reschedule_calls ==
           APP_MESH_GATEWAY_COMMAND_PRIORITY_MAX_SCHEDULE_ATTEMPTS);
    assert(failure.count == 1u);
    assert(failure.error == -EBUSY);
    assert(failure.admission_cutoff == 201u);
    assert(receive_abort_requests == 1u);
    assert(receive_abort_clears == 1u);

    app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
        NULL, NULL);
    shim_priority_target_work = NULL;
    shim_retry_work_reschedule_result = 0;
}

static void test_gateway_ble_ingress_waits_for_safe_boundary_then_preserves_result_identity(void)
{
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1234),
        .dst_id = MESH_BROADCAST_ID,
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
    struct mesh_pending_tx result_ack = {
        .state = MESH_RELAY_TX_WAIT_GATEWAY_ACK,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = GATEWAY_ID,
    };
    bool command_handled;
    struct app_gateway_command_ingress_item decoded;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t payload_len = 0u;
    size_t frame_len = 0u;

    receive_abort_requests = 0u;
    receive_abort_clears = 0u;
    fixture.now_ms = 100u;
    fixture.ranging_active = true;
    ingress_ops.ctx = &fixture;
    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          61u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          ROUTE_EPOCH) == PROTO_OK);
    command.payload_len = (uint16_t)payload_len;
    assert(serial_frame_encode_packet(&command, payload, frame, sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_mesh_command_orchestrator_gateway_ingress(&ingress_ops,
                                                         frame,
                                                         frame_len,
                                                         &decoded,
                                                         &command_handled) == 0);
    assert(command_handled);
    assert(fixture.admission_count == 1u);
    assert(receive_abort_requests == 1u);
    assert(fixture.work.reschedule_calls == 0u);

    /* DS-TWR owns the radio until its receive loop reports a safe boundary. */
    assert(fixture.ranging_active);
    assert(fixture.work.reschedule_calls == 0u);
    assert(app_mesh_command_orchestrator_safe_boundary(&fixture.orchestrator,
                                                       true,
                                                       gateway_safe_boundary_schedule,
                                                       &fixture) == -EAGAIN);
    fixture.ranging_active = false;
    assert(app_mesh_command_orchestrator_safe_boundary(&fixture.orchestrator,
                                                       false,
                                                       gateway_safe_boundary_schedule,
                                                       &fixture) == 0);
    assert(fixture.work.reschedule_calls == 1u);
    assert(fixture.work.last_queue == &fixture.priority_work_queue);

    run_scheduled_gateway_command(&fixture);
    assert(fixture.flood_count >= 1u);
    assert(fixture.anchor_receive_count > 0u);
    assert(fixture.anchor_receive_count == 1u);
    assert(fixture.anchor_result.packet.msg_type == MSG_COMMAND_RESULT);
    assert(fixture.anchor_result.packet.dst_id == GATEWAY_ID);
    assert(fixture.anchor_result.packet.session_id ==
           fixture.orchestrator.gateway_flow.outbound.packet.session_id);
    assert(fixture.anchor_result.packet.seq ==
           fixture.orchestrator.gateway_flow.outbound.packet.seq);
    assert(fixture.gatt_result_count == 1u);
    assert(app_mesh_ch9_core_ack_wait_active(&result_ack, true));
    assert(fixture.error_count == 0u);
}

static void test_gateway_ingress_preserves_a_valid_non_command_frame(void)
{
    struct ingress_scenario_fixture fixture = {0};
    struct app_gateway_command_ingress_item decoded;
    const struct app_gateway_command_ingress_ops ingress_ops = {
        .gateway_role = true,
        .admit = ingress_admit,
        .submit_priority = ingress_submit_priority,
        .cancel_admitted = ingress_cancel,
        .emit_result = ingress_emit_error,
        .ctx = &fixture,
    };
    const uint8_t payload[] = {0x11u, 0x22u, 0x33u};
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .src_id = UINT64_C(0x1234),
        .dst_id = GATEWAY_ID,
        .session_id = ROUTE_EPOCH,
        .seq = 99u,
        .payload_len = sizeof(payload),
    };
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    bool command_handled = true;

    assert(serial_frame_encode_packet(&packet, payload, frame, sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_mesh_command_orchestrator_gateway_ingress(&ingress_ops,
                                                         frame,
                                                         frame_len,
                                                         &decoded,
                                                         &command_handled) == 0);
    assert(!command_handled);
    assert(fixture.admission_count == 0u);
    assert(decoded.packet.msg_type == MSG_MESH_DATA);
    assert(decoded.packet.seq == packet.seq);
    assert(decoded.payload_len == sizeof(payload));
    assert(memcmp(decoded.payload, payload, sizeof(payload)) == 0);
}

static void test_concurrent_ingress_cannot_mutate_active_dispatch(void)
{
    struct ingress_scenario_fixture fixture = {0};
    struct app_mesh_command_orchestrator active_before;
    struct app_gateway_command_ingress_item decoded;
    struct app_gateway_command_ingress_ops ingress_ops = {
        .gateway_role = true,
        .admit = ingress_admit,
        .submit_priority = ingress_submit_noop,
        .cancel_admitted = ingress_cancel,
        .emit_result = ingress_emit_error,
        .ctx = &fixture,
    };
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1234),
        .dst_id = MESH_BROADCAST_ID,
        .session_id = ROUTE_EPOCH,
        .seq = 71u,
    };
    struct proto_packet diagnostic = {
        .msg_type = MSG_MESH_DATA,
        .src_id = UINT64_C(0x5678),
        .dst_id = GATEWAY_ID,
        .session_id = ROUTE_EPOCH,
        .seq = 91u,
    };
    const struct app_mesh_flood_ops flood_ops = {
        .now_ms = command_flow_now,
        .sleep_until_ms = command_flow_sleep_until,
        .defer_active = command_flow_not_deferred,
        .c5_quiet = command_flow_c5_quiet,
        .random_u32 = command_flow_random,
        .send = dwm_c5_transmit_boundary,
        .ctx = &fixture,
    };
    struct app_mesh_flood_result flood_result;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t payload_len = 0u;
    size_t frame_len = 0u;
    bool command_handled = false;

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          71u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          ROUTE_EPOCH) == PROTO_OK);
    command.payload_len = (uint16_t)payload_len;
    assert(serial_frame_encode_packet(&command,
                                      payload,
                                      frame,
                                      sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_mesh_command_orchestrator_gateway_ingress(&ingress_ops,
                                                         frame,
                                                         frame_len,
                                                         &decoded,
                                                         &command_handled) == 0);
    assert(command_handled && fixture.queued_valid);
    assert(app_mesh_command_orchestrator_activate(&fixture.orchestrator,
                                                  &fixture.queued) == 0);
    app_mesh_command_orchestrator_mark_safe_boundary(&fixture.orchestrator);
    fixture.now_ms = 100u;
    assert(app_mesh_command_orchestrator_prepare_flood(&fixture.orchestrator,
                                                       GATEWAY_ID,
                                                       fixture.now_ms,
                                                       command.seq) ==
           PROTO_OK);
    active_before = fixture.orchestrator;

    /* Higher-priority BLE RX admits B while route-owned A is mid-dispatch. */
    fixture.queued_valid = false;
    command.seq = 72u;
    assert(gateway_command_rebind_broadcast_sequence(payload,
                                                     payload_len,
                                                     72u) == PROTO_OK);
    assert(serial_frame_encode_packet(&command,
                                      payload,
                                      frame,
                                      sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_mesh_command_orchestrator_gateway_ingress(&ingress_ops,
                                                         frame,
                                                         frame_len,
                                                         &decoded,
                                                         &command_handled) == 0);
    assert(command_handled && fixture.queued.packet.seq == 72u);
    assert(memcmp(&fixture.orchestrator,
                  &active_before,
                  sizeof(active_before)) == 0);

    /* A non-command decode is also caller-owned scratch. */
    diagnostic.payload_len = 1u;
    payload[0] = 0xa5u;
    assert(serial_frame_encode_packet(&diagnostic,
                                      payload,
                                      frame,
                                      sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_mesh_command_orchestrator_gateway_ingress(&ingress_ops,
                                                         frame,
                                                         frame_len,
                                                         &decoded,
                                                         &command_handled) == 0);
    assert(!command_handled);
    assert(memcmp(&fixture.orchestrator,
                  &active_before,
                  sizeof(active_before)) == 0);

    assert(app_mesh_command_orchestrator_send_flood(&fixture.orchestrator,
                                                    &flood_ops,
                                                    &flood_result) == 0);
    assert(flood_result.sent_count > 0u);
    assert(fixture.orchestrator.gateway_flow.outbound.packet.seq == 71u);
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
        .stage_click_handoff = stage_click_handoff,
        .commit_click_handoff = commit_click_handoff,
        .rollback_click_handoff = rollback_click_handoff,
        .cancel_timeout = cancel_timeout,
        .schedule_timeout = schedule_timeout,
        .requeue_click_report = requeue_click_report,
        .discard_requeued_click_report = discard_requeued_click_report,
        .cancel_active_tx = cancel_active_tx,
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
    fixture.relay = &relay;
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_COUNT_AS_CLICK));
    assert(app_mesh_c5_wake_claim_requires_anchor_handoff(
        FLAG_COUNT_AS_CLICK, true));
    assert(app_mesh_c5_connected_gap_rx_action(true, false, false) ==
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
    assert(mesh_relay_tx_active(&relay));
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == 0);
    assert(result.outbox_cleared);
    assert(result.timeout_cancelled);
    assert(result.click_report_requeued);
    assert(result.active_tx_cancelled);
    assert(!result.click_report_requeue_failed);
    assert(result.transaction_committed);
    assert(result.custody_owner == APP_MESH_CLICK_PREEMPT_OWNER_DURABLE_HANDOFF);
    assert(fixture.clear_calls == 1u);
    assert(fixture.cancel_calls == 1u);
    assert(fixture.requeue_calls == 1u);
    assert(fixture.stage_calls == 1u && fixture.commit_calls == 1u);
    assert(fixture.requeued.packet.msg_type == MSG_CLICK_REPORT);
    assert(fixture.requeued.packet.src_id == ANCHOR_ID);
    assert(fixture.requeued.packet.seq == 9u);
    assert(fixture.requeued.payload_len == sizeof(payload));
    assert(memcmp(fixture.requeued.payload, payload, sizeof(payload)) == 0);
    assert(!mesh_relay_tx_active(&relay));

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
        .stage_click_handoff = stage_click_handoff,
        .commit_click_handoff = commit_click_handoff,
        .rollback_click_handoff = rollback_click_handoff,
        .cancel_timeout = cancel_timeout,
        .schedule_timeout = schedule_timeout,
        .requeue_click_report = requeue_click_report,
        .discard_requeued_click_report = discard_requeued_click_report,
        .cancel_active_tx = cancel_active_tx,
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
    assert(fixture.schedule_calls == 1u);

    memset(&fixture, 0, sizeof(fixture));
    fixture.schedule_ret = -EIO;
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -EIO);
    assert(!result.outbox_saved);
    assert(!result.timeout_scheduled);

    memset(&fixture, 0, sizeof(fixture));
    fixture.requeue_ret = -ENOSPC;
    start_gateway_bound_tx(&relay, MSG_CLICK_REPORT, ANCHOR_ID, 73u, NULL, 0u);
    fixture.relay = &relay;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_ID, 2020u, &plan) ==
           PROTO_OK);
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -ENOSPC);
    assert(result.click_report_requeue_failed);
    assert(fixture.cancel_calls == 1u);
    assert(fixture.clear_calls == 0u);
    assert(fixture.stage_calls == 1u && fixture.commit_calls == 1u);

    memset(&fixture, 0, sizeof(fixture));
    fixture.cancel_ret = -EIO;
    start_gateway_bound_tx(&relay, MSG_CLICK_REPORT, ANCHOR_ID, 74u, NULL, 0u);
    fixture.relay = &relay;
    assert(mesh_prepare_click_preemption(&relay, ANCHOR_ID, 2030u, &plan) ==
           PROTO_OK);
    assert(app_mesh_apply_click_preempt_plan(&plan, &ops, &result) == -EIO);
    assert(!result.click_report_requeued);
    assert(!result.timeout_cancelled);
    assert(fixture.clear_calls == 0u);
    assert(fixture.rollback_calls == 0u);

    memset(&fixture, 0, sizeof(fixture));
    fixture.clear_ret = -EIO;
    start_gateway_bound_tx(&relay, MSG_CLICK_REPORT, ANCHOR_ID, 75u, NULL, 0u);
    fixture.relay = &relay;
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
    struct mesh_outbound ack = pending_hop_ack(ack_entry.seq);
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

static void test_survey_go_duplicate_identity_commits_after_admission(void)
{
    struct app_mesh_command_orchestrator orchestrator = {0};
    struct gateway_command_options options;
    struct survey_round_go go = {
        .operation_generation = UINT64_C(0x000000010000004D),
        .round_commitment = {0xA5u},
        .survey_id = 77u,
        .round_id = 9u,
    };
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 77u,
        .seq = 41u,
        .ttl = MESH_DEFAULT_TTL,
    };
    enum command_id command_id;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    uint8_t schedules = 0u;
    bool broadcast;
    bool expired;
    bool duplicate;

    assert(survey_round_go_append_tlvs(payload,
                                        sizeof(payload),
                                        &payload_len,
                                        &go) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          41u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          ROUTE_EPOCH) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXECUTE_DELAY_MS,
                          1000u) == PROTO_OK);
    command.payload_len = (uint16_t)payload_len;

    assert(app_mesh_command_orchestrator_anchor_receive(
               &orchestrator,
               &command,
               payload,
               payload_len,
               GATEWAY_ID,
               100u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(command_id == CMD_SURVEY_GO && broadcast && !expired &&
           !duplicate);
    /* The first local admission fails; its identity must remain replayable. */

    assert(app_mesh_command_orchestrator_anchor_receive(
               &orchestrator,
               &command,
               payload,
               payload_len,
               GATEWAY_ID,
               101u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(!duplicate);
    schedules++;
    app_mesh_command_orchestrator_anchor_commit(&orchestrator,
                                                 &command,
                                                 &options,
                                                 101u);

    assert(app_mesh_command_orchestrator_anchor_receive(
               &orchestrator,
               &command,
               payload,
               payload_len,
               GATEWAY_ID,
               102u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    if (!duplicate) {
        schedules++;
    }
    assert(duplicate);
    assert(schedules == 1u);
}

static void test_broadcast_transport_retry_requires_explicit_semantic_commit(void)
{
    struct app_mesh_command_orchestrator orchestrator = {0};
    struct gateway_command_options options;
    struct mesh_relay relay;
    struct mesh_relay_result relay_result;
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 81u,
        .seq = 42u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    enum command_id command_id = CMD_VENDOR_BASE;
    struct proto_packet retry;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    bool broadcast = false;
    bool expired = false;
    bool duplicate = false;

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          81u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          ROUTE_EPOCH) == PROTO_OK);
    command.payload_len = (uint16_t)payload_len;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    ANCHOR_ID,
                    GATEWAY_ID,
                    ROUTE_EPOCH);
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &command,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID,
                                            80u,
                                            100u,
                                            3u,
                                            &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_OK);
    assert((relay_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    assert((relay_result.actions & MESH_RELAY_ACTION_FORWARD) != 0u);

    assert(app_mesh_command_orchestrator_anchor_receive(
               &orchestrator,
               &command,
               payload,
               payload_len,
               GATEWAY_ID,
               100u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(command_id == CMD_GET_STATUS && broadcast && !expired &&
           !duplicate);

    /*
     * Parsing and policy classification cannot commit the duplicate
     * identity. A caller that fails to gain execution or terminal-result
     * custody must be able to receive the same command again.
     */
    retry = command;
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &retry,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID,
                                            80u,
                                            101u,
                                            4u,
                                            &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_ERR_STALE);
    assert((relay_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    assert((relay_result.actions & MESH_RELAY_ACTION_FORWARD) == 0u);
    assert((relay_result.actions & MESH_RELAY_ACTION_DROP) == 0u);
    assert(app_mesh_command_orchestrator_anchor_receive(
               &orchestrator,
               &retry,
               payload,
               payload_len,
               GATEWAY_ID,
               101u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(!duplicate);

    app_mesh_command_orchestrator_anchor_commit(&orchestrator,
                                                 &retry,
                                                 &options,
                                                 101u);
    assert(mesh_relay_handle_rx_with_random(&relay,
                                            &retry,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID,
                                            80u,
                                            102u,
                                            5u,
                                            &relay_result) == PROTO_OK);
    assert(relay_result.status == PROTO_ERR_STALE);
    assert((relay_result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u);
    assert((relay_result.actions & MESH_RELAY_ACTION_FORWARD) == 0u);
    assert((relay_result.actions & MESH_RELAY_ACTION_DROP) == 0u);
    assert(app_mesh_command_orchestrator_anchor_receive(
               &orchestrator,
               &retry,
               payload,
               payload_len,
               GATEWAY_ID,
               102u,
               &command_id,
               &options,
               &broadcast,
               &expired,
               &duplicate) == PROTO_OK);
    assert(duplicate);
}

int main(void)
{
    test_gateway_command_aborts_receive_before_priority_scheduling();
    test_gateway_priority_contention_retires_only_frozen_generation();
    test_gateway_priority_retry_owner_rejection_cannot_orphan_generation();
    test_gateway_ble_ingress_waits_for_safe_boundary_then_preserves_result_identity();
    test_gateway_ingress_preserves_a_valid_non_command_frame();
    test_concurrent_ingress_cannot_mutate_active_dispatch();
    test_click_claim_requeues_one_local_report_without_corruption();
    test_click_preemption_custody_failures_are_explicit();
    test_ch9_ack_wait_and_send_keep_receive_open();
    test_paused_delivery_attaches_one_loss_tlv_until_sent();
    test_click_survey_and_transit_order_defers_command_during_ranging();
    test_survey_go_duplicate_identity_commits_after_admission();
    test_broadcast_transport_retry_requires_explicit_semantic_commit();
    return 0;
}
