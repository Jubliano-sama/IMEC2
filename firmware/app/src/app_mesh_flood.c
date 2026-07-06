#include "app_mesh_flood.h"

#include <errno.h>

static bool flood_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint16_t flood_remaining_delay_ms(uint32_t now_ms, uint32_t deadline_ms)
{
    uint32_t delay_ms;

    if (flood_deadline_reached(now_ms, deadline_ms)) {
        return 0u;
    }

    delay_ms = deadline_ms - now_ms;
    return delay_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)delay_ms;
}

uint8_t app_mesh_flood_repeat_limit(void)
{
    return (uint8_t)(1u + (FLOOD_RELAY_BURST_MS / FLOOD_RELAY_REPEAT_MS));
}

int app_mesh_flood_send_bounded(const struct mesh_outbound *out,
                                const struct app_mesh_flood_ops *ops,
                                struct app_mesh_flood_result *result)
{
    struct app_mesh_flood_result local_result = {0};
    struct mesh_outbound tx;
    uint32_t first_due_ms;
    uint32_t route_reply_rx_open_ms = 0u;
    uint16_t route_reply_rx_delay_ms = 0u;
    uint8_t repeat_limit;
    bool update_route_reply_rx_eta;
    int last_send_ret = 0;

    if (out == NULL || ops == NULL ||
        ops->now_ms == NULL ||
        ops->sleep_until_ms == NULL ||
        ops->defer_active == NULL ||
        ops->c5_quiet == NULL ||
        ops->send == NULL ||
        out->packet.dst_id != MESH_BROADCAST_ID ||
        out->next_hop_id != MESH_BROADCAST_ID ||
        out->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ||
        FLOOD_RELAY_REPEAT_MS == 0u) {
        return -EINVAL;
    }

    first_due_ms = out->earliest_tx_ms != 0u ? out->earliest_tx_ms : ops->now_ms(ops->ctx);
    local_result.first_due_ms = first_due_ms;
    repeat_limit = app_mesh_flood_repeat_limit();
    update_route_reply_rx_eta =
        mesh_route_request_reply_rx_delay_ms(out, &route_reply_rx_delay_ms);
    if (update_route_reply_rx_eta) {
        route_reply_rx_open_ms = first_due_ms + route_reply_rx_delay_ms;
    }

    for (uint8_t repeat = 0u; repeat < repeat_limit; repeat++) {
        uint32_t due_ms = first_due_ms + ((uint32_t)repeat * FLOOD_RELAY_REPEAT_MS);

        local_result.last_due_ms = due_ms;
        if (ops->defer_active(ops->ctx)) {
            local_result.deferred_count++;
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.sent_count > 0u ? 0 : -EAGAIN;
        }
        if (!flood_deadline_reached(ops->now_ms(ops->ctx), due_ms)) {
            ops->sleep_until_ms(due_ms, ops->ctx);
        }
        if (ops->defer_active(ops->ctx)) {
            local_result.deferred_count++;
            if (result != NULL) {
                *result = local_result;
            }
            return local_result.sent_count > 0u ? 0 : -EAGAIN;
        }
        if (!ops->c5_quiet(C5_POLITE_SNIFF_MS, ops->ctx)) {
            local_result.busy_skip_count++;
            continue;
        }

        if (update_route_reply_rx_eta) {
            tx = *out;
            (void)mesh_route_request_set_reply_rx_delay_ms(
                &tx,
                flood_remaining_delay_ms(ops->now_ms(ops->ctx),
                                         route_reply_rx_open_ms));
            last_send_ret = ops->send(&tx, ops->ctx);
        } else {
            last_send_ret = ops->send(out, ops->ctx);
        }
        if (last_send_ret == 0) {
            local_result.sent_count++;
            continue;
        }
        if (last_send_ret == -EBUSY || last_send_ret == -EAGAIN) {
            local_result.busy_skip_count++;
            continue;
        }
        if (result != NULL) {
            *result = local_result;
        }
        return last_send_ret;
    }

    if (result != NULL) {
        *result = local_result;
    }
    if (local_result.sent_count > 0u) {
        return 0;
    }
    return last_send_ret < 0 ? last_send_ret : -EAGAIN;
}
