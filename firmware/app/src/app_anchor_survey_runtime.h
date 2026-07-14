#ifndef APP_ANCHOR_SURVEY_RUNTIME_H
#define APP_ANCHOR_SURVEY_RUNTIME_H

#include "app_radio_low_power_policy.h"
#include "gateway_command.h"
#include "protocol.h"
#include "survey.h"

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dwm3000_range_result;

struct app_anchor_survey_runtime_ops {
    struct k_work_q *work_queue;
    int (*send_command_result)(const struct proto_packet *command,
                               enum command_id command_id,
                               enum command_status status,
                               uint8_t reason,
                               const struct command_result_id *collection_result_id,
                               uint32_t collection_epoch_id);
    int (*enter_low_power)(enum app_radio_low_power_mode mode,
                           const char *reason);
    void (*set_uwb_busy)(bool busy);
    void (*note_uwb_awake_since)(int64_t start_ms,
                                 uint32_t already_counted_us);
    int (*start_uwb_scan)(void);
    int (*queue_sample_result)(
        const struct survey_pair *pair,
        uint16_t sample_index,
        uint64_t reporter_id,
        const struct dwm3000_range_result *range_result);
    uint32_t (*report_queue_used)(void);
    void (*report_schedule)(uint32_t delay_ms);
    bool (*relay_tx_active)(void);
    bool (*connected_radio_active)(void);
};

int app_anchor_survey_runtime_init(
    const struct app_anchor_survey_runtime_ops *ops);
int app_anchor_survey_runtime_start(void);

uint16_t app_anchor_survey_runtime_next_sequence(void);
bool app_anchor_survey_runtime_discovery_is_pending(void);
bool app_anchor_survey_runtime_abort_requested(void);
void app_anchor_survey_runtime_queue_discovery(
    const struct survey_discovery_config *config,
    uint32_t start_ms);
void app_anchor_survey_runtime_schedule_ms(uint32_t delay_ms);

void app_anchor_survey_runtime_handle_pair_prepare(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_anchor_survey_runtime_start_pair_from_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    enum command_status *status,
    uint8_t *reason);
int app_anchor_survey_runtime_bind_pair_start_delivery(
    const struct proto_packet *command,
    uint32_t delivery_handle);
bool app_anchor_survey_runtime_cancel_pair_start(
    const struct proto_packet *command);
void app_anchor_survey_runtime_abort_pair(void);
bool app_anchor_survey_runtime_abort_pair_matching(
    const struct survey_pair *pair,
    uint32_t session_id);

#endif
