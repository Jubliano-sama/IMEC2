#include "app_node_comm_gateway_route_refresh.h"
#include "app_node_comm_sync.h"

#include <zephyr/kernel.h>

#include <errno.h>
#include <string.h>

#define ROUTE_REFRESH_PRE_RF_RETRY_MS 10u
#define ROUTE_REFRESH_MAX_OUTER_RETRIES 8u
#define ROUTE_REFRESH_RETRY_BASE_MS 100u
#define ROUTE_REFRESH_RETRY_MAX_MS 5000u
#define ROUTE_REFRESH_PROTOCOL_DEADLINE_MS \
    APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS
struct route_refresh_state {
    const struct app_node_comm_gateway_route_refresh_config *config;
    struct mesh_gateway_route_adv_snapshot snapshot;
    struct app_mesh_flood_progress flood;
    struct proto_packet correlation;
    uint32_t sequence;
    uint32_t due_ms;
    uint32_t response_due_ms;
    uint32_t absolute_deadline_ms;
    uint32_t paused_at_ms;
    uint32_t operation_generation;
    uint8_t retry_round;
    uint8_t outer_sent_count;
    uint8_t burst_index;
    uint8_t burst_count;
    uint8_t active : 1;
    uint8_t forced : 1;
    uint8_t correlated : 1;
    uint8_t paused : 1;
    uint8_t scheduled : 1;
    uint8_t wake_sent : 1;
    uint8_t outer_sent : 1;
    uint8_t in_flight : 1;
    uint8_t resume_pending : 1;
    uint8_t absolute_deadline_valid : 1;
    uint8_t response_due_valid : 1;
};

struct route_refresh_operation {
    const struct app_node_comm_gateway_route_refresh_config *config;
    struct mesh_gateway_route_adv_snapshot snapshot;
    struct app_mesh_flood_progress flood;
    uint32_t generation;
    uint32_t sequence;
    uint32_t response_due_ms;
    uint32_t absolute_deadline_ms;
    uint8_t outer_sent_count;
    uint8_t burst_index;
    uint8_t burst_count;
    bool wake_sent;
    bool outer_sent;
    bool absolute_deadline_valid;
    bool response_due_valid;
};

static struct route_refresh_state route_refresh;
static struct k_work_delayable route_refresh_work;
static atomic_t route_refresh_pause_intent;

static bool refresh_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t refresh_deadline_add(uint32_t now_ms, uint32_t delay_ms)
{
    /* All route-refresh intervals are bounded below INT32_MAX. */
    return now_ms + delay_ms;
}

static uint32_t refresh_wait_ms(uint32_t now_ms, uint32_t deadline_ms)
{
    return refresh_deadline_reached(now_ms, deadline_ms) ? 0u :
           deadline_ms - now_ms;
}

static int refresh_operation_next_sequence(
    struct route_refresh_operation *operation)
{
    uint32_t sequence = 0u;
    int ret;

    if (operation == NULL || operation->config == NULL) {
        return -EINVAL;
    }
    if (operation->config->next_sequence == NULL) {
        return -ENOTSUP;
    }
    ret = operation->config->next_sequence(operation->config->ctx, &sequence);
    if (ret < 0) {
        return ret;
    }
    if (sequence == 0u) {
        return -EILSEQ;
    }
    operation->sequence = sequence;
    return 0;
}

static int refresh_schedule(uint32_t delay_ms)
{
    const struct app_node_comm_gateway_route_refresh_config *config;
    uint32_t generation;
    uint32_t now_ms;
    uint32_t due_ms;
    int ret;

    if (route_refresh.config == NULL ||
        !route_refresh.config->gateway_role ||
        route_refresh.config->schedule == NULL) {
        return -EINVAL;
    }
    config = route_refresh.config;
    generation = route_refresh.operation_generation;
    app_node_comm_sync_unlock();
    now_ms = config->now_ms == NULL ? 0u : config->now_ms(config->ctx);
    (void)app_node_comm_sync_lock();
    if (route_refresh.config != config ||
        route_refresh.operation_generation != generation ||
        route_refresh.paused) {
        return -ECANCELED;
    }
    if (route_refresh.absolute_deadline_valid) {
        uint32_t remaining_ms = refresh_wait_ms(
            now_ms, route_refresh.absolute_deadline_ms);

        if (delay_ms > remaining_ms) {
            delay_ms = remaining_ms;
        }
    }
    due_ms = refresh_deadline_add(now_ms, delay_ms);
    route_refresh.due_ms = due_ms;
    route_refresh.scheduled = true;
    app_node_comm_sync_unlock();
    ret = config->schedule(config->ctx, &route_refresh_work, delay_ms);
    (void)app_node_comm_sync_lock();

    if (ret < 0 && route_refresh.config == config &&
        route_refresh.operation_generation == generation &&
        route_refresh.due_ms == due_ms) {
        route_refresh.scheduled = false;
    }
    return ret;
}

