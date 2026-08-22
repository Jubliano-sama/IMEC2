#include "survey_gateway_transaction.h"

#include <errno.h>
#include <string.h>

_Static_assert(SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS > 0u,
               "survey pair control floor must be nonzero");

static bool survey_gateway_due_owner_valid(enum survey_gateway_due_owner owner)
{
    return owner >= SURVEY_GATEWAY_DUE_ROUND_OBSERVATION &&
           owner < SURVEY_GATEWAY_DUE_OWNER_COUNT;
}

static bool survey_gateway_due_reached(uint32_t now_ms, uint32_t due_ms)
{
    return (int32_t)(now_ms - due_ms) >= 0;
}

void survey_gateway_due_registry_init(
    struct survey_gateway_due_registry *registry)
{
    if (registry != NULL) {
        memset(registry, 0, sizeof(*registry));
    }
}

bool survey_gateway_due_registry_next(
    const struct survey_gateway_due_registry *registry,
    uint32_t now_ms,
    uint32_t *delay_ms)
{
    bool found = false;
    uint32_t selected_delay_ms = 0u;

    if (registry == NULL || delay_ms == NULL) {
        return false;
    }
    for (size_t i = 0u; i < SURVEY_GATEWAY_DUE_OWNER_COUNT; i++) {
        const uint8_t owner_bit = (uint8_t)(UINT8_C(1) << i);
        uint32_t candidate_delay_ms;

        if ((registry->valid_mask & owner_bit) == 0u) {
            continue;
        }
        candidate_delay_ms = survey_gateway_due_reached(
            now_ms, registry->due_ms[i]) ?
                                 0u : registry->due_ms[i] - now_ms;
        if (!found || candidate_delay_ms < selected_delay_ms) {
            found = true;
            selected_delay_ms = candidate_delay_ms;
        }
    }
    if (found) {
        *delay_ms = selected_delay_ms;
    }
    return found;
}

int survey_gateway_due_registry_rearm(
    const struct survey_gateway_due_registry *registry,
    uint32_t now_ms,
    survey_gateway_due_arm_fn arm,
    void *arm_context)
{
    uint32_t delay_ms;

    if (registry == NULL || arm == NULL) {
        return -EINVAL;
    }
    if (!survey_gateway_due_registry_next(registry, now_ms, &delay_ms)) {
        return 0;
    }
    return arm(arm_context, delay_ms);
}

int survey_gateway_due_registry_schedule_after(
    struct survey_gateway_due_registry *registry,
    enum survey_gateway_due_owner owner,
    uint32_t now_ms,
    uint32_t delay_ms,
    survey_gateway_due_arm_fn arm,
    void *arm_context)
{
    uint8_t owner_bit;

    if (registry == NULL || !survey_gateway_due_owner_valid(owner) ||
        arm == NULL) {
        return -EINVAL;
    }
    owner_bit = (uint8_t)(UINT8_C(1) << owner);
    if ((registry->valid_mask & owner_bit) == 0u ||
        (!survey_gateway_due_reached(now_ms, registry->due_ms[owner]) &&
         delay_ms < registry->due_ms[owner] - now_ms)) {
        registry->due_ms[owner] = now_ms + delay_ms;
    }
    registry->valid_mask |= owner_bit;
    return survey_gateway_due_registry_rearm(registry, now_ms, arm,
                                             arm_context);
}

void survey_gateway_due_registry_cancel(
    struct survey_gateway_due_registry *registry,
    enum survey_gateway_due_owner owner)
{
    if (registry == NULL || !survey_gateway_due_owner_valid(owner)) {
        return;
    }
    registry->valid_mask &= (uint8_t)~(UINT8_C(1) << owner);
}

