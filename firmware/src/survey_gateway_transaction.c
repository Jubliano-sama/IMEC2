#include "survey_gateway_transaction.h"

#include <errno.h>
#include <string.h>

static uint8_t peer_mask(const struct survey_pair *pair, uint64_t target_id)
{
    if (pair == NULL || target_id == 0u) {
        return 0u;
    }
    if (target_id == pair->initiator_id) {
        return SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK;
    }
    if (target_id == pair->responder_id) {
        return SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    }
    return 0u;
}

static bool command_id_valid(enum command_id command_id)
{
    return command_id == CMD_SURVEY_PREPARE_PAIR ||
           command_id == CMD_SURVEY_START_PAIR;
}

static void expire_recent(struct survey_gateway_transaction *context,
                          uint64_t now_ms)
{
    for (size_t i = 0u; i < SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT; i++) {
        if (context->recent[i].valid &&
            now_ms >= context->recent[i].expires_at_ms) {
            memset(&context->recent[i], 0, sizeof(context->recent[i]));
        }
    }
}

static struct survey_gateway_transaction_recent *find_recent(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key)
{
    for (size_t i = 0u; i < SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT; i++) {
        if (context->recent[i].valid &&
            node_transaction_key_equal(&context->recent[i].key, key)) {
            return &context->recent[i];
        }
    }
    return NULL;
}

static void retain_result(struct survey_gateway_transaction *context,
                          uint32_t result_fingerprint,
                          uint32_t result_token)
{
    struct survey_gateway_transaction_recent *recent =
        &context->recent[context->recent_next];

    *recent = (struct survey_gateway_transaction_recent) {
        .key = context->active.spec.key,
        .request_fingerprint = context->active.spec.request_fingerprint,
        .result_fingerprint = result_fingerprint,
        .result_token = result_token,
        .expires_at_ms = context->active.spec.absolute_deadline_ms,
        .valid = true,
    };
    context->recent_next =
        (uint8_t)((context->recent_next + 1u) %
                  SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT);
}

static void set_cleanup_from_side_effects(
    struct survey_gateway_transaction *context)
{
    context->cleanup_mask |= context->prepared_mask;
    if (context->active.remote_side_effect_possible &&
        context->active_command_id == CMD_SURVEY_PREPARE_PAIR) {
        context->cleanup_mask |= peer_mask(&context->pair,
                                           context->active_target_id);
    }
    context->cleanup_mask |= context->possible_prepare_mask;
    context->abandoning = true;
}

static void abandon_active(struct survey_gateway_transaction *context,
                           uint64_t now_ms)
{
    enum node_transaction_action action;

    if (context->active.state == NODE_TRANSACTION_ACTIVE) {
        (void)node_transaction_cancel(&context->active, now_ms, &action);
    }
    set_cleanup_from_side_effects(context);
}

void survey_gateway_transaction_init(
    struct survey_gateway_transaction *context)
{
    if (context != NULL) {
        memset(context, 0, sizeof(*context));
        node_transaction_init(&context->active);
    }
}

int survey_gateway_transaction_load_pair(
    struct survey_gateway_transaction *context,
    const struct survey_pair *pair)
{
    if (context == NULL || pair == NULL ||
        survey_pair_validate(pair) != PROTO_OK) {
        return -EINVAL;
    }
    if (context->abandoning ||
        context->active.state != NODE_TRANSACTION_EMPTY) {
        return -EBUSY;
    }

    context->pair = *pair;
    context->prepared_mask = 0u;
    context->possible_prepare_mask = 0u;
    context->cleanup_mask = 0u;
    context->pair_loaded = true;
    context->conflict = false;
    return 0;
}

int survey_gateway_transaction_begin(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    enum command_id command_id,
    uint32_t request_fingerprint,
    uint32_t client_token,
    uint32_t delivery_handle,
    uint64_t absolute_deadline_ms,
    uint64_t now_ms)
{
    struct node_transaction_spec spec;
    uint8_t target_mask;
    int ret;

    if (context == NULL || key == NULL || !context->pair_loaded ||
        context->abandoning ||
        !command_id_valid(command_id) || request_fingerprint == 0u ||
        client_token == 0u) {
        return -EINVAL;
    }
    target_mask = peer_mask(&context->pair, key->responder_id);
    if (target_mask == 0u || key->requester_id == 0u ||
        key->requester_id == key->responder_id ||
        key->session_id != context->pair.survey_id ||
        key->transaction_id == 0u ||
        key->operation_id != (uint16_t)command_id) {
        return -EINVAL;
    }

    spec = (struct node_transaction_spec) {
        .key = *key,
        .request_fingerprint = request_fingerprint,
        .client_token = client_token,
        .absolute_deadline_ms = absolute_deadline_ms,
        .cleanup_required = true,
    };
    ret = node_transaction_begin(&context->active, &spec,
                                 delivery_handle, now_ms);
    if (ret < 0) {
        return ret;
    }
    context->active_command_id = command_id;
    context->active_target_id = key->responder_id;
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        context->possible_prepare_mask |= target_mask;
    }
    return 0;
}

