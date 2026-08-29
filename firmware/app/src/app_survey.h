#ifndef APP_SURVEY_H
#define APP_SURVEY_H

#include "discovery_assignment.h"
#include "protocol.h"
#include "survey_protocol.h"
#include "survey_response_lane.h"

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_survey_ops {
    int (*send_control)(const struct survey_control *control,
                        uint64_t *origin_ms);
    int (*emit_event)(const struct survey_event *event);
    void (*wake_gateway_rx)(void);
    void (*gateway_terminal)(void);
    int (*anchor_upstream)(uint64_t *parent_id, uint8_t *hop_count);
    int (*anchor_reschedule)(struct k_work_delayable *work,
                             uint32_t delay_ms);
};

struct app_survey_gateway_roster {
    struct survey_assignment_identity assignment;
    uint64_t node_ids[SURVEY_MAX_ANCHORS];
    uint8_t slots[SURVEY_MAX_ANCHORS];
    uint8_t hop_counts[SURVEY_MAX_ANCHORS];
    size_t node_count;
};

int app_survey_init(const struct app_survey_ops *ops);

int app_survey_gateway_start(
    const struct app_survey_gateway_roster *roster,
    struct survey_identity *identity_out);
int app_survey_gateway_submit_plan(
    const struct survey_host_plan_request *request,
    struct survey_plan_build_result *result_out);
int app_survey_gateway_abort(const struct survey_identity *identity);
int app_survey_gateway_status(struct survey_event *event_out);
bool app_survey_gateway_active(void);
bool app_survey_gateway_response_window(uint64_t now_ms,
                                        uint64_t *round_deadline_ms);
bool app_survey_gateway_response_pending_wait_ms(uint64_t now_ms,
                                                 uint32_t *wait_ms);
bool app_survey_gateway_radio_quiet(uint64_t now_ms, uint32_t *wait_ms);
int app_survey_gateway_handle_bundle(
    const struct survey_response_bundle *bundle,
    uint64_t received_at_ms,
    struct survey_response_hop_ack *ack);

int app_survey_anchor_note_ram_roster(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint8_t table_slot_count,
    uint32_t assignment_epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment);
void app_survey_anchor_clear_ram_roster(void);
int app_survey_anchor_apply_control(const struct proto_packet *packet,
                                    const struct survey_control *control);
bool app_survey_anchor_active(void);

#endif
