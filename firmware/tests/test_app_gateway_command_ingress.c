#include "app_gateway_command_ingress.h"

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
    bool cancelled[3];
    int admit_ret;
    int submit_ret;
};

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

static int submit_priority(void *ctx)
{
    struct ingress_fixture *fixture = ctx;

    fixture->submit_count++;
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
            return 0;
        }
    }
    return -ENOENT;
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
        .admit = admit,
        .submit_priority = submit_priority,
        .cancel_admitted = cancel_admitted,
        .emit_result = emit_result,
        .ctx = fixture,
    };
}

static size_t command_frame(uint16_t seq, uint8_t *frame, size_t frame_cap)
{
    const uint8_t payload[] = {
        TLV_COMMAND_ID, 2u,
        (uint8_t)CMD_FORCE_REDISCOVERY,
        (uint8_t)(CMD_FORCE_REDISCOVERY >> 8),
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = UINT64_C(0x1111),
        .dst_id = UINT64_C(0x9000),
        .session_id = 77u,
        .seq = seq,
    };
    size_t frame_len = 0u;

    packet.payload_len = sizeof(payload);
    assert(serial_frame_encode_packet(&packet, payload, frame, frame_cap,
                                      &frame_len) == PROTO_OK);
    return frame_len;
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
    test_priority_failure_cancels_exact_admitted_command_before_one_error();
    test_queue_admission_failure_reports_once_without_priority_submit();
    test_non_command_decodes_for_normal_gateway_routing();
    return 0;
}
