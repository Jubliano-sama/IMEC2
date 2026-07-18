#include "survey.h"

#include "dwm3000_timing.h"
#include "mesh_relay.h"
#include "node_comm.h"
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

static int survey_plan_pairs_into_gateway_context(
    struct survey_gateway_context *context,
    const struct survey_reachability_report *reports,
    size_t report_count);

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
        config->slot_count > SURVEY_DISCOVERY_MAX_SLOT_COUNT) {
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
    for (uint8_t opportunity = 0u;
         opportunity < SURVEY_DISCOVERY_OPPORTUNITY_COUNT;
         opportunity++) {
        uint32_t base_ms = opportunity == 0u ? 0u :
            SURVEY_DISCOVERY_RETRY_BASE_MS << (opportunity - 1u);

        cursor += (uint64_t)base_ms * 2u +
                  (uint64_t)config->slot_ms * config->slot_count;
    }
    return cursor > UINT32_MAX ? 0u : (uint32_t)cursor;
}

uint32_t survey_discovery_duration_ms(const struct survey_discovery_config *config)
{
    uint32_t nominal_duration_ms;

    if (survey_discovery_config_validate(config) != PROTO_OK) {
        return 0u;
    }
    nominal_duration_ms = survey_discovery_nominal_duration_ms(config);
    if (nominal_duration_ms == 0u ||
        nominal_duration_ms > UINT32_MAX / 2u) {
        return 0u;
    }
    /*
     * The second, equally sized horizon is continuous receive time and the
     * bounded retry envelope for nominal opportunities refused before RF.
     * Runtime retries use identity-seeded exponential jitter inside this
     * envelope; the fixed end keeps gateway collection timing deterministic.
     */
    return nominal_duration_ms * 2u;
}

uint8_t survey_discovery_opportunity_slot(uint64_t anchor_id,
                                          uint32_t survey_id,
                                          uint8_t opportunity,
                                          uint8_t slot_count)
{
    uint64_t seed;

    if (anchor_id == 0u || survey_id == 0u || slot_count == 0u ||
        opportunity >= SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
        return 0u;
    }
    seed = anchor_id ^ ((uint64_t)survey_id << 17) ^
           ((uint64_t)(opportunity + 1u) * UINT64_C(0x9e3779b97f4a7c15));
    return (uint8_t)(survey_mix64(seed) % slot_count);
}

static uint32_t survey_discovery_retry_base_ms(uint8_t opportunity)
{
    return opportunity == 0u ? 0u :
           SURVEY_DISCOVERY_RETRY_BASE_MS << (opportunity - 1u);
}

int survey_discovery_opportunity_window_ms(
    const struct survey_discovery_config *config,
    uint8_t opportunity,
    uint32_t *start_ms,
    uint32_t *end_ms)
{
    uint64_t cursor = 0u;

    if (survey_discovery_config_validate(config) != PROTO_OK ||
        opportunity >= SURVEY_DISCOVERY_OPPORTUNITY_COUNT ||
        (start_ms == NULL && end_ms == NULL)) {
        return PROTO_ERR_ARG;
    }
    for (uint8_t current = 0u; current <= opportunity; current++) {
        uint32_t base_ms = survey_discovery_retry_base_ms(current);
        uint64_t window_ms = (uint64_t)base_ms * 2u +
                             (uint64_t)config->slot_ms * config->slot_count;

        if (current == opportunity) {
            if (cursor > UINT32_MAX || cursor + window_ms > UINT32_MAX) {
                return PROTO_ERR_NO_SPACE;
            }
            if (start_ms != NULL) {
                *start_ms = (uint32_t)cursor;
            }
            if (end_ms != NULL) {
                *end_ms = (uint32_t)(cursor + window_ms);
            }
            return PROTO_OK;
        }
        cursor += window_ms;
    }
    return PROTO_ERR_ARG;
}