int survey_gateway_transaction_note_delivery_terminal(
    struct survey_gateway_transaction *context,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    int ret;

    if (context == NULL) {
        return -EINVAL;
    }
    ret = node_transaction_note_request_terminal(&context->active,
                                                 event, now_ms, action);
    if (ret < 0) {
        return ret;
    }
    if (context->active.state == NODE_TRANSACTION_ABANDONING ||
        context->active.state == NODE_TRANSACTION_ABANDONED) {
        if (!context->active.remote_side_effect_possible &&
            context->active_command_id == CMD_SURVEY_PREPARE_PAIR) {
            context->possible_prepare_mask &=
                (uint8_t)~peer_mask(&context->pair,
                                    context->active_target_id);
        }
        set_cleanup_from_side_effects(context);
    }
    return 0;
}

int survey_gateway_transaction_reconcile_result(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    uint32_t request_fingerprint,
    uint32_t result_fingerprint,
    uint32_t result_token,
    enum command_status status,
    uint64_t now_ms,
    enum survey_gateway_transaction_result *result,
    enum node_transaction_action *action)
{
    struct survey_gateway_transaction_recent *recent;
    enum node_transaction_result_disposition disposition;
    uint8_t target_mask;
    int ret;

    if (context == NULL || key == NULL || result == NULL || action == NULL ||
        status > COMMAND_INTERNAL_ERROR) {
        return -EINVAL;
    }
    expire_recent(context, now_ms);
    recent = find_recent(context, key);
    if (recent != NULL &&
        !node_transaction_key_equal(&context->active.spec.key, key)) {
        if (recent->request_fingerprint == request_fingerprint &&
            recent->result_fingerprint == result_fingerprint &&
            recent->result_token == result_token) {
            *result = SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE;
            *action = NODE_TRANSACTION_ACTION_NONE;
            return 0;
        }
        context->conflict = true;
        abandon_active(context, now_ms);
        *result = SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT;
        *action = NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED;
        return 0;
    }

    ret = node_transaction_reconcile_result(&context->active,
                                            key,
                                            request_fingerprint,
                                            result_fingerprint,
                                            result_token,
                                            now_ms,
                                            &disposition,
                                            action);
    if (ret < 0) {
        return ret;
    }
    switch (disposition) {
    case NODE_TRANSACTION_RESULT_ACCEPTED:
        retain_result(context, result_fingerprint, result_token);
        target_mask = peer_mask(&context->pair, context->active_target_id);
        if (status == COMMAND_OK) {
            if (context->active_command_id == CMD_SURVEY_PREPARE_PAIR) {
                context->prepared_mask |= target_mask;
                context->possible_prepare_mask &= (uint8_t)~target_mask;
            }
            *result = SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK;
        } else {
            set_cleanup_from_side_effects(context);
            *result = SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_FAILURE;
            *action = context->cleanup_mask != 0u ?
                      NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED :
                      NODE_TRANSACTION_ACTION_TERMINAL_ABANDON;
        }
        return 0;
    case NODE_TRANSACTION_RESULT_DUPLICATE:
        *result = SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE;
        return 0;
    case NODE_TRANSACTION_RESULT_LATE_AFTER_ABANDON:
        *result = SURVEY_GATEWAY_TRANSACTION_RESULT_LATE;
        return 0;
    case NODE_TRANSACTION_RESULT_CONFLICT:
        context->conflict = true;
        abandon_active(context, now_ms);
        *result = SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT;
        *action = NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED;
        return 0;
    case NODE_TRANSACTION_RESULT_STALE:
    default:
        *result = SURVEY_GATEWAY_TRANSACTION_RESULT_STALE;
        return 0;
    }
}

bool survey_gateway_transaction_service(
    struct survey_gateway_transaction *context,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    bool expired;

    if (context == NULL) {
        return false;
    }
    expire_recent(context, now_ms);
    expired = node_transaction_service(&context->active, now_ms, action);
    if (expired) {
        set_cleanup_from_side_effects(context);
    }
    return expired;
}

int survey_gateway_transaction_phase_complete(
    struct survey_gateway_transaction *context)
{
    int ret;

