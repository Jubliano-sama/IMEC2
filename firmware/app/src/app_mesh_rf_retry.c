#include "app_mesh_rf_retry.h"

#include "app_wake_train_politeness.h"
#include "node_comm.h"

#include <limits.h>
#include <string.h>

static bool retry_key_valid(const struct app_mesh_rf_retry_key *key)
{
    return key != NULL && key->source_id != 0u &&
           key->operation > APP_MESH_RF_RETRY_OPERATION_NONE &&
           key->operation <= APP_MESH_RF_RETRY_OPERATION_COLLECTION_EACK;
}

static bool retry_key_equal(const struct app_mesh_rf_retry_key *lhs,
                            const struct app_mesh_rf_retry_key *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->source_id == rhs->source_id &&
           lhs->destination_id == rhs->destination_id &&
           lhs->session_id == rhs->session_id &&
           lhs->sequence == rhs->sequence &&
           lhs->message_type == rhs->message_type &&
           lhs->operation == rhs->operation;
}

static uint32_t retry_key_seed(const struct app_mesh_rf_retry_key *key)
{
    const uint32_t words[] = {
        (uint32_t)key->source_id,
        (uint32_t)(key->source_id >> 32),
        (uint32_t)key->destination_id,
        (uint32_t)(key->destination_id >> 32),
        key->session_id,
        key->sequence,
        key->message_type,
        key->operation,
    };
    uint32_t seed = UINT32_C(0x811c9dc5);

    for (size_t i = 0u; i < sizeof(words) / sizeof(words[0]); i++) {
        seed ^= words[i];
        seed *= UINT32_C(0x01000193);
    }
    return seed == 0u ? UINT32_C(0x6d2b79f5) : seed;
}

static uint32_t retry_round_seed(uint32_t key_seed, uint16_t retry_round)
{
    uint32_t mixed = key_seed ^
                     ((uint32_t)retry_round * UINT32_C(0x9e3779b9));

    mixed ^= mixed >> 16;
    mixed *= UINT32_C(0x7feb352d);
    mixed ^= mixed >> 15;
    mixed *= UINT32_C(0x846ca68b);
    mixed ^= mixed >> 16;
    return mixed;
}

uint32_t app_mesh_rf_retry_next_delay_ms(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    uint32_t attempt_entropy)
{
    uint32_t key_seed;
    uint32_t attempt_seed;
    uint32_t delay_ms = 0u;
    uint16_t next_round;

    if (state == NULL || !retry_key_valid(key) ||
        policy > APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD) {
        return 0u;
    }

    if (!state->active || !retry_key_equal(&state->key, key)) {
        memset(state, 0, sizeof(*state));
        state->key = *key;
        state->active = true;
    }

    next_round = state->retry_round == UINT16_MAX ? UINT16_MAX :
                 (uint16_t)(state->retry_round + 1u);
    key_seed = retry_key_seed(key);
    attempt_seed = retry_round_seed(key_seed ^ attempt_entropy, next_round);
    if (attempt_seed == 0u) {
        attempt_seed = UINT32_C(0x6d2b79f5);
    }
    if (policy == APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN) {
        uint16_t retry_index = next_round - 1u;

        delay_ms = app_wake_train_politeness_backoff_ms(
            retry_index > UINT8_MAX ? UINT8_MAX : (uint8_t)retry_index,
            attempt_seed);
    } else if (node_comm_retry_backoff_ms(
                   policy == APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD ?
                       NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD :
                       NODE_COMM_PROFILE_DURABLE_RELIABLE_UPLINK,
                   attempt_seed,
                   next_round,
                   &delay_ms) != 0) {
        return 0u;
    }

    state->retry_round = next_round;
    return delay_ms;
}

void app_mesh_rf_retry_note_success(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key)
{
    app_mesh_rf_retry_forget(state, key);
}

void app_mesh_rf_retry_forget(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key)
{
    if (state != NULL && state->active && retry_key_equal(&state->key, key)) {
        app_mesh_rf_retry_reset(state);
    }
}

void app_mesh_rf_retry_reset(struct app_mesh_rf_retry_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static struct app_mesh_rf_retry_state *retry_bank_find(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key)
{
    if (bank == NULL || bank->states == NULL || bank->state_count == 0u) {
        return NULL;
    }

    for (size_t i = 0u; i < bank->state_count; i++) {
        if (bank->states[i].active &&
            retry_key_equal(&bank->states[i].key, key)) {
            return &bank->states[i];
        }
    }
    return NULL;
}

uint32_t app_mesh_rf_retry_bank_next_delay_ms(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    uint32_t attempt_entropy)
{
    struct app_mesh_rf_retry_state *state;

    if (!retry_key_valid(key)) {
        return 0u;
    }
    state = retry_bank_find(bank, key);
    if (state == NULL && bank != NULL && bank->states != NULL) {
        for (size_t i = 0u; i < bank->state_count; i++) {
            if (!bank->states[i].active) {
                state = &bank->states[i];
                break;
            }
        }
    }
    if (state == NULL && bank != NULL && bank->states != NULL &&
        bank->state_count > 0u) {
        state = &bank->states[bank->replacement_cursor % bank->state_count];
        bank->replacement_cursor++;
        app_mesh_rf_retry_reset(state);
    }
    return app_mesh_rf_retry_next_delay_ms(state, key, policy,
                                           attempt_entropy);
}

void app_mesh_rf_retry_bank_note_success(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key)
{
    app_mesh_rf_retry_bank_forget(bank, key);
}

void app_mesh_rf_retry_bank_forget(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key)
{
    struct app_mesh_rf_retry_state *state = retry_bank_find(bank, key);

    app_mesh_rf_retry_forget(state, key);
}

void app_mesh_rf_retry_bank_reset(struct app_mesh_rf_retry_bank *bank)
{
    if (bank == NULL || bank->states == NULL) {
        return;
    }
    for (size_t i = 0u; i < bank->state_count; i++) {
        app_mesh_rf_retry_reset(&bank->states[i]);
    }
    bank->replacement_cursor = 0u;
}
