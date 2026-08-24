#include "app_mesh_flood.h"

#include <errno.h>

static bool flood_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t flood_age_add(uint32_t age_ms, uint32_t elapsed_ms)
{
    return UINT32_MAX - age_ms < elapsed_ms ? UINT32_MAX :
           age_ms + elapsed_ms;
}

static void flood_count_saturating_increment(uint8_t *count)
{
    if (*count < UINT8_MAX) {
        (*count)++;
    }
}

static bool flood_progress_timed_out(
    const struct app_mesh_flood_progress *progress,
    uint32_t now_ms)
{
    return progress->absolute_deadline_valid &&
           flood_deadline_reached(now_ms, progress->absolute_deadline_ms);
}

static uint32_t flood_clamp_due_to_deadline(
    const struct app_mesh_flood_progress *progress,
    uint32_t due_ms)
{
    if (progress->absolute_deadline_valid &&
        flood_deadline_reached(due_ms, progress->absolute_deadline_ms)) {
        return progress->absolute_deadline_ms;
    }
    return due_ms;
}

static int flood_prepare_attempt(const struct mesh_outbound *base,
                                 uint32_t age_origin_ms,
                                 uint32_t now_ms,
                                 struct mesh_outbound *attempt)
{
    uint32_t elapsed_ms = now_ms - age_origin_ms;
    *attempt = *base;
    attempt->packet.message_age_ms = flood_age_add(
        base->packet.message_age_ms, elapsed_ms);
    /* The lower radio sender may account for a final bounded handoff delay. */
    attempt->queued_at_ms = now_ms;
    attempt->queued_at_valid = true;
    return 0;
}

uint8_t app_mesh_flood_repeat_limit(void)
{
    return FLOOD_RELAY_REPEAT_COUNT;
}

uint32_t app_mesh_flood_backoff_ms(uint8_t retry_index, uint32_t random_value)
{
    uint32_t base_ms = C5_POLITE_BACKOFF_MIN_MS;
    uint32_t jitter_window_ms;

    for (uint8_t i = 0u; i < retry_index; i++) {
        if (base_ms >= C5_POLITE_BACKOFF_MAX_MS / 2u) {
            base_ms = C5_POLITE_BACKOFF_MAX_MS;
            break;
        }
        base_ms *= 2u;
    }
    if (base_ms > C5_POLITE_BACKOFF_MAX_MS) {
        base_ms = C5_POLITE_BACKOFF_MAX_MS;
    }

    jitter_window_ms = C5_POLITE_BACKOFF_MAX_MS - base_ms;
    if (jitter_window_ms >= base_ms) {
        jitter_window_ms = base_ms - 1u;
    }
    if (jitter_window_ms == 0u) {
        return base_ms;
    }
    return base_ms + (random_value % (jitter_window_ms + 1u));
}

static bool flood_destination_valid(const struct mesh_outbound *out)
{
    bool broadcast;
    bool targeted_command;

    if (out == NULL) {
        return false;
    }

    broadcast = out->packet.dst_id == MESH_BROADCAST_ID &&
                out->next_hop_id == MESH_BROADCAST_ID;
    targeted_command = out->packet.dst_id != 0u &&
                       out->packet.dst_id != MESH_BROADCAST_ID &&
                       out->next_hop_id != 0u &&
                       out->next_hop_id != MESH_BROADCAST_ID &&
                       out->packet.msg_type == MSG_COMMAND;
    return broadcast || targeted_command;
}

static bool flood_args_valid(const struct mesh_outbound *out,
                             const struct app_mesh_flood_ops *ops)
{
    return out != NULL && ops != NULL &&
           ops->now_ms != NULL &&
           ops->sleep_until_ms != NULL &&
           ops->defer_active != NULL &&
           ops->c5_quiet != NULL &&
           ops->random_u32 != NULL &&
           ops->send != NULL &&
           flood_destination_valid(out) &&
           out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD &&
           out->packet.msg_type != MSG_ROUTE_REQ &&
           FLOOD_RELAY_REPEAT_MS != 0u;
}

