#include "app_gateway_eack_policy.h"

#include <errno.h>
#include <string.h>

static bool return_target_valid(uint64_t next_hop_id)
{
    return next_hop_id != 0u && next_hop_id != MESH_BROADCAST_ID;
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

static void result_note_mode(struct app_gateway_eack_policy_result *result,
                             enum app_gateway_eack_send_mode mode)
{
    if (result != NULL) {
        result->mode = mode;
    }
}

int app_gateway_eack_send(struct mesh_outbound *eack,
                          uint64_t return_next_hop_id,
                          const struct app_gateway_eack_policy_ops *ops,
                          struct app_gateway_eack_policy_result *result)
{
    struct mesh_event_plan plan = {0};
    int ret;

    result_init(result);
    if (eack == NULL || ops == NULL || ops->send_c5_flood == NULL) {
        return -EINVAL;
    }

    if (return_target_valid(return_next_hop_id) &&
        ops->plan_channel9 != NULL &&
        ops->prepare_channel9 != NULL &&
        ops->send_channel9 != NULL) {
        ret = ops->plan_channel9(return_next_hop_id, &plan, ops->ctx);
        result_note_channel9_plan(result, ret);
        if (ret == 0) {
            eack->next_hop_id = return_next_hop_id;
            eack->radio_channel = MESH_EVENT_CHANNEL;
            ret = ops->prepare_channel9(eack, &plan, ops->ctx);
            result_note_channel9_prepare(result, ret);
            if (ret != 0) {
                goto c5_fallback;
            }
            ret = ops->send_channel9(eack, ops->ctx);
            result_note_channel9_send(result, ret);
            if (ret == 0) {
                result_note_mode(result, APP_GATEWAY_EACK_SEND_CHANNEL9);
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
        }
    }

c5_fallback:
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
