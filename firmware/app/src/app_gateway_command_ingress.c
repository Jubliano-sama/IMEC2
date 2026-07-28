#include "app_gateway_command_ingress.h"

#include "serial_frame.h"

#include <errno.h>
#include <string.h>

int app_gateway_command_identity_from_item(
    const struct app_gateway_command_ingress_item *item,
    struct app_gateway_command_identity *identity)
{
    if (item == NULL || identity == NULL) {
        return -EINVAL;
    }

    *identity = (struct app_gateway_command_identity) {
        .msg_type = item->packet.msg_type,
        .src_id = item->packet.src_id,
        .dst_id = item->packet.dst_id,
        .session_id = item->packet.session_id,
        .seq = item->packet.seq,
        .admission_id = item->admission_id,
        .command_id = item->command_id,
    };
    return 0;
}

bool app_gateway_command_identity_matches(
    const struct app_gateway_command_identity *identity,
    const struct app_gateway_command_ingress_item *item)
{
    if (identity == NULL || item == NULL) {
        return false;
    }
    return identity->msg_type == item->packet.msg_type &&
           identity->src_id == item->packet.src_id &&
           identity->dst_id == item->packet.dst_id &&
           identity->session_id == item->packet.session_id &&
           identity->seq == item->packet.seq &&
           identity->admission_id == item->admission_id &&
           identity->command_id == item->command_id;
}

int app_gateway_command_ingress_handle_frame(
    const struct app_gateway_command_ingress_ops *ops,
    const uint8_t *frame,
    size_t frame_len,
    struct app_gateway_command_ingress_item *item_out,
    bool *command_handled)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    struct app_gateway_command_identity identity;
    int ret;

    if (ops == NULL || !ops->gateway_role || frame == NULL || item_out == NULL ||
        command_handled == NULL || ops->admit == NULL ||
        ops->submit_priority == NULL || ops->cancel_admitted == NULL ||
        ops->emit_result == NULL) {
        return -EINVAL;
    }

    memset(item_out, 0, sizeof(*item_out));
    *command_handled = false;
    ret = serial_frame_decode_packet(frame, frame_len, &item_out->packet,
                                     item_out->payload, sizeof(item_out->payload),
                                     &item_out->payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (ops->note_decoded != NULL) {
        ops->note_decoded(ops->ctx, item_out);
    }
    if (item_out->packet.msg_type != MSG_COMMAND) {
        return 0;
    }

    *command_handled = true;
    if (gateway_command_extract_id(item_out->payload, item_out->payload_len,
                                   &command_id) != PROTO_OK) {
        command_id = CMD_VENDOR_BASE;
    }
    item_out->command_id = command_id;
    if (ops->is_preemptive != NULL &&
        ops->is_preemptive(ops->ctx, item_out)) {
        if (ops->submit_preemptive == NULL) {
            return -EINVAL;
        }
        ret = ops->submit_preemptive(ops->ctx, item_out);
        if (ret < 0) {
            ops->emit_result(ops->ctx, &item_out->packet, command_id,
                             COMMAND_BUSY, (uint8_t)(-ret));
        }
        return ret;
    }
    ret = ops->admit(ops->ctx, item_out);
    if (ret < 0) {
        ops->emit_result(ops->ctx, &item_out->packet, command_id,
                         COMMAND_BUSY, (uint8_t)(-ret));
        return ret;
    }
    ret = app_gateway_command_identity_from_item(item_out, &identity);
    if (ret < 0) {
        return ret;
    }
    ret = ops->submit_priority(ops->ctx);
    if (ret == -EAGAIN) {
        /* The backend retained admission and owns the bounded resubmit. */
        return 0;
    }
    if (ret < 0) {
        /* Dispatch must honor the identity tombstone even if removal failed. */
        (void)ops->cancel_admitted(ops->ctx, &identity);
        ops->emit_result(ops->ctx, &item_out->packet, command_id,
                         COMMAND_INTERNAL_ERROR, (uint8_t)(-ret));
    }
    return ret;
}