static int app_mesh_flood_send_resume_limit(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_progress *progress,
    struct app_mesh_flood_result *result,
    uint8_t repeat_limit)
{
    struct mesh_outbound attempt_tx;
    int send_ret;

    if (!flood_args_valid(out, ops) || progress == NULL ||
        repeat_limit == 0u ||
        repeat_limit > app_mesh_flood_repeat_limit()) {
        return -EINVAL;
    }
    if (!progress->initialized) {
        progress->due_ms = out->earliest_tx_valid ?
                           out->earliest_tx_ms : ops->now_ms(ops->ctx);
        progress->age_origin_ms = out->queued_at_valid ?
                                  out->queued_at_ms : progress->due_ms;
        progress->absolute_deadline_ms = ops->absolute_deadline_ms;
        progress->absolute_deadline_valid = ops->absolute_deadline_valid;
        progress->result.first_due_ms = progress->due_ms;
        progress->initialized = true;
    }
    if (progress->complete) {
        if (result != NULL) {
            *result = progress->result;
        }
        return progress->result.sent_count > 0u ? 0 : -EAGAIN;
    }
    if (flood_progress_timed_out(progress, ops->now_ms(ops->ctx))) {
        if (result != NULL) {
            *result = progress->result;
        }
        return -ETIMEDOUT;
    }

    while (progress->next_opportunity < repeat_limit) {
        progress->result.last_due_ms = progress->due_ms;
        if (ops->defer_active(ops->ctx)) {
            flood_count_saturating_increment(
                &progress->result.deferred_count);
            if (result != NULL) {
                *result = progress->result;
            }
            return -EAGAIN;
        }
        if (!flood_deadline_reached(ops->now_ms(ops->ctx), progress->due_ms)) {
            if (progress->absolute_deadline_valid &&
                flood_deadline_reached(progress->due_ms,
                                       progress->absolute_deadline_ms)) {
                ops->sleep_until_ms(progress->absolute_deadline_ms,
                                    ops->ctx);
                if (result != NULL) {
                    *result = progress->result;
                }
                return -ETIMEDOUT;
            }
            ops->sleep_until_ms(progress->due_ms, ops->ctx);
        }
        if (flood_progress_timed_out(progress, ops->now_ms(ops->ctx))) {
            if (result != NULL) {
                *result = progress->result;
            }
            return -ETIMEDOUT;
        }
        if (ops->defer_active(ops->ctx)) {
            flood_count_saturating_increment(
                &progress->result.deferred_count);
            if (result != NULL) {
                *result = progress->result;
            }
            return -EAGAIN;
        }
        if (!ops->c5_quiet(C5_POLITE_SNIFF_MS, ops->ctx)) {
            flood_count_saturating_increment(
                &progress->result.busy_skip_count);
            progress->due_ms = flood_clamp_due_to_deadline(
                progress, ops->now_ms(ops->ctx) +
                    app_mesh_flood_backoff_ms(progress->backoff_index,
                                              ops->random_u32(ops->ctx)));
            if (progress->backoff_index < UINT8_MAX) {
                progress->backoff_index++;
            }
            if (result != NULL) {
                *result = progress->result;
            }
            return -EAGAIN;
        }

        send_ret = flood_prepare_attempt(
            out, progress->age_origin_ms, ops->now_ms(ops->ctx), &attempt_tx);
        if (send_ret < 0) {
            if (result != NULL) {
                *result = progress->result;
            }
            return send_ret;
        }
        if (ops->defer_active(ops->ctx)) {
            flood_count_saturating_increment(
                &progress->result.deferred_count);
            if (result != NULL) {
                *result = progress->result;
            }
            return -EAGAIN;
        }
        if (flood_progress_timed_out(progress, ops->now_ms(ops->ctx))) {
            if (result != NULL) {
                *result = progress->result;
            }
            return -ETIMEDOUT;
        }
        send_ret = ops->send(&attempt_tx, ops->ctx);
        if (send_ret == 0) {
            progress->next_opportunity++;
            flood_count_saturating_increment(&progress->result.sent_count);
            progress->backoff_index = 0u;
            progress->due_ms = flood_clamp_due_to_deadline(
                progress,
                ops->now_ms(ops->ctx) + FLOOD_RELAY_REPEAT_MS);
            continue;
        }
        if (send_ret == -EBUSY || send_ret == -EAGAIN) {
            flood_count_saturating_increment(
                &progress->result.busy_skip_count);
            progress->due_ms = flood_clamp_due_to_deadline(
                progress, ops->now_ms(ops->ctx) +
                    app_mesh_flood_backoff_ms(progress->backoff_index,
                                              ops->random_u32(ops->ctx)));
            if (progress->backoff_index < UINT8_MAX) {
                progress->backoff_index++;
            }
            if (result != NULL) {
                *result = progress->result;
            }
            return -EAGAIN;
        }
        if (result != NULL) {
            *result = progress->result;
        }
        return send_ret;
    }

    progress->complete = true;
    if (result != NULL) {
        *result = progress->result;
    }
    if (progress->result.sent_count > 0u) {
        return 0;
    }
    return -EAGAIN;
}

int app_mesh_flood_send_bounded_resume(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_progress *progress,
    struct app_mesh_flood_result *result)
{
    return app_mesh_flood_send_resume_limit(
        out, ops, progress, result, app_mesh_flood_repeat_limit());
}

int app_mesh_flood_send_opportunity(
    const struct mesh_outbound *out,
    const struct app_mesh_flood_ops *ops,
    struct app_mesh_flood_result *result)
{
    struct app_mesh_flood_progress progress = {0};

    return app_mesh_flood_send_resume_limit(out, ops, &progress, result, 1u);
}

void app_mesh_flood_progress_rebase(struct app_mesh_flood_progress *progress,
                                    uint32_t paused_ms)
{
    if (progress == NULL || !progress->initialized || progress->complete) {
        return;
    }
    /* Wrap is intentional: all scheduled intervals are below INT32_MAX. */
    progress->due_ms += paused_ms;
}

int app_mesh_flood_send_bounded(const struct mesh_outbound *out,
                                const struct app_mesh_flood_ops *ops,
                                struct app_mesh_flood_result *result)
{
    struct app_mesh_flood_progress progress = {0};
    int ret = app_mesh_flood_send_bounded_resume(out, ops, &progress, result);

    /* Preserve the legacy one-shot contract for existing flood callers. */
    if (ret == -EAGAIN && progress.result.sent_count > 0u) {
        return 0;
    }
    return ret;
}