uint32_t app_node_comm_gateway_route_refresh_response_priority_wait_ms(
    uint32_t now_ms)
{
    const struct app_node_comm_gateway_route_refresh_config *config;
    uint32_t response_due_ms;
    bool response_due_valid;
    uint32_t wait_ms = 0u;

    if (app_node_comm_sync_lock() < 0) {
        return 0u;
    }
    config = route_refresh.config;
    response_due_ms = route_refresh.response_due_ms;
    response_due_valid = route_refresh.response_due_valid;
    app_node_comm_sync_unlock();
    if (config != NULL && config->response_priority_active != NULL &&
        response_due_valid &&
        config->response_priority_active(now_ms, config->ctx)) {
        wait_ms = refresh_wait_ms(now_ms, response_due_ms);
    }
    return wait_ms;
}

bool app_node_comm_gateway_route_refresh_response_priority_due(uint32_t now_ms)
{
    const struct app_node_comm_gateway_route_refresh_config *config;
    uint32_t response_due_ms;
    bool response_due_valid;

    if (app_node_comm_sync_lock() < 0) {
        return false;
    }
    config = route_refresh.config;
    response_due_ms = route_refresh.response_due_ms;
    response_due_valid = route_refresh.response_due_valid;
    app_node_comm_sync_unlock();
    return config != NULL && config->response_priority_active != NULL &&
           config->response_priority_active(now_ms, config->ctx) &&
           (!response_due_valid ||
            refresh_wait_ms(now_ms, response_due_ms) == 0u);
}

static uint32_t refresh_retry_delay_ms(uint8_t retry_round,
                                       uint32_t random_value)
{
    uint32_t base_ms = ROUTE_REFRESH_RETRY_BASE_MS;

    for (uint8_t i = 0u; i < retry_round; i++) {
        if (base_ms >= ROUTE_REFRESH_RETRY_MAX_MS / 2u) {
            base_ms = ROUTE_REFRESH_RETRY_MAX_MS;
            break;
        }
        base_ms *= 2u;
    }
    return base_ms + (random_value % base_ms);
}

static bool refresh_event_prepare_locked(
    enum app_node_comm_route_refresh_event_kind kind,
    uint8_t sent_count,
    int result,
    const struct app_node_comm_gateway_route_refresh_config **config_out,
    struct app_node_comm_route_refresh_event *event_out)
{
    if (config_out == NULL || event_out == NULL) {
        return false;
    }
    *config_out = route_refresh.config;
    *event_out = (struct app_node_comm_route_refresh_event) {
        .kind = kind,
        .correlation = route_refresh.correlation,
        .gateway_sequence = route_refresh.sequence,
        .attempt = (uint8_t)(route_refresh.retry_round + 1u),
        .sent_count = sent_count,
        .result = result,
        .correlated = route_refresh.correlated,
    };
    return route_refresh.correlated && *config_out != NULL &&
           (*config_out)->observe != NULL;
}

static void refresh_reset_outer_attempt(void)
{
    memset(&route_refresh.snapshot, 0, sizeof(route_refresh.snapshot));
    memset(&route_refresh.flood, 0, sizeof(route_refresh.flood));
    route_refresh.wake_sent = false;
    route_refresh.outer_sent = false;
    route_refresh.outer_sent_count = 0u;
    route_refresh.burst_index = 0u;
    route_refresh.burst_count = 0u;
}

