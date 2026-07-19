#include "survey.h"

#include "dwm3000_timing.h"
#include "mesh_relay.h"
#include "operation_policy.h"
#include "survey_round_control.h"
#include "uwb.h"

#include <string.h>

_Static_assert(sizeof(struct survey_pair) == 24u,
               "survey pair public layout changed");
_Static_assert(SURVEY_GATEWAY_PAIR_MAX_RERUNS < UINT8_MAX,
               "survey pair rerun count must fit observability attempts");
_Static_assert(SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS >=
                   MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS,
               "survey control base must cover gateway-ACK custody retries");
_Static_assert(PROTO_TLV_U32_ENCODED_LEN + PROTO_TLV_U64_ENCODED_LEN +
                   PROTO_TLV_U16_ENCODED_LEN +
                   SURVEY_GATEWAY_MAX_PEERS_PER_REPORT *
                       (PROTO_TLV_HEADER_LEN + SURVEY_REACHABILITY_ENTRY_LEN) <=
                   UWB_MESH_MAX_PAYLOAD_LEN,
               "maximum survey report must fit one mesh payload");
_Static_assert(SURVEY_SAMPLE_TLV_MAX_LEN <= UWB_MESH_MAX_PAYLOAD_LEN,
               "round-owned survey sample must fit one mesh payload");

bool survey_sample_count_valid(uint16_t sample_count)
{
    return sample_count >= SURVEY_MIN_SAMPLE_COUNT &&
           sample_count <= SURVEY_MAX_SAMPLE_COUNT;
}

uint8_t survey_gateway_hop_count_from_report_ttl(uint8_t remaining_ttl)
{
    if (remaining_ttl == 0u || remaining_ttl > SURVEY_DEFAULT_TTL) {
        return 0u;
    }
    return (uint8_t)(SURVEY_DEFAULT_TTL - remaining_ttl + 1u);
}

uint32_t survey_pair_control_timeout_ms(uint8_t gateway_hop_count)
{
    if (gateway_hop_count == 0u ||
        gateway_hop_count > SURVEY_DEFAULT_TTL) {
        return SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
    }
    return SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS +
           ((uint32_t)(gateway_hop_count - 1u) *
            SURVEY_PAIR_CONTROL_PER_HOP_TIMEOUT_MS);
}

uint32_t survey_discovery_report_custody_ms(uint8_t gateway_hop_count)
{
    uint8_t effective_hop_count =
        gateway_hop_count == 0u || gateway_hop_count > SURVEY_DEFAULT_TTL ?
            SURVEY_DEFAULT_TTL : gateway_hop_count;

    return SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS +
           ((uint32_t)(effective_hop_count - 1u) *
            SURVEY_DISCOVERY_REPORT_CUSTODY_PER_ADDITIONAL_HOP_MS);
}

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

