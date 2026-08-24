#include "app_gateway_command_ingress.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_gateway_command_flow.h"

#include "mesh_relay.h"
#include "serial_frame.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

struct ingress_fixture {
    struct app_gateway_command_ingress_item admitted[3];
    struct app_gateway_command_identity cancelled_identity;
    struct proto_packet result_command;
    struct proto_packet executed[3];
    enum command_id result_command_id;
    enum command_status result_status;
    uint8_t result_reason;
    uint8_t admit_count;
    uint8_t submit_count;
    uint8_t cancel_count;
    uint8_t result_count;
    uint8_t execute_count;
    uint32_t last_admission_cutoff;
    bool cancelled[3];
    int admit_ret;
    int submit_ret;
    int cancel_ret;
};

static struct app_gateway_command_ingress_ops ops_for(
    struct ingress_fixture *fixture);
static size_t command_frame(uint16_t seq, uint8_t *frame, size_t frame_cap);
static size_t command_frame_for(uint16_t seq,
                                enum command_id command_id,
                                uint8_t *frame,
                                size_t frame_cap);
static size_t command_frame_for_ttl_target(uint16_t seq,
                                           enum command_id command_id,
                                           uint8_t ttl,
                                           uint64_t dst_id,
                                           uint8_t *frame,
                                           size_t frame_cap);
static size_t command_frame_for_payload(
    const struct proto_packet *packet_template,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *frame,
    size_t frame_cap);

static int admit(void *ctx, struct app_gateway_command_ingress_item *item)
{
    struct ingress_fixture *fixture = ctx;

    if (fixture->admit_ret < 0) {
        return fixture->admit_ret;
    }
    assert(fixture->admit_count < 3u);
    item->admission_id = (uint32_t)fixture->admit_count + 1u;
    fixture->admitted[fixture->admit_count++] = *item;
    return 0;
}

static int submit_priority(void *ctx, uint32_t admission_cutoff)
{
    struct ingress_fixture *fixture = ctx;

    fixture->submit_count++;
    fixture->last_admission_cutoff = admission_cutoff;
    return fixture->submit_ret;
}

static int cancel_admitted(void *ctx,
                           const struct app_gateway_command_identity *identity)
{
    struct ingress_fixture *fixture = ctx;

    fixture->cancel_count++;
    fixture->cancelled_identity = *identity;
    for (uint8_t i = 0u; i < fixture->admit_count; i++) {
        if (app_gateway_command_identity_matches(identity, &fixture->admitted[i])) {
            fixture->cancelled[i] = true;
            return fixture->cancel_ret;
        }
    }
    return -ENOENT;
}

static void test_cancel_failure_still_reports_one_terminal_result(void)
{
    struct ingress_fixture fixture = {
        .submit_ret = -EIO,
        .cancel_ret = -ENOSPC,
    };
    struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
    struct app_gateway_command_ingress_item item;
    bool command_handled;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = command_frame(44u, frame, sizeof(frame));

    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == -EIO);
    assert(command_handled);
    assert(fixture.cancel_count == 1u);
    assert(fixture.result_count == 1u);
    assert(fixture.result_command.seq == 44u);
    assert(fixture.result_status == COMMAND_INTERNAL_ERROR);
}

static void execute_admitted_in_order(struct ingress_fixture *fixture)
{
    for (uint8_t i = 0u; i < fixture->admit_count; i++) {
        if (!fixture->cancelled[i]) {
            fixture->executed[fixture->execute_count++] =
                fixture->admitted[i].packet;
        }
    }
}

static void emit_result(void *ctx,
                        const struct proto_packet *command,
                        enum command_id command_id,
                        enum command_status status,
                        uint8_t reason)
{
    struct ingress_fixture *fixture = ctx;

    fixture->result_count++;
    fixture->result_command = *command;
    fixture->result_command_id = command_id;
    fixture->result_status = status;
    fixture->result_reason = reason;
}

static struct app_gateway_command_ingress_ops ops_for(
    struct ingress_fixture *fixture)
{
    return (struct app_gateway_command_ingress_ops) {
        .gateway_role = true,
        .gateway_id = UINT64_C(0x9000),
        .admit = admit,
        .submit_priority = submit_priority,
        .cancel_admitted = cancel_admitted,
        .emit_result = emit_result,
        .ctx = fixture,
    };
}

static size_t command_frame(uint16_t seq, uint8_t *frame, size_t frame_cap)
{
    return command_frame_for(
        seq, CMD_FORCE_REDISCOVERY, frame, frame_cap);
}

static size_t command_frame_for(uint16_t seq,
                                enum command_id command_id,
                                uint8_t *frame,
                                size_t frame_cap)
{
    return command_frame_for_ttl_target(
        seq, command_id, 1u, UINT64_C(0x9000), frame, frame_cap);
}