bool survey_gateway_due_registry_consume_due(
    struct survey_gateway_due_registry *registry,
    uint32_t now_ms)
{
    bool consumed = false;

    if (registry == NULL) {
        return false;
    }
    for (size_t i = 0u; i < SURVEY_GATEWAY_DUE_OWNER_COUNT; i++) {
        const uint8_t owner_bit = (uint8_t)(UINT8_C(1) << i);

        if ((registry->valid_mask & owner_bit) != 0u &&
            survey_gateway_due_reached(now_ms, registry->due_ms[i])) {
            registry->valid_mask &= (uint8_t)~owner_bit;
            consumed = true;
        }
    }
    return consumed;
}

bool survey_gateway_receive_in_interval(uint64_t first_received_at_ms,
                                        uint32_t started_at_ms,
                                        uint32_t deadline_ms)
{
    const uint32_t received_at_ms = (uint32_t)first_received_at_ms;

    return deadline_ms != started_at_ms &&
           survey_gateway_due_reached(deadline_ms, started_at_ms) &&
           survey_gateway_due_reached(received_at_ms, started_at_ms) &&
           !survey_gateway_due_reached(received_at_ms, deadline_ms);
}

void survey_gateway_observation_origin_reset(
    struct survey_gateway_observation_origin *origin)
{
    if (origin != NULL) {
        memset(origin, 0, sizeof(*origin));
    }
}

bool survey_gateway_observation_origin_freeze(
    struct survey_gateway_observation_origin *origin,
    uint32_t started_at_ms)
{
    if (origin == NULL || origin->valid) {
        return false;
    }
    origin->started_at_ms = started_at_ms;
    origin->valid = true;
    return true;
}

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
                          const uint8_t result_digest[
                              SEMANTIC_DIGEST_SHA256_LEN],
                          uint32_t result_token)
{
    struct survey_gateway_transaction_recent *recent =
        &context->recent[context->recent_next];

    memset(recent, 0, sizeof(*recent));
    recent->key = context->active.spec.key;
    memcpy(recent->request_digest,
           context->active.spec.request_digest,
           sizeof(recent->request_digest));
    memcpy(recent->result_digest,
           result_digest,
           sizeof(recent->result_digest));
    recent->result_token = result_token;
    recent->expires_at_ms = context->active.spec.absolute_deadline_ms;
    recent->valid = true;
    context->recent_next =
        (uint8_t)((context->recent_next + 1u) %
                  SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT);
}

static void freeze_cleanup_deadline(
    struct survey_gateway_transaction *context,
    uint64_t now_ms)
{
    if (context->cleanup_deadline_ms != 0u) {
        return;
    }
    context->cleanup_deadline_ms =
        UINT64_MAX - now_ms <
                SURVEY_GATEWAY_TRANSACTION_CLEANUP_TIMEOUT_MS ?
            UINT64_MAX :
            now_ms + SURVEY_GATEWAY_TRANSACTION_CLEANUP_TIMEOUT_MS;
}

static void set_cleanup_from_side_effects(
    struct survey_gateway_transaction *context,
    uint64_t now_ms)
{
    freeze_cleanup_deadline(context, now_ms);
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
    set_cleanup_from_side_effects(context, now_ms);
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
    context->cleanup_deadline_ms = 0u;
    context->active_started_at_ms = 0u;
    context->pair_loaded = true;
    context->conflict = false;
    return 0;
}

