#include "app_gateway_eack_policy.h"

#include <errno.h>
#include <string.h>

static bool return_target_valid(uint64_t next_hop_id)
{
    return next_hop_id != 0u && next_hop_id != MESH_BROADCAST_ID;
}

static bool return_target_already_tried(const uint64_t *return_next_hop_ids,
                                        size_t candidate_index,
                                        uint64_t next_hop_id)
{
    for (size_t i = 0u; i < candidate_index; i++) {
        if (return_next_hop_ids[i] == next_hop_id) {
            return true;
        }
    }
    return false;
}

static void result_init(struct app_gateway_eack_policy_result *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->mode = APP_GATEWAY_EACK_SEND_NONE;
    }
}

static void result_note_channel9_plan(struct app_gateway_eack_policy_result *result,
                                      int ret)
{
    if (result != NULL) {
        result->channel9_plan_ret = ret;
    }
}

static void result_note_channel9_send(struct app_gateway_eack_policy_result *result,
                                      int ret)
{
    if (result != NULL) {
        result->channel9_send_ret = ret;
    }
}

static void result_note_channel9_prepare(struct app_gateway_eack_policy_result *result,
                                         int ret)
{
    if (result != NULL) {
        result->channel9_prepare_ret = ret;
    }
}

static void result_note_c5_send(struct app_gateway_eack_policy_result *result,
                                int ret)
{
    if (result != NULL) {
        result->c5_send_ret = ret;
    }
}

static void result_note_channel9_candidate(struct app_gateway_eack_policy_result *result)
{
    if (result != NULL && result->channel9_candidate_count < UINT8_MAX) {
        result->channel9_candidate_count++;
    }
}

static void result_note_channel9_attempt(struct app_gateway_eack_policy_result *result)
{
    if (result != NULL && result->channel9_attempt_count < UINT8_MAX) {
        result->channel9_attempt_count++;
    }
}

static void result_note_channel9_next_hop(struct app_gateway_eack_policy_result *result,
                                          uint64_t next_hop_id)
{
    if (result != NULL) {
        result->channel9_next_hop_id = next_hop_id;
    }
}

static void result_note_mode(struct app_gateway_eack_policy_result *result,
                             enum app_gateway_eack_send_mode mode)
{
    if (result != NULL) {
        result->mode = mode;
    }
}

int app_gateway_eack_send_to_candidates_with_current_channel9(
    struct mesh_outbound *eack,
    uint64_t current_channel9_next_hop_id,
    const struct mesh_event_plan *current_channel9_plan,
    const uint64_t *return_next_hop_ids,
    size_t return_next_hop_count,
    const struct app_gateway_eack_policy_ops *ops,
    struct app_gateway_eack_policy_result *result)
{
    struct mesh_outbound original_eack;
    bool current_channel9_bound;
    int ret;

    result_init(result);
    if (eack == NULL || ops == NULL || ops->send_c5_flood == NULL) {
        return -EINVAL;
    }

    if (return_next_hop_count > 0u && return_next_hop_ids == NULL) {
        return -EINVAL;
    }

    original_eack = *eack;
    current_channel9_bound =
        return_target_valid(current_channel9_next_hop_id) &&
        current_channel9_plan != NULL &&
        ops->prepare_channel9 != NULL &&
        ops->send_channel9 != NULL;
    if (current_channel9_bound) {
        *eack = original_eack;
        eack->next_hop_id = current_channel9_next_hop_id;
        eack->radio_channel = MESH_EVENT_CHANNEL;
        eack->earliest_tx_ms = 0u;
        ret = ops->prepare_channel9(eack, current_channel9_plan, ops->ctx);
        result_note_channel9_prepare(result, ret);
        if (ret != 0) {
            goto current_channel9_done;
        }
        result_note_channel9_next_hop(result, current_channel9_next_hop_id);
        result_note_channel9_attempt(result);
        ret = ops->send_channel9(eack, ops->ctx);
        result_note_channel9_send(result, ret);
        if (ret == 0) {
            result_note_mode(result, APP_GATEWAY_EACK_SEND_CURRENT_CHANNEL9);
            result_note_channel9_next_hop(result, current_channel9_next_hop_id);
            if (ops->note_tx_sent != NULL) {
                ops->note_tx_sent(eack, ops->ctx);
            }
            if (ops->note_channel9_tx != NULL) {
                ops->note_channel9_tx(current_channel9_next_hop_id,
                                      current_channel9_plan->start_ms,
                                      ops->ctx);
            }
            return 0;
        }
    }
current_channel9_done:
    if (current_channel9_bound) {
        *eack = original_eack;
        return ret;
    }

