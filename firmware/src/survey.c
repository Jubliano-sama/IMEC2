#include "survey.h"

#include "dwm3000_timing.h"
#include "mesh_relay.h"
#include "operation_policy.h"
#include "survey_round_control.h"
#include "uwb.h"

#include <string.h>

_Static_assert(sizeof(struct survey_pair) == 32u,
               "survey pair public layout changed");
_Static_assert(SURVEY_GATEWAY_PAIR_MAX_RERUNS < UINT8_MAX,
               "survey pair rerun count must fit observability attempts");
_Static_assert(SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS >=
                   MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS,
               "survey control base must cover gateway-ACK custody retries");
_Static_assert(SURVEY_PAIR_CONTROL_BASE_TIMEOUT_MS >=
                   ROUTE_GATEWAY_ACK_PROGRESS_TIMEOUT_MS,
               "survey control base must cover a proved multi-hop ACK return");
_Static_assert(PROTO_TLV_U32_ENCODED_LEN +
                   (2u * PROTO_TLV_U64_ENCODED_LEN) +
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

uint32_t survey_discovery_required_start_delay_ms(uint8_t max_hop_count)
{
    uint8_t effective_hop_count =
        max_hop_count == 0u || max_hop_count > SURVEY_DEFAULT_TTL ?
            SURVEY_DEFAULT_TTL : max_hop_count;
    uint32_t control_delivery_ms =
        ((uint32_t)effective_hop_count +
         SURVEY_DISCOVERY_ORIGIN_REDRIVE_COUNT) *
            SURVEY_DISCOVERY_CONTROL_HOP_BUDGET_MS +
        SURVEY_DISCOVERY_PHY_PREP_BUDGET_MS + 1u;

    return control_delivery_ms < SURVEY_DISCOVERY_START_DELAY_FLOOR_MS ?
        SURVEY_DISCOVERY_START_DELAY_FLOOR_MS : control_delivery_ms;
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

uint32_t survey_pair_control_round_trip_timeout_ms(
    uint8_t gateway_hop_count)
{
    uint32_t request_timeout_ms =
        survey_pair_control_timeout_ms(gateway_hop_count);

    return request_timeout_ms >
               UINT32_MAX - request_timeout_ms ?
           UINT32_MAX :
           request_timeout_ms + request_timeout_ms;
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

uint64_t survey_discovery_report_deadline_ms(uint64_t now_ms,
                                             uint32_t eligible_tx_ms,
                                             uint8_t gateway_hop_count)
{
    int32_t until_eligible_ms =
        (int32_t)(eligible_tx_ms - (uint32_t)now_ms);
    uint64_t eligible_at_ms;
    uint32_t custody_ms =
        survey_discovery_report_custody_ms(gateway_hop_count);

    if (until_eligible_ms > 0) {
        uint32_t delay_ms = (uint32_t)until_eligible_ms;

        if (UINT64_MAX - now_ms < delay_ms) {
            return UINT64_MAX;
        }
        eligible_at_ms = now_ms + delay_ms;
    } else {
        uint64_t elapsed_ms = (uint64_t)(-(int64_t)until_eligible_ms);

        /*
         * eligible_tx_ms is a boot-relative 32-bit timestamp.  Within its
         * half-range-valid service horizon, reconstruct the same 64-bit
         * eligible instant instead of granting a fresh custody window on
         * every delayed admission or preemption retry.
         */
        eligible_at_ms = elapsed_ms > now_ms ? 0u : now_ms - elapsed_ms;
    }
    return UINT64_MAX - eligible_at_ms < custody_ms ?
           UINT64_MAX : eligible_at_ms + custody_ms;
}

int survey_extract_expected_node_count_tlv(const uint8_t *payload,
                                           size_t payload_len,
                                           uint16_t *expected_count,
                                           bool *present)
{
    const uint8_t *value;
    uint16_t decoded_count;
    uint8_t value_len;
    int ret;

    if (payload == NULL || expected_count == NULL || present == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find_unique(payload, payload_len, TLV_EXPECTED_NODE_COUNT,
                          &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        *expected_count = 0u;
        *present = false;
        return PROTO_OK;
    }
    if (ret != PROTO_OK || value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    decoded_count = proto_get_u16_le(value);
    if (decoded_count == 0u ||
        decoded_count > SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_MALFORMED;
    }
    *expected_count = decoded_count;
    *present = true;
    return PROTO_OK;
}

enum survey_gateway_collection_decision survey_gateway_collection_decide(
    bool emission_horizon_elapsed,
    bool safety_deadline_elapsed,
    size_t report_count,
    uint16_t expected_count,
    bool expected_present)
{
    if (!emission_horizon_elapsed) {
        return SURVEY_GATEWAY_COLLECTION_WAIT;
    }
    if (!expected_present) {
        return safety_deadline_elapsed ?
            SURVEY_GATEWAY_COLLECTION_CLOSE :
            SURVEY_GATEWAY_COLLECTION_WAIT;
    }
    if (report_count > expected_count) {
        return SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH;
    }
    if (report_count == expected_count) {
        return SURVEY_GATEWAY_COLLECTION_CLOSE;
    }
    return safety_deadline_elapsed ?
        SURVEY_GATEWAY_COLLECTION_COUNT_MISMATCH :
        SURVEY_GATEWAY_COLLECTION_WAIT;
}

bool survey_gateway_discovery_collection_survives_terminal(
    bool delivered,
    uint8_t attempts_started)
{
    return delivered || attempts_started > 0u;
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
        (pair->operation_generation != 0u &&
         survey_operation_session_id(pair->operation_generation) == 0u) ||
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
        sample->range_status > RANGE_TIMING_INVALID ||
        sample->range_status == RANGE_STS_QUALITY_FAIL) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

int survey_pair_result_next_round_id(uint16_t current_round_id,
                                     uint16_t *next_round_id)
{
    if (next_round_id == NULL) {
        return PROTO_ERR_ARG;
    }
    if (current_round_id > SURVEY_PAIR_RESULT_MAX_BATCH_COUNT) {
        return PROTO_ERR_MALFORMED;
    }
    if (current_round_id == SURVEY_PAIR_RESULT_MAX_BATCH_COUNT) {
        return PROTO_ERR_NO_SPACE;
    }

    *next_round_id = (uint16_t)(current_round_id + 1u);
    return PROTO_OK;
}

int survey_pair_result_transport_sequence(uint16_t round_id,
                                          uint16_t sample_index,
                                          uint16_t *sequence)
{
    uint32_t candidate;

    if (sequence == NULL) {
        return PROTO_ERR_ARG;
    }
    if (round_id > SURVEY_PAIR_RESULT_MAX_BATCH_COUNT ||
        sample_index >= SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        return PROTO_ERR_MALFORMED;
    }

    /*
     * Legacy PREPARE/START controls omit the synchronized-round TLV. Their
     * immutable survey_id remains the packet session, while sample_index + 1
     * supplies the nonzero per-run sequence. A legacy sender cannot express a
     * rerun inside the same survey_id; first accepted results therefore remain
     * authoritative for that compatibility mode.
     */
    candidate = round_id == SURVEY_LEGACY_ROUND_ID ?
        (uint32_t)sample_index + 1u :
        ((uint32_t)(round_id - 1u) *
         SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) +
        (uint32_t)sample_index + 1u;
    if (candidate == 0u ||
        candidate > SURVEY_PAIR_RESULT_TRANSPORT_SEQUENCE_MAX ||
        candidate > UINT16_MAX) {
        return PROTO_ERR_MALFORMED;
    }

    *sequence = (uint16_t)candidate;
    return PROTO_OK;
}

bool survey_sample_distance_usable(const struct survey_sample *sample)
{
    return sample != NULL && sample->range_status == RANGE_OK &&
           sample->distance_mm > SURVEY_MIN_USABLE_DISTANCE_MM;
}

bool survey_sample_matches_pair_run(const struct survey_sample *sample,
                                    const struct survey_pair *pair,
                                    uint16_t round_id)
{
    return sample != NULL && pair != NULL && round_id != 0u &&
           sample->round_id == round_id &&
           sample->pair.operation_generation ==
               pair->operation_generation &&
           sample->pair.survey_id == pair->survey_id &&
           sample->pair.initiator_id == pair->initiator_id &&
           sample->pair.responder_id == pair->responder_id &&
           sample->pair.sample_count == pair->sample_count;
}

int survey_sample_observation_identity_capture(
    const struct survey_sample *sample,
    struct survey_sample_observation_identity *identity)
{
    int ret;

    if (sample == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_sample_validate(sample);
    if (ret != PROTO_OK) {
        return ret;
    }
    proto_put_u32_le(identity->encoded, (uint32_t)sample->distance_mm);
    identity->encoded[4] = sample->quality;
    identity->encoded[5] = (uint8_t)sample->range_status;
    return PROTO_OK;
}

bool survey_sample_observation_identity_valid(
    const struct survey_sample_observation_identity *identity)
{
    return identity != NULL &&
           identity->encoded[5] !=
               SURVEY_SAMPLE_OBSERVATION_IDENTITY_INVALID;
}

bool survey_sample_observation_identity_equal(
    const struct survey_sample_observation_identity *left,
    const struct survey_sample_observation_identity *right)
{
    return survey_sample_observation_identity_valid(left) &&
           survey_sample_observation_identity_valid(right) &&
           memcmp(left->encoded,
                  right->encoded,
                  SURVEY_SAMPLE_OBSERVATION_IDENTITY_LEN) == 0;
}

int survey_pair_note_sample_masks(const struct survey_sample *sample,
                                  uint64_t reporter_id,
                                  uint16_t *usable_mask,
                                  uint16_t *responder_usable_mask,
                                  uint16_t *initiator_unusable_mask,
                                  uint16_t *responder_unusable_mask,
                                  bool *changed)
{
    uint16_t sample_bit;
    if (sample == NULL || usable_mask == NULL ||
        responder_usable_mask == NULL ||
        initiator_unusable_mask == NULL ||
        responder_unusable_mask == NULL || changed == NULL) {
        return PROTO_ERR_ARG;
    }
    if (survey_sample_validate(sample) != PROTO_OK ||
        sample->pair.sample_count > 16u ||
        reporter_id != sample->pair.responder_id) {
        return PROTO_ERR_MALFORMED;
    }

    *changed = false;
    sample_bit = (uint16_t)(UINT16_C(1) << sample->sample_index);
    if (survey_sample_distance_usable(sample)) {
        if ((*responder_usable_mask & sample_bit) != 0u) {
            return PROTO_OK;
        }
        *responder_usable_mask |= sample_bit;
        *usable_mask |= sample_bit;
        *initiator_unusable_mask &= (uint16_t)~sample_bit;
        *responder_unusable_mask &= (uint16_t)~sample_bit;
        *changed = true;
        return PROTO_OK;
    }

    if ((*usable_mask & sample_bit) != 0u) {
        return PROTO_OK;
    }
    if ((*responder_unusable_mask & sample_bit) == 0u) {
        *responder_unusable_mask |= sample_bit;
        *changed = true;
    }
    return PROTO_OK;
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
    (void)initiator_unusable_mask;
    return missing_mask != 0u &&
           (responder_unusable_mask & missing_mask) == missing_mask;
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
    /*
     * Production survey IDs are host correlation values and may be reused
     * after a gateway restart.  Bind the PHY nonce to the durable operation
     * generation so a delayed frame from an earlier incarnation cannot pass
     * the expected-session check for the replacement operation.  Preserve the
     * generation-zero derivation for bounded legacy/native callers.
     */
    if (pair->operation_generation != 0u) {
        value ^= survey_mix64(
            pair->operation_generation ^ UINT64_C(0x5355525645594e43));
    }
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

int survey_reachability_report_endpoints_validate(
    uint64_t anchor_id,
    uint64_t gateway_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count)
{
    if (entries == NULL && entry_count != 0u) {
        return PROTO_ERR_ARG;
    }
    if (anchor_id == 0u || gateway_id == 0u ||
        anchor_id == gateway_id) {
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
        if (entries[i].peer_id == anchor_id ||
            entries[i].peer_id == gateway_id) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            if (entries[j].peer_id == entries[i].peer_id) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }
    return PROTO_OK;
}

static bool survey_reachability_entry_precedes(
    const struct survey_reachability_entry *left,
    const struct survey_reachability_entry *right)
{
    if (left->quality != right->quality) {
        return left->quality > right->quality;
    }
    if (left->rssi_dbm != right->rssi_dbm) {
        return left->rssi_dbm > right->rssi_dbm;
    }
    return left->peer_id < right->peer_id;
}

static void survey_reachability_entries_sort(
    struct survey_reachability_entry *entries,
    size_t entry_count)
{
    for (size_t i = 1u; i < entry_count; i++) {
        const struct survey_reachability_entry value = entries[i];
        size_t insert = i;

        while (insert > 0u &&
               survey_reachability_entry_precedes(&value,
                                                  &entries[insert - 1u])) {
            entries[insert] = entries[insert - 1u];
            insert--;
        }
        entries[insert] = value;
    }
}

int survey_reachability_entry_retain(
    struct survey_reachability_entry *entries,
    size_t entry_cap,
    size_t *entry_count,
    const struct survey_reachability_entry *candidate)
{
    size_t candidate_index = SIZE_MAX;

    if (entries == NULL || entry_count == NULL || candidate == NULL) {
        return PROTO_ERR_ARG;
    }
    if (*entry_count > entry_cap ||
        entry_cap > SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
        return PROTO_ERR_MALFORMED;
    }
    if (survey_reachability_entry_validate(candidate) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    for (size_t i = 0u; i < *entry_count; i++) {
        if (survey_reachability_entry_validate(&entries[i]) != PROTO_OK) {
            return PROTO_ERR_MALFORMED;
        }
        if (entries[i].peer_id == candidate->peer_id) {
            candidate_index = i;
        }
    }

    survey_reachability_entries_sort(entries, *entry_count);
    if (candidate_index != SIZE_MAX) {
        for (size_t i = 0u; i < *entry_count; i++) {
            if (entries[i].peer_id == candidate->peer_id) {
                candidate_index = i;
                break;
            }
        }
        if (survey_reachability_entry_precedes(candidate,
                                               &entries[candidate_index])) {
            entries[candidate_index] = *candidate;
            survey_reachability_entries_sort(entries, *entry_count);
        }
        return PROTO_OK;
    }
    if (*entry_count < entry_cap) {
        entries[*entry_count] = *candidate;
        (*entry_count)++;
        survey_reachability_entries_sort(entries, *entry_count);
        return PROTO_OK;
    }
    if (entry_cap == 0u) {
        return PROTO_ERR_NO_SPACE;
    }
    if (survey_reachability_entry_precedes(candidate,
                                           &entries[*entry_count - 1u])) {
        entries[*entry_count - 1u] = *candidate;
        survey_reachability_entries_sort(entries, *entry_count);
    }
    return PROTO_OK;
}

static bool survey_assignment_commitment_is_zero(
    const struct discovery_assignment_table_commitment *commitment)
{
    static const struct discovery_assignment_table_commitment zero = {0};

    return commitment == NULL ||
           memcmp(commitment, &zero, sizeof(zero)) == 0;
}

int survey_discovery_config_validate(const struct survey_discovery_config *config)
{
    bool assignment_identity_absent;
    bool assignment_identity_present;

    if (config == NULL) {
        return PROTO_ERR_ARG;
    }
    assignment_identity_absent =
        config->assignment_epoch == 0u &&
        config->assignment_table_seq == 0u &&
        survey_assignment_commitment_is_zero(
            &config->assignment_table_commitment);
    assignment_identity_present =
        config->assignment_epoch != 0u &&
        config->assignment_table_seq != 0u &&
        !survey_assignment_commitment_is_zero(
            &config->assignment_table_commitment);
    if (config->survey_id == 0u ||
        (config->operation_generation != 0u &&
         survey_operation_session_id(config->operation_generation) == 0u) ||
        config->start_delay_ms == 0u ||
        config->start_delay_ms > SURVEY_DISCOVERY_MAX_START_DELAY_MS ||
        config->slot_ms < survey_discovery_probe_tx_budget_ms() +
                              SURVEY_DISCOVERY_RX_GUARD_MS ||
        config->slot_ms > SURVEY_DISCOVERY_MAX_SLOT_MS ||
        config->slot_count == 0u ||
        config->slot_count > SURVEY_DISCOVERY_MAX_SLOT_COUNT ||
        config->round_count == 0u ||
        config->round_count > SURVEY_DISCOVERY_MAX_ROUND_COUNT ||
        (!assignment_identity_absent && !assignment_identity_present)) {
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

int survey_discovery_opportunity_slot_tx_ms(
    const struct survey_discovery_config *config,
    uint8_t slot,
    uint8_t opportunity,
    uint32_t *tx_ms)
{
    uint32_t start_ms;
    uint32_t end_ms;
    uint64_t tx;

    if (tx_ms == NULL || config == NULL ||
        survey_discovery_opportunity_window_ms(config,
                                               opportunity,
                                               &start_ms,
                                               &end_ms) != PROTO_OK ||
        slot >= config->slot_count) {
        return PROTO_ERR_ARG;
    }
    tx = (uint64_t)start_ms + (uint64_t)slot * config->slot_ms;
    if (tx >= end_ms || tx > UINT32_MAX) {
        return PROTO_ERR_NO_SPACE;
    }
    *tx_ms = (uint32_t)tx;
    return PROTO_OK;
}

int survey_discovery_opportunity_tx_ms(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t *tx_ms)
{
    uint8_t slot;

    if (tx_ms == NULL || anchor_id == 0u || config == NULL ||
        survey_discovery_config_validate(config) != PROTO_OK) {
        return PROTO_ERR_ARG;
    }
    slot = survey_discovery_opportunity_slot(anchor_id,
                                             config->survey_id,
                                             opportunity,
                                             config->slot_count);
    return survey_discovery_opportunity_slot_tx_ms(config,
                                                   slot,
                                                   opportunity,
                                                   tx_ms);
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

int survey_discovery_schedule_slot_attempt(
    const struct survey_discovery_config *config,
    uint8_t slot,
    uint8_t opportunity,
    uint32_t earliest_relative_ms,
    struct survey_discovery_attempt_schedule *schedule)
{
    uint32_t raw_tx_ms;
    uint32_t tx_budget_ms;
    int ret;

    if (schedule == NULL || config == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(schedule, 0, sizeof(*schedule));
    ret = survey_discovery_opportunity_window_ms(config, opportunity,
                                                 &schedule->window_start_ms,
                                                 &schedule->window_end_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_discovery_opportunity_slot_tx_ms(config, slot, opportunity,
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

int survey_discovery_schedule_attempt(
    const struct survey_discovery_config *config,
    uint64_t anchor_id,
    uint8_t opportunity,
    uint32_t earliest_relative_ms,
    struct survey_discovery_attempt_schedule *schedule)
{
    uint8_t slot;

    if (schedule == NULL || anchor_id == 0u || config == NULL ||
        survey_discovery_config_validate(config) != PROTO_OK) {
        return PROTO_ERR_ARG;
    }
    slot = survey_discovery_opportunity_slot(anchor_id,
                                             config->survey_id,
                                             opportunity,
                                             config->slot_count);
    return survey_discovery_schedule_slot_attempt(config,
                                                  slot,
                                                  opportunity,
                                                  earliest_relative_ms,
                                                  schedule);
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
                                     uint8_t gateway_hop_count,
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
        gateway_hop_count == 0u ||
        gateway_hop_count > SURVEY_DEFAULT_TTL ||
        anchor_slot >= config->slot_count) {
        return PROTO_ERR_MALFORMED;
    }

    discovery_duration_ms = survey_discovery_duration_ms(config);
    delay = (uint64_t)discovery_duration_ms +
            ((((uint64_t)(gateway_hop_count - 1u) * config->slot_count) +
              anchor_slot) * report_slot_ms);
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

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
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
    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
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

    ret = tlv_find_unique(payload, payload_len, type,
                          &tlv_value, &value_len);
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
    return survey_gateway_begin_operation(context,
                                          survey_id,
                                          0u,
                                          sample_count);
}

int survey_gateway_begin_operation(struct survey_gateway_context *context,
                                   uint32_t survey_id,
                                   uint64_t operation_generation,
                                   uint16_t sample_count)
{
    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    if (survey_id == 0u ||
        (operation_generation != 0u &&
         survey_operation_session_id(operation_generation) == 0u) ||
        !survey_sample_count_valid(sample_count)) {
        return PROTO_ERR_MALFORMED;
    }

    memset(context, 0, sizeof(*context));
    for (size_t i = 0u; i < SURVEY_GATEWAY_MAX_REPORTS; i++) {
        context->reports[i].metadata = UINT8_MAX;
        context->reports[i].reverse_next_hop_index = UINT8_MAX;
    }
    context->operation_generation = operation_generation;
    context->survey_id = survey_id;
    context->sample_count = sample_count;
    return PROTO_OK;
}

static int survey_gateway_report_metadata_decode(
    const struct survey_gateway_report_slot *slot,
    size_t *entry_count,
    enum command_status *report_status)
{
    const uint8_t count = slot->metadata & 0x0fu;
    const uint8_t status = slot->metadata >> 4u;

    if (slot->metadata == UINT8_MAX ||
        count > SURVEY_GATEWAY_MAX_PEERS_PER_REPORT ||
        status > COMMAND_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }
    if (entry_count != NULL) {
        *entry_count = count;
    }
    if (report_status != NULL) {
        *report_status = (enum command_status)status;
    }
    return PROTO_OK;
}

static uint8_t survey_gateway_compact_entry_node_index(
    const struct survey_gateway_compact_reachability_entry *entry)
{
    return entry->peer_index & SURVEY_GATEWAY_COMPACT_NODE_INDEX_MASK;
}

static uint8_t survey_gateway_report_order(
    const struct survey_gateway_report_slot *slot)
{
    uint8_t report_order = 0u;

    for (uint8_t i = 0u;
         i < SURVEY_GATEWAY_REPORT_ORDER_PART_COUNT;
         i++) {
        report_order |=
            (uint8_t)(((slot->entries[i].peer_index >> 6u) & 0x03u) <<
                      (2u * i));
    }
    return report_order;
}

static void survey_gateway_report_order_set(
    struct survey_gateway_report_slot *slot,
    uint8_t report_order)
{
    for (uint8_t i = 0u;
         i < SURVEY_GATEWAY_REPORT_ORDER_PART_COUNT;
         i++) {
        slot->entries[i].peer_index =
            (uint8_t)((slot->entries[i].peer_index &
                       SURVEY_GATEWAY_COMPACT_NODE_INDEX_MASK) |
                      (((report_order >> (2u * i)) & 0x03u) << 6u));
    }
}

static int survey_gateway_node_table_validate(
    const struct survey_gateway_context *context)
{
    if (context->node_count > SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_MALFORMED;
    }
    for (size_t i = 0u; i < context->node_count; i++) {
        if (context->node_ids[i] == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            if (context->node_ids[j] == context->node_ids[i]) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }
    return PROTO_OK;
}

int survey_gateway_context_validate(
    const struct survey_gateway_context *context)
{
    size_t valid_report_count = 0u;
    bool report_orders_seen[SURVEY_GATEWAY_MAX_REPORTS] = {false};
    int ret;

    if (context == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_gateway_node_table_validate(context);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (context->report_count > SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_MALFORMED;
    }
    for (size_t node_index = 0u;
         node_index < SURVEY_GATEWAY_MAX_REPORTS;
         node_index++) {
        const struct survey_gateway_report_slot *slot =
            &context->reports[node_index];
        size_t entry_count;

        if (slot->metadata == UINT8_MAX) {
            continue;
        }
        if (node_index >= context->node_count ||
            survey_gateway_report_metadata_decode(
                slot, &entry_count, NULL) != PROTO_OK) {
            return PROTO_ERR_MALFORMED;
        }
        const uint8_t report_order = survey_gateway_report_order(slot);

        if (report_order >= context->report_count ||
            report_orders_seen[report_order]) {
            return PROTO_ERR_MALFORMED;
        }
        report_orders_seen[report_order] = true;
        if (slot->reverse_next_hop_index != UINT8_MAX &&
            slot->reverse_next_hop_index >= context->node_count) {
            return PROTO_ERR_MALFORMED;
        }
        if (slot->reverse_quality > 100u ||
            slot->reverse_hop_count > SURVEY_DEFAULT_TTL) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t i = 0u; i < entry_count; i++) {
            const struct survey_gateway_compact_reachability_entry *entry =
                &slot->entries[i];
            const uint8_t peer_index =
                survey_gateway_compact_entry_node_index(entry);

            if ((i >= SURVEY_GATEWAY_REPORT_ORDER_PART_COUNT &&
                 (entry->peer_index &
                  (uint8_t)~SURVEY_GATEWAY_COMPACT_NODE_INDEX_MASK) != 0u) ||
                peer_index >= context->node_count ||
                peer_index == node_index ||
                entry->quality > 100u) {
                return PROTO_ERR_MALFORMED;
            }
        }
        valid_report_count++;
    }
    return valid_report_count == context->report_count ?
           PROTO_OK : PROTO_ERR_MALFORMED;
}

static int survey_gateway_node_index_for_id(
    const struct survey_gateway_context *context,
    uint64_t node_id,
    uint8_t *node_index)
{
    if (node_id == 0u || node_index == NULL) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint8_t i = 0u; i < context->node_count; i++) {
        if (context->node_ids[i] == node_id) {
            *node_index = i;
            return PROTO_OK;
        }
    }
    return PROTO_ERR_NOT_FOUND;
}

static int survey_gateway_report_node_index_at(
    const struct survey_gateway_context *context,
    size_t report_index,
    uint8_t *node_index)
{
    int ret;

    if (node_index == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_gateway_context_validate(context);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (report_index >= context->report_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    for (uint8_t i = 0u; i < context->node_count; i++) {
        if (context->reports[i].metadata != UINT8_MAX &&
            survey_gateway_report_order(&context->reports[i]) ==
                report_index) {
            *node_index = i;
            return PROTO_OK;
        }
    }
    return PROTO_ERR_MALFORMED;
}

static int survey_gateway_resolve_prospective_node(
    const struct survey_gateway_context *context,
    uint64_t node_id,
    uint64_t *new_node_ids,
    uint8_t *new_node_count,
    uint8_t *node_index)
{
    int ret = survey_gateway_node_index_for_id(context, node_id, node_index);

    if (ret == PROTO_OK) {
        return PROTO_OK;
    }
    if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    for (uint8_t i = 0u; i < *new_node_count; i++) {
        if (new_node_ids[i] == node_id) {
            *node_index = (uint8_t)(context->node_count + i);
            return PROTO_OK;
        }
    }
    if ((size_t)context->node_count + *new_node_count >=
        SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_NO_SPACE;
    }
    new_node_ids[*new_node_count] = node_id;
    *node_index = (uint8_t)(context->node_count + *new_node_count);
    (*new_node_count)++;
    return PROTO_OK;
}

int survey_gateway_report_info_at(
    const struct survey_gateway_context *context,
    size_t report_index,
    uint64_t *anchor_id,
    size_t *entry_count,
    enum command_status *report_status)
{
    uint8_t node_index;
    int ret;

    if (context == NULL || anchor_id == NULL || entry_count == NULL ||
        report_status == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_gateway_report_node_index_at(
        context, report_index, &node_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_gateway_report_metadata_decode(
        &context->reports[node_index], entry_count, report_status);
    if (ret != PROTO_OK) {
        return ret;
    }
    *anchor_id = context->node_ids[node_index];
    return PROTO_OK;
}

int survey_gateway_report_entry_at(
    const struct survey_gateway_context *context,
    size_t report_index,
    size_t entry_index,
    struct survey_reachability_entry *entry)
{
    const struct survey_gateway_compact_reachability_entry *compact;
    uint8_t node_index;
    size_t entry_count;
    int ret;

    if (context == NULL || entry == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_gateway_report_node_index_at(
        context, report_index, &node_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_gateway_report_metadata_decode(
        &context->reports[node_index], &entry_count, NULL);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (entry_index >= entry_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    compact = &context->reports[node_index].entries[entry_index];
    const uint8_t peer_index =
        survey_gateway_compact_entry_node_index(compact);

    if (peer_index >= context->node_count) {
        return PROTO_ERR_MALFORMED;
    }
    *entry = (struct survey_reachability_entry) {
        .peer_id = context->node_ids[peer_index],
        .rssi_dbm = compact->rssi_dbm,
        .quality = compact->quality,
    };
    return PROTO_OK;
}

int survey_gateway_reach_report_compare(
    const struct survey_gateway_context *context,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    enum command_status report_status)
{
    uint8_t node_index;
    size_t stored_entry_count;
    enum command_status stored_status;
    int ret;

    if (context == NULL || (entries == NULL && entry_count != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (anchor_id == 0u ||
        report_status < COMMAND_OK ||
        report_status > COMMAND_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }
    if (entry_count > SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
        return PROTO_ERR_NO_SPACE;
    }
    for (size_t i = 0u; i < entry_count; i++) {
        ret = survey_reachability_entry_validate(&entries[i]);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (entries[i].peer_id == anchor_id) {
            return PROTO_ERR_MALFORMED;
        }
    }
    ret = survey_gateway_context_validate(context);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_gateway_node_index_for_id(
        context, anchor_id, &node_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (context->reports[node_index].metadata == UINT8_MAX) {
        return PROTO_ERR_NOT_FOUND;
    }
    ret = survey_gateway_report_metadata_decode(
        &context->reports[node_index],
        &stored_entry_count,
        &stored_status);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (stored_entry_count != entry_count || stored_status != report_status) {
        return PROTO_ERR_MALFORMED;
    }
    for (size_t i = 0u; i < entry_count; i++) {
        const struct survey_gateway_compact_reachability_entry *stored =
            &context->reports[node_index].entries[i];
        const uint8_t peer_index =
            survey_gateway_compact_entry_node_index(stored);

        if (peer_index >= context->node_count ||
            context->node_ids[peer_index] != entries[i].peer_id ||
            stored->rssi_dbm != entries[i].rssi_dbm ||
            stored->quality != entries[i].quality) {
            return PROTO_ERR_MALFORMED;
        }
    }
    return PROTO_OK;
}

int survey_gateway_note_reach_report_with_reverse_hint_status(
    struct survey_gateway_context *context,
    uint32_t survey_id,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    enum command_status report_status,
    const struct survey_gateway_reverse_hint *reverse_hint)
{
    uint64_t new_node_ids[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT + 2u];
    uint8_t entry_node_indices[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    uint8_t new_node_count = 0u;
    uint8_t anchor_index;
    uint8_t reverse_next_hop_index = UINT8_MAX;
    struct survey_gateway_report_slot *slot;
    int ret;

    if (context == NULL || (entries == NULL && entry_count != 0u)) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u || survey_id != context->survey_id) {
        return PROTO_ERR_STALE;
    }
    if (anchor_id == 0u ||
        report_status < COMMAND_OK ||
        report_status > COMMAND_INTERNAL_ERROR) {
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
        ret = survey_reachability_entry_validate(&entries[i]);

        if (ret != PROTO_OK) {
            return ret;
        }
        if (entries[i].peer_id == anchor_id) {
            return PROTO_ERR_MALFORMED;
        }
    }

    ret = survey_gateway_reach_report_compare(
        context, anchor_id, entries, entry_count, report_status);
    if (ret == PROTO_OK) {
        return PROTO_OK;
    }
    if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (context->report_count >= SURVEY_GATEWAY_MAX_REPORTS) {
        return PROTO_ERR_NO_SPACE;
    }

    ret = survey_gateway_resolve_prospective_node(
        context, anchor_id, new_node_ids, &new_node_count, &anchor_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    for (size_t i = 0u; i < entry_count; i++) {
        ret = survey_gateway_resolve_prospective_node(
            context,
            entries[i].peer_id,
            new_node_ids,
            &new_node_count,
            &entry_node_indices[i]);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (reverse_hint != NULL) {
        ret = survey_gateway_resolve_prospective_node(
            context,
            reverse_hint->next_hop_id,
            new_node_ids,
            &new_node_count,
            &reverse_next_hop_index);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    slot = &context->reports[anchor_index];
    if (slot->metadata != UINT8_MAX) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint8_t i = 0u; i < new_node_count; i++) {
        context->node_ids[context->node_count + i] = new_node_ids[i];
    }
    context->node_count = (uint8_t)(context->node_count + new_node_count);
    memset(slot, 0, sizeof(*slot));
    slot->reverse_next_hop_index = UINT8_MAX;
    for (size_t i = 0u; i < entry_count; i++) {
        slot->entries[i] =
            (struct survey_gateway_compact_reachability_entry) {
                .peer_index = entry_node_indices[i],
                .rssi_dbm = entries[i].rssi_dbm,
                .quality = entries[i].quality,
            };
    }
    if (reverse_hint != NULL) {
        slot->reverse_next_hop_index = reverse_next_hop_index;
        slot->reverse_quality = reverse_hint->quality;
        slot->reverse_hop_count = reverse_hint->hop_count;
    }
    survey_gateway_report_order_set(slot, context->report_count);
    slot->metadata =
        (uint8_t)(((uint8_t)report_status << 4u) | (uint8_t)entry_count);
    context->report_count++;
    context->pairs_planned = false;
    context->topology_complete = false;
    context->pair_count = 0u;
    return PROTO_OK;
}

int survey_gateway_note_reach_report_with_reverse_hint(
    struct survey_gateway_context *context,
    uint32_t survey_id,
    uint64_t anchor_id,
    const struct survey_reachability_entry *entries,
    size_t entry_count,
    const struct survey_gateway_reverse_hint *reverse_hint)
{
    return survey_gateway_note_reach_report_with_reverse_hint_status(
        context,
        survey_id,
        anchor_id,
        entries,
        entry_count,
        COMMAND_OK,
        reverse_hint);
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
    uint8_t node_index;
    const struct survey_gateway_report_slot *slot;
    int ret;

    if (context == NULL || reverse_hint == NULL) {
        return PROTO_ERR_ARG;
    }
    if (context->survey_id == 0u) {
        return PROTO_ERR_STALE;
    }
    if (target_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = survey_gateway_context_validate(context);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_gateway_node_index_for_id(
        context, target_id, &node_index);
    if (ret != PROTO_OK) {
        return ret;
    }
    slot = &context->reports[node_index];
    if (slot->metadata == UINT8_MAX ||
        slot->reverse_next_hop_index == UINT8_MAX) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (slot->reverse_next_hop_index >= context->node_count) {
        return PROTO_ERR_MALFORMED;
    }
    *reverse_hint = (struct survey_gateway_reverse_hint) {
        .target_id = context->node_ids[node_index],
        .next_hop_id =
            context->node_ids[slot->reverse_next_hop_index],
        .quality = slot->reverse_quality,
        .hop_count = slot->reverse_hop_count,
        .valid = true,
    };
    return PROTO_OK;
}

int survey_gateway_pair_at(const struct survey_gateway_context *context,
                           size_t pair_index,
                           struct survey_pair *pair)
{
    const struct survey_gateway_pair_entry *stored;
    struct survey_pair decoded;
    int ret;

    if (context == NULL || pair == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!context->pairs_planned) {
        return PROTO_ERR_STALE;
    }
    if (pair_index >= context->pair_count) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (context->pair_count > SURVEY_GATEWAY_MAX_PAIRS ||
        survey_gateway_context_validate(context) != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }
    stored = &context->pairs[pair_index];
    if (stored->initiator_index >= context->node_count ||
        stored->responder_index >= context->node_count ||
        stored->initiator_index == stored->responder_index) {
        return PROTO_ERR_MALFORMED;
    }

    decoded = (struct survey_pair) {
        .operation_generation = context->operation_generation,
        .survey_id = context->survey_id,
        .initiator_id = context->node_ids[stored->initiator_index],
        .responder_id = context->node_ids[stored->responder_index],
        .sample_count = context->sample_count,
    };
    ret = survey_pair_validate(&decoded);
    if (ret != PROTO_OK) {
        return ret;
    }
    *pair = decoded;
    return PROTO_OK;
}

int survey_extract_reach_request_tlvs(const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t *survey_id,
                                      uint32_t *duration_ms)
{
    uint32_t decoded_duration_ms;
    uint32_t decoded_survey_id;
    int ret;

    if (payload == NULL || survey_id == NULL || duration_ms == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_find_u32_tlv(payload,
                              payload_len,
                              TLV_SURVEY_ID,
                              &decoded_survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u32_tlv(payload,
                              payload_len,
                              TLV_DURATION_MS,
                              &decoded_duration_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (decoded_survey_id == 0u || decoded_duration_ms == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    *survey_id = decoded_survey_id;
    *duration_ms = decoded_duration_ms;
    return PROTO_OK;
}

int survey_extract_reach_report_tlvs(const uint8_t *payload,
                                     size_t payload_len,
                                     uint32_t *survey_id,
                                     uint64_t *anchor_id,
                                     struct survey_reachability_entry *entries,
                                     size_t entry_cap,
                                     size_t *entry_count)
{
    struct survey_reachability_entry decoded_entries[
        SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    uint64_t decoded_anchor_id;
    uint32_t decoded_survey_id;
    size_t decoded_entry_count = 0u;
    size_t offset = 0u;
    int ret;

    if (payload == NULL || survey_id == NULL || anchor_id == NULL ||
        entry_count == NULL || (entries == NULL && entry_cap != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = survey_find_u32_tlv(payload,
                              payload_len,
                              TLV_SURVEY_ID,
                              &decoded_survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u64_tlv(payload,
                              payload_len,
                              TLV_ANCHOR_ID,
                              &decoded_anchor_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (decoded_survey_id == 0u || decoded_anchor_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

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
            if (entry.peer_id == decoded_anchor_id) {
                return PROTO_ERR_MALFORMED;
            }
            if (decoded_entry_count >= entry_cap ||
                decoded_entry_count >=
                    SURVEY_GATEWAY_MAX_PEERS_PER_REPORT) {
                return PROTO_ERR_NO_SPACE;
            }
            decoded_entries[decoded_entry_count] = entry;
            decoded_entry_count++;
        }

        offset += len;
    }

    *survey_id = decoded_survey_id;
    *anchor_id = decoded_anchor_id;
    if (decoded_entry_count > 0u) {
        memcpy(entries,
               decoded_entries,
               decoded_entry_count * sizeof(decoded_entries[0]));
    }
    *entry_count = decoded_entry_count;
    return PROTO_OK;
}

int survey_extract_discovery_start_tlvs(const uint8_t *payload,
                                        size_t payload_len,
                                        struct survey_discovery_config *config)
{
    struct survey_discovery_config decoded = {0};
    struct operation_policy_set policies;
    const uint8_t *slot_count_value = NULL;
    const uint8_t *assignment_identity = NULL;
    uint8_t slot_count_len = 0u;
    uint8_t assignment_identity_len = 0u;
    uint32_t duration_ms = 0u;
    int ret;

    if (payload == NULL || config == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_operation_generation_extract_tlv(
        payload, payload_len, &decoded.operation_generation);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    ret = survey_find_u32_tlv(payload,
                              payload_len,
                              TLV_SURVEY_ID,
                              &decoded.survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u32_tlv(payload,
                              payload_len,
                              TLV_DISCOVERY_START_DELAY_MS,
                              &decoded.start_delay_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u16_tlv(payload,
                              payload_len,
                              TLV_DISCOVERY_SLOT_MS,
                              &decoded.slot_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_find_unique(payload,
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
    decoded.slot_count = slot_count_value[0];
    ret = tlv_find_unique(payload,
                          payload_len,
                          TLV_SURVEY_ASSIGNMENT_IDENTITY,
                          &assignment_identity,
                          &assignment_identity_len);
    if (ret == PROTO_OK) {
        if (assignment_identity_len !=
            2u * sizeof(uint32_t) +
                sizeof(decoded.assignment_table_commitment.bytes)) {
            return PROTO_ERR_MALFORMED;
        }
        decoded.assignment_epoch =
            proto_get_u32_le(&assignment_identity[0]);
        decoded.assignment_table_seq =
            proto_get_u32_le(&assignment_identity[sizeof(uint32_t)]);
        memcpy(decoded.assignment_table_commitment.bytes,
               &assignment_identity[2u * sizeof(uint32_t)],
               sizeof(decoded.assignment_table_commitment.bytes));
    } else if (ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    decoded.round_count = SURVEY_DISCOVERY_MAX_ROUND_COUNT;
    ret = operation_policy_set_from_tlvs(payload, payload_len, &policies);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (policies.discovery_present) {
        if (policies.discovery.start_delay_ms != decoded.start_delay_ms ||
            policies.discovery.slot_ms != decoded.slot_ms ||
            policies.discovery.slot_count != decoded.slot_count) {
            return PROTO_ERR_MALFORMED;
        }
        decoded.round_count = policies.discovery.round_count;
    }
    ret = survey_find_u32_tlv(payload, payload_len, TLV_DURATION_MS, &duration_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (survey_discovery_config_validate(&decoded) != PROTO_OK ||
        duration_ms != survey_discovery_duration_ms(&decoded)) {
        return PROTO_ERR_MALFORMED;
    }
    *config = decoded;
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

    ret = tlv_find_unique(payload,
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
    ret = tlv_find_unique(payload,
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
    struct survey_pair parsed = {0};
    int ret;

    if (pair == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_operation_generation_extract_tlv(
        payload, payload_len, &parsed.operation_generation);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    ret = survey_find_u32_tlv(payload, payload_len, TLV_SURVEY_ID, &parsed.survey_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &parsed.initiator_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &parsed.responder_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = survey_find_u16_tlv(payload, payload_len, TLV_SAMPLE_COUNT, &parsed.sample_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    ret = survey_pair_validate(&parsed);
    if (ret == PROTO_OK) {
        *pair = parsed;
    }
    return ret;
}

uint32_t survey_operation_session_id(uint64_t operation_generation)
{
    uint32_t session_id = (uint32_t)operation_generation;

    return operation_generation == 0u || session_id == 0u ? 0u : session_id;
}

int survey_operation_generation_append_tlv(uint8_t *payload,
                                           size_t payload_cap,
                                           size_t *offset,
                                           uint64_t operation_generation)
{
    if (operation_generation == 0u ||
        survey_operation_session_id(operation_generation) == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    return tlv_append_u64(payload,
                          payload_cap,
                          offset,
                          TLV_SURVEY_OPERATION_GENERATION,
                          operation_generation);
}

int survey_operation_generation_extract_tlv(
    const uint8_t *payload,
    size_t payload_len,
    uint64_t *operation_generation)
{
    int ret;

    if (payload == NULL || operation_generation == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_find_u64_tlv(payload,
                              payload_len,
                              TLV_SURVEY_OPERATION_GENERATION,
                              operation_generation);
    if (ret != PROTO_OK) {
        return ret;
    }
    return *operation_generation == 0u ||
           survey_operation_session_id(*operation_generation) == 0u ?
               PROTO_ERR_MALFORMED : PROTO_OK;
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
    if (config->operation_generation != 0u) {
        ret = survey_operation_generation_append_tlv(
            payload,
            payload_cap,
            offset,
            config->operation_generation);
        if (ret != PROTO_OK) {
            return ret;
        }
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
    if (config->assignment_epoch != 0u) {
        uint8_t identity[2u * sizeof(uint32_t) +
                         SEMANTIC_DIGEST_SHA256_LEN];

        proto_put_u32_le(&identity[0], config->assignment_epoch);
        proto_put_u32_le(&identity[sizeof(uint32_t)],
                         config->assignment_table_seq);
        memcpy(&identity[2u * sizeof(uint32_t)],
               config->assignment_table_commitment.bytes,
               sizeof(config->assignment_table_commitment.bytes));
        ret = tlv_append_bytes(payload,
                               payload_cap,
                               offset,
                               TLV_SURVEY_ASSIGNMENT_IDENTITY,
                               identity,
                               sizeof(identity));
        if (ret != PROTO_OK) {
            return ret;
        }
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
    if (pair->operation_generation != 0u) {
        ret = survey_operation_generation_append_tlv(
            payload,
            payload_cap,
            offset,
            pair->operation_generation);
        if (ret != PROTO_OK) {
            return ret;
        }
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

int survey_pair_result_payload_validate(const uint8_t *payload,
                                        size_t payload_len,
                                        struct survey_sample *sample)
{
    struct survey_sample parsed = {0};
    uint8_t canonical[SURVEY_SAMPLE_TLV_MAX_LEN];
    size_t canonical_len = 0u;
    size_t offset;
    uint8_t previous_stage = 0u;
    bool timestamp_seen = false;
    int ret;

    if (payload == NULL || sample == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_extract_sample_tlvs(payload, payload_len, &parsed);
    if (ret != PROTO_OK ||
        survey_append_sample_tlvs(canonical,
                                  sizeof(canonical),
                                  &canonical_len,
                                  &parsed) != PROTO_OK ||
        canonical_len >= payload_len ||
        memcmp(canonical, payload, canonical_len) != 0) {
        return PROTO_ERR_MALFORMED;
    }

    offset = canonical_len;
    while (offset < payload_len) {
        uint8_t expected_len;
        uint8_t stage;
        uint8_t type;
        uint8_t value_len;

        if (payload_len - offset < PROTO_TLV_HEADER_LEN) {
            return PROTO_ERR_MALFORMED;
        }
        type = payload[offset];
        value_len = payload[offset + 1u];
        offset += PROTO_TLV_HEADER_LEN;
        if (value_len > payload_len - offset) {
            return PROTO_ERR_MALFORMED;
        }

        switch (type) {
        case TLV_TIMESTAMP_MS:
            stage = 1u;
            expected_len = sizeof(uint64_t);
            timestamp_seen = true;
            break;
        case TLV_UWB_RSL_DBM:
            stage = 2u;
            expected_len = sizeof(int8_t);
            break;
        case TLV_UWB_CLOCK_OFFSET_RAW:
            stage = 3u;
            expected_len = sizeof(uint16_t);
            break;
        case TLV_CLICKER_CLOCK_OFFSET_RAW:
            stage = 4u;
            expected_len = sizeof(uint16_t);
            break;
        case TLV_UWB_CARRIER_INTEGRATOR:
            stage = 5u;
            expected_len = sizeof(int32_t);
            break;
        case TLV_UWB_RAW_TIMESTAMPS:
            stage = 6u;
            expected_len = 6u * sizeof(uint32_t);
            break;
        default:
            return PROTO_ERR_MALFORMED;
        }
        if (value_len != expected_len || stage <= previous_stage ||
            (stage != 1u && !timestamp_seen)) {
            return PROTO_ERR_MALFORMED;
        }
        previous_stage = stage;
        offset += value_len;
    }
    if (!timestamp_seen) {
        return PROTO_ERR_MALFORMED;
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
    uint32_t session_id;
    int ret;

    if (packet == NULL || sample == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_sample_validate(sample);
    if (ret != PROTO_OK) {
        return ret;
    }
    session_id = sample->pair.operation_generation == 0u ?
        sample->pair.survey_id :
        survey_operation_session_id(sample->pair.operation_generation);
    if (!ids_are_valid(reporter_id, gateway_id) ||
        seq == 0u || payload_len == 0u || session_id == 0u ||
        reporter_id != sample->pair.responder_id) {
        return PROTO_ERR_MALFORMED;
    }

    *packet = (struct proto_packet) {
        .msg_type = MSG_SURVEY_PAIR_RESULT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = reporter_id,
        .dst_id = gateway_id,
        .session_id = session_id,
        .seq = seq,
        .ttl = SURVEY_DEFAULT_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
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
                                                   sample->pair.responder_id,
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
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (gateway_id == MESH_BROADCAST_ID || survey_id == 0u || seq == 0u ||
        payload_len == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    *packet = (struct proto_packet) {
        .msg_type = MSG_SURVEY_REACH_REQ,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = gateway_id,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = survey_id,
        .seq = seq,
        .ttl = SURVEY_DEFAULT_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
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
    if (!ids_are_valid(anchor_id, gateway_id) || survey_id == 0u ||
        seq == 0u || payload_len == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    *packet = (struct proto_packet) {
        .msg_type = MSG_SURVEY_REACH_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = anchor_id,
        .dst_id = gateway_id,
        .session_id = survey_id,
        .seq = seq,
        .ttl = SURVEY_DEFAULT_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
    return PROTO_OK;
}

int survey_init_discovery_start_packet(struct proto_packet *packet,
                                       uint64_t gateway_id,
                                       const struct survey_discovery_config *config,
                                       uint16_t seq,
                                       uint8_t payload_len)
{
    uint32_t session_id;
    int ret;

    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_discovery_config_validate(config);
    if (ret != PROTO_OK) {
        return ret;
    }
    session_id = config->operation_generation == 0u ?
        config->survey_id :
        survey_operation_session_id(config->operation_generation);
    if (gateway_id == MESH_BROADCAST_ID || seq == 0u ||
        payload_len == 0u ||
        session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    *packet = (struct proto_packet) {
        .msg_type = MSG_SURVEY_DISCOVERY_START,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = gateway_id,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = session_id,
        .seq = seq,
        .ttl = SURVEY_DEFAULT_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
    return PROTO_OK;
}

int survey_init_discovery_report_packet(struct proto_packet *packet,
                                        uint64_t anchor_id,
                                        uint64_t gateway_id,
                                        uint32_t survey_id,
                                        uint64_t operation_generation,
                                        uint32_t boot_incarnation,
                                        uint16_t seq,
                                        uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(anchor_id, gateway_id) || survey_id == 0u ||
        seq == 0u || payload_len == 0u || boot_incarnation == 0u ||
        (operation_generation != 0u &&
         survey_operation_session_id(operation_generation) == 0u)) {
        return PROTO_ERR_MALFORMED;
    }

    *packet = (struct proto_packet) {
        .msg_type = MSG_SURVEY_DISCOVERY_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = anchor_id,
        .dst_id = gateway_id,
        .session_id = boot_incarnation,
        .seq = seq,
        .ttl = SURVEY_DEFAULT_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
    return PROTO_OK;
}

uint16_t survey_discovery_sequence_next(uint16_t *sequence_state)
{
    if (sequence_state == NULL || *sequence_state == UINT16_MAX) {
        return 0u;
    }
    (*sequence_state)++;
    return *sequence_state;
}

int survey_init_pair_prepare_packet(struct proto_packet *packet,
                                    const struct survey_pair *pair,
                                    uint64_t gateway_id,
                                    uint64_t target_id,
                                    uint16_t seq,
                                    uint8_t payload_len)
{
    uint32_t session_id;
    int ret;

    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = survey_pair_validate(pair);
    if (ret != PROTO_OK) {
        return ret;
    }
    session_id = pair->operation_generation == 0u ?
        pair->survey_id :
        survey_operation_session_id(pair->operation_generation);
    if (!ids_are_valid(gateway_id, pair->initiator_id) ||
        !ids_are_valid(gateway_id, pair->responder_id) ||
        (target_id != pair->initiator_id &&
         target_id != pair->responder_id) ||
        seq == 0u || payload_len == 0u || session_id == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    *packet = (struct proto_packet) {
        .msg_type = MSG_SURVEY_PAIR_PREPARE,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = gateway_id,
        .dst_id = target_id,
        .session_id = session_id,
        .seq = seq,
        .ttl = SURVEY_DEFAULT_TTL,
        .payload_len = payload_len,
        .message_age_ms = 0u,
    };
    return PROTO_OK;
}