int survey_discovery_opportunity_tx_ms(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t *tx_ms)
{
    uint32_t start_ms;
    uint32_t end_ms;
    uint32_t base_ms;
    uint32_t jitter_ms = 0u;
    uint8_t slot;
    uint64_t tx;

    if (tx_ms == NULL || anchor_id == 0u ||
        survey_discovery_opportunity_window_ms(config,
                                               opportunity,
                                               &start_ms,
                                               &end_ms) != PROTO_OK) {
        return PROTO_ERR_ARG;
    }
    base_ms = survey_discovery_retry_base_ms(opportunity);
    slot = survey_discovery_opportunity_slot(anchor_id,
                                             config->survey_id,
                                             opportunity,
                                             config->slot_count);
    if (base_ms != 0u) {
        uint64_t seed = anchor_id ^ ((uint64_t)config->survey_id << 29) ^
                        ((uint64_t)(opportunity + 1u) *
                         UINT64_C(0xd1b54a32d192ed03));

        jitter_ms = (uint32_t)(survey_mix64(seed) % base_ms);
    }
    tx = (uint64_t)start_ms + base_ms + jitter_ms +
         (uint64_t)slot * config->slot_ms;
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
    uint32_t nominal_duration_ms;
    uint32_t raw_tx_ms;
    uint32_t tx_budget_ms;
    uint32_t offset_ms = 0u;
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
        nominal_duration_ms = survey_discovery_nominal_duration_ms(config);
        if (nominal_duration_ms == 0u) {
            return PROTO_ERR_NO_SPACE;
        }
        offset_ms = nominal_duration_ms;
        if (UINT32_MAX - schedule->window_start_ms < offset_ms ||
            UINT32_MAX - schedule->window_end_ms < offset_ms ||
            UINT32_MAX - schedule->tx_ms < offset_ms ||
            UINT32_MAX - schedule->latest_tx_start_ms < offset_ms ||
            UINT32_MAX - schedule->slot_end_ms < offset_ms) {
            return PROTO_ERR_NO_SPACE;
        }
        schedule->window_start_ms += offset_ms;
        schedule->window_end_ms += offset_ms;
        schedule->tx_ms += offset_ms;
        schedule->latest_tx_start_ms += offset_ms;
        schedule->slot_end_ms += offset_ms;
        schedule->deferred = true;
        if (earliest_relative_ms > schedule->latest_tx_start_ms) {
            return PROTO_ERR_BUSY;
        }
    }
    return PROTO_OK;
}

static uint32_t survey_discovery_probe_retry_seed(
    uint64_t anchor_id,
    uint32_t survey_id,
    uint8_t opportunity)
{
    uint64_t mixed = survey_mix64(
        anchor_id ^ ((uint64_t)survey_id << 21) ^
        ((uint64_t)MSG_UWB_SURVEY_DISCOVERY_PROBE << 48) ^
        ((uint64_t)(opportunity + 1u) *
         UINT64_C(0x9e3779b97f4a7c15)));
    uint32_t seed = (uint32_t)mixed ^ (uint32_t)(mixed >> 32);

    return seed == 0u ? UINT32_C(0x6d2b79f5) : seed;
}

int survey_discovery_probe_attempt_begin(
    struct survey_discovery_probe_attempt *attempt,
    uint8_t opportunity)
{
    if (attempt == NULL ||
        opportunity >= SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
        return PROTO_ERR_ARG;
    }
    memset(attempt, 0, sizeof(*attempt));
    attempt->opportunity = opportunity;
    attempt->initialized = true;
    return PROTO_OK;
}