    if (ops->plan_channel9 != NULL &&
        ops->prepare_channel9 != NULL &&
        ops->send_channel9 != NULL) {
        for (size_t i = 0u; i < return_next_hop_count; i++) {
            struct mesh_event_plan plan = {0};
            const uint64_t return_next_hop_id = return_next_hop_ids[i];

            if (!return_target_valid(return_next_hop_id) ||
                return_target_already_tried(return_next_hop_ids, i, return_next_hop_id)) {
                continue;
            }

            result_note_channel9_candidate(result);
            ret = ops->plan_channel9(return_next_hop_id, &plan, ops->ctx);
            result_note_channel9_plan(result, ret);
            if (ret != 0) {
                continue;
            }

            *eack = original_eack;
            eack->next_hop_id = return_next_hop_id;
            eack->radio_channel = MESH_EVENT_CHANNEL;
            ret = ops->prepare_channel9(eack, &plan, ops->ctx);
            result_note_channel9_prepare(result, ret);
            if (ret != 0) {
                continue;
            }
            result_note_channel9_next_hop(result, return_next_hop_id);
            result_note_channel9_attempt(result);
            ret = ops->send_channel9(eack, ops->ctx);
            result_note_channel9_send(result, ret);
            if (ret == 0) {
                result_note_mode(result, APP_GATEWAY_EACK_SEND_CHANNEL9);
                result_note_channel9_next_hop(result, return_next_hop_id);
                if (ops->note_tx_sent != NULL) {
                    ops->note_tx_sent(eack, ops->ctx);
                }
                if (ops->note_channel9_tx != NULL) {
                    ops->note_channel9_tx(return_next_hop_id,
                                          plan.start_ms,
                                          ops->ctx);
                }
                return 0;
            }

            /*
             * This callback may have started real RF.  Preserve the exact
             * EACK and return the failure so the shared communication retry
             * gate inserts randomized exponential backoff before any
             * alternate Channel 9 lane or Channel 5 recovery attempt.
             */
            *eack = original_eack;
            return ret;
        }
    }

    *eack = original_eack;
    eack->next_hop_id = MESH_BROADCAST_ID;
    eack->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    eack->earliest_tx_ms = 0u;
    ret = ops->send_c5_flood(eack, ops->ctx);
    result_note_c5_send(result, ret);
    if (ret == 0) {
        result_note_mode(result, APP_GATEWAY_EACK_SEND_C5_FLOOD);
        if (ops->note_tx_sent != NULL) {
            ops->note_tx_sent(eack, ops->ctx);
        }
    }
    return ret;
}

int app_gateway_eack_send_to_candidates(struct mesh_outbound *eack,
                                        const uint64_t *return_next_hop_ids,
                                        size_t return_next_hop_count,
                                        const struct app_gateway_eack_policy_ops *ops,
                                        struct app_gateway_eack_policy_result *result)
{
    return app_gateway_eack_send_to_candidates_with_current_channel9(
        eack,
        0u,
        NULL,
        return_next_hop_ids,
        return_next_hop_count,
        ops,
        result);
}

int app_gateway_eack_send(struct mesh_outbound *eack,
                          uint64_t return_next_hop_id,
                          const struct app_gateway_eack_policy_ops *ops,
                          struct app_gateway_eack_policy_result *result)
{
    return app_gateway_eack_send_to_candidates(eack, &return_next_hop_id, 1u, ops, result);
}
