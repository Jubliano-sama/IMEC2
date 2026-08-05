#include "app_gateway_command_ingress.h"

#include "serial_frame.h"

#include <errno.h>
#include <string.h>

bool app_gateway_command_ingress_contention_retryable(int error)
{
    return error == -EAGAIN || error == -EBUSY || error == -ENOSPC;
}

bool app_gateway_command_admission_within_cutoff(uint32_t admission_id,
                                                uint32_t admission_cutoff)
{
    uint32_t distance;

    if (admission_id == 0u || admission_cutoff == 0u) {
        return false;
    }
    distance = admission_cutoff - admission_id;
    return distance < UINT32_C(0x80000000);
}

int app_gateway_command_ingress_validate_command(
    const struct app_gateway_command_ingress_item *item,
    uint64_t gateway_id)
{
    struct gateway_command_options options;
    enum command_id command_id;
    int ret;

    if (item == NULL || item->packet.msg_type != MSG_COMMAND ||
        item->packet.payload_len != item->payload_len) {
        return -EINVAL;
    }
    ret = gateway_command_extract_id(item->payload,
                                     item->payload_len,
                                     &command_id);
    if (ret != PROTO_OK || command_id != item->command_id) {
        return -EBADMSG;
    }
    ret = gateway_command_extract_options(item->payload,
                                          item->payload_len,
                                          &options);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }

    if (gateway_id != 0u &&
        item->command_id == CMD_SURVEY_ABORT &&
        item->packet.dst_id == gateway_id) {
        /*
         * The gateway-local recovery command is a deliberately closed
         * command-ID-only schema.  It bypasses normal command serialization,
         * so its complete host envelope must be canonical before that
         * authority is even classified as preemptive.
         */
        if (item->packet.flags != 0u ||
            item->packet.src_id == 0u ||
            item->packet.src_id == gateway_id ||
            item->packet.session_id == 0u ||
            item->packet.seq == 0u ||
            item->packet.ttl != 1u ||
            item->packet.message_age_ms != 0u ||
            item->payload_len != PROTO_TLV_U16_ENCODED_LEN ||
            item->payload[0] != TLV_COMMAND_ID ||
            item->payload[1] != sizeof(uint16_t)) {
            return -EBADMSG;
        }
    }
    return 0;
}

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
    ret = gateway_command_extract_id(item_out->payload,
                                     item_out->payload_len,
                                     &command_id);
    if (ret != PROTO_OK) {
        item_out->command_id = CMD_VENDOR_BASE;
        ops->emit_result(ops->ctx,
                         &item_out->packet,
                         CMD_VENDOR_BASE,
                         COMMAND_MALFORMED_PAYLOAD,
                         EBADMSG);
        return -EBADMSG;
    }
    item_out->command_id = command_id;
    ret = app_gateway_command_ingress_validate_command(item_out,
                                                       ops->gateway_id);
    if (ret < 0) {
        ops->emit_result(ops->ctx,
                         &item_out->packet,
                         command_id,
                         COMMAND_MALFORMED_PAYLOAD,
                         EBADMSG);
        return -EBADMSG;
    }
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
    ret = ops->submit_priority(ops->ctx, item_out->admission_id);
    if (ret < 0) {
        if (app_gateway_command_ingress_contention_retryable(ret)) {
            /*
             * Admission already transferred FIFO and result custody. The
             * submitter retains a bounded retry owner for ordinary resource
             * contention, so neither a cancellation tombstone nor a terminal
             * result is valid here.
             */
            return 0;
        }
        /* Dispatch must honor the identity tombstone even if removal failed. */
        (void)ops->cancel_admitted(ops->ctx, &identity);
        ops->emit_result(ops->ctx, &item_out->packet, command_id,
                         COMMAND_INTERNAL_ERROR, (uint8_t)(-ret));
    }
    return ret;
}