static void refresh_complete(int result)
{
    const struct app_node_comm_gateway_route_refresh_config *config =
        route_refresh.config;
    struct app_node_comm_route_refresh_event event = {
        .kind = APP_NODE_COMM_ROUTE_REFRESH_COMPLETE,
        .correlation = route_refresh.correlation,
        .gateway_sequence = route_refresh.sequence,
        .attempt = (uint8_t)(route_refresh.retry_round + 1u),
        .sent_count = route_refresh.outer_sent_count,
        .result = result,
        .correlated = route_refresh.correlated,
    };
    bool observe = route_refresh.forced && route_refresh.correlated &&
                   config != NULL && config->observe != NULL;

    route_refresh.active = false;
    route_refresh.forced = false;
    route_refresh.correlated = false;
    route_refresh.absolute_deadline_ms = 0u;
    route_refresh.absolute_deadline_valid = false;
    route_refresh.due_ms = 0u;
    route_refresh.scheduled = false;
    refresh_reset_outer_attempt();

    app_node_comm_sync_unlock();
    if (observe) {
        config->observe(config->ctx, &event);
    }
    (void)app_node_comm_sync_lock();
}

static bool refresh_failure_retryable(int ret)
{
    return ret == -EAGAIN || ret == -EBUSY || ret == -EIO ||
           ret == -ETIMEDOUT || ret == -ECANCELED;
}

static void refresh_retry_or_complete(int ret)
{
    const struct app_node_comm_gateway_route_refresh_config *config;
    struct app_node_comm_route_refresh_event event;
    uint32_t generation = route_refresh.operation_generation;

    if (route_refresh.forced && refresh_failure_retryable(ret) &&
        route_refresh.retry_round < ROUTE_REFRESH_MAX_OUTER_RETRIES) {
        uint8_t retry_round = route_refresh.retry_round;
        uint32_t random_value;
        uint32_t retry_ms;
        bool observe;
        int schedule_ret;

        config = route_refresh.config;
        app_node_comm_sync_unlock();
        random_value = config == NULL || config->random_u32 == NULL ? 0u :
                       config->random_u32(config->ctx);
        (void)app_node_comm_sync_lock();
        if (route_refresh.operation_generation != generation ||
            route_refresh.paused || route_refresh.config != config ||
            route_refresh.retry_round != retry_round) {
            return;
        }
        retry_ms = refresh_retry_delay_ms(retry_round, random_value);
        observe = refresh_event_prepare_locked(
            APP_NODE_COMM_ROUTE_REFRESH_BACKOFF, 0u, ret, &config, &event);
        app_node_comm_sync_unlock();
        if (observe) {
            config->observe(config->ctx, &event);
        }
        (void)app_node_comm_sync_lock();
        if (route_refresh.operation_generation != generation ||
            route_refresh.paused || route_refresh.config != config ||
            route_refresh.retry_round != retry_round) {
            return;
        }
        route_refresh.retry_round++;
        refresh_reset_outer_attempt();
        schedule_ret = refresh_schedule(retry_ms);
        if (schedule_ret < 0 &&
            route_refresh.operation_generation == generation &&
            !route_refresh.paused) {
            refresh_complete(schedule_ret);
        }
        return;
    }
    refresh_complete(ret == -EAGAIN || ret == -EBUSY ? -ETIMEDOUT : ret);
}

static uint32_t refresh_flood_now(void *ctx)
{
    const struct route_refresh_operation *operation = ctx;

    return operation == NULL || operation->config == NULL ||
           operation->config->now_ms == NULL ? 0u :
           operation->config->now_ms(operation->config->ctx);
}

static void refresh_flood_sleep(uint32_t due_ms, void *ctx)
{
    const struct route_refresh_operation *operation = ctx;

    if (operation != NULL && operation->config != NULL &&
        operation->config->sleep_until_ms != NULL) {
        operation->config->sleep_until_ms(due_ms,
                                          operation->config->ctx);
    }
}

static bool refresh_flood_defer(void *ctx)
{
    const struct route_refresh_operation *operation = ctx;

    if (atomic_get(&route_refresh_pause_intent) != 0 ||
        operation == NULL || operation->config == NULL ||
        (operation->config->policy_running != NULL &&
         !operation->config->policy_running(operation->config->ctx))) {
        return true;
    }
    return operation->config->defer_active != NULL &&
           operation->config->defer_active(operation->config->ctx);
}

static bool refresh_flood_quiet(uint32_t sniff_ms, void *ctx)
{
    const struct route_refresh_operation *operation = ctx;

    return operation != NULL && operation->config != NULL &&
           operation->config->c5_quiet != NULL &&
           operation->config->c5_quiet(sniff_ms,
                                       operation->config->ctx);
}

static uint32_t refresh_flood_random(void *ctx)
{
    const struct route_refresh_operation *operation = ctx;

    return operation == NULL || operation->config == NULL ||
           operation->config->random_u32 == NULL ? 0u :
           operation->config->random_u32(operation->config->ctx);
}