int survey_gateway_transaction_begin(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    enum command_id command_id,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t client_token,
    uint32_t delivery_handle,
    uint64_t absolute_deadline_ms,
    uint64_t now_ms)
{
    struct node_transaction_spec spec;
    uint8_t target_mask;
    int ret;

    if (context == NULL || key == NULL || request_digest == NULL ||
        !context->pair_loaded ||
        context->abandoning ||
        !command_id_valid(command_id) ||
        client_token == 0u) {
        return -EINVAL;
    }
    target_mask = peer_mask(&context->pair, key->responder_id);
    const uint32_t operation_session_id =
        context->pair.operation_generation == 0u ?
            context->pair.survey_id :
            survey_operation_session_id(
                context->pair.operation_generation);

    if (target_mask == 0u || key->requester_id == 0u ||
        key->requester_id == key->responder_id ||
        key->session_id != operation_session_id ||
        key->transaction_id == 0u ||
        key->operation_id != (uint16_t)command_id) {
        return -EINVAL;
    }

    memset(&spec, 0, sizeof(spec));
    spec.key = *key;
    memcpy(spec.request_digest,
           request_digest,
           sizeof(spec.request_digest));
    spec.client_token = client_token;
    spec.absolute_deadline_ms = absolute_deadline_ms;
    spec.cleanup_required = true;
    ret = node_transaction_begin(&context->active, &spec,
                                 delivery_handle, now_ms);
    if (ret < 0) {
        return ret;
    }
    context->active_command_id = command_id;
    context->active_target_id = key->responder_id;
    context->active_started_at_ms = (uint32_t)now_ms;
    context->close_intent = SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE;
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
            uint8_t target_mask = peer_mask(
                &context->pair, context->active_target_id);

            context->possible_prepare_mask &=
                (uint8_t)~target_mask;
            if ((context->prepared_mask & target_mask) == 0u) {
                /*
                 * A prior deadline/cancel may already have copied the
                 * possible-PREPARE bit into cleanup_mask. Authoritative
                 * zero-attempt terminal evidence retires that conservative
                 * debt unless a confirmed PREPARE independently owns it.
                 */
                context->cleanup_mask &= (uint8_t)~target_mask;
            }
        }
        set_cleanup_from_side_effects(context, now_ms);
    }
    return 0;
}

int survey_gateway_transaction_note_delivery_redrive(
    struct survey_gateway_transaction *context,
    const struct node_comm_terminal_event *event,
    uint64_t now_ms,
    enum node_transaction_action *action)
{
    if (context == NULL) {
        return -EINVAL;
    }
    if (context->close_intent !=
        SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE) {
        return -ECANCELED;
    }
    return node_transaction_note_request_redrive(
        &context->active, event, now_ms, action);
}

