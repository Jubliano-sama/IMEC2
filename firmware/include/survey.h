#ifndef SURVEY_H
#define SURVEY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_MIN_SAMPLE_COUNT 1u
#define SURVEY_MAX_SAMPLE_COUNT 1000u
#define SURVEY_DEFAULT_TTL 4u

struct survey_pair {
    uint32_t survey_id;
    uint64_t initiator_id;
    uint64_t responder_id;
    uint16_t sample_count;
};

struct survey_sample {
    struct survey_pair pair;
    uint16_t sample_index;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status range_status;
};

bool survey_sample_count_valid(uint16_t sample_count);
int survey_pair_validate(const struct survey_pair *pair);
int survey_sample_validate(const struct survey_sample *sample);
int survey_append_pair_tlvs(uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *offset,
                                 const struct survey_pair *pair);
int survey_append_sample_tlvs(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   const struct survey_sample *sample);
int survey_init_result_packet(struct proto_packet *packet,
                                   const struct survey_sample *sample,
                                   uint64_t gateway_id,
                                   uint16_t seq,
                                   uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
