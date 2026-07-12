#include "mesh_runtime.h"

#include "protocol.h"

#include <limits.h>
#include <string.h>

static uint8_t work_priority(enum mesh_runtime_work_kind kind)
{
    switch (kind) {
    case MESH_RUNTIME_WORK_GATEWAY_COMMAND:
        return 0u;
    case MESH_RUNTIME_WORK_LOCAL_CLICK:
        return 1u;
    case MESH_RUNTIME_WORK_EVENT_REPAIR:
        return 2u;
    case MESH_RUNTIME_WORK_TRANSIT:
        return 3u;
    default:
        return UINT8_MAX;
    }
}

static int work_index(const struct mesh_runtime *runtime,
                      enum mesh_runtime_work_kind kind,
                      uint64_t token)
{
    for (size_t i = 0u; i < MESH_RUNTIME_WORK_CAPACITY; i++) {
        if (runtime->work[i].valid && runtime->work[i].kind == kind &&
            runtime->work[i].token == token) {
            return (int)i;
        }
    }
    return -1;
}

static int free_work_index(const struct mesh_runtime *runtime)
{
    for (size_t i = 0u; i < MESH_RUNTIME_WORK_CAPACITY; i++) {
        if (!runtime->work[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

static int next_ready_index(const struct mesh_runtime *runtime, uint64_t now_us)
{
    int best = -1;

    for (size_t i = 0u; i < MESH_RUNTIME_WORK_CAPACITY; i++) {
        const struct mesh_runtime_work *candidate = &runtime->work[i];
        const struct mesh_runtime_work *selected;
        uint8_t candidate_priority;
        uint8_t selected_priority;

        if (!candidate->valid || candidate->ready_us > now_us) {
            continue;
        }
        if (best < 0) {
            best = (int)i;
            continue;
        }
        selected = &runtime->work[best];
        candidate_priority = work_priority(candidate->kind);
        selected_priority = work_priority(selected->kind);
        if (candidate_priority < selected_priority ||
            (candidate_priority == selected_priority &&
             candidate->enqueue_order < selected->enqueue_order)) {
            best = (int)i;
        }
    }
    return best;
}

static int schedule_work(const struct mesh_runtime *runtime,
                         enum mesh_runtime_work_kind kind,
                         uint64_t token,
                         uint64_t at_us)
{
    return runtime->ops.schedule == NULL ? MESH_RUNTIME_OK :
           runtime->ops.schedule(kind, token, at_us, runtime->ops.ctx);
}

static void trace_action(const struct mesh_runtime *runtime,
                         const struct mesh_runtime_action *action,
                         uint64_t at_us)
{
    if (runtime->ops.trace != NULL) {
        runtime->ops.trace(action->kind,
                           action->token,
                           at_us,
                           runtime->ops.ctx);
    }
}

void mesh_runtime_init(struct mesh_runtime *runtime,
                       struct mesh_relay *relay,
                       uint64_t local_id,
                       const struct mesh_runtime_ops *ops)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->relay = relay;
    runtime->local_id = local_id;
    if (ops != NULL) {
        runtime->ops = *ops;
    }
}

int mesh_runtime_submit(struct mesh_runtime *runtime,
                        enum mesh_runtime_work_kind kind,
                        uint64_t token,
                        uint64_t ready_us)
{
    int index;

    if (runtime == NULL || work_priority(kind) == UINT8_MAX) {
        return MESH_RUNTIME_ERR_ARG;
    }
    index = work_index(runtime, kind, token);
    if (index >= 0) {
        if (ready_us < runtime->work[index].ready_us) {
            runtime->work[index].ready_us = ready_us;
        }
        return schedule_work(runtime,
                             kind,
                             token,
                             runtime->work[index].ready_us);
    }
    index = free_work_index(runtime);
    if (index < 0) {
        return MESH_RUNTIME_ERR_CAPACITY;
    }
    runtime->next_enqueue_order++;
    if (runtime->next_enqueue_order == 0u) {
        runtime->next_enqueue_order = 1u;
    }
    runtime->work[index] = (struct mesh_runtime_work) {
        .ready_us = ready_us,
        .token = token,
        .enqueue_order = runtime->next_enqueue_order,
        .kind = kind,
        .valid = true,
    };
    return schedule_work(runtime, kind, token, ready_us);
}

int mesh_runtime_reserve_transit(struct mesh_runtime *runtime,
                                 const struct mesh_outbound *outbound,
                                 uint64_t ready_us)
{
    int ret;

    if (runtime == NULL || outbound == NULL || runtime->transit_reserved) {
        return MESH_RUNTIME_ERR_ARG;
    }
    runtime->transit = *outbound;
    runtime->transit_reserved = true;
    ret = mesh_runtime_submit(runtime,
                              MESH_RUNTIME_WORK_TRANSIT,
                              outbound->packet.seq,
                              ready_us);
    if (ret != MESH_RUNTIME_OK) {
        runtime->transit_reserved = false;
    }
    return ret;
}

int mesh_runtime_claim_radio(struct mesh_runtime *runtime,
                             enum mesh_runtime_radio_owner owner,
                             uint64_t start_us,
                             uint64_t end_us)
{
    if (runtime == NULL || owner == MESH_RUNTIME_RADIO_NONE ||
        end_us <= start_us) {
        return MESH_RUNTIME_ERR_ARG;
    }
    if (!mesh_runtime_radio_safe(runtime, start_us)) {
        return MESH_RUNTIME_ERR_RADIO_BUSY;
    }
    runtime->radio_owner = owner;
    runtime->radio_busy_until_us = end_us;
    return MESH_RUNTIME_OK;
}

int mesh_runtime_release_radio(struct mesh_runtime *runtime,
                               enum mesh_runtime_radio_owner owner,
                               uint64_t now_us)
{
    if (runtime == NULL || owner == MESH_RUNTIME_RADIO_NONE ||
        runtime->radio_owner != owner || now_us < runtime->radio_busy_until_us) {
        return MESH_RUNTIME_ERR_STATE;
    }
    runtime->radio_owner = MESH_RUNTIME_RADIO_NONE;
    runtime->radio_busy_until_us = now_us;
    return MESH_RUNTIME_OK;
}

bool mesh_runtime_radio_safe(const struct mesh_runtime *runtime,
                             uint64_t now_us)
{
    return runtime != NULL &&
           (runtime->radio_owner == MESH_RUNTIME_RADIO_NONE ||
            now_us >= runtime->radio_busy_until_us);
}

int mesh_runtime_run_boundary(struct mesh_runtime *runtime,
                              uint64_t now_us,
                              struct mesh_runtime_action *action)
{
    struct mesh_runtime_work selected;
    int index;
    int ret;

    if (runtime == NULL || action == NULL) {
        return MESH_RUNTIME_ERR_ARG;
    }
    memset(action, 0, sizeof(*action));
    if (!mesh_runtime_radio_safe(runtime, now_us)) {
        action->kind = MESH_RUNTIME_ACTION_WAIT_SAFE_BOUNDARY;
        action->runnable_at_us = runtime->radio_busy_until_us;
        return MESH_RUNTIME_OK;
    }
    runtime->radio_owner = MESH_RUNTIME_RADIO_NONE;
    runtime->radio_busy_until_us = now_us;
    index = next_ready_index(runtime, now_us);
    if (index < 0) {
        return MESH_RUNTIME_OK;
    }
    selected = runtime->work[index];
    memset(&runtime->work[index], 0, sizeof(runtime->work[index]));
    action->token = selected.token;
    action->runnable_at_us = now_us;

    switch (selected.kind) {
    case MESH_RUNTIME_WORK_GATEWAY_COMMAND:
        action->kind = MESH_RUNTIME_ACTION_RUN_GATEWAY_COMMAND;
        break;
    case MESH_RUNTIME_WORK_LOCAL_CLICK:
        action->kind = MESH_RUNTIME_ACTION_START_LOCAL_CLICK;
        if (runtime->relay != NULL) {
            ret = mesh_prepare_click_preemption(runtime->relay,
                                                runtime->local_id,
                                                (uint32_t)(now_us / 1000u),
                                                &action->click_preemption);
            if (ret != PROTO_OK) {
                return ret;
            }
            mesh_relay_abandon_transit_reservations(runtime->relay);
        }
        if (runtime->transit_reserved) {
            int transit_index = work_index(runtime,
                                           MESH_RUNTIME_WORK_TRANSIT,
                                           runtime->transit.packet.seq);

            if (transit_index >= 0) {
                memset(&runtime->work[transit_index],
                       0,
                       sizeof(runtime->work[transit_index]));
            }
            runtime->transit_reserved = false;
            runtime->transit_abandon_count++;
            action->transit_reservation_abandoned = true;
        }
        break;
    case MESH_RUNTIME_WORK_EVENT_REPAIR:
        action->kind = MESH_RUNTIME_ACTION_REPAIR_SELECTED_EVENT;
        action->peer_id = runtime->selected_repair_peer_id;
        break;
    case MESH_RUNTIME_WORK_TRANSIT:
        action->kind = MESH_RUNTIME_ACTION_RUN_TRANSIT;
        runtime->transit_reserved = false;
        break;
    default:
        return MESH_RUNTIME_ERR_STATE;
    }
    trace_action(runtime, action, now_us);
    return MESH_RUNTIME_OK;
}

int mesh_runtime_handle_ack_timeout(struct mesh_runtime *runtime,
                                    uint32_t now_ms,
                                    uint32_t random_value,
                                    struct mesh_relay_result *core_result)
{
    const struct route_candidate *selected;
    uint64_t selected_peer;
    int ret;

    if (runtime == NULL || runtime->relay == NULL || core_result == NULL) {
        return MESH_RUNTIME_ERR_ARG;
    }
    ret = mesh_relay_tick_with_random(runtime->relay,
                                      now_ms,
                                      random_value,
                                      core_result);
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((core_result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        runtime->route_discovery_count++;
    }

    selected = route_selected(&runtime->relay->upstream);
    if (runtime->relay->pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
        selected == NULL || selected->channel9_timing_valid) {
        return MESH_RUNTIME_OK;
    }
    selected_peer = selected->next_hop_id;
    if (selected_peer == 0u || selected_peer == runtime->local_id) {
        return MESH_RUNTIME_ERR_STATE;
    }
    runtime->selected_repair_peer_id = selected_peer;
    runtime->repair_requested_us = (uint64_t)now_ms * 1000u;
    runtime->repair_original_retry_after_ms =
        runtime->relay->pending.retry_after_ms;
    runtime->event_repair_pending = true;
    return mesh_runtime_submit(runtime,
                               MESH_RUNTIME_WORK_EVENT_REPAIR,
                               selected_peer,
                               runtime->repair_requested_us);
}

int mesh_runtime_complete_event_repair(struct mesh_runtime *runtime,
                                       const struct mesh_event_timing *timing,
                                       uint32_t now_ms)
{
    int ret;

    if (runtime == NULL || runtime->relay == NULL || timing == NULL ||
        !runtime->event_repair_pending ||
        runtime->selected_repair_peer_id == 0u) {
        return MESH_RUNTIME_ERR_ARG;
    }
    ret = mesh_relay_set_channel9_timing(runtime->relay,
                                         runtime->selected_repair_peer_id,
                                         timing);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_defer_pending_retry(runtime->relay, now_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    runtime->event_repair_pending = false;
    runtime->selected_repair_peer_id = 0u;
    return MESH_RUNTIME_OK;
}