static int refresh_flood_send(const struct mesh_outbound *out, void *ctx)
{
    const struct route_refresh_operation *operation = ctx;

    return operation == NULL || operation->config == NULL ||
           operation->config->send == NULL ?
           -ENOTSUP :
           operation->config->send(out, operation->config->ctx);
}

static int refresh_prepare_outer(struct route_refresh_operation *operation,
                                 uint32_t now_ms,
                                 struct mesh_outbound *outbound)
{
    bool first_build;
    int ret;

    if (operation == NULL || operation->config == NULL) {
        return -EINVAL;
    }
    first_build = !operation->snapshot.valid;
    if (first_build) {
        ret = refresh_operation_next_sequence(operation);
        if (ret < 0) {
            return ret;
        }
    }
    ret = operation->config->build == NULL ?
          -ENOTSUP :
          operation->config->build(operation->config->ctx,
                                   operation->sequence,
                                   now_ms,
                                   &operation->snapshot,
                                   outbound);
    if (ret < 0 || !operation->snapshot.valid) {
        if (first_build) {
            memset(&operation->snapshot, 0,
                   sizeof(operation->snapshot));
        }
        return ret < 0 ? ret : -EIO;
    }
    if (first_build) {
        operation->burst_count = (uint8_t)(1u +
            outbound->flood_retry_count);
    } else if (outbound->packet.session_id != operation->sequence ||
               outbound->packet.seq != operation->snapshot.packet_seq ||
               outbound->queued_at_ms != operation->snapshot.queued_at_ms) {
        return -EIO;
    }
    if (outbound->payload_len != MESH_GATEWAY_ROUTE_ADV_PAYLOAD_LEN &&
        outbound->payload_len != MESH_GATEWAY_ROUTE_ADV_POLICY_PAYLOAD_LEN) {
        return -EMSGSIZE;
    }
    if (outbound->packet.message_age_ms != 0u) {
        return -EIO;
    }
    return 0;
}

static void refresh_operation_copy_locked(
    struct route_refresh_operation *operation)
{
    operation->config = route_refresh.config;
    operation->snapshot = route_refresh.snapshot;
    operation->flood = route_refresh.flood;
    operation->generation = route_refresh.operation_generation;
    operation->sequence = route_refresh.sequence;
    operation->response_due_ms = route_refresh.response_due_ms;
    operation->response_due_valid = route_refresh.response_due_valid;
    operation->absolute_deadline_ms = route_refresh.absolute_deadline_ms;
    operation->outer_sent_count = route_refresh.outer_sent_count;
    operation->burst_index = route_refresh.burst_index;
    operation->burst_count = route_refresh.burst_count;
    operation->wake_sent = route_refresh.wake_sent;
    operation->outer_sent = route_refresh.outer_sent;
    operation->absolute_deadline_valid =
        route_refresh.absolute_deadline_valid;
}

static void refresh_operation_commit_locked(
    const struct route_refresh_operation *operation)
{
    route_refresh.snapshot = operation->snapshot;
    route_refresh.flood = operation->flood;
    route_refresh.sequence = operation->sequence;
    route_refresh.outer_sent_count = operation->outer_sent_count;
    route_refresh.burst_index = operation->burst_index;
    route_refresh.burst_count = operation->burst_count;
    route_refresh.wake_sent = operation->wake_sent;
    route_refresh.outer_sent = operation->outer_sent;
}

static bool refresh_operation_pause_requested(void)
{
    return atomic_get(&route_refresh_pause_intent) != 0;
}

