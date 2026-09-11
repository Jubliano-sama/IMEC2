/* Execute the canonical RF CANCEL adapter and absolute receive wait against
 * the existing production survey lifecycle fixture. Only radio input, time,
 * work scheduling and forwarding queue admission are fake. */
#define main hia_fixture_main
#define survey_ops hia_fixture_ops
#define dwm3000_driver_request_receive_abort hia_driver_request_receive_abort
#include "test_app_hia_table_survey.c"
#undef main
#undef survey_ops
#undef dwm3000_driver_request_receive_abort
#undef DWM3000_RECEIVE_ABORT_MESH_CONTROL

#include "app_wake_train_politeness.h"
#include "app_mesh_c5_priority.h"
#include "dwm3000_driver.h"
#define NETWORK_ID UINT32_C(0x494d4543)
#define CONFIG_IMEC_MESH_ROUTE_TEST 1
#define CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL require_relay
#define CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS required_relay_hops
#define IS_ENABLED(value) (value)
#define K_NO_WAIT 0u
#define K_MSEC(value) (value)
#define ARG_UNUSED(value) (void)(value)

struct k_work { int unused; };
struct k_work_delayable { uint32_t due_ms; };
static int anchor_uwb_scan_work_q;
static unsigned forward_schedules, stop_feeds;
static int schedule_status;

static int k_work_reschedule_for_queue(int *queue,
    struct k_work_delayable *work, uint32_t delay_ms)
{
    assert(queue == &anchor_uwb_scan_work_q && lock_depth == 0u);
    forward_schedules++;
    work->due_ms = now_ms + delay_ms;
    /* Queue ownership is serial: never execute the callback inline from RX. */
    return schedule_status;
}

static void app_watchdog_stop_feeding(void) { stop_feeds++; }

static struct {
    bool (*anchor_receive_cancel)(const uint8_t *, size_t);
} survey_ops;
static unsigned queue_calls, progress_calls, rx_calls;
static bool queue_accept;
static bool require_relay;
static uint8_t required_relay_hops;
static uint8_t queued_frame[UWB_MESH_MAX_FRAME_LEN];
static size_t queued_len;
static uint32_t queued_received_at;
static unsigned cancel_control_decode_calls;

static int counted_cancel_control_decode(const uint8_t *payload,
    size_t payload_len, struct survey_control *control)
{
    cancel_control_decode_calls++;
    return survey_control_extract_tlvs(payload, payload_len, control);
}

static bool local_anchor_discovery_assignment_get(uint32_t *epoch,
    uint8_t *slot, uint8_t *count)
{
    *epoch = assignment_policy.committed_epoch;
    *slot = 0u;
    *count = 2u;
    return assignment_policy.provisioned;
}

static bool mesh_queue_from_frame_received_at(const uint8_t *frame,
    size_t frame_len, uint8_t quality, uint8_t channel, uint32_t received_at)
{
    assert(!anchor_state.active && anchor_state.aborted);
    assert(lock_depth == 0u && quality == 0u && channel == MESH_EVENT_CHANNEL);
    assert(frame_len <= sizeof(queued_frame));
    queue_calls++;
    memcpy(queued_frame, frame, frame_len);
    queued_len = frame_len;
    queued_received_at = received_at;
    return queue_accept;
}

#define survey_control_extract_tlvs counted_cancel_control_decode
#include "survey_cancel_adapter.inc"
#undef survey_control_extract_tlvs

struct wire_control {
    struct mesh_outbound outbound;
    uint64_t previous_hop;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len;
};

static void encode_wire(struct wire_control *wire, uint32_t network,
                         uint64_t physical_destination)
{
    int ret = uwb_mesh_frame_encode(network, wire->previous_hop, physical_destination,
        &wire->outbound.packet, wire->outbound.payload,
        wire->frame, sizeof(wire->frame), &wire->frame_len);
    if (ret != PROTO_OK) {
        fprintf(stderr, "wire encode ret=%d dst=%llu flags=%u bytes=%u\n",
            ret, (unsigned long long)physical_destination,
            wire->outbound.packet.flags, wire->outbound.packet.payload_len);
    }
    assert(ret == PROTO_OK);
}

