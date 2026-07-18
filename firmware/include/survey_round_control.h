#ifndef SURVEY_ROUND_CONTROL_H
#define SURVEY_ROUND_CONTROL_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SURVEY_LEGACY_ROUND_ID 0u
#define SURVEY_ROUND_GO_BASE_EXECUTE_DELAY_MS 2500u

struct survey_round_go {
    uint32_t survey_id;
    uint16_t round_id;
};

/* A present round ID is always nonzero; omission decodes as legacy round 0. */
int survey_round_id_append_tlv(uint8_t *payload,
                               size_t payload_cap,
                               size_t *offset,
                               uint16_t round_id);
int survey_round_id_extract_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint16_t *round_id);

/* GO is a broadcast MSG_COMMAND carrying command, survey, and nonzero round. */
int survey_round_go_append_tlvs(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                const struct survey_round_go *go);
int survey_round_go_from_tlvs(const uint8_t *payload,
                              size_t payload_len,
                              struct survey_round_go *go);
int survey_round_go_init_packet(struct proto_packet *packet,
                                uint64_t gateway_id,
                                uint32_t survey_id,
                                uint16_t seq,
                                uint16_t payload_len);
/* One-hop commands use the base; every additional RF hop adds one complete
 * randomized relay-backoff horizon. */
uint32_t survey_round_go_execute_delay_ms(uint8_t gateway_hop_count);

#ifdef __cplusplus
}
#endif

#endif
