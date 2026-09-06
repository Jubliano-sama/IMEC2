/* Exact extraction from firmware/app/src/app_gateway_ble.c.
 * Source SHA256: e9ae426678c3e0f642f782623b4fac55d4543a449e9d90e535054e9be5547e01
 */
#include "protocol.h"
#include <stdio.h>

static bool gateway_host_custody_supported(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    /* Only the durable assignment publisher is command-event host custody.
     * Generic observability packets are self-addressed best-effort telemetry. */
    if (packet->msg_type == MSG_GATEWAY_COMMAND_EVENT) {
        return packet->flags == FLAG_GATEWAY_ACK_REQUIRED;
    }
    if ((packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
        return true;
    case MSG_MESH_DATA:
        return (packet->flags & FLAG_DIAGNOSTIC) != 0u;
    default:
        return false;
    }
}

int main(void)
{
    struct proto_packet packet = {.flags = FLAG_GATEWAY_ACK_REQUIRED};
    packet.msg_type = MSG_SURVEY_EVENT;
    printf("ACK-required MSG_SURVEY_EVENT: %d\n",
           gateway_host_custody_supported(&packet));
    packet.msg_type = MSG_COMMAND_RESULT;
    printf("ACK-required MSG_COMMAND_RESULT: %d\n",
           gateway_host_custody_supported(&packet));
    return 0;
}