static void make_wire(struct wire_control *wire, enum command_id command,
                       const struct survey_control *control)
{
    memset(wire, 0, sizeof(*wire));
    wire->previous_hop = GATEWAY_ID;
    size_t len = encode_command(wire->outbound.payload, command, control);
    uint8_t *payload = wire->outbound.payload;
    size_t cap = sizeof(wire->outbound.payload);
    assert(tlv_append_u8(payload, cap, &len, TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload, cap, &len, TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload, cap, &len, TLV_COMMAND_SEQ, 701u) == PROTO_OK);
    assert(tlv_append_u32(payload, cap, &len, TLV_FLOOD_EPOCH_ID, 702u) == PROTO_OK);
    assert(tlv_append_u32(payload, cap, &len, TLV_COMMAND_EXPIRY_S,
                          SURVEY_HARD_CAP_MS / 1000u) == PROTO_OK);
    wire->outbound.payload_len = (uint16_t)len;
    wire->outbound.packet = (struct proto_packet) {
        .msg_type = MSG_COMMAND, .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY_ID, .dst_id = MESH_BROADCAST_ID,
        .session_id = 701u, .seq = 17u, .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        .payload_len = (uint16_t)len, .message_age_ms = 13u,
    };
    assert(gateway_command_append_default_flood_controls(&wire->outbound) == PROTO_OK);
    encode_wire(wire, NETWORK_ID, MESH_BROADCAST_ID);
}

static struct survey_control begin_cancel_fixture(bool rf_owned)
{
    memset(&anchor_survey_pending_cancel, 0, sizeof(anchor_survey_pending_cancel));
    queue_calls = progress_calls = rx_calls = 0u;
    cancel_control_decode_calls = 0u;
    forward_schedules = stop_feeds = 0u;
    schedule_status = 0;
    queue_accept = true;
    require_relay = false;
    required_relay_hops = 0u;
    queued_len = 0u;
    queued_received_at = 0u;
    survey_ops.anchor_receive_cancel = anchor_survey_receive_cancel;
    struct survey_control start = begin_active_survey(rf_owned);
    return (struct survey_control) {
        .phase = SURVEY_PHASE_ABORT, .identity = start.identity,
    };
}

static void assert_rejected(const struct wire_control *wire,
                             const struct survey_snapshot *before);

static void cancel_labeled_plan_never_enters_the_large_plan_decoder(void)
{
    struct survey_control cancel = begin_cancel_fixture(true);
    struct survey_snapshot before = snapshot();
    struct survey_control plan = {
        .phase = SURVEY_PHASE_PLAN, .identity = cancel.identity,
        .plan_present = true,
        .plan = {
            .identity = cancel.identity, .execution_start_delay_ms = 2000u,
            .self_stop_delay_ms = 10000u, .pair_count = 1u, .wave_count = 1u,
            .batch_index = 0u, .final_batch = true,
            .pairs = {{.initiator_slot = 0u, .responder_slot = 1u, .wave_index = 0u}},
        },
    };
    struct wire_control wire;
    assert(survey_plan_commitment(&plan.plan, plan.plan.commitment));
    make_wire(&wire, CMD_SURVEY_CANCEL, &plan);
    assert(wire.outbound.payload_len <= ANCHOR_SURVEY_CANCEL_PAYLOAD_MAX);
    semantic_digest_sha256_call_count_reset();
    assert_rejected(&wire, &before);
    assert(cancel_control_decode_calls == 0u);
    assert(semantic_digest_sha256_call_count() == 0u);
}

static void assert_rejected(const struct wire_control *wire,
                             const struct survey_snapshot *before)
{
    bool accepted = anchor_survey_receive_cancel(wire->frame, wire->frame_len);
    if (accepted) {
        fprintf(stderr, "unexpected CANCEL admission src=%llu dst=%llu flags=%u bytes=%zu\n",
            (unsigned long long)wire->outbound.packet.src_id,
            (unsigned long long)wire->outbound.packet.dst_id,
            wire->outbound.packet.flags, wire->frame_len);
    }
    assert(!accepted);
    assert_survey_preserved(before);
    assert(anchor_survey_pending_cancel.frame_len == 0u && queue_calls == 0u);
    assert(forward_schedules == 0u && stop_feeds == 0u);
    assert(anchor_survey_forward_cancel() == 0 && queue_calls == 0u);
}