static size_t command_frame_for_ttl_target(uint16_t seq,
                                           enum command_id command_id,
                                           uint8_t ttl,
                                           uint64_t dst_id,
                                           uint8_t *frame,
                                           size_t frame_cap)
{
    const uint8_t payload[] = {
        TLV_COMMAND_ID, 2u,
        (uint8_t)command_id,
        (uint8_t)(command_id >> 8),
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1111),
        .dst_id = dst_id,
        .session_id = 77u,
        .seq = seq,
        .ttl = ttl,
    };
    size_t frame_len = 0u;

    packet.payload_len = sizeof(payload);
    assert(serial_frame_encode_packet(&packet, payload, frame, frame_cap,
                                      &frame_len) == PROTO_OK);
    return frame_len;
}

static size_t command_frame_for_payload(
    const struct proto_packet *packet_template,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *frame,
    size_t frame_cap)
{
    struct proto_packet packet = *packet_template;
    size_t frame_len = 0u;

    packet.payload_len = payload_len;
    assert(serial_frame_encode_packet(&packet,
                                      payload,
                                      frame,
                                      frame_cap,
                                      &frame_len) == PROTO_OK);
    return frame_len;
}

static void test_ingress_to_c5_normalizes_command_class_ttl(void)
{
    static const struct {
        enum command_id command_id;
        uint8_t canonical_ttl;
    } command_cases[] = {
        {CMD_PING, FLOOD_EPOCH_GLOBAL_TTL},
        {CMD_ASSIGN_DISCOVERY_SLOTS, FLOOD_EPOCH_GLOBAL_TTL},
    };
    static const uint8_t host_ttls[] = {
        0u,
        FLOOD_EPOCH_GLOBAL_TTL,
        1u,
        UINT8_MAX,
    };
    const uint64_t gateway_id = UINT64_C(0x9000);
    const uint64_t target_id = UINT64_C(0xa100);

    for (size_t command_index = 0u;
         command_index < sizeof(command_cases) / sizeof(command_cases[0]);
         command_index++) {
        for (size_t ttl_index = 0u;
             ttl_index < sizeof(host_ttls) / sizeof(host_ttls[0]);
             ttl_index++) {
            struct ingress_fixture fixture = {0};
            struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
            struct app_gateway_command_ingress_item item;
            struct app_mesh_gateway_command_flow flow;
            struct mesh_relay target;
            const struct route_candidate *route;
            bool command_handled = false;
            uint8_t origin_ttl = 0u;
            uint8_t frame[SERIAL_FRAME_MAX_LEN];
            size_t frame_len = command_frame_for_ttl_target(
                (uint16_t)(100u + command_index * 10u + ttl_index),
                command_cases[command_index].command_id,
                host_ttls[ttl_index],
                target_id,
                frame,
                sizeof(frame));

            assert(app_gateway_command_ingress_handle_frame(
                       &ops,
                       frame,
                       frame_len,
                       &item,
                       &command_handled) == 0);
            assert(command_handled);
            assert(fixture.admit_count == 1u);
            assert(app_mesh_gateway_command_flow_prepare(
                       &item.packet,
                       item.payload,
                       item.payload_len,
                       gateway_id,
                       1000u,
                       1u,
                       &flow) == PROTO_OK);
            assert(flow.outbound.packet.ttl ==
                   command_cases[command_index].canonical_ttl);
            assert(app_mesh_c5_gateway_control_origin_ttl(
                flow.outbound.packet.msg_type,
                (uint16_t)flow.command_id,
                &origin_ttl));
            assert(origin_ttl ==
                   command_cases[command_index].canonical_ttl);

            mesh_relay_init(&target,
                            MESH_RELAY_ROLE_ANCHOR,
                            target_id,
                            gateway_id,
                            41u);
            assert(mesh_relay_note_gateway_control_reverse_route(
                       &target,
                       &flow.outbound.packet,
                       gateway_id,
                       95u,
                       origin_ttl,
                       1001u) == PROTO_OK);
            route = route_selected(&target.upstream);
            assert(route != NULL);
            assert(route->next_hop_id == gateway_id);
            assert(route->hop_count == 0u);
        }
    }
}

static void test_ordinary_command_retains_serialized_priority_dispatch(void)
{
    struct ingress_fixture fixture = {0};
    struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
    struct app_gateway_command_ingress_item item;
    bool command_handled;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = command_frame(46u, frame, sizeof(frame));

    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == 0);
    assert(command_handled);
    assert(item.command_id == CMD_FORCE_REDISCOVERY);
    assert(fixture.admit_count == 1u);
    assert(fixture.submit_count == 1u);
}