int survey_pair_validate(const struct survey_pair *pair)
{
    if (pair == NULL) {
        return PROTO_ERR_ARG;
    }
    if (pair->survey_id == 0u ||
        pair->initiator_id == 0u ||
        pair->responder_id == 0u ||
        pair->initiator_id == pair->responder_id ||
        !survey_sample_count_valid(pair->sample_count)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_sample_validate(const struct survey_sample *sample)
{
    int ret;

    if (sample == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_pair_validate(&sample->pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (sample->sample_index >= sample->pair.sample_count || sample->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    if (sample->range_status < RANGE_OK ||
        sample->range_status > RANGE_TIMING_INVALID) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

bool survey_sample_distance_usable(const struct survey_sample *sample)
{
    return sample != NULL && sample->range_status == RANGE_OK &&
           sample->distance_mm > SURVEY_MIN_USABLE_DISTANCE_MM;
}

bool survey_pair_missing_samples_all_unusable(
    uint16_t sample_count,
    uint16_t usable_mask,
    uint16_t initiator_unusable_mask,
    uint16_t responder_unusable_mask)
{
    uint16_t expected_mask;
    uint16_t missing_mask;

    if (!survey_sample_count_valid(sample_count) || sample_count > 16u) {
        return false;
    }
    expected_mask = sample_count == 16u ? UINT16_MAX :
                    (uint16_t)((UINT16_C(1) << sample_count) - 1u);
    missing_mask = expected_mask & (uint16_t)~usable_mask;
    return missing_mask != 0u &&
           (initiator_unusable_mask & responder_unusable_mask &
            missing_mask) == missing_mask;
}

static uint64_t survey_mix64(uint64_t value)
{
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value == 0u ? 1u : value;
}

uint64_t survey_sample_nonce(const struct survey_pair *pair, uint16_t sample_index)
{
    uint64_t value;

    if (survey_pair_validate(pair) != PROTO_OK ||
        sample_index >= pair->sample_count) {
        return 0u;
    }

    value = ((uint64_t)pair->survey_id << 32) ^
            pair->initiator_id ^
            (pair->responder_id << 1) ^
            ((uint64_t)(sample_index + 1u) << 16) ^
            ((uint64_t)sample_index << 48);
    return survey_mix64(value);
}

int survey_reachability_entry_validate(const struct survey_reachability_entry *entry)
{
    if (entry == NULL) {
        return PROTO_ERR_ARG;
    }
    if (entry->peer_id == 0u || entry->quality > 100u) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_discovery_config_validate(const struct survey_discovery_config *config)
{
    if (config == NULL) {
        return PROTO_ERR_ARG;
    }
    if (config->survey_id == 0u ||
        config->start_delay_ms == 0u ||
        config->start_delay_ms > SURVEY_DISCOVERY_MAX_START_DELAY_MS ||
        config->slot_ms < survey_discovery_probe_tx_budget_ms() +
                              SURVEY_DISCOVERY_RX_GUARD_MS ||
        config->slot_ms > SURVEY_DISCOVERY_MAX_SLOT_MS ||
        config->slot_count == 0u ||
        config->slot_count > SURVEY_DISCOVERY_MAX_SLOT_COUNT ||
        config->round_count == 0u ||
        config->round_count > SURVEY_DISCOVERY_MAX_ROUND_COUNT) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static uint32_t survey_discovery_nominal_duration_ms(
    const struct survey_discovery_config *config)
{
    uint64_t cursor = 0u;

    if (config == NULL) {
        return 0u;
    }
    cursor = (uint64_t)config->slot_ms * config->slot_count *
             config->round_count;
    return cursor > UINT32_MAX ? 0u : (uint32_t)cursor;
}

uint32_t survey_discovery_duration_ms(const struct survey_discovery_config *config)
{
    if (survey_discovery_config_validate(config) != PROTO_OK) {
        return 0u;
    }
    return survey_discovery_nominal_duration_ms(config);
}

uint8_t survey_discovery_opportunity_slot(uint64_t anchor_id,
                                          uint32_t survey_id,
                                          uint8_t opportunity,
                                          uint8_t slot_count)
{
    uint64_t seed;

    if (anchor_id == 0u || survey_id == 0u || slot_count == 0u ||
        opportunity >= SURVEY_DISCOVERY_MAX_ROUND_COUNT) {
        return 0u;
    }
    seed = anchor_id ^ ((uint64_t)survey_id << 17) ^
           ((uint64_t)(opportunity + 1u) * UINT64_C(0x9e3779b97f4a7c15));
    return (uint8_t)(survey_mix64(seed) % slot_count);
}

int survey_discovery_opportunity_window_ms(
    const struct survey_discovery_config *config,
    uint8_t opportunity,
    uint32_t *start_ms,
    uint32_t *end_ms)
{
    uint64_t round_ms;
    uint64_t cursor;

    if (survey_discovery_config_validate(config) != PROTO_OK ||
        opportunity >= config->round_count ||
        (start_ms == NULL && end_ms == NULL)) {
        return PROTO_ERR_ARG;
    }
    round_ms = (uint64_t)config->slot_ms * config->slot_count;
    cursor = round_ms * opportunity;
    if (cursor > UINT32_MAX || cursor + round_ms > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    if (start_ms != NULL) {
        *start_ms = (uint32_t)cursor;
    }
    if (end_ms != NULL) {
        *end_ms = (uint32_t)(cursor + round_ms);
    }
    return PROTO_OK;
}

int survey_discovery_opportunity_tx_ms(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t *tx_ms)
{
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t slot;
    uint64_t tx;

    if (tx_ms == NULL || anchor_id == 0u ||
        survey_discovery_opportunity_window_ms(config,
                                               opportunity,
                                               &start_ms,
                                               &end_ms) != PROTO_OK) {
        return PROTO_ERR_ARG;
    }
    slot = survey_discovery_opportunity_slot(anchor_id,
                                             config->survey_id,
                                             opportunity,
                                             config->slot_count);
    tx = (uint64_t)start_ms + (uint64_t)slot * config->slot_ms;
    if (tx >= end_ms || tx > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    *tx_ms = (uint32_t)tx;
    return PROTO_OK;
}

uint32_t survey_discovery_probe_tx_budget_ms(void)
{
    uint64_t airtime_us = dwm3000_timing_airtime_us_ceil(
        DWM3000_TIMING_PHY_CH5_WAKE, UWB_SURVEY_DISCOVERY_PROBE_LEN);
    uint64_t airtime_ms;

    if (airtime_us == 0u) {
        return UINT32_MAX;
    }
    airtime_ms = (airtime_us + 999u) / 1000u;
    airtime_ms = airtime_ms < SURVEY_DISCOVERY_TX_TIMEOUT_MS ?
                 SURVEY_DISCOVERY_TX_TIMEOUT_MS : airtime_ms;
    if (airtime_ms > UINT32_MAX - SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS) {
        return UINT32_MAX;
    }
    return (uint32_t)airtime_ms + SURVEY_DISCOVERY_TX_TRANSITION_GUARD_MS;
}

int survey_discovery_schedule_attempt(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t earliest_relative_ms,
    struct survey_discovery_attempt_schedule *schedule)
{
    uint32_t raw_tx_ms;
    uint32_t tx_budget_ms;
    int ret;

    if (schedule == NULL || anchor_id == 0u) {
        return PROTO_ERR_ARG;
    }
    memset(schedule, 0, sizeof(*schedule));
    ret = survey_discovery_opportunity_window_ms(config, opportunity,
                                                 &schedule->window_start_ms,
                                                 &schedule->window_end_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_discovery_opportunity_tx_ms(config, anchor_id, opportunity,
                                             &raw_tx_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    tx_budget_ms = survey_discovery_probe_tx_budget_ms();
    if (tx_budget_ms == UINT32_MAX ||
        UINT32_MAX - raw_tx_ms < config->slot_ms ||
        UINT32_MAX - raw_tx_ms < SURVEY_DISCOVERY_RX_GUARD_MS) {
        return PROTO_ERR_NO_SPACE;
    }

    schedule->tx_ms = raw_tx_ms + SURVEY_DISCOVERY_RX_GUARD_MS;
    schedule->slot_end_ms = raw_tx_ms + config->slot_ms;
    if (schedule->slot_end_ms < tx_budget_ms) {
        return PROTO_ERR_NO_SPACE;
    }
    schedule->latest_tx_start_ms = schedule->slot_end_ms - tx_budget_ms;
    if (schedule->tx_ms > schedule->latest_tx_start_ms ||
        earliest_relative_ms > schedule->latest_tx_start_ms) {
        return PROTO_ERR_BUSY;
    }
    return PROTO_OK;
}

static bool survey_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

int survey_pending_report_begin(struct survey_pending_report_state *state,
                                uint32_t survey_id,
                                uint32_t now_ms,
                                uint32_t earliest_attempt_ms)
{
    uint32_t deadline_base_ms;

    if (state == NULL || survey_id == 0u || state->active) {
        return PROTO_ERR_ARG;
    }
    deadline_base_ms = survey_time_reached(now_ms, earliest_attempt_ms) ?
                       now_ms : earliest_attempt_ms;
    memset(state, 0, sizeof(*state));
    state->survey_id = survey_id;
    state->next_attempt_ms = earliest_attempt_ms;
    state->deadline_ms = deadline_base_ms +
                         SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS;
    state->active = true;
    return PROTO_OK;
}

enum survey_pending_report_action survey_pending_report_action(
    const struct survey_pending_report_state *state,
    uint32_t now_ms)
{
    if (state == NULL || !state->active) {
        return SURVEY_PENDING_REPORT_IDLE;
    }
    if (survey_time_reached(now_ms, state->deadline_ms)) {
        return SURVEY_PENDING_REPORT_EXPIRED;
    }
    return survey_time_reached(now_ms, state->next_attempt_ms) ?
           SURVEY_PENDING_REPORT_ATTEMPT : SURVEY_PENDING_REPORT_WAIT;
}

uint32_t survey_pending_report_delay_ms(
    const struct survey_pending_report_state *state,
    uint32_t now_ms)
{
    uint32_t target_ms;

    if (state == NULL || !state->active) {
        return 0u;
    }
    target_ms = survey_time_reached(state->next_attempt_ms,
                                    state->deadline_ms) ?
                state->deadline_ms : state->next_attempt_ms;
    return survey_time_reached(now_ms, target_ms) ? 0u : target_ms - now_ms;
}

int survey_pending_report_note_temporary_failure(
    struct survey_pending_report_state *state,
    uint32_t now_ms)
{
    uint32_t backoff_ms = SURVEY_DISCOVERY_REPORT_RETRY_INITIAL_MS;
    uint8_t shift;

    if (state == NULL || !state->active) {
        return PROTO_ERR_ARG;
    }
    if (survey_time_reached(now_ms, state->deadline_ms)) {
        return PROTO_ERR_BUSY;
    }
    shift = state->retry_count > 3u ? 3u : (uint8_t)state->retry_count;
    backoff_ms <<= shift;
    if (backoff_ms > SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS) {
        backoff_ms = SURVEY_DISCOVERY_REPORT_RETRY_MAX_MS;
    }
    if (state->retry_count < UINT16_MAX) {
        state->retry_count++;
    }
    state->next_attempt_ms = now_ms + backoff_ms;
    if (survey_time_reached(state->next_attempt_ms, state->deadline_ms)) {
        state->next_attempt_ms = state->deadline_ms;
    }
    return PROTO_OK;
}

void survey_pending_report_clear(struct survey_pending_report_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int survey_discovery_timing_from_age(const struct survey_discovery_config *config,
                                     uint32_t message_age_ms,
                                     struct survey_discovery_timing *timing)
{
    uint32_t duration_ms;
    uint32_t end_age_ms;

    if (timing == NULL) {
        return PROTO_ERR_ARG;
    }
    if (survey_discovery_config_validate(config) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }

    memset(timing, 0, sizeof(*timing));
    duration_ms = survey_discovery_duration_ms(config);
    timing->duration_ms = duration_ms;

    if (message_age_ms < config->start_delay_ms) {
        timing->pending = true;
        timing->wait_ms = config->start_delay_ms - message_age_ms;
        return PROTO_OK;
    }

    if (UINT32_MAX - config->start_delay_ms < duration_ms) {
        end_age_ms = UINT32_MAX;
    } else {
        end_age_ms = config->start_delay_ms + duration_ms;
    }
    if (message_age_ms >= end_age_ms) {
        timing->expired = true;
        timing->elapsed_ms = duration_ms;
        return PROTO_OK;
    }

    timing->active = true;
    timing->elapsed_ms = message_age_ms - config->start_delay_ms;
    return PROTO_OK;
}

int survey_discovery_start_at_ms(const struct survey_discovery_timing *timing,
                                 uint32_t received_at_ms,
                                 uint32_t *start_at_ms)
{
    if (timing == NULL || start_at_ms == NULL || timing->expired ||
        timing->pending == timing->active) {
        return PROTO_ERR_ARG;
    }

    *start_at_ms = timing->pending ? received_at_ms + timing->wait_ms :
                                    received_at_ms - timing->elapsed_ms;
    return PROTO_OK;
}

int survey_discovery_report_delay_ms(const struct survey_discovery_config *config,
                                     uint8_t anchor_slot,
                                     uint32_t report_slot_ms,
                                     uint32_t *delay_ms)
{
    uint32_t discovery_duration_ms;
    uint64_t delay;

    if (delay_ms == NULL) {
        return PROTO_ERR_ARG;
    }
    if (survey_discovery_config_validate(config) != PROTO_OK ||
        report_slot_ms == 0u ||
        anchor_slot >= config->slot_count) {
        return PROTO_ERR_MALFORMED;
    }

    discovery_duration_ms = survey_discovery_duration_ms(config);
    delay = (uint64_t)discovery_duration_ms +
            ((uint64_t)anchor_slot * report_slot_ms);
    if (delay > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }

    *delay_ms = (uint32_t)delay;
    return PROTO_OK;
}

static int survey_find_u16_tlv(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int survey_find_u8_tlv(const uint8_t *payload,
                              size_t payload_len,
                              uint8_t type,
                              uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = tlv_value[0];
    return PROTO_OK;
}

static int survey_find_u32_tlv(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int survey_find_u64_tlv(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u64_le(tlv_value);
    return PROTO_OK;
}

int survey_gateway_begin(struct survey_gateway_context *context,
                         uint32_t survey_id,
                         uint16_t sample_count)
{
    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (survey_id == 0u || !survey_sample_count_valid(sample_count)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(context, 0, sizeof(*context));
    context->survey_id = survey_id;
    context->sample_count = sample_count;
    return PROTO_OK;
}

static struct survey_gateway_report_slot *
survey_gateway_find_report(struct survey_gateway_context *context,
                           uint64_t anchor_id)
{
    for (size_t i = 0u; i < context->report_count; i++) {
        if (context->reports[i].valid &&
            context->reports[i].anchor_id == anchor_id) {
            return &context->reports[i];
        }
    }
    return NULL;
}

int survey_gateway_note_reach_report_with_reverse_hint(
    struct survey_gateway_context *context,
    uint32_t survey_id,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    const struct survey_gateway_reverse_hint *reverse_hint)
{
    struct survey_gateway_report_slot *slot;

    if (context == NULL || (entries == NULL && entry_count != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u || survey_id != context->survey_id) {
        return PROTO_ERR_STALE;
    }
    if (anchor_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (reverse_hint != NULL &&
        (!reverse_hint->valid ||
         reverse_hint->target_id != anchor_id ||
         reverse_hint->next_hop_id == 0u ||
         reverse_hint->hop_count > SURVEY_DEFAULT_TTL ||
         reverse_hint->quality > 100u)) {
        return PROTO_ERR_MALFORMED;
    }
    if (entry_count > SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
        return PROTO_ERR_NO_SPACE;
    }

    for (size_t i = 0u; i < entry_count; i++) {
        int ret = survey_reachability_entry_validate(&entries[i]);

        if (ret != PROTO_OK) {
            return ret;
        }
        if (entries[i].peer_id == anchor_id) {
            return PROTO_ERR_MALFORMED;
        }
    }

    slot = survey_gateway_find_report(context, anchor_id);
    if (slot != NULL) {
        return PROTO_OK;
    }
    if (context->report_count >= SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_NO_SPACE;
    }
    slot = &context->reports[context->report_count];
    context->report_count++;

    memset(slot, 0, sizeof(*slot));
    slot->anchor_id = anchor_id;
    slot->entry_count = entry_count;
    slot->valid = true;
    if (reverse_hint != NULL) {
        slot->reverse_next_hop_id = reverse_hint->next_hop_id;
        slot->reverse_quality = reverse_hint->quality;
        slot->reverse_hop_count = reverse_hint->hop_count;
        slot->reverse_hint_valid = true;
    }
    if (entry_count > 0u) {
        memcpy(slot->entries, entries, entry_count * sizeof(entries[0]));
    }
    context->pairs_planned = false;
    context->pair_count = 0u;
    context->next_pair_index = 0u;
    return PROTO_OK;
}

int survey_gateway_note_reach_report(struct survey_gateway_context *context,
                                     uint32_t survey_id,
                                     uint64_t anchor_id,
                                     const struct survey_reachability_entry *entries,
                                     size_t entry_count)
{
    return survey_gateway_note_reach_report_with_reverse_hint(context,
                                                              survey_id,
                                                              anchor_id,
                                                              entries,
                                                              entry_count,
                                                              NULL);
}

int survey_gateway_reverse_hint_for_target(
    const struct survey_gateway_context *context,
    uint64_t target_id,
    struct survey_gateway_reverse_hint *reverse_hint)
{
    if (context == NULL || reverse_hint == NULL) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u) {
        return PROTO_ERR_STALE;
    }
    if (target_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t i = 0u; i < context->report_count; i++) {
        const struct survey_gateway_report_slot *slot = &context->reports[i];

        if (slot->valid &&
            slot->anchor_id == target_id &&
            slot->reverse_hint_valid) {
            *reverse_hint = (struct survey_gateway_reverse_hint) {
                .target_id = slot->anchor_id,
                .next_hop_id = slot->reverse_next_hop_id,
                .quality = slot->reverse_quality,
                .hop_count = slot->reverse_hop_count,
                .valid = true,
            };
            return PROTO_OK;
        }
    }
    return PROTO_ERR_NOT_FOUND;
}

int survey_gateway_pair_at(const struct survey_gateway_context *context,
                           size_t pair_index,
                           struct survey_pair *pair)
{
    if (context == NULL || pair == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!context->pairs_planned) {
        return PROTO_ERR_STALE;
    }
    if (pair_index >= context->pair_count) {
        return PROTO_ERR_NOT_FOUND;
    }

    *pair = (struct survey_pair) {
        .survey_id = context->survey_id,
        .initiator_id = context->pairs[pair_index].initiator_id,
        .responder_id = context->pairs[pair_index].responder_id,
        .sample_count = context->sample_count,
    };
    return PROTO_OK;
}

int survey_gateway_next_pair(struct survey_gateway_context *context,
                             struct survey_pair *pair)
{
    int ret;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_gateway_pair_at(context, context->next_pair_index, pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    context->next_pair_index++;
    return PROTO_OK;
}

static bool survey_gateway_auto_status_valid(enum command_status status)
{
    return status >= COMMAND_OK && status <= COMMAND_INTERNAL_ERROR;
}

static int survey_gateway_auto_stage_command(enum survey_gateway_auto_stage stage,
                                             enum command_id *command_id)
{
    if (command_id == NULL) {
        return PROTO_ERR_ARG;
    }

    switch (stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        *command_id = CMD_SURVEY_PREPARE_PAIR;
        return PROTO_OK;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        *command_id = CMD_SURVEY_START_PAIR;
        return PROTO_OK;
    default:
        return PROTO_ERR_STALE;
    }
}

static int survey_gateway_auto_stage_target(const struct survey_gateway_auto_context *context,
                                            uint64_t *target_id)
{
    if (context == NULL || target_id == NULL) {
        return PROTO_ERR_ARG;
    }

    switch (context->stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        *target_id = context->pair.initiator_id;
        return PROTO_OK;
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
        *target_id = context->pair.responder_id;
        return PROTO_OK;
    default:
        return PROTO_ERR_STALE;
    }
}

int survey_gateway_auto_begin(struct survey_gateway_auto_context *context)
{
    if (context == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(context, 0, sizeof(*context));
    context->stage = SURVEY_GATEWAY_AUTO_IDLE;
    return PROTO_OK;
}

int survey_gateway_auto_next_action(struct survey_gateway_auto_context *auto_context,
                                    struct survey_gateway_context *gateway_context,
                                    struct survey_gateway_auto_action *action)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    uint64_t target_id = 0u;
    int ret;

    if (auto_context == NULL || gateway_context == NULL || action == NULL) {
        return PROTO_ERR_ARG;
    }
    if (auto_context->waiting) {
        return PROTO_ERR_BUSY;
    }

    memset(action, 0, sizeof(*action));
    if (!auto_context->running) {
        auto_context->running = true;
        auto_context->stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
    }
    if (auto_context->stage == SURVEY_GATEWAY_AUTO_IDLE ||
        auto_context->stage == SURVEY_GATEWAY_AUTO_LOAD_PAIR) {
        ret = survey_gateway_next_pair(gateway_context, &auto_context->pair);
        if (ret == PROTO_ERR_NOT_FOUND) {
            auto_context->running = false;
            auto_context->stage = SURVEY_GATEWAY_AUTO_IDLE;
            action->complete = true;
            return PROTO_OK;
        }
        if (ret != PROTO_OK) {
            return ret;
        }
        auto_context->pair_reruns_started = 0u;
        auto_context->stage = SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
    }

    ret = survey_gateway_auto_stage_command(auto_context->stage, &command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_gateway_auto_stage_target(auto_context, &target_id);
    if (ret != PROTO_OK) {
        return ret;
    }

    action->pair = auto_context->pair;
    action->stage = auto_context->stage;
    action->command_id = command_id;
    action->target_id = target_id;
    return PROTO_OK;
}

bool survey_gateway_auto_no_unstarted_pairs(
    const struct survey_gateway_auto_context *auto_context,
    const struct survey_gateway_context *gateway_context)
{
    return auto_context != NULL && gateway_context != NULL &&
           auto_context->running && !auto_context->waiting &&
           auto_context->stage == SURVEY_GATEWAY_AUTO_LOAD_PAIR &&
           gateway_context->next_pair_index >= gateway_context->pair_count;
}

int survey_gateway_auto_mark_waiting(struct survey_gateway_auto_context *context)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    uint64_t target_id = 0u;
    int ret;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!context->running) {
        return PROTO_ERR_STALE;
    }
    if (context->waiting) {
        return PROTO_ERR_BUSY;
    }

    ret = survey_gateway_auto_stage_command(context->stage, &command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_gateway_auto_stage_target(context, &target_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (command_id == CMD_VENDOR_BASE || target_id == 0u) {
        return PROTO_ERR_STALE;
    }

    context->waiting = true;
    return PROTO_OK;
}

bool survey_gateway_auto_command_matches(const struct survey_gateway_auto_context *context,
                                         enum command_id command_id,
                                         uint64_t target_id,
                                         uint32_t survey_id)
{
    enum command_id expected_command = CMD_VENDOR_BASE;
    uint64_t expected_target = 0u;

    if (context == NULL || !context->running || !context->waiting ||
        survey_id == 0u || target_id == 0u) {
        return false;
    }
    if (survey_gateway_auto_stage_command(context->stage, &expected_command) != PROTO_OK ||
        survey_gateway_auto_stage_target(context, &expected_target) != PROTO_OK) {
        return false;
    }

    return command_id == expected_command &&
           target_id == expected_target &&
           survey_id == context->pair.survey_id;
}

int survey_gateway_auto_retry_pending(struct survey_gateway_auto_context *context,
                                      enum command_id command_id,
                                      uint64_t target_id,
                                      uint32_t survey_id)
{
    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!context->waiting ||
        !survey_gateway_auto_command_matches(context,
                                             command_id,
                                             target_id,
                                             survey_id)) {
        return PROTO_ERR_NOT_FOUND;
    }

    context->waiting = false;
    return PROTO_OK;
}

int survey_gateway_auto_rerun_pair(
    struct survey_gateway_auto_context *context)
{
    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (context->waiting) {
        return PROTO_ERR_BUSY;
    }
    if (!context->running || context->stage != SURVEY_GATEWAY_AUTO_LOAD_PAIR ||
        survey_pair_validate(&context->pair) != PROTO_OK) {
        return PROTO_ERR_STALE;
    }
    if (context->pair_reruns_started >= SURVEY_GATEWAY_PAIR_MAX_RERUNS) {
        return PROTO_ERR_NO_SPACE;
    }

    context->pair_reruns_started++;
    context->stage = SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR;
    return PROTO_OK;
}

int survey_gateway_auto_note_result(struct survey_gateway_auto_context *context,
                                    enum command_id command_id,
                                    uint64_t target_id,
                                    uint32_t survey_id,
                                    enum command_status status,
                                    bool *pair_launched,
                                    bool *pair_skipped)
{
    bool launched = false;
    bool skipped = false;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!survey_gateway_auto_status_valid(status)) {
        return PROTO_ERR_MALFORMED;
    }
    if (!survey_gateway_auto_command_matches(context, command_id, target_id, survey_id)) {
        return PROTO_ERR_NOT_FOUND;
    }

    context->waiting = false;
    if (status != COMMAND_OK) {
        context->stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
        skipped = true;
    } else {
        switch (context->stage) {
        case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
            context->stage = SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER;
            break;
        case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
            context->stage = SURVEY_GATEWAY_AUTO_START_RESPONDER;
            break;
        case SURVEY_GATEWAY_AUTO_START_RESPONDER:
            context->stage = SURVEY_GATEWAY_AUTO_START_INITIATOR;
            break;
        case SURVEY_GATEWAY_AUTO_START_INITIATOR:
            context->stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
            launched = true;
            break;
        default:
            return PROTO_ERR_STALE;
        }
    }

    if (pair_launched != NULL) {
        *pair_launched = launched;
    }
    if (pair_skipped != NULL) {
        *pair_skipped = skipped;
    }
    return PROTO_OK;
}

int survey_extract_reach_request_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t *survey_id,
                                      uint32_t *duration_ms)
{
    int ret;

    ret = survey_find_u32_tlv(payload, payload_len, TLV_SURVEY_ID, survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u32_tlv(payload, payload_len, TLV_DURATION_MS, duration_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    return *survey_id == 0u || *duration_ms == 0u ? PROTO_ERR_MALFORMED : PROTO_OK;
}

int survey_extract_reach_report_tlvs(const uint8_t *payload,
                                     size_t payload_len,
                                     uint32_t *survey_id,
                                     uint64_t *anchor_id,
                                     struct survey_reachability_entry *entries,
                                     size_t entry_cap,
                                     size_t *entry_count)
{
    size_t offset = 0u;
    int ret;

    if (payload == NULL || survey_id == NULL || anchor_id == NULL ||
        entry_count == NULL || (entries == NULL && entry_cap != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = survey_find_u32_tlv(payload, payload_len, TLV_SURVEY_ID, survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u64_tlv(payload, payload_len, TLV_ANCHOR_ID, anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (*survey_id == 0u || *anchor_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    *entry_count = 0u;
    while (offset < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        len = payload[offset + 1u];
        offset += 2u;
        if (payload_len - offset < len) {
            return PROTO_ERR_MALFORMED;
        }

        if (type == TLV_REACHABILITY_ENTRY) {
            struct survey_reachability_entry entry;

            if (len != SURVEY_REACHABILITY_ENTRY_LEN) {
                return PROTO_ERR_MALFORMED;
            }
            entry.peer_id = proto_get_u64_le(&payload[offset]);
            entry.rssi_dbm = (int8_t)payload[offset + 8u];
            entry.quality = payload[offset + 9u];
            ret = survey_reachability_entry_validate(&entry);
            if (ret != PROTO_OK) {
                return ret;
            }
            if (entry.peer_id == *anchor_id) {
                return PROTO_ERR_MALFORMED;
            }
            if (*entry_count >= entry_cap) {
                return PROTO_ERR_NO_SPACE;
            }
            entries[*entry_count] = entry;
            (*entry_count)++;
        }

        offset += len;
    }

    return PROTO_OK;
}

int survey_extract_discovery_start_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        struct survey_discovery_config *config)
{
    struct operation_policy_set policies;
    const uint8_t *slot_count_value = NULL;
    uint8_t slot_count_len = 0u;
    uint32_t duration_ms = 0u;
    int ret;

    if (payload == NULL || config == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(config, 0, sizeof(*config));
    ret = survey_find_u32_tlv(payload, payload_len, TLV_SURVEY_ID, &config->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u32_tlv(payload,
                              payload_len,
                              TLV_DISCOVERY_START_DELAY_MS,
                              &config->start_delay_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u16_tlv(payload,
                              payload_len,
                              TLV_DISCOVERY_SLOT_MS,
                              &config->slot_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_find(payload,
                   payload_len,
                   TLV_DISCOVERY_SLOT_COUNT,
                   &slot_count_value,
                   &slot_count_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (slot_count_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }
    config->slot_count = slot_count_value[0];
    config->round_count = SURVEY_DISCOVERY_MAX_ROUND_COUNT;
    ret = operation_policy_set_from_tlvs(payload, payload_len, &policies);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (policies.discovery_present) {
        if (policies.discovery.start_delay_ms != config->start_delay_ms ||
            policies.discovery.slot_ms != config->slot_ms ||
            policies.discovery.slot_count != config->slot_count) {
            return PROTO_ERR_MALFORMED;
        }
        config->round_count = policies.discovery.round_count;
    }
    ret = survey_find_u32_tlv(payload, payload_len, TLV_DURATION_MS, &duration_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (survey_discovery_config_validate(config) != PROTO_OK ||
        duration_ms != survey_discovery_duration_ms(config)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_extract_discovery_slot_count_tlv(const uint8_t *payload,
                                            size_t payload_len,
                                            uint8_t default_slot_count,
                                            uint8_t *slot_count)
{
    const uint8_t *slot_count_value = NULL;
    uint8_t slot_count_len = 0u;
    uint8_t value = default_slot_count;
    int ret;

    if (payload == NULL || slot_count == NULL) {
        return PROTO_ERR_ARG;
    }
    if (default_slot_count == 0u ||
        default_slot_count > SURVEY_DISCOVERY_MAX_SLOT_COUNT) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_find(payload,
                   payload_len,
                   TLV_DISCOVERY_SLOT_COUNT,
                   &slot_count_value,
                   &slot_count_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *slot_count = value;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (slot_count_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    value = slot_count_value[0];
    if (value == 0u || value > SURVEY_DISCOVERY_MAX_SLOT_COUNT) {
        return PROTO_ERR_MALFORMED;
    }

    *slot_count = value;
    return PROTO_OK;
}

int survey_extract_ml_anchor_pair_request_tlvs(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t default_slot_count,
    struct survey_ml_anchor_pair_request *request)
{
    const uint8_t *slot_count_value = NULL;
    uint8_t slot_count_len = 0u;
    uint8_t slot_count = default_slot_count;
    int ret;

    if (payload == NULL || request == NULL) {
        return PROTO_ERR_ARG;
    }
    if (default_slot_count < SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT ||
        default_slot_count > SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_MALFORMED;
    }

    memset(request, 0, sizeof(*request));
    ret = tlv_find(payload,
                   payload_len,
                   TLV_DISCOVERY_SLOT_COUNT,
                   &slot_count_value,
                   &slot_count_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        request->discovery_slot_count = slot_count;
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (slot_count_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    slot_count = slot_count_value[0];
    if (slot_count < SURVEY_ML_ANCHOR_PAIR_MIN_DISCOVERY_SLOT_COUNT ||
        slot_count > SURVEY_ML_ANCHOR_PAIR_MAX_DISCOVERY_SLOT_COUNT) {
        return PROTO_ERR_MALFORMED;
    }

    request->discovery_slot_count = slot_count;
    return PROTO_OK;
}

int survey_extract_pair_tlvs(const uint8_t *payload,
                             size_t payload_len,
                             struct survey_pair *pair)
{
    int ret;

    if (pair == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_find_u32_tlv(payload, payload_len, TLV_SURVEY_ID, &pair->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &pair->initiator_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &pair->responder_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u16_tlv(payload, payload_len, TLV_SAMPLE_COUNT, &pair->sample_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    return survey_pair_validate(pair);
}

int survey_append_reach_request_tlvs(uint8_t *payload,
                                          size_t payload_cap,
                                          size_t *offset,
                                          uint32_t survey_id,
                                          uint32_t duration_ms)
{
    int ret;

    if (survey_id == 0u || duration_ms == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset, TLV_DURATION_MS, duration_ms);
}

int survey_append_discovery_start_tlvs(uint8_t *payload,
                                       size_t payload_cap,
                                       size_t *offset,
                                       const struct survey_discovery_config *config)
{
    uint32_t duration_ms;
    int ret;

    ret = survey_discovery_config_validate(config);
    if (ret != PROTO_OK) {
        return ret;
    }
    duration_ms = survey_discovery_duration_ms(config);

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, config->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         offset,
                         TLV_DISCOVERY_START_DELAY_MS,
                         config->start_delay_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload,
                         payload_cap,
                         offset,
                         TLV_DISCOVERY_SLOT_MS,
                         config->slot_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload,
                        payload_cap,
                        offset,
                        TLV_DISCOVERY_SLOT_COUNT,
                        config->slot_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload, payload_cap, offset, TLV_DURATION_MS, duration_ms);
}

int survey_append_reachability_entry_tlv(uint8_t *payload,
                                              size_t payload_cap,
                                              size_t *offset,
                                              const struct survey_reachability_entry *entry)
{
    uint8_t raw[SURVEY_REACHABILITY_ENTRY_LEN];
    int ret;

    ret = survey_reachability_entry_validate(entry);
    if (ret != PROTO_OK) {
        return ret;
    }

    proto_put_u64_le(raw, entry->peer_id);
    raw[8] = (uint8_t)entry->rssi_dbm;
    raw[9] = entry->quality;
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_REACHABILITY_ENTRY,
                            raw,
                            sizeof(raw));
}

int survey_append_reach_report_tlvs(uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *offset,
                                         uint32_t survey_id,
                                         uint64_t anchor_id,
                                         const struct survey_reachability_entry *entries,
                                         size_t entry_count)
{
    int ret;

    if (survey_id == 0u || anchor_id == 0u ||
        (entries == NULL && entry_count != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_ANCHOR_ID, anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    for (size_t i = 0u; i < entry_count; i++) {
        ret = survey_append_reachability_entry_tlv(payload,
                                                   payload_cap,
                                                   offset,
                                                   &entries[i]);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return PROTO_OK;
}

int survey_append_pair_tlvs(uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *offset,
                                 const struct survey_pair *pair)
{
    int ret;

    ret = survey_pair_validate(pair);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = tlv_append_u32(payload, payload_cap, offset, TLV_SURVEY_ID, pair->survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_INITIATOR_ID, pair->initiator_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, offset, TLV_RESPONDER_ID, pair->responder_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_COUNT, pair->sample_count);
}

int survey_append_sample_tlvs(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct survey_sample *sample)
{
    int ret;

    ret = survey_sample_validate(sample);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = survey_append_pair_tlvs(payload, payload_cap, offset, &sample->pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (sample->round_id != SURVEY_LEGACY_ROUND_ID) {
        ret = survey_round_id_append_tlv(payload, payload_cap, offset,
                                         sample->round_id);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_SAMPLE_INDEX, sample->sample_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_i32(payload, payload_cap, offset, TLV_DISTANCE_MM, sample->distance_mm);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, offset, TLV_QUALITY, sample->quality);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_RANGE_STATUS, (uint8_t)sample->range_status);
}

int survey_extract_sample_tlvs(const uint8_t *payload,
                               size_t payload_len,
                               struct survey_sample *sample)
{
    struct survey_sample parsed = {0};
    uint32_t distance_mm;
    uint8_t range_status;
    int ret;

    if (payload == NULL || sample == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_extract_pair_tlvs(payload, payload_len, &parsed.pair);
    if (ret == PROTO_OK) {
        ret = survey_round_id_extract_tlv(payload, payload_len,
                                          &parsed.round_id);
    }
    if (ret == PROTO_OK) {
        ret = survey_find_u16_tlv(payload, payload_len, TLV_SAMPLE_INDEX,
                                  &parsed.sample_index);
    }
    if (ret == PROTO_OK) {
        ret = survey_find_u32_tlv(payload, payload_len, TLV_DISTANCE_MM,
                                  &distance_mm);
        if (ret == PROTO_OK) {
            parsed.distance_mm = (int32_t)distance_mm;
        }
    }
    if (ret == PROTO_OK) {
        ret = survey_find_u8_tlv(payload, payload_len, TLV_QUALITY,
                                 &parsed.quality);
    }
    if (ret == PROTO_OK) {
        ret = survey_find_u8_tlv(payload, payload_len, TLV_RANGE_STATUS,
                                 &range_status);
        if (ret == PROTO_OK) {
            parsed.range_status = (enum range_status)range_status;
        }
    }
    if (ret == PROTO_OK) {
        ret = survey_sample_validate(&parsed);
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    *sample = parsed;
    return PROTO_OK;
}

int survey_init_result_packet_from_reporter(struct proto_packet *packet,
                                            const struct survey_sample *sample,
                                            uint64_t reporter_id,
                                            uint64_t gateway_id,
                                            uint16_t seq,
                                            uint8_t payload_len)
{
    int ret;

    if (packet == NULL || gateway_id == 0u || reporter_id == 0u) {
        return PROTO_ERR_ARG;
    }

    ret = survey_sample_validate(sample);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (reporter_id != sample->pair.initiator_id &&
        reporter_id != sample->pair.responder_id) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SURVEY_PAIR_RESULT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = reporter_id;
    packet->dst_id = gateway_id;
    packet->session_id = sample->pair.survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int survey_init_result_packet(struct proto_packet *packet,
                                   const struct survey_sample *sample,
                                   uint64_t gateway_id,
                                   uint16_t seq,
                                   uint8_t payload_len)
{
    if (sample == NULL) {
        return PROTO_ERR_ARG;
    }
    return survey_init_result_packet_from_reporter(packet,
                                                   sample,
                                                   sample->pair.initiator_id,
                                                   gateway_id,
                                                   seq,
                                                   payload_len);
}

int survey_init_reach_request_packet(struct proto_packet *packet,
                                     uint64_t gateway_id,
                                     uint32_t survey_id,
                                     uint16_t seq,
                                     uint8_t payload_len)
{
    if (packet == NULL || gateway_id == 0u || survey_id == 0u) {
        return PROTO_ERR_ARG;
    }

    packet->msg_type = MSG_SURVEY_REACH_REQ;
    packet->flags = FLAG_DIAGNOSTIC;
    packet->src_id = gateway_id;
    packet->dst_id = 0u;
    packet->session_id = survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int survey_init_reach_report_packet(struct proto_packet *packet,
                                         uint64_t anchor_id,
                                         uint64_t gateway_id,
                                         uint32_t survey_id,
                                         uint16_t seq,
                                         uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || survey_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SURVEY_REACH_REPORT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int survey_init_discovery_start_packet(struct proto_packet *packet,
                                       uint64_t gateway_id,
                                       const struct survey_discovery_config *config,
                                       uint16_t seq,
                                       uint8_t payload_len)
{
    int ret;

    if (packet == NULL || gateway_id == 0u) {
        return PROTO_ERR_ARG;
    }
    ret = survey_discovery_config_validate(config);
    if (ret != PROTO_OK) {
        return ret;
    }

    packet->msg_type = MSG_SURVEY_DISCOVERY_START;
    packet->flags = FLAG_DIAGNOSTIC;
    packet->src_id = gateway_id;
    packet->dst_id = 0u;
    packet->session_id = config->survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int survey_init_discovery_report_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t survey_id,
                                        uint16_t seq,
                                        uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || survey_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_SURVEY_DISCOVERY_REPORT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet->src_id = anchor_id;
    packet->dst_id = gateway_id;
    packet->session_id = survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}

int survey_init_pair_prepare_packet(struct proto_packet *packet,
                                    const struct survey_pair *pair,
                                    uint64_t gateway_id,
                                    uint16_t seq,
                                    uint8_t payload_len)
{
    int ret;

    if (packet == NULL || gateway_id == 0u) {
        return PROTO_ERR_ARG;
    }

    ret = survey_pair_validate(pair);
    if (ret != PROTO_OK) {
        return ret;
    }

    packet->msg_type = MSG_SURVEY_PAIR_PREPARE;
    packet->flags = FLAG_DIAGNOSTIC;
    packet->src_id = gateway_id;
    packet->dst_id = pair->initiator_id;
    packet->session_id = pair->survey_id;
    packet->seq = seq;
    packet->ttl = SURVEY_DEFAULT_TTL;
    packet->payload_len = payload_len;
    packet->message_age_ms = 0u;
    return PROTO_OK;
}