static void rejects_foreign_controls_and_invalid_envelopes(void)
{
    for (unsigned rf_owned = 0u; rf_owned < 2u; rf_owned++) {
        struct survey_control cancel = begin_cancel_fixture(rf_owned != 0u);
        struct survey_snapshot before = snapshot();
        struct wire_control wire;
        for (unsigned field = 0u; field < 6u; field++) {
            struct survey_control stale = cancel;
            switch (field) {
            case 0: stale.identity.generation++; break;
            case 1: stale.identity.assignment.assignment_epoch++; break;
            case 2: stale.identity.assignment.table_command_seq++; break;
            case 3: stale.identity.assignment.table_commitment.bytes[0] ^= 1u; break;
            case 4: stale.identity.assignment.slot_span++; break;
            case 5: stale.identity.assignment.max_hop_count++; break;
            }
            make_wire(&wire, CMD_SURVEY_CANCEL, &stale);
            assert_rejected(&wire, &before);
        }
        struct survey_control start = start_control();
        make_wire(&wire, CMD_SURVEY_START, &start);
        assert_rejected(&wire, &before);
        make_wire(&wire, CMD_SURVEY_CANCEL, &start);
        assert_rejected(&wire, &before);
        make_wire(&wire, CMD_PING, NULL);
        assert_rejected(&wire, &before);
        make_wire(&wire, CMD_SURVEY_CANCEL, NULL);
        assert_rejected(&wire, &before);

        for (unsigned variant = 0u; variant < 10u; variant++) {
            make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
            switch (variant) {
            case 0: wire.frame[wire.frame_len - 1u] ^= 1u; break;
            case 1: encode_wire(&wire, NETWORK_ID + 1u, MESH_BROADCAST_ID); break;
            case 2:
                wire.outbound.packet.dst_id = DEVICE_ID + 1u;
                encode_wire(&wire, NETWORK_ID, DEVICE_ID + 1u);
                break;
            case 3:
                wire.outbound.packet.src_id = DEVICE_ID + 1u;
                encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
                break;
            case 4:
                wire.outbound.packet.flags |= FLAG_COUNT_AS_CLICK;
                encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
                break;
            case 5: wire.frame_len--; break;
            case 6:
                wire.outbound.packet.ttl = 0u;
                encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
                break;
            case 7:
                wire.outbound.packet.ttl--;
                encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
                break;
            case 8:
                wire.outbound.packet.dst_id = DEVICE_ID + 1u;
                encode_wire(&wire, NETWORK_ID, DEVICE_ID);
                break;
            case 9: wire.frame_len = sizeof(wire.frame); break;
            }
            assert_rejected(&wire, &before);
        }
    }
}

static void broadcast_cancel_must_include_this_anchor(void)
{
    struct survey_control cancel = begin_cancel_fixture(true);
    struct survey_snapshot before = snapshot();
    for (unsigned included = 0u; included < 2u; included++) {
        struct wire_control wire;
        make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
        size_t len = wire.outbound.payload_len;
        for (size_t offset = 0u; offset < len; offset += 2u + wire.outbound.payload[offset + 1u]) {
            if (wire.outbound.payload[offset] == TLV_COMMAND_SCOPE) {
                wire.outbound.payload[offset + 2u] = CMD_SCOPE_ALL_REGISTERED;
                break;
            }
        }
        assert(tlv_append_u16(wire.outbound.payload, sizeof(wire.outbound.payload),
            &len, TLV_MEMBERSHIP_EPOCH, 1u) == PROTO_OK);
        assert(tlv_append_u16(wire.outbound.payload, sizeof(wire.outbound.payload),
            &len, TLV_EXPECTED_NODE_COUNT, 1u) == PROTO_OK);
        assert(tlv_append_u64(wire.outbound.payload, sizeof(wire.outbound.payload),
            &len, TLV_EXPECTED_NODE_ID, included ? DEVICE_ID : DEVICE_ID + 1u) == PROTO_OK);
        wire.outbound.payload_len = wire.outbound.packet.payload_len = (uint16_t)len;
        encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
        if (!included) {
            assert_rejected(&wire, &before);
        } else {
            assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
            assert(!anchor_state.active && receive_aborts == 1u && queue_calls == 0u);
            assert(anchor_survey_forward_cancel() == 0 && queue_calls == 1u);
        }
    }
}

