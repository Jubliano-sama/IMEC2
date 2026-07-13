#ifndef APP_STACK_WORKLOAD_DIAG_H
#define APP_STACK_WORKLOAD_DIAG_H

#include "app_stack_diag.h"
#include "report.h"

#include <stdint.h>

struct app_stack_workload_diag_pressure {
    uint16_t queue_depth;
    uint16_t custody_depth;
    uint16_t credit_available;
    uint16_t retry_depth;
    uint16_t drain_depth;
};

/* Correlates a real production packet with its stack-evidence run. */
#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
void app_stack_workload_diag_click_admit(const struct proto_packet *packet,
                                         uint16_t queue_depth,
                                         uint16_t custody_depth);
void app_stack_workload_diag_click_sample(const struct proto_packet *packet,
                                          uint16_t queue_depth,
                                          uint16_t custody_depth);
void app_stack_workload_diag_click_release(const struct proto_packet *packet,
                                           int result,
                                           uint16_t queue_depth,
                                           uint16_t custody_depth);

void app_stack_workload_diag_cir_admit(const struct proto_packet *packet,
                                       uint16_t queue_depth,
                                       uint16_t custody_depth);
void app_stack_workload_diag_cir_sample(const struct proto_packet *packet,
                                        uint16_t queue_depth,
                                        uint16_t custody_depth);
void app_stack_workload_diag_cir_release(const struct proto_packet *packet,
                                         int result,
                                         uint16_t queue_depth,
                                         uint16_t custody_depth);

void app_stack_workload_diag_relay_admit(const struct proto_packet *packet,
                                         uint16_t queue_depth,
                                         uint16_t custody_depth);
void app_stack_workload_diag_relay_sample(const struct proto_packet *packet,
                                          uint16_t queue_depth,
                                          uint16_t custody_depth);
void app_stack_workload_diag_relay_release(const struct proto_packet *packet,
                                           int result,
                                           uint16_t queue_depth,
                                           uint16_t custody_depth);

void app_stack_workload_diag_click_activity_admit(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_click_activity_sample(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_click_activity_release(
    const struct proto_packet *packet, int result, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_anchor_survey_admit(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_anchor_survey_sample(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_anchor_survey_release(
    const struct proto_packet *packet, int result, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_gateway_report_cycle(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_gateway_control_admit(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_gateway_control_sample(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth);
void app_stack_workload_diag_gateway_control_release(
    const struct proto_packet *packet, int result, uint16_t queue_depth,
    uint16_t custody_depth);

void app_stack_workload_diag_ble_admit(const struct proto_packet *packet,
                                       uint16_t queue_depth,
                                       uint16_t custody_depth);
void app_stack_workload_diag_ble_sample(const struct proto_packet *packet,
                                        uint16_t queue_depth,
                                        uint16_t custody_depth);
void app_stack_workload_diag_ble_release(const struct proto_packet *packet,
                                         int result,
                                         uint16_t queue_depth,
                                         uint16_t custody_depth);
void app_stack_workload_diag_ble_release_all(int result,
                                             uint16_t queue_depth,
                                             uint16_t custody_depth);
void app_stack_workload_diag_ble_release_all_with_pressure(
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure);
void app_stack_workload_diag_ble_admit_with_pressure(
    const struct proto_packet *packet,
    enum app_stack_diag_owner owner,
    const struct app_stack_workload_diag_pressure *pressure);
void app_stack_workload_diag_ble_sample_with_pressure(
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure);
void app_stack_workload_diag_ble_terminal_with_pressure(
    const struct proto_packet *packet,
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure);
#else
#define APP_STACK_WORKLOAD_DIAG_NOOP(prefix) \
    static inline void app_stack_workload_diag_##prefix##_admit( \
        const struct proto_packet *packet, uint16_t queue_depth, uint16_t custody_depth) \
    { (void)packet; (void)queue_depth; (void)custody_depth; } \
    static inline void app_stack_workload_diag_##prefix##_sample( \
        const struct proto_packet *packet, uint16_t queue_depth, uint16_t custody_depth) \
    { (void)packet; (void)queue_depth; (void)custody_depth; } \
    static inline void app_stack_workload_diag_##prefix##_release( \
        const struct proto_packet *packet, int result, uint16_t queue_depth, uint16_t custody_depth) \
    { (void)packet; (void)result; (void)queue_depth; (void)custody_depth; }

APP_STACK_WORKLOAD_DIAG_NOOP(click)
APP_STACK_WORKLOAD_DIAG_NOOP(cir)
APP_STACK_WORKLOAD_DIAG_NOOP(relay)
APP_STACK_WORKLOAD_DIAG_NOOP(ble)
APP_STACK_WORKLOAD_DIAG_NOOP(click_activity)
APP_STACK_WORKLOAD_DIAG_NOOP(anchor_survey)
APP_STACK_WORKLOAD_DIAG_NOOP(gateway_control)
static inline void app_stack_workload_diag_gateway_report_cycle(
    const struct proto_packet *packet, uint16_t queue_depth,
    uint16_t custody_depth)
{ (void)packet; (void)queue_depth; (void)custody_depth; }
static inline void app_stack_workload_diag_ble_admit_with_pressure(
    const struct proto_packet *packet, enum app_stack_diag_owner owner,
    const struct app_stack_workload_diag_pressure *pressure)
{ (void)packet; (void)owner; (void)pressure; }
static inline void app_stack_workload_diag_ble_sample_with_pressure(
    const struct proto_packet *packet,
    const struct app_stack_workload_diag_pressure *pressure)
{ (void)packet; (void)pressure; }
static inline void app_stack_workload_diag_ble_terminal_with_pressure(
    const struct proto_packet *packet, enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{ (void)packet; (void)outcome; (void)pressure; }
static inline void app_stack_workload_diag_ble_release_all_with_pressure(
    enum app_stack_diag_terminal_outcome outcome,
    const struct app_stack_workload_diag_pressure *pressure)
{ (void)outcome; (void)pressure; }

static inline void app_stack_workload_diag_ble_release_all(
    int result, uint16_t queue_depth, uint16_t custody_depth)
{
    (void)result;
    (void)queue_depth;
    (void)custody_depth;
}
#endif

#endif