int survey_discovery_probe_attempt_defer(
    struct survey_discovery_probe_attempt *attempt,
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint32_t retry_origin_ms,
    uint32_t absolute_deadline_ms)
{
    uint32_t delay_ms = 0u;
    uint32_t remaining_ms;
    int ret;

    if (attempt == NULL || anchor_id == 0u ||
        survey_discovery_config_validate(config) != PROTO_OK ||
        !attempt->initialized ||
        attempt->opportunity >= SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
        return PROTO_ERR_ARG;
    }
    if (attempt->rf_started ||
        (int32_t)(retry_origin_ms - absolute_deadline_ms) >= 0) {
        return PROTO_ERR_BUSY;
    }
    if (attempt->retry_round < UINT16_MAX) {
        attempt->retry_round++;
    }
    ret = node_comm_retry_backoff_ms(
        NODE_COMM_PROFILE_RELIABLE_PROTOCOL_RESPONSE,
        survey_discovery_probe_retry_seed(anchor_id, config->survey_id,
                                          attempt->opportunity),
        attempt->retry_round,
        &delay_ms);
    if (ret < 0) {
        return PROTO_ERR_BUSY;
    }
    remaining_ms = absolute_deadline_ms - retry_origin_ms;
    if (delay_ms > remaining_ms) {
        delay_ms = remaining_ms;
    }
    attempt->due_ms = retry_origin_ms + delay_ms;
    attempt->pending = true;
    return PROTO_OK;
}

int survey_discovery_probe_attempt_note_rf_started(
    struct survey_discovery_probe_attempt *attempt)
{
    if (attempt == NULL ||
        !attempt->initialized ||
        attempt->opportunity >= SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
        return PROTO_ERR_ARG;
    }
    if (attempt->rf_started) {
        return PROTO_ERR_STALE;
    }
    attempt->rf_started = true;
    attempt->pending = false;
    return PROTO_OK;
}

size_t survey_discovery_probe_real_attempt_count(
    const struct survey_discovery_probe_attempt *attempts,
    size_t attempt_count)
{
    size_t count = 0u;

    if (attempts == NULL ||
        attempt_count > SURVEY_DISCOVERY_OPPORTUNITY_COUNT) {
        return 0u;
    }
    for (size_t i = 0u; i < attempt_count; i++) {
        count += attempts[i].rf_started ? 1u : 0u;
    }
    return count;
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

static const struct survey_gateway_report_slot *
survey_gateway_round_report_for_anchor(
    const struct survey_gateway_context *context,
    uint64_t anchor_id);

static void survey_gateway_orient_pair_for_control(
    const struct survey_gateway_context *context,
    struct survey_gateway_pair_entry *pair)
{
    const struct survey_gateway_report_slot *initiator;
    const struct survey_gateway_report_slot *responder;
    uint64_t swap_id;

    if (context == NULL || pair == NULL) {
        return;
    }
    initiator = survey_gateway_round_report_for_anchor(
        context, pair->initiator_id);
    responder = survey_gateway_round_report_for_anchor(
        context, pair->responder_id);
    if (initiator == NULL || responder == NULL ||
        !initiator->reverse_hint_valid || !responder->reverse_hint_valid ||
        initiator->reverse_hop_count == 0u ||
        initiator->reverse_hop_count > SURVEY_DEFAULT_TTL ||
        responder->reverse_hop_count == 0u ||
        responder->reverse_hop_count > SURVEY_DEFAULT_TTL ||
        initiator->reverse_hop_count <= responder->reverse_hop_count) {
        return;
    }

    swap_id = pair->initiator_id;
    pair->initiator_id = pair->responder_id;
    pair->responder_id = swap_id;
}

int survey_gateway_plan_pairs(struct survey_gateway_context *context)
{
    struct survey_reachability_report reports[SURVEY_GATEWAY_MAX_REPORTS];
    size_t report_count = 0u;
    int ret;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u) {
        return PROTO_ERR_STALE;
    }

    for (size_t i = 0u; i < context->report_count; i++) {
        const struct survey_gateway_report_slot *slot = &context->reports[i];

        if (!slot->valid) {
            continue;
        }
        reports[report_count].anchor_id = slot->anchor_id;
        reports[report_count].entries = slot->entries;
        reports[report_count].entry_count = slot->entry_count;
        report_count++;
    }

    ret = survey_plan_pairs_into_gateway_context(context,
                                                 reports,
                                                 report_count);
    if (ret != PROTO_OK) {
        context->pairs_planned = false;
        context->pair_count = 0u;
        context->next_pair_index = 0u;
        return ret;
    }
    for (size_t i = 0u; i < context->pair_count; i++) {
        survey_gateway_orient_pair_for_control(context, &context->pairs[i]);
    }

    context->pairs_planned = true;
    context->next_pair_index = 0u;
    return PROTO_OK;
}

