#ifndef APP_ANCHOR_SURVEY_DISCOVERY_H
#define APP_ANCHOR_SURVEY_DISCOVERY_H

#include "protocol.h"
#include "survey.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum app_anchor_survey_discovery_admission {
    APP_ANCHOR_SURVEY_DISCOVERY_ACCEPTED = 0,
    APP_ANCHOR_SURVEY_DISCOVERY_DUPLICATE,
    APP_ANCHOR_SURVEY_DISCOVERY_BUSY,
};

struct app_anchor_survey_discovery_ops {
    bool (*abort_requested)(void);
    void (*abort_pair)(void);
    void (*preempt_radio)(uint32_t survey_id);
    enum app_anchor_survey_discovery_admission (*admit_start)(
        const struct survey_discovery_config *config);
    int (*queue_start)(const struct survey_discovery_config *config,
                       uint32_t start_ms,
                       uint32_t delay_ms);
    int (*schedule_work_ms)(uint32_t delay_ms);
    int (*boot_incarnation)(uint32_t *incarnation);
    uint16_t (*next_sequence)(void);
};

int app_anchor_survey_discovery_init(
    const struct app_anchor_survey_discovery_ops *ops);
void app_anchor_survey_discovery_handle_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_anchor_survey_discovery_run(
    const struct survey_discovery_config *config,
    uint32_t start_ms,
    bool *functional_radio_outcome);
int app_anchor_survey_discovery_stage_empty_report(
    const struct survey_discovery_config *config,
    uint32_t start_ms);
int app_anchor_survey_discovery_retry_report(void);
int app_anchor_survey_discovery_report_custody_status(
    uint64_t operation_generation);
/*
 * Retire only discovery-report custody from generations older than a
 * strictly newer durable survey operation.  Any live communication handle is
 * abandoned before the RAM owner is cleared; no gateway ACK is synthesized.
 * Returns -EINPROGRESS while the exact older owner is still draining.
 */
int app_anchor_survey_discovery_supersede_before(
    uint64_t operation_generation,
    bool *retirement_pending);
bool app_anchor_survey_discovery_report_staged(uint32_t operation_session_id);
int app_anchor_survey_delivery_gateway_confirmed(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN]);
int app_anchor_survey_delivery_transport_released(
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    bool preempted);

#endif
