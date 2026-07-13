#ifndef APP_GATEWAY_ASSIGNMENT_PUBLISHER_H
#define APP_GATEWAY_ASSIGNMENT_PUBLISHER_H

#include "app_gateway_command_observability.h"
#include "discovery_assignment.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES 50u
#define APP_GATEWAY_ASSIGNMENT_PUBLISHER_RAM_BUDGET_BYTES 704u

typedef int (*app_gateway_assignment_publisher_emit_fn)(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx);

struct app_gateway_assignment_publisher_ops {
    app_gateway_assignment_publisher_emit_fn emit_if_available;
    void *ctx;
};

struct app_gateway_assignment_publisher_diagnostics {
    uint16_t mapping_count;
    uint16_t sent_mappings;
    uint16_t duplicate_batches;
    uint32_t inflight_event_seq;
    bool active;
    bool terminal_pending;
};

int app_gateway_assignment_publisher_init(
    const struct app_gateway_assignment_publisher_ops *ops);
int app_gateway_assignment_publisher_stage_batch(
    const struct gateway_command_event *base_event,
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint16_t duplicate_count);
void app_gateway_assignment_publisher_stage_table_ready(
    const struct gateway_command_event *event);
bool app_gateway_assignment_publisher_capture_terminal(
    const struct gateway_command_event *event);
void app_gateway_assignment_publisher_pump(void);
void app_gateway_assignment_publisher_note_sent(uint32_t event_seq);
void app_gateway_assignment_publisher_get_diagnostics(
    struct app_gateway_assignment_publisher_diagnostics *diagnostics);

#endif