static void refresh_work_handler(struct k_work *work)
{
    struct route_refresh_operation operation = {0};
    struct app_mesh_flood_ops flood_ops;
    struct mesh_outbound outbound;
    const struct app_node_comm_gateway_route_refresh_config *config;
    bool allowed;
    bool radio_control_started = false;
    bool policy_running;
    bool restart_scan = false;
    bool resume_pending = false;
    bool resume_flood = false;
    uint32_t now_ms;
    int ret;

    (void)work;
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    if (!route_refresh.active || route_refresh.paused ||
        route_refresh.in_flight || route_refresh.config == NULL) {
        app_node_comm_sync_unlock();
        return;
    }
    config = route_refresh.config;
    app_node_comm_sync_unlock();

    now_ms = config->now_ms == NULL ? 0u : config->now_ms(config->ctx);
    policy_running = config->policy_running == NULL ||
                     config->policy_running(config->ctx);
    allowed = config->allowed != NULL && config->allowed(config->ctx);

    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    if (!route_refresh.active || route_refresh.paused ||
        route_refresh.in_flight || route_refresh.config != config ||
        !policy_running) {
        app_node_comm_sync_unlock();
        return;
    }
    route_refresh.due_ms = 0u;
    route_refresh.scheduled = false;
    if (route_refresh.absolute_deadline_valid &&
        refresh_deadline_reached(now_ms,
                                 route_refresh.absolute_deadline_ms)) {
        refresh_complete(-ETIMEDOUT);
        app_node_comm_sync_unlock();
        return;
    }
    if (!route_refresh.forced && !allowed) {
        route_refresh.active = false;
        route_refresh.response_due_ms = 0u;
        route_refresh.response_due_valid = false;
        app_node_comm_sync_unlock();
        return;
    }
    route_refresh.in_flight = true;
    refresh_operation_copy_locked(&operation);
    app_node_comm_sync_unlock();

    flood_ops = (struct app_mesh_flood_ops) {
        .now_ms = refresh_flood_now,
        .sleep_until_ms = refresh_flood_sleep,
        .defer_active = refresh_flood_defer,
        .c5_quiet = refresh_flood_quiet,
        .random_u32 = refresh_flood_random,
        .send = refresh_flood_send,
        .absolute_deadline_ms = operation.absolute_deadline_ms,
        .absolute_deadline_valid = operation.absolute_deadline_valid,
        .ctx = &operation,
    };
    ret = refresh_prepare_outer(&operation, now_ms, &outbound);
    if (ret < 0) {
        goto finish;
    }
    if (config->begin_radio_control != NULL) {
        ret = config->begin_radio_control(config->ctx);
        if (ret < 0) {
            goto finish;
        }
        radio_control_started = true;
    }
    if (config->stop_role_scan != NULL) {
        config->stop_role_scan(config->ctx);
        restart_scan = true;
    }

    while (operation.burst_index < operation.burst_count) {
        struct app_mesh_flood_result result = {0};

        if (refresh_operation_pause_requested()) {
            ret = -EAGAIN;
            goto finish;
        }
        if (!operation.wake_sent) {
            uint32_t wake_start_ms = outbound.earliest_tx_ms;
            uint32_t current_ms = config->now_ms == NULL ? 0u :
                                  config->now_ms(config->ctx);
            bool response_priority =
                config->response_priority_active != NULL &&
                config->response_priority_active(current_ms, config->ctx) &&
                (!operation.response_due_valid ||
                 refresh_wait_ms(current_ms,
                                 operation.response_due_ms) == 0u);

            if (!response_priority &&
                wake_start_ms > config->wake_train_ms) {
                wake_start_ms -= config->wake_train_ms;
            }
            if (!refresh_deadline_reached(current_ms, wake_start_ms)) {
                refresh_flood_sleep(wake_start_ms, &operation);
            }
            if (refresh_operation_pause_requested() ||
                (config->policy_running != NULL &&
                 !config->policy_running(config->ctx))) {
                ret = -EAGAIN;
                goto finish;
            }
            if (!response_priority) {
                ret = config->send_wake == NULL ? -ENOTSUP :
                      config->send_wake(config->ctx,
                                        "gateway-route-adv");
                if (ret < 0) {
                    goto finish;
                }
            }
            operation.wake_sent = true;
        }
        ret = app_mesh_flood_send_bounded_resume(&outbound,
                                                  &flood_ops,
                                                  &operation.flood,
                                                  &result);
        if (ret == -EAGAIN && !operation.flood.complete) {
            resume_flood = true;
            goto finish;
        }
        if (result.sent_count > 0u) {
            operation.outer_sent = true;
            operation.outer_sent_count =
                UINT8_MAX - operation.outer_sent_count < result.sent_count ?
                UINT8_MAX :
                (uint8_t)(operation.outer_sent_count + result.sent_count);
        }
        if (ret < 0 && !operation.flood.complete) {
            goto finish;
        }
        operation.burst_index++;
        operation.wake_sent = false;
        memset(&operation.flood, 0, sizeof(operation.flood));
        if (operation.burst_index < operation.burst_count) {
            outbound.earliest_tx_ms =
                refresh_deadline_add(config->now_ms == NULL ? 0u :
                                     config->now_ms(config->ctx),
                                     FLOOD_POST_ROOT_GUARD_MS);
            outbound.earliest_tx_valid = true;
        }
    }
    ret = operation.outer_sent ? 0 : (ret < 0 ? ret : -EAGAIN);

finish:
    if (radio_control_started && config->end_radio_control != NULL) {
        config->end_radio_control(config->ctx);
    }
    if (restart_scan && config->restart_role_scan != NULL) {
        config->restart_role_scan(config->ctx);
    }
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    if (route_refresh.operation_generation != operation.generation &&
        !route_refresh.paused) {
        const bool successor_active = route_refresh.active;

        route_refresh.in_flight = false;
        if (successor_active) {
            (void)refresh_schedule(0u);
            return;
        }
        app_node_comm_sync_unlock();
        return;
    }
    refresh_operation_commit_locked(&operation);
    route_refresh.in_flight = false;
    resume_pending = route_refresh.resume_pending;
    route_refresh.resume_pending = false;
    if (route_refresh.paused) {
        app_node_comm_sync_unlock();
        if (resume_pending) {
            uint32_t resume_ms = config->now_ms == NULL ? 0u :
                                 config->now_ms(config->ctx);

            app_node_comm_gateway_route_refresh_resume(resume_ms);
        }
        return;
    }
    if (resume_flood) {
        ret = refresh_schedule(ROUTE_REFRESH_PRE_RF_RETRY_MS);
        if (ret < 0) {
            refresh_retry_or_complete(ret);
        }
        app_node_comm_sync_unlock();
        return;
    }
    if (operation.outer_sent) {
        const struct app_node_comm_gateway_route_refresh_config *event_config;
        struct app_node_comm_route_refresh_event event;
        uint32_t generation = route_refresh.operation_generation;
        uint32_t sent_ms;
        bool observe;
        bool response_due;

        observe = refresh_event_prepare_locked(
            APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT,
            operation.outer_sent_count, 0, &event_config, &event);
        app_node_comm_sync_unlock();
        if (observe) {
            event_config->observe(event_config->ctx, &event);
        }
        if (app_node_comm_sync_lock() < 0) {
            return;
        }
        if (route_refresh.operation_generation != generation ||
            route_refresh.paused || route_refresh.config != config) {
            app_node_comm_sync_unlock();
            return;
        }
        app_node_comm_sync_unlock();
        sent_ms = config->now_ms == NULL ? 0u :
                  config->now_ms(config->ctx);
        if (config->note_sent != NULL) {
            config->note_sent(&outbound, sent_ms, config->ctx);
        }
        response_due =
            app_node_comm_gateway_route_refresh_response_priority_due(sent_ms);
        if (response_due && config->clear_response_priority != NULL) {
            config->clear_response_priority(config->ctx);
        }
        if (app_node_comm_sync_lock() < 0) {
            return;
        }
        if (route_refresh.operation_generation != generation ||
            route_refresh.paused) {
            app_node_comm_sync_unlock();
            return;
        }
        refresh_complete(0);
        app_node_comm_sync_unlock();
        return;
    }
    refresh_retry_or_complete(ret < 0 ? ret : -EAGAIN);
    app_node_comm_sync_unlock();
}