    if (context == NULL) {
        return -EINVAL;
    }
    if (context->active.state != NODE_TRANSACTION_SUCCEEDED ||
        !context->active.request_delivery_terminal) {
        return -EBUSY;
    }
    ret = node_transaction_retire(&context->active);
    if (ret < 0) {
        return ret;
    }
    context->active_command_id = CMD_VENDOR_BASE;
    context->active_target_id = 0u;
    return 0;
}

void survey_gateway_transaction_require_cleanup(
    struct survey_gateway_transaction *context,
    bool include_both_peers,
    uint64_t now_ms)
{
    if (context == NULL || !context->pair_loaded) {
        return;
    }
    abandon_active(context, now_ms);
    if (include_both_peers) {
        context->cleanup_mask |=
            SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
            SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    }
}

uint8_t survey_gateway_transaction_cleanup_mask(
    const struct survey_gateway_transaction *context)
{
    return context == NULL ? 0u : context->cleanup_mask;
}

bool survey_gateway_transaction_cleanup_pending(
    const struct survey_gateway_transaction *context)
{
    return context != NULL &&
           (context->abandoning || context->cleanup_mask != 0u ||
            context->active.state == NODE_TRANSACTION_ABANDONING);
}

enum survey_gateway_drive_action survey_gateway_drive_action(
    const struct survey_gateway_drive_state *state)
{
    if (state == NULL) {
        return SURVEY_GATEWAY_DRIVE_NONE;
    }
    if (state->cleanup_pending) {
        return SURVEY_GATEWAY_DRIVE_POLL_CLEANUP;
    }
    if (!state->survey_active) {
        return SURVEY_GATEWAY_DRIVE_NONE;
    }
    if (state->boundary_pending) {
        return SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY;
    }
    if (state->auto_running && !state->auto_waiting &&
        !state->pair_observation_active) {
        return SURVEY_GATEWAY_DRIVE_RUN_NOW;
    }
    return SURVEY_GATEWAY_DRIVE_NONE;
}

bool survey_gateway_transaction_request_fingerprint(
    const struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    uint32_t *request_fingerprint)
{
    if (context == NULL || key == NULL || request_fingerprint == NULL) {
        return false;
    }
    if (context->active.state != NODE_TRANSACTION_EMPTY &&
        node_transaction_key_equal(&context->active.spec.key, key)) {
        *request_fingerprint = context->active.spec.request_fingerprint;
        return true;
    }
    for (size_t i = 0u; i < SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT; i++) {
        if (context->recent[i].valid &&
            node_transaction_key_equal(&context->recent[i].key, key)) {
            *request_fingerprint = context->recent[i].request_fingerprint;
            return true;
        }
    }
    return false;
}

int survey_gateway_transaction_note_cleanup_started(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask_value)
{
    uint8_t valid_mask = SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK |
                         SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;

    if (context == NULL || peer_mask_value == 0u ||
        (peer_mask_value & (uint8_t)~valid_mask) != 0u ||
        (context->cleanup_mask & peer_mask_value) != peer_mask_value) {
        return -EINVAL;
    }
    return 0;
}

int survey_gateway_transaction_note_cleanup_complete(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask_value,
    uint64_t now_ms)
{
    enum node_transaction_action action;
    int ret;

    if (context == NULL ||
        (peer_mask_value == 0u && context->cleanup_mask != 0u) ||
        (peer_mask_value != 0u &&
         (context->cleanup_mask & peer_mask_value) != peer_mask_value)) {
        return -EINVAL;
    }
    context->cleanup_mask &= (uint8_t)~peer_mask_value;
    if (context->cleanup_mask != 0u) {
        return 0;
    }

    if (context->active.state == NODE_TRANSACTION_ABANDONING) {
        ret = node_transaction_cleanup_complete(&context->active,
                                                &context->active.spec.key,
                                                now_ms,
                                                &action);
        if (ret < 0) {
            return ret;
        }
    }
    if (context->active.state == NODE_TRANSACTION_SUCCEEDED ||
        context->active.state == NODE_TRANSACTION_ABANDONED) {
        ret = node_transaction_retire(&context->active);
        if (ret < 0) {
            return ret;
        }
    }
    context->active_command_id = CMD_VENDOR_BASE;
    context->active_target_id = 0u;
    context->prepared_mask = 0u;
    context->possible_prepare_mask = 0u;
    context->abandoning = false;
    return 0;
}

void survey_gateway_transaction_pair_complete(
    struct survey_gateway_transaction *context,
    bool success,
    uint64_t now_ms)
{
    if (context == NULL) {
        return;
    }
    if (!success) {
        survey_gateway_transaction_require_cleanup(context, true, now_ms);
        return;
    }
    context->prepared_mask = 0u;
    context->possible_prepare_mask = 0u;
    context->cleanup_mask = 0u;
    context->pair_loaded = false;
    context->abandoning = false;
}
