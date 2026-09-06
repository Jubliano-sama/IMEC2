#include "app_anchor_action_runtime.h"

#include "app_board.h"
#include "app_config.h"
#include "app_durable_state.h"
#include "app_mesh_gateway_command_flow.h"
#include "app_state.h"
#include "gateway_command.h"

#include <errno.h>
#include <zephyr/kernel.h>

/* Called under the existing serialized anchor command owner, after survey
 * admission and result reservation. No new radio or persistent owner. */
static struct app_anchor_actions actions;

static int identify(void *ctx)
{
    ARG_UNUSED(ctx);
    return status_identify_anchor();
}

static int read_battery(void *ctx, uint16_t *mv)
{
    ARG_UNUSED(ctx);
    int ret = battery_sample_lithium_mv(mv);

    status_debug_printf("DBG_ANCHOR_BATTERY ret=%d mv=%u at=%llu\n", ret,
                        ret == 0 ? *mv : 0u,
                        (unsigned long long)k_uptime_get());
    return ret;
}

int app_anchor_action_handle(const struct proto_packet *command,
    const uint8_t *payload, size_t payload_len,
    const struct app_node_comm_reservation_lease *reservation)
{
    const struct app_anchor_action_ops ops = {
        .identify = identify, .read_battery = read_battery,
    };
    struct app_anchor_action_result result;
    app_node_comm_envelope outbound = {0};
    enum command_id command_id;
    uint32_t epoch = 0u, boot = 0u, requested_epoch = 0u;
    uint8_t slot, count, hops = 1u;
    uint64_t now = (uint64_t)k_uptime_get();
    size_t length;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || reservation == NULL ||
        gateway_command_extract_id(payload, payload_len, &command_id) != PROTO_OK ||
        !app_anchor_action_command(command_id)) {
        return -EINVAL;
    }
    (void)local_anchor_discovery_assignment_get(&epoch, &slot, &count);
    ret = app_durable_state_boot_incarnation(&boot);
    if (ret < 0) {
        return ret;
    }
    ret = app_anchor_action_execute(&actions, command, payload, payload_len,
        DEVICE_ID, GATEWAY_ID, boot, epoch, now, &ops, &result);
    if (ret < 0) {
        return ret;
    }
    ret = app_anchor_action_result_payload(command_id, DEVICE_ID, epoch,
        &result, outbound.payload, sizeof(outbound.payload), &length);
    if (ret != PROTO_OK) {
        return -EMSGSIZE;
    }
    outbound.payload_len = (uint16_t)length;
    ret = app_mesh_gateway_command_flow_init_result(&outbound, command,
        DEVICE_ID, GATEWAY_ID, false);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    now = (uint64_t)k_uptime_get();
    uint64_t age = now - result.sampled_at_ms;
    outbound.packet.message_age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    (void)app_anchor_action_request(payload, payload_len, &requested_epoch, &hops);
    ret = app_node_comm_commit_protocol_response(reservation, &outbound,
        now + app_anchor_action_delivery_ms(hops),
        ((uint32_t)command_id << 16) | command->seq, NULL);
    status_debug_printf("DBG_ANCHOR_ACTION cmd=%u session=%u seq=%u status=%u ret=%d\n",
        command_id, command->session_id, command->seq, result.status, ret);
    return ret;
}