static void forced_hop_policy_cannot_be_bypassed_by_cancel(void)
{
    struct survey_control cancel = begin_cancel_fixture(true);
    struct survey_snapshot before = snapshot();
    struct wire_control wire;
    require_relay = true;
    required_relay_hops = 2u;
    make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
    assert_rejected(&wire, &before);
    wire.previous_hop = DEVICE_ID + 10u;
    wire.outbound.packet.ttl = FLOOD_EPOCH_GLOBAL_TTL - 1u;
    encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
    assert_rejected(&wire, &before);
    wire.outbound.packet.ttl = FLOOD_EPOCH_GLOBAL_TTL - 2u;
    encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
    assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
    assert(!anchor_state.active && receive_aborts == 1u && queue_calls == 0u);
    assert(anchor_survey_forward_cancel() == 0 && queue_calls == 1u);
}

static void rejects_expired_delayed_and_noncanonical_command_options(void)
{
    struct survey_control cancel = begin_cancel_fixture(true);
    struct survey_snapshot before = snapshot();
    for (unsigned variant = 0u; variant < 4u; variant++) {
        struct wire_control wire;
        make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
        size_t len = wire.outbound.payload_len;
        switch (variant) {
        case 0:
            wire.outbound.packet.message_age_ms = SURVEY_HARD_CAP_MS;
            break;
        case 1:
            assert(tlv_append_u32(wire.outbound.payload, sizeof(wire.outbound.payload),
                &len, TLV_EXECUTE_DELAY_MS, 1000u) == PROTO_OK);
            break;
        case 2:
            assert(mesh_append_command_id(wire.outbound.payload, sizeof(wire.outbound.payload),
                &len, CMD_SURVEY_CANCEL) == PROTO_OK);
            break;
        case 3:
            for (size_t offset = 0u; offset < len; offset += 2u + wire.outbound.payload[offset + 1u]) {
                if (wire.outbound.payload[offset] == TLV_COMMAND_SCOPE) {
                    wire.outbound.payload[offset] = TLV_REASON;
                    break;
                }
            }
            break;
        }
        wire.outbound.payload_len = wire.outbound.packet.payload_len = (uint16_t)len;
        encode_wire(&wire, NETWORK_ID, MESH_BROADCAST_ID);
        assert_rejected(&wire, &before);
    }
}

static void matching_cancel_releases_before_deferred_forwarding(void)
{
    for (unsigned rf_owned = 0u; rf_owned < 2u; rf_owned++) {
        struct survey_control cancel = begin_cancel_fixture(rf_owned != 0u);
        struct wire_control wire;
        make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
        uint32_t received_at = now_ms;
        queue_accept = false;
        assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
        assert(!anchor_state.active && anchor_state.aborted);
        assert(anchor_state.action == APP_SURVEY_ANCHOR_ACTION_NONE);
        assert(anchor_rx_lifecycle.operation == PROTOCOL_RX_OPERATION_NONE);
        assert(receive_aborts == 1u && queue_calls == 0u && lock_depth == 0u);
        assert(forward_schedules == 1u && stop_feeds == 0u);
        assert(anchor_survey_cancel_forward_work.due_ms == received_at);
        assert(anchor_survey_pending_cancel.frame_len == wire.frame_len);
        assert(memcmp(anchor_survey_pending_cancel.frame, wire.frame, wire.frame_len) == 0);
        assert(!anchor_survey_receive_cancel(wire.frame, wire.frame_len));
        assert(receive_aborts == 1u && queue_calls == 0u);

        now_ms += 100u;
        anchor_survey_cancel_forward_work_handler(NULL);
        assert(queue_calls == 1u && queued_received_at == received_at);
        assert(forward_schedules == 2u && stop_feeds == 0u);
        assert(anchor_survey_cancel_forward_work.due_ms == now_ms + 250u);
        assert(queued_len == wire.frame_len && memcmp(queued_frame, wire.frame, queued_len) == 0);
        assert(anchor_survey_pending_cancel.frame_len == wire.frame_len);
        now_ms += 100u;
        queue_accept = true;
        anchor_survey_cancel_forward_work_handler(NULL);
        assert(queue_calls == 2u && queued_received_at == received_at);
        assert(forward_schedules == 2u && stop_feeds == 0u);
        assert(anchor_survey_pending_cancel.frame_len == 0u);
        assert(anchor_survey_forward_cancel() == 0 && queue_calls == 2u);
        assert(!anchor_survey_receive_cancel(wire.frame, wire.frame_len));
        assert(receive_aborts == 1u);
    }
}

