#ifndef SURVEY_ROUND_CONTROL_H
#define SURVEY_ROUND_CONTROL_H

#include "protocol.h"
#include "semantic_digest.h"
#include "survey.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_LEGACY_ROUND_ID 0u
/*
 * START responder is confirmed before START initiator is sent. Reserve two
 * complete control-result horizons plus the local skew margin so both
 * targeted START packets can carry one age-compensated future release.
 */
#define SURVEY_ROUND_START_EXECUTE_DELAY_MS                               \
    ((2u * SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS) +                      \
     SURVEY_PAIR_START_SKEW_MARGIN_MS)

struct survey_round_plan_entry {
    struct survey_pair pair;
    uint8_t lane_index;
    uint8_t plan_pair_index;
    uint8_t reruns_started;
};

struct survey_round_plan_identity {
    uint64_t operation_generation;
    uint32_t survey_id;
    uint32_t operation_session_id;
    uint32_t execute_delay_ms;
    uint32_t observation_window_ms;
    uint16_t round_id;
    uint8_t max_parallel_pairs;
    uint8_t max_reruns;
};

/* A present round ID is always nonzero; omission decodes as legacy round 0. */
int survey_round_id_append_tlv(uint8_t *payload,
                               size_t payload_cap,
                               size_t *offset,
                               uint16_t round_id);
int survey_round_id_extract_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint16_t *round_id);

int survey_round_commitment_compute(
    const struct survey_round_plan_identity *identity,
    const struct survey_round_plan_entry *entries,
    size_t entry_count,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN]);
int survey_pair_control_commitment_compute(
    const struct survey_pair *pair,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN]);
int survey_round_commitment_append_tlv(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN]);
int survey_round_commitment_extract_tlv(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN]);

#ifdef __cplusplus
}
#endif

#endif