static const struct survey_gateway_report_slot *
survey_gateway_round_report_for_anchor(
    const struct survey_gateway_context *context,
    uint64_t anchor_id)
{
    for (size_t i = 0u; i < context->report_count; i++) {
        const struct survey_gateway_report_slot *slot = &context->reports[i];

        if (slot->valid && slot->anchor_id == anchor_id) {
            return slot;
        }
    }
    return NULL;
}

static bool survey_gateway_round_report_has_peer(
    const struct survey_gateway_report_slot *slot,
    uint64_t peer_id)
{
    for (size_t i = 0u; i < slot->entry_count; i++) {
        if (slot->entries[i].peer_id == peer_id) {
            return true;
        }
    }
    return false;
}

static bool survey_gateway_round_reports_share_peer(
    const struct survey_gateway_report_slot *first,
    const struct survey_gateway_report_slot *second)
{
    for (size_t i = 0u; i < first->entry_count; i++) {
        for (size_t j = 0u; j < second->entry_count; j++) {
            if (first->entries[i].peer_id == second->entries[j].peer_id) {
                return true;
            }
        }
    }
    return false;
}

static bool survey_gateway_round_hops_prove_separation(
    const struct survey_gateway_report_slot *first,
    const struct survey_gateway_report_slot *second)
{
    uint8_t first_hops;
    uint8_t second_hops;

    if (!first->reverse_hint_valid || !second->reverse_hint_valid) {
        return false;
    }
    first_hops = first->reverse_hop_count;
    second_hops = second->reverse_hop_count;
    if (first_hops == 0u || first_hops > SURVEY_DEFAULT_TTL ||
        second_hops == 0u || second_hops > SURVEY_DEFAULT_TTL) {
        return false;
    }
    return first_hops > second_hops ?
        (uint8_t)(first_hops - second_hops) >= 2u :
        (uint8_t)(second_hops - first_hops) >= 2u;
}

static bool survey_gateway_round_anchors_conflict(
    const struct survey_gateway_context *context,
    uint64_t first_id,
    uint64_t second_id)
{
    const struct survey_gateway_report_slot *first =
        survey_gateway_round_report_for_anchor(context, first_id);
    const struct survey_gateway_report_slot *second =
        survey_gateway_round_report_for_anchor(context, second_id);

    if (first == NULL || second == NULL) {
        return true;
    }
    if (survey_gateway_round_report_has_peer(first, second_id) ||
        survey_gateway_round_report_has_peer(second, first_id)) {
        return true;
    }
    if (first->entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT &&
        second->entry_count < SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
        return false;
    }
    return !survey_gateway_round_hops_prove_separation(first, second);
}

static bool survey_gateway_round_pairs_conflict(
    const struct survey_gateway_context *context,
    const struct survey_gateway_pair_entry *first,
    const struct survey_gateway_pair_entry *second)
{
    const uint64_t first_ids[] = {
        first->initiator_id,
        first->responder_id,
    };
    const uint64_t second_ids[] = {
        second->initiator_id,
        second->responder_id,
    };

    for (size_t i = 0u; i < 2u; i++) {
        for (size_t j = 0u; j < 2u; j++) {
            const struct survey_gateway_report_slot *first_report =
                survey_gateway_round_report_for_anchor(context, first_ids[i]);
            const struct survey_gateway_report_slot *second_report =
                survey_gateway_round_report_for_anchor(context, second_ids[j]);

            if (first_ids[i] == second_ids[j] ||
                survey_gateway_round_anchors_conflict(
                    context, first_ids[i], second_ids[j]) ||
                first_report == NULL || second_report == NULL ||
                survey_gateway_round_reports_share_peer(
                    first_report, second_report)) {
                return true;
            }
        }
    }
    return false;
}