static void forwarding_schedule_failure_preserves_bytes_and_fails_closed(void)
{
    for (unsigned fail_retry = 0u; fail_retry < 2u; fail_retry++) {
        struct survey_control cancel = begin_cancel_fixture(true);
        struct wire_control wire;
        make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
        if (!fail_retry) schedule_status = -EIO;
        assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
        assert(!anchor_state.active && anchor_state.aborted);
        if (fail_retry) {
            assert(stop_feeds == 0u);
            queue_accept = false;
            schedule_status = -EIO;
            anchor_survey_cancel_forward_work_handler(NULL);
        }
        assert(stop_feeds == 1u);
        assert(anchor_survey_pending_cancel.frame_len == wire.frame_len);
        assert(memcmp(anchor_survey_pending_cancel.frame, wire.frame, wire.frame_len) == 0);
    }
}

static void pending_old_forward_cannot_block_new_surveys_local_cancel(void)
{
    struct survey_control cancel = begin_cancel_fixture(true);
    struct wire_control wire;
    make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
    assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
    queue_accept = false;
    anchor_survey_cancel_forward_work_handler(NULL);
    assert(anchor_survey_pending_cancel.frame_len != 0u);

    struct survey_control next = start_control();
    next.identity.generation++;
    const struct proto_packet packet = {.msg_type = MSG_COMMAND, .src_id = GATEWAY_ID};
    assert(app_survey_anchor_apply_control(&packet, &next) == 0);
    assert(anchor_state.active);
    cancel.identity = next.identity;
    make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
    assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
    assert(!anchor_state.active && anchor_state.aborted && receive_aborts == 2u);
    assert(anchor_survey_pending_cancel.frame_len == wire.frame_len);
    assert(memcmp(anchor_survey_pending_cancel.frame, wire.frame, wire.frame_len) == 0);
}

static void full_queue_forwarding_expires_without_extending_receipt_time(void)
{
    for (unsigned wrap = 0u; wrap < 2u; wrap++) {
        struct survey_control cancel = begin_cancel_fixture(true);
        struct wire_control wire;
        if (wrap) {
            const uint32_t shift = UINT32_MAX - 2000u;
            now_ms += shift;
            anchor_state.neighbor_start_ms += shift;
            anchor_state.self_stop_ms += shift;
            anchor_rx_lifecycle.deadline_ms += shift;
            scheduled_at += shift;
        }
        make_wire(&wire, CMD_SURVEY_CANCEL, &cancel);
        assert(anchor_survey_receive_cancel(wire.frame, wire.frame_len));
        uint32_t received_at = now_ms;
        queue_accept = false;
        now_ms += SURVEY_CONTROL_ORIGIN_BUDGET_MS - 1u;
        assert(anchor_survey_forward_cancel() == -EAGAIN);
        assert(queue_calls == 1u && queued_received_at == received_at);
        now_ms++;
        assert(anchor_survey_forward_cancel() == 0 && queue_calls == 1u);
        assert(anchor_survey_pending_cancel.frame_len == 0u);
        assert(!anchor_state.active && receive_aborts == 1u);
    }
}

static const struct wire_control *rx_inputs[8];
static size_t rx_input_count, rx_input_index;
static uint32_t expected_rx_deadline;
static int receive_error;