static void test_priority_failure_cancels_exact_admitted_command_before_one_error(void)
{
    struct ingress_fixture fixture = {.submit_ret = -EIO};
    struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
    struct app_gateway_command_ingress_item item;
    bool command_handled;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = command_frame(41u, frame, sizeof(frame));

    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == -EIO);
    assert(command_handled);
    assert(fixture.admit_count == 1u);
    assert(fixture.submit_count == 1u);
    assert(fixture.last_admission_cutoff ==
           fixture.admitted[0].admission_id);
    assert(fixture.cancel_count == 1u);
    assert(app_gateway_command_identity_matches(&fixture.cancelled_identity,
                                                &fixture.admitted[0]));
    assert(fixture.result_count == 1u);
    assert(fixture.result_command.seq == 41u);
    assert(fixture.result_status == COMMAND_INTERNAL_ERROR);
    assert(fixture.result_reason == (uint8_t)EIO);

    fixture.submit_ret = 0;
    frame_len = command_frame(42u, frame, sizeof(frame));
    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == 0);
    frame_len = command_frame(43u, frame, sizeof(frame));
    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == 0);
    execute_admitted_in_order(&fixture);
    assert(fixture.execute_count == 2u);
    assert(fixture.executed[0].seq == 42u);
    assert(fixture.executed[1].seq == 43u);
}

static void test_priority_contention_retains_accepted_command_custody(void)
{
    static const int contention_errors[] = {
        -EAGAIN,
        -EBUSY,
        -ENOSPC,
    };

    for (size_t i = 0u;
         i < sizeof(contention_errors) / sizeof(contention_errors[0]);
         i++) {
        struct ingress_fixture fixture = {
            .submit_ret = contention_errors[i],
        };
        struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
        struct app_gateway_command_ingress_item item;
        bool command_handled = false;
        uint8_t frame[SERIAL_FRAME_MAX_LEN];
        size_t frame_len = command_frame(
            (uint16_t)(80u + i), frame, sizeof(frame));

        assert(app_gateway_command_ingress_handle_frame(
                   &ops,
                   frame,
                   frame_len,
                   &item,
                   &command_handled) == 0);
        assert(command_handled);
        assert(fixture.admit_count == 1u);
        assert(fixture.submit_count == 1u);
        assert(fixture.last_admission_cutoff == item.admission_id);
        assert(fixture.cancel_count == 0u);
        assert(fixture.result_count == 0u);
        execute_admitted_in_order(&fixture);
        assert(fixture.execute_count == 1u);
        assert(fixture.executed[0].seq == (uint16_t)(80u + i));
    }
}

static void test_admission_cutoff_is_wrap_safe_and_excludes_newer_items(void)
{
    assert(app_gateway_command_admission_within_cutoff(7u, 7u));
    assert(app_gateway_command_admission_within_cutoff(6u, 7u));
    assert(!app_gateway_command_admission_within_cutoff(8u, 7u));
    assert(app_gateway_command_admission_within_cutoff(UINT32_MAX, 1u));
    assert(!app_gateway_command_admission_within_cutoff(2u, 1u));
    assert(!app_gateway_command_admission_within_cutoff(
        UINT32_C(0x80000001), 1u));
    assert(!app_gateway_command_admission_within_cutoff(0u, 1u));
    assert(!app_gateway_command_admission_within_cutoff(1u, 0u));
}

static void test_queue_admission_failure_reports_once_without_priority_submit(void)
{
    struct ingress_fixture fixture = {.admit_ret = -ENOSPC};
    struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
    struct app_gateway_command_ingress_item item;
    bool command_handled;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = command_frame(42u, frame, sizeof(frame));

    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == -ENOSPC);
    assert(command_handled);
    assert(fixture.admit_count == 0u);
    assert(fixture.submit_count == 0u);
    assert(fixture.cancel_count == 0u);
    assert(fixture.result_count == 1u);
    assert(fixture.result_status == COMMAND_BUSY);
}

static void test_non_command_decodes_for_normal_gateway_routing(void)
{
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .src_id = UINT64_C(0x2222),
        .dst_id = UINT64_C(0x9000),
        .session_id = 12u,
        .seq = 5u,
    };
    const uint8_t payload[] = {0xaau};
    struct ingress_fixture fixture = {0};
    struct app_gateway_command_ingress_ops ops = ops_for(&fixture);
    struct app_gateway_command_ingress_item item;
    bool command_handled;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;

    packet.payload_len = sizeof(payload);
    assert(serial_frame_encode_packet(&packet, payload, frame, sizeof(frame),
                                      &frame_len) == PROTO_OK);
    assert(app_gateway_command_ingress_handle_frame(&ops, frame, frame_len,
                                                    &item,
                                                    &command_handled) == 0);
    assert(!command_handled);
    assert(item.packet.msg_type == MSG_MESH_DATA);
    assert(item.payload_len == sizeof(payload));
    assert(item.payload[0] == payload[0]);
    assert(fixture.admit_count == 0u);
    assert(fixture.result_count == 0u);
}

int main(void)
{
    test_ingress_to_c5_normalizes_command_class_ttl();
    test_ordinary_command_retains_serialized_priority_dispatch();
    test_priority_failure_cancels_exact_admitted_command_before_one_error();
    test_priority_contention_retains_accepted_command_custody();
    test_admission_cutoff_is_wrap_safe_and_excludes_newer_items();
    test_cancel_failure_still_reports_one_terminal_result();
    test_queue_admission_failure_reports_once_without_priority_submit();
    test_non_command_decodes_for_normal_gateway_routing();
    return 0;
}