int survey_gateway_transaction_reconcile_result(
    struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    const uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN],
    const uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN],
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

    if (context == NULL || key == NULL || request_digest == NULL ||
        result_digest == NULL || result == NULL || action == NULL ||
        status > COMMAND_INTERNAL_ERROR) {
        return -EINVAL;
    }
    expire_recent(context, now_ms);
    recent = find_recent(context, key);
    if (recent != NULL &&
        !node_transaction_key_equal(&context->active.spec.key, key)) {
        if (semantic_digest_equal(recent->request_digest,
                                  request_digest,
                                  SEMANTIC_DIGEST_SHA256_LEN) &&
            semantic_digest_equal(recent->result_digest,
                                  result_digest,
                                  SEMANTIC_DIGEST_SHA256_LEN) &&
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
                                            request_digest,
                                            result_digest,
                                            result_token,
                                            now_ms,
                                            &disposition,
                                            action);
    if (ret < 0) {
        return ret;
    }
    switch (disposition) {
    case NODE_TRANSACTION_RESULT_ACCEPTED:
        retain_result(context, result_digest, result_token);
        target_mask = peer_mask(&context->pair, context->active_target_id);
        if (status == COMMAND_OK) {
            if (context->active_command_id == CMD_SURVEY_PREPARE_PAIR) {
                context->prepared_mask |= target_mask;
                context->possible_prepare_mask &= (uint8_t)~target_mask;
            }
            *result = SURVEY_GATEWAY_TRANSACTION_RESULT_ACCEPTED_OK;
        } else {
            set_cleanup_from_side_effects(context, now_ms);
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
        set_cleanup_from_side_effects(context, now_ms);
    }
    return expired;
}

int survey_gateway_transaction_request_close(
    struct survey_gateway_transaction *context,
    enum survey_gateway_transaction_close_intent intent)
{
    if (context == NULL ||
        intent <= SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE ||
        intent > SURVEY_GATEWAY_TRANSACTION_CLOSE_INTERNAL ||
        context->active.state == NODE_TRANSACTION_EMPTY) {
        return -EINVAL;
    }
    if (context->close_intent == intent) {
        return 0;
    }
    if (context->close_intent != SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE) {
        return -EALREADY;
    }
    context->close_intent = intent;
    return 0;
}

enum survey_gateway_transaction_close_intent
survey_gateway_transaction_close_requested(
    const struct survey_gateway_transaction *context)
{
    return context == NULL ? SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE :
                             context->close_intent;
}

void survey_gateway_transaction_clear_close_request(
    struct survey_gateway_transaction *context)
{
    if (context != NULL) {
        context->close_intent = SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE;
    }
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
    context->active_started_at_ms = 0u;
    context->close_intent = SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE;
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

uint64_t survey_gateway_transaction_cleanup_deadline(
    const struct survey_gateway_transaction *context)
{
    return context == NULL ? 0u : context->cleanup_deadline_ms;
}

bool survey_gateway_transaction_pair_plan_fits_minimum_budget(
    size_t pair_count,
    uint32_t remaining_ms)
{
    return pair_count <=
           (size_t)(remaining_ms / SURVEY_GATEWAY_PAIR_MINIMUM_CONTROL_MS);
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
    if (state->control_confirmation_pending) {
        return SURVEY_GATEWAY_DRIVE_WAIT_CONFIRMATION;
    }
    if (state->control_inflight || state->round_observing) {
        return SURVEY_GATEWAY_DRIVE_POLL_WAIT;
    }
    if (state->round_drive_ready) {
        return SURVEY_GATEWAY_DRIVE_RUN_NOW;
    }
    return SURVEY_GATEWAY_DRIVE_NONE;
}

bool survey_gateway_transaction_request_digest(
    const struct survey_gateway_transaction *context,
    const struct node_transaction_key *key,
    uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (context == NULL || key == NULL || request_digest == NULL) {
        return false;
    }
    if (context->active.state != NODE_TRANSACTION_EMPTY &&
        node_transaction_key_equal(&context->active.spec.key, key)) {
        memcpy(request_digest,
               context->active.spec.request_digest,
               SEMANTIC_DIGEST_SHA256_LEN);
        return true;
    }
    for (size_t i = 0u; i < SURVEY_GATEWAY_TRANSACTION_RECENT_COUNT; i++) {
        if (context->recent[i].valid &&
            node_transaction_key_equal(&context->recent[i].key, key)) {
            memcpy(request_digest,
                   context->recent[i].request_digest,
                   SEMANTIC_DIGEST_SHA256_LEN);
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
    if (context->active.state != NODE_TRANSACTION_EMPTY &&
        !context->active.request_delivery_terminal) {
        return -EINPROGRESS;
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
    context->active_started_at_ms = 0u;
    context->prepared_mask = 0u;
    context->possible_prepare_mask = 0u;
    context->cleanup_deadline_ms = 0u;
    context->abandoning = false;
    return 0;
}

int survey_gateway_transaction_note_cleanup_lease_expired(
    struct survey_gateway_transaction *context,
    uint8_t peer_mask_value,
    uint64_t now_ms)
{
    /*
     * Expiry of the immutable remote PREPARE lease is terminal authority:
     * the peer can no longer retain START_PENDING for this pair even when an
     * ABORT result could not be obtained. Keep that proof distinct at the
     * API boundary while sharing the exact local retirement transition.
     */
    return survey_gateway_transaction_note_cleanup_complete(
        context, peer_mask_value, now_ms);
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
    context->cleanup_deadline_ms = 0u;
    context->pair_loaded = false;
    context->abandoning = false;
    context->close_intent = SURVEY_GATEWAY_TRANSACTION_CLOSE_NONE;
}