void app_node_comm_gateway_route_refresh_init(
    const struct app_node_comm_gateway_route_refresh_config *config,
    uint32_t sequence_seed)
{
    ARG_UNUSED(sequence_seed);
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    memset(&route_refresh, 0, sizeof(route_refresh));
    atomic_set(&route_refresh_pause_intent, 0);
    route_refresh.config = config;
    k_work_init_delayable(&route_refresh_work, refresh_work_handler);
    app_node_comm_sync_unlock();
}

static int route_refresh_request_bounded(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation,
    uint32_t timeout_ms)
{
    const struct app_node_comm_gateway_route_refresh_config *config;
    uint32_t now_ms;
    bool allowed;
    bool response_priority;
    bool stop_role_scan = false;
    bool restart_role_scan = false;
    uint32_t request_generation = 0u;
    int ret;

    if (timeout_ms == 0u || UINT32_MAX - delay_ms < timeout_ms) {
        return -EINVAL;
    }
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    if (route_refresh.config == NULL ||
        !route_refresh.config->gateway_role) {
        ret = -EINVAL;
        goto out;
    }
    config = route_refresh.config;
    if (!forced && route_refresh.forced) {
        ret = 0;
        goto out;
    }
    if (forced && route_refresh.forced && route_refresh.correlated) {
        ret = -EBUSY;
        goto out;
    }
    if (route_refresh.paused) {
        ret = -ESHUTDOWN;
        goto out;
    }
    app_node_comm_sync_unlock();
    now_ms = config->now_ms == NULL ? 0u : config->now_ms(config->ctx);
    allowed = config->allowed != NULL && config->allowed(config->ctx);
    response_priority = config->response_priority_active != NULL &&
        config->response_priority_active(now_ms, config->ctx);
    ret = app_node_comm_sync_lock();
    if (ret < 0) {
        return ret;
    }
    if (route_refresh.config != config || route_refresh.paused) {
        ret = -ECANCELED;
        goto out;
    }
    if (!forced && !allowed) {
        ret = -ENOTSUP;
        goto out;
    }
    if (forced) {
        route_refresh.operation_generation++;
        if (route_refresh.operation_generation == 0u) {
            route_refresh.operation_generation = 1u;
        }
        route_refresh.retry_round = 0u;
        route_refresh.forced = true;
        route_refresh.correlated = correlation != NULL;
        if (correlation != NULL) {
            route_refresh.correlation = *correlation;
        } else {
            memset(&route_refresh.correlation, 0,
                   sizeof(route_refresh.correlation));
        }
        route_refresh.absolute_deadline_ms = refresh_deadline_add(
            now_ms, delay_ms + timeout_ms);
        route_refresh.absolute_deadline_valid = true;
        refresh_reset_outer_attempt();
    } else if (!route_refresh.active) {
        route_refresh.absolute_deadline_ms = refresh_deadline_add(
            now_ms, delay_ms + timeout_ms);
        route_refresh.absolute_deadline_valid = true;
    }
    (void)reason;
    route_refresh.active = true;

    if (response_priority) {
        uint32_t candidate_due_ms = refresh_deadline_add(now_ms, delay_ms);

        if (route_refresh.response_due_valid &&
            (int32_t)(candidate_due_ms - route_refresh.response_due_ms) >= 0) {
            delay_ms = refresh_wait_ms(now_ms,
                                       route_refresh.response_due_ms);
        } else {
            route_refresh.response_due_ms = candidate_due_ms;
            route_refresh.response_due_valid = true;
        }
        if (delay_ms <= ROUTE_REFRESH_PRE_RF_RETRY_MS) {
            delay_ms = 0u;
            stop_role_scan = config->stop_role_scan != NULL;
        }
    }
    request_generation = route_refresh.operation_generation;
    if (stop_role_scan) {
        /*
         * Transfer scan ownership before publishing zero-delay work. A
         * scheduler is allowed to run the worker inline, so stopping after
         * refresh_schedule() can undo the worker's final restart.
         */
        app_node_comm_sync_unlock();
        config->stop_role_scan(config->ctx);
        ret = app_node_comm_sync_lock();
        if (ret < 0) {
            if (config->restart_role_scan != NULL) {
                config->restart_role_scan(config->ctx);
            }
            return ret;
        }
        if (route_refresh.config != config ||
            route_refresh.operation_generation != request_generation ||
            route_refresh.paused || !route_refresh.active) {
            restart_role_scan = !route_refresh.paused;
            ret = -ECANCELED;
            goto out;
        }
    }
    ret = refresh_schedule(delay_ms);
    if (ret < 0 &&
        route_refresh.config == config &&
        route_refresh.operation_generation == request_generation) {
        route_refresh.active = false;
        route_refresh.due_ms = 0u;
        route_refresh.scheduled = false;
        if (forced) {
            route_refresh.forced = false;
            route_refresh.correlated = false;
        }
    }
    if (ret < 0 && stop_role_scan && !route_refresh.paused) {
        restart_role_scan = true;
    }
out:
    app_node_comm_sync_unlock();
    if (restart_role_scan && config->restart_role_scan != NULL) {
        config->restart_role_scan(config->ctx);
    }
    return ret;
}