int survey_gateway_plan_pair_rounds(
    const struct survey_gateway_context *context,
    struct survey_pair_round_metadata *metadata,
    size_t metadata_cap,
    size_t *round_count)
{
    uint8_t round_total = 0u;

    if (context == NULL || round_count == NULL ||
        (metadata == NULL && context->pair_count != 0u)) {
        return PROTO_ERR_ARG;
    }
    *round_count = 0u;
    if (!context->pairs_planned) {
        return PROTO_ERR_STALE;
    }
    if (context->pair_count > SURVEY_GATEWAY_MAX_PAIRS ||
        metadata_cap < context->pair_count) {
        return PROTO_ERR_NO_SPACE;
    }

    for (size_t i = 0u; i < context->pair_count; i++) {
        const struct survey_gateway_pair_entry *pair = &context->pairs[i];

        if (pair->initiator_id == 0u || pair->responder_id == 0u ||
            pair->initiator_id == pair->responder_id) {
            return PROTO_ERR_MALFORMED;
        }
        if (survey_gateway_round_report_for_anchor(
                context, pair->initiator_id) == NULL ||
            survey_gateway_round_report_for_anchor(
                context, pair->responder_id) == NULL) {
            return PROTO_ERR_NOT_FOUND;
        }
    }

    for (size_t i = 0u; i < context->pair_count; i++) {
        uint8_t conflicting_rounds[(SURVEY_GATEWAY_MAX_PAIRS + 7u) / 8u] = {0};
        uint8_t selected_round = 0u;
        uint8_t position = 0u;

        for (size_t j = 0u; j < i; j++) {
            if (survey_gateway_round_pairs_conflict(
                    context, &context->pairs[i], &context->pairs[j])) {
                const uint8_t round = metadata[j].round_index;

                conflicting_rounds[round / 8u] |=
                    (uint8_t)(1u << (round % 8u));
            }
        }
        while (selected_round < round_total &&
               (conflicting_rounds[selected_round / 8u] &
                (uint8_t)(1u << (selected_round % 8u))) != 0u) {
            selected_round++;
        }
        if (selected_round == round_total) {
            round_total++;
        }
        for (size_t j = 0u; j < i; j++) {
            if (metadata[j].round_index == selected_round) {
                position++;
            }
        }
        metadata[i] = (struct survey_pair_round_metadata) {
            .round_index = selected_round,
            .pair_index_in_round = position,
        };
    }

    for (size_t i = 0u; i < context->pair_count; i++) {
        uint8_t pairs_in_round = 0u;

        for (size_t j = 0u; j < context->pair_count; j++) {
            if (metadata[j].round_index == metadata[i].round_index) {
                pairs_in_round++;
            }
        }
        metadata[i].pair_count_in_round = pairs_in_round;
    }

    *round_count = round_total;
    return PROTO_OK;
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

struct survey_pair_candidate {
    size_t peer_index;
    int8_t rssi_dbm;
    uint8_t quality;
    bool mutual;
};

struct survey_connect_candidate {
    struct survey_pair_candidate pair;
    size_t report_index;
    bool valid;
};

struct survey_pair_plan_output {
    struct survey_pair *full_pairs;
    struct survey_gateway_pair_entry *gateway_pairs;
    size_t pair_cap;
};

_Static_assert(SURVEY_GATEWAY_MAX_PAIRS >=
               (SURVEY_GATEWAY_MAX_REPORTS * SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) / 2u,
               "survey pair storage must hold the bounded topology");

static const struct survey_reachability_entry *survey_report_find_peer(
    const struct survey_reachability_report *report,
    uint64_t peer_id)
{
    for (size_t i = 0u; i < report->entry_count; i++) {
        if (report->entries[i].peer_id == peer_id) {
            return &report->entries[i];
        }
    }
    return NULL;
}

static bool survey_candidate_precedes(const struct survey_pair_candidate *left,
                                      const struct survey_pair_candidate *right,
                                      const struct survey_reachability_report *const *ordered)
{
    if (left->mutual != right->mutual) {
        return left->mutual;
    }
    if (left->quality != right->quality) {
        return left->quality > right->quality;
    }
    if (left->rssi_dbm != right->rssi_dbm) {
        return left->rssi_dbm > right->rssi_dbm;
    }
    return ordered[left->peer_index]->anchor_id <
           ordered[right->peer_index]->anchor_id;
}

static bool survey_pair_candidate_build(
    const struct survey_reachability_report *const *ordered,
    size_t report_index,
    size_t peer_index,
    struct survey_pair_candidate *candidate)
{
    const struct survey_reachability_entry *forward =
        survey_report_find_peer(ordered[report_index],
                                ordered[peer_index]->anchor_id);
    const struct survey_reachability_entry *reverse =
        survey_report_find_peer(ordered[peer_index],
                                ordered[report_index]->anchor_id);

    if (candidate == NULL || (forward == NULL && reverse == NULL)) {
        return false;
    }
    candidate->peer_index = peer_index;
    candidate->mutual = forward != NULL && reverse != NULL;
    if (candidate->mutual) {
        candidate->quality = forward->quality < reverse->quality ?
                             forward->quality : reverse->quality;
        candidate->rssi_dbm = forward->rssi_dbm < reverse->rssi_dbm ?
                              forward->rssi_dbm : reverse->rssi_dbm;
    } else if (forward != NULL) {
        candidate->quality = forward->quality;
        candidate->rssi_dbm = forward->rssi_dbm;
    } else {
        candidate->quality = reverse->quality;
        candidate->rssi_dbm = reverse->rssi_dbm;
    }
    return true;
}

static size_t survey_component_root(size_t *parents, size_t index)
{
    size_t root = index;

    while (parents[root] != root) {
        root = parents[root];
    }
    while (parents[index] != index) {
        size_t next = parents[index];

        parents[index] = root;
        index = next;
    }
    return root;
}

static bool survey_connect_candidate_precedes(
    const struct survey_connect_candidate *left,
    const struct survey_connect_candidate *right,
    const uint8_t *degree,
    const struct survey_reachability_report *const *ordered)
{
    const uint8_t left_max = degree[left->report_index] >
        degree[left->pair.peer_index] ? degree[left->report_index] :
                                       degree[left->pair.peer_index];
    const uint8_t right_max = degree[right->report_index] >
        degree[right->pair.peer_index] ? degree[right->report_index] :
                                        degree[right->pair.peer_index];
    const uint8_t left_sum = degree[left->report_index] +
        degree[left->pair.peer_index];
    const uint8_t right_sum = degree[right->report_index] +
        degree[right->pair.peer_index];

    if (!right->valid) {
        return true;
    }
    if (left_max != right_max) {
        return left_max < right_max;
    }
    if (left_sum != right_sum) {
        return left_sum < right_sum;
    }
    if (survey_candidate_precedes(&left->pair, &right->pair, ordered)) {
        return true;
    }
    if (survey_candidate_precedes(&right->pair, &left->pair, ordered)) {
        return false;
    }
    return ordered[left->report_index]->anchor_id <
           ordered[right->report_index]->anchor_id;
}

static int survey_append_planned_pair(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_reachability_report *const *ordered,
    size_t report_index,
    size_t peer_index,
    struct survey_pair_plan_output *output,
    size_t *count,
    uint8_t *degree)
{
    const uint64_t initiator_id = ordered[report_index]->anchor_id;
    const uint64_t responder_id = ordered[peer_index]->anchor_id;

    if (*count >= output->pair_cap) {
        return PROTO_ERR_NO_SPACE;
    }
    if (output->full_pairs != NULL) {
        output->full_pairs[*count] = (struct survey_pair) {
            .survey_id = survey_id,
            .initiator_id = initiator_id,
            .responder_id = responder_id,
            .sample_count = sample_count,
        };
    } else {
        output->gateway_pairs[*count] = (struct survey_gateway_pair_entry) {
            .initiator_id = initiator_id,
            .responder_id = responder_id,
        };
    }
    degree[report_index]++;
    degree[peer_index]++;
    (*count)++;
    return PROTO_OK;
}

static bool survey_pair_already_planned(
    const struct survey_pair_plan_output *output,
    size_t count,
    uint64_t first_id,
    uint64_t second_id)
{
    for (size_t i = 0u; i < count; i++) {
        const uint64_t initiator_id = output->full_pairs != NULL ?
            output->full_pairs[i].initiator_id :
            output->gateway_pairs[i].initiator_id;
        const uint64_t responder_id = output->full_pairs != NULL ?
            output->full_pairs[i].responder_id :
            output->gateway_pairs[i].responder_id;

        if (initiator_id == first_id && responder_id == second_id) {
            return true;
        }
    }
    return false;
}

static int survey_connect_report_graph(
    uint32_t survey_id,
    uint16_t sample_count,
    const struct survey_reachability_report *const *ordered,
    size_t report_count,
    struct survey_pair_plan_output *output,
    size_t *count,
    uint8_t *degree)
{
    size_t parents[SURVEY_GATEWAY_MAX_REPORTS];

    if (report_count > 1u && output->pair_cap < report_count - 1u) {
        return PROTO_ERR_NO_SPACE;
    }
    for (size_t i = 0u; i < report_count; i++) {
        parents[i] = i;
    }
    for (size_t edge = 0u; edge + 1u < report_count; edge++) {
        struct survey_connect_candidate best = {0};

        for (size_t i = 0u; i < report_count; i++) {
            if (degree[i] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                continue;
            }
            for (size_t j = i + 1u; j < report_count; j++) {
                struct survey_connect_candidate candidate = {
                    .report_index = i,
                    .valid = true,
                };

                if (degree[j] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR ||
                    survey_component_root(parents, i) ==
                        survey_component_root(parents, j) ||
                    !survey_pair_candidate_build(ordered, i, j,
                                                 &candidate.pair)) {
                    continue;
                }
                if (survey_connect_candidate_precedes(&candidate, &best,
                                                      degree, ordered)) {
                    best = candidate;
                }
            }
        }
        if (!best.valid) {
            return PROTO_ERR_NOT_FOUND;
        }
        int ret = survey_append_planned_pair(
            survey_id, sample_count, ordered, best.report_index,
            best.pair.peer_index, output, count, degree);
        if (ret != PROTO_OK) {
            return ret;
        }
        size_t first_root = survey_component_root(parents, best.report_index);
        size_t second_root = survey_component_root(parents,
                                                   best.pair.peer_index);

        parents[second_root] = first_root;
    }
    return PROTO_OK;
}

static int survey_plan_pairs_from_reachability_into(
    uint32_t survey_id,
    const struct survey_reachability_report *reports,
    size_t report_count,
    uint16_t sample_count,
    struct survey_pair_plan_output *output,
    size_t *pair_count)
{
    const struct survey_reachability_report *ordered[SURVEY_GATEWAY_MAX_REPORTS];
    uint8_t degree[SURVEY_GATEWAY_MAX_REPORTS] = {0};
    size_t count = 0u;

    if (reports == NULL || output == NULL || pair_count == NULL ||
        (output->full_pairs == NULL) == (output->gateway_pairs == NULL)) {
        return PROTO_ERR_ARG;
    }
    if (survey_id == 0u || !survey_sample_count_valid(sample_count)) {
        return PROTO_ERR_MALFORMED;
    }
    if (report_count > SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_NO_SPACE;
    }

    *pair_count = 0u;
    for (size_t i = 0u; i < report_count; i++) {
        const struct survey_reachability_report *report = &reports[i];

        if (report->anchor_id == 0u ||
            (report->entries == NULL && report->entry_count != 0u)) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < report->entry_count; j++) {
            const struct survey_reachability_entry *entry = &report->entries[j];
            int ret;

            ret = survey_reachability_entry_validate(entry);
            if (ret != PROTO_OK) {
                return ret;
            }
            if (entry->peer_id == report->anchor_id) {
                return PROTO_ERR_MALFORMED;
            }
        }
        ordered[i] = report;
    }

    for (size_t i = 1u; i < report_count; i++) {
        const struct survey_reachability_report *value = ordered[i];
        size_t j = i;

        while (j > 0u && ordered[j - 1u]->anchor_id > value->anchor_id) {
            ordered[j] = ordered[j - 1u];
            j--;
        }
        ordered[j] = value;
    }

    int ret = survey_connect_report_graph(survey_id, sample_count, ordered,
                                          report_count, output,
                                          &count, degree);
    if (ret != PROTO_OK) {
        return ret;
    }

    for (size_t i = 0u; i < report_count; i++) {
        struct survey_pair_candidate candidates[SURVEY_GATEWAY_MAX_REPORTS];
        size_t candidate_count = 0u;

        for (size_t j = i + 1u; j < report_count; j++) {
            struct survey_pair_candidate candidate;

            if (!survey_pair_candidate_build(ordered, i, j, &candidate)) {
                continue;
            }

            size_t insert = candidate_count;
            while (insert > 0u &&
                   survey_candidate_precedes(&candidate,
                                             &candidates[insert - 1u],
                                             ordered)) {
                candidates[insert] = candidates[insert - 1u];
                insert--;
            }
            candidates[insert] = candidate;
            candidate_count++;
        }

        for (size_t j = 0u; j < candidate_count &&
             degree[i] < SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR; j++) {
            const size_t peer_index = candidates[j].peer_index;

            if (degree[peer_index] >= SURVEY_GATEWAY_MAX_PAIRS_PER_ANCHOR) {
                continue;
            }
            if (survey_pair_already_planned(output, count,
                                            ordered[i]->anchor_id,
                                            ordered[peer_index]->anchor_id)) {
                continue;
            }
            ret = survey_append_planned_pair(
                survey_id, sample_count, ordered, i, peer_index, output,
                &count, degree);
            if (ret != PROTO_OK) {
                return ret;
            }
        }
    }

    *pair_count = count;
    return PROTO_OK;
}

int survey_plan_pairs_from_reachability(uint32_t survey_id,
                                        const struct survey_reachability_report *reports,
                                        size_t report_count,
                                        uint16_t sample_count,
                                        struct survey_pair *pairs,
                                        size_t pair_cap,
                                        size_t *pair_count)
{
    struct survey_pair_plan_output output = {
        .full_pairs = pairs,
        .pair_cap = pair_cap,
    };

    return survey_plan_pairs_from_reachability_into(survey_id,
                                                    reports,
                                                    report_count,
                                                    sample_count,
                                                    &output,
                                                    pair_count);
}

static int survey_plan_pairs_into_gateway_context(
    struct survey_gateway_context *context,
    const struct survey_reachability_report *reports,
    size_t report_count)
{
    struct survey_pair_plan_output output = {
        .gateway_pairs = context->pairs,
        .pair_cap = SURVEY_GATEWAY_MAX_PAIRS,
    };

    return survey_plan_pairs_from_reachability_into(context->survey_id,
                                                    reports,
                                                    report_count,
                                                    context->sample_count,
                                                    &output,
                                                    &context->pair_count);
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