int dwm3000_driver_receive_frame_continuous(uint32_t timeout_ms,
    uint8_t *frame, size_t frame_cap, size_t *frame_len, uint8_t *quality,
    int8_t *rsl_dbm, enum dwm3000_rx_failure *failure)
{
    assert(quality == NULL && rsl_dbm == NULL);
    assert(timeout_ms == expected_rx_deadline - now_ms);
    rx_calls++;
    *failure = DWM3000_RX_FAILURE_NONE;
    if (receive_error != 0) {
        now_ms++;
        return receive_error;
    }
    if (rx_input_index < rx_input_count) {
        const struct wire_control *input = rx_inputs[rx_input_index++];
        assert(input->frame_len <= frame_cap && timeout_ms >= 5u);
        now_ms += 5u;
        memcpy(frame, input->frame, input->frame_len);
        *frame_len = input->frame_len;
        return 0;
    }
    now_ms += timeout_ms;
    return -ETIMEDOUT;
}

static void app_watchdog_note_radio_progress(void) { progress_calls++; }
#include "survey_cancel_wait.inc"

static void absolute_wait_ignores_other_traffic_and_stops_for_exact_cancel(void)
{
    for (unsigned cancel_received = 0u; cancel_received < 2u; cancel_received++) {
        struct survey_control cancel = begin_cancel_fixture(true);
        struct survey_snapshot before = snapshot();
        struct wire_control unrelated, stale, malformed, matching;
        make_wire(&unrelated, CMD_SURVEY_START, &(struct survey_control) {
            .phase = SURVEY_PHASE_NEIGHBOR_START, .identity = cancel.identity,
            .start_delay_present = true, .start_delay_ms = 500u,
            .self_stop_delay_present = true, .self_stop_delay_ms = 10000u,
        });
        cancel.identity.generation++;
        make_wire(&stale, CMD_SURVEY_CANCEL, &cancel);
        cancel.identity.generation--;
        make_wire(&matching, CMD_SURVEY_CANCEL, &cancel);
        malformed = matching;
        malformed.frame[malformed.frame_len - 1u] ^= 1u;
        rx_inputs[0] = &unrelated; rx_inputs[1] = &stale;
        rx_inputs[2] = &malformed; rx_inputs[3] = &matching;
        rx_input_count = cancel_received ? 4u : 3u;
        rx_input_index = 0u; receive_error = 0;
        uint32_t started_at = now_ms;
        expected_rx_deadline = started_at + 50u;
        int ret = anchor_wait_until(cancel.identity.generation, expected_rx_deadline);
        assert(rx_calls == 4u && queue_calls == 0u);
        if (cancel_received) {
            assert(ret == -ECANCELED && now_ms == started_at + 20u);
            assert(progress_calls == 3u);
            assert(!anchor_state.active && anchor_state.aborted && receive_aborts == 1u);
            assert(anchor_survey_forward_cancel() == 0 && queue_calls == 1u);
        } else {
            assert(ret == 0 && now_ms == expected_rx_deadline);
            assert(progress_calls == 4u);
            assert_survey_preserved(&before);
        }
    }
    struct survey_control cancel = begin_cancel_fixture(true);
    expected_rx_deadline = now_ms + 50u;
    receive_error = -EIO;
    assert(anchor_wait_until(cancel.identity.generation, expected_rx_deadline) == -EIO);
    assert(rx_calls == 1u && progress_calls == 0u && queue_calls == 0u);
}

int main(void)
{
    rejects_foreign_controls_and_invalid_envelopes();
    cancel_labeled_plan_never_enters_the_large_plan_decoder();
    broadcast_cancel_must_include_this_anchor();
    forced_hop_policy_cannot_be_bypassed_by_cancel();
    rejects_expired_delayed_and_noncanonical_command_options();
    matching_cancel_releases_before_deferred_forwarding();
    forwarding_schedule_failure_preserves_bytes_and_fails_closed();
    pending_old_forward_cannot_block_new_surveys_local_cancel();
    full_queue_forwarding_expires_without_extending_receipt_time();
    absolute_wait_ignores_other_traffic_and_stops_for_exact_cancel();
    puts("production survey RF CANCEL admission, deferred forwarding and absolute waits passed");
    return 0;
}