int app_node_comm_gateway_route_refresh_request(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation)
{
    return app_node_comm_gateway_route_refresh_request_bounded(
        delay_ms,
        reason,
        forced,
        correlation,
        ROUTE_REFRESH_PROTOCOL_DEADLINE_MS);
}

int app_node_comm_gateway_route_refresh_request_bounded(
    uint32_t delay_ms,
    const char *reason,
    bool forced,
    const struct proto_packet *correlation,
    uint32_t timeout_ms)
{
    return route_refresh_request_bounded(delay_ms,
                                         reason,
                                         forced,
                                         correlation,
                                         timeout_ms);
}

bool app_node_comm_gateway_route_refresh_pending_wait_ms(
    uint32_t now_ms, uint32_t *wait_ms)
{
    const struct app_node_comm_gateway_route_refresh_config *config;
    uint32_t due_ms;
    bool active;
    bool forced;
    bool scheduled;
    bool allowed;

    if (app_node_comm_sync_lock() < 0) {
        return false;
    }
    config = route_refresh.config;
    active = route_refresh.active;
    forced = route_refresh.forced;
    scheduled = route_refresh.scheduled;
    due_ms = route_refresh.due_ms;
    app_node_comm_sync_unlock();
    allowed = config != NULL && config->gateway_role &&
              (forced || (config->allowed != NULL &&
                          config->allowed(config->ctx)));
    if (!allowed || !active || !scheduled) {
        return false;
    }
    if (wait_ms != NULL) {
        *wait_ms = refresh_wait_ms(now_ms, due_ms);
    }
    return true;
}

bool app_node_comm_gateway_route_refresh_due(uint32_t now_ms)
{
    uint32_t wait_ms = 0u;

    return app_node_comm_gateway_route_refresh_pending_wait_ms(now_ms,
                                                                &wait_ms) &&
           wait_ms == 0u;
}

uint32_t app_node_comm_gateway_route_refresh_due_ms(void)
{
    uint32_t due_ms = 0u;

    if (app_node_comm_sync_lock() == 0) {
        due_ms = route_refresh.due_ms;
        app_node_comm_sync_unlock();
    }
    return due_ms;
}

int app_node_comm_gateway_route_refresh_schedule_now(void)
{
    int ret = app_node_comm_sync_lock();

    if (ret < 0) {
        return ret;
    }
    ret = route_refresh.active ? refresh_schedule(0u) : -ENOENT;
    app_node_comm_sync_unlock();
    return ret;
}

void app_node_comm_gateway_route_refresh_response_priority_clear(void)
{
    if (app_node_comm_sync_lock() == 0) {
        route_refresh.response_due_ms = 0u;
        route_refresh.response_due_valid = false;
        app_node_comm_sync_unlock();
    }
}

void app_node_comm_gateway_route_refresh_pause(uint32_t now_ms)
{
    atomic_set(&route_refresh_pause_intent, 1);
    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    if (route_refresh.paused) {
        app_node_comm_sync_unlock();
        return;
    }
    route_refresh.paused = true;
    route_refresh.paused_at_ms = now_ms;
    route_refresh.operation_generation++;
    if (route_refresh.operation_generation == 0u) {
        route_refresh.operation_generation = 1u;
    }
    (void)k_work_cancel_delayable(&route_refresh_work);
    app_node_comm_sync_unlock();
}

void app_node_comm_gateway_route_refresh_resume(uint32_t now_ms)
{
    uint32_t paused_ms;
    uint32_t resume_due_ms;

    if (app_node_comm_sync_lock() < 0) {
        return;
    }
    if (!route_refresh.paused) {
        app_node_comm_sync_unlock();
        return;
    }
    if (route_refresh.in_flight) {
        route_refresh.resume_pending = true;
        app_node_comm_sync_unlock();
        return;
    }
    paused_ms = now_ms - route_refresh.paused_at_ms;
    route_refresh.paused = false;
    route_refresh.paused_at_ms = 0u;
    atomic_set(&route_refresh_pause_intent, 0);
    if (!route_refresh.active) {
        app_node_comm_sync_unlock();
        return;
    }
    route_refresh.due_ms = refresh_deadline_add(route_refresh.due_ms,
                                                paused_ms);
    app_mesh_flood_progress_rebase(&route_refresh.flood, paused_ms);
    resume_due_ms = route_refresh.flood.initialized &&
                    !route_refresh.flood.complete ?
                    route_refresh.flood.due_ms : route_refresh.due_ms;
    if (route_refresh.absolute_deadline_valid &&
        refresh_deadline_reached(now_ms,
                                 route_refresh.absolute_deadline_ms)) {
        if (refresh_schedule(0u) < 0) {
            refresh_complete(-EIO);
        }
        app_node_comm_sync_unlock();
        return;
    }
    if (refresh_schedule(refresh_wait_ms(now_ms, resume_due_ms)) < 0) {
        refresh_complete(-EIO);
    }
    app_node_comm_sync_unlock();
}
