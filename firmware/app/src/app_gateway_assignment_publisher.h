#ifndef APP_GATEWAY_ASSIGNMENT_PUBLISHER_H
#define APP_GATEWAY_ASSIGNMENT_PUBLISHER_H

#include "app_gateway_command_observability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES 50u
#define APP_GATEWAY_ASSIGNMENT_PUBLISHER_RAM_BUDGET_BYTES 704u

typedef int (*app_gateway_assignment_publisher_emit_fn)(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx);
/* Reserve a fresh host-stream identity without queueing or exposing it. */
typedef int (*app_gateway_assignment_publisher_reserve_fn)(
    struct gateway_command_event *event,
    void *ctx);
typedef int (*app_gateway_assignment_publisher_complete_fn)(
    const struct gateway_command_event *base_event,
    void *ctx);

struct app_gateway_assignment_publisher_ops {
    app_gateway_assignment_publisher_emit_fn emit_if_available;
    app_gateway_assignment_publisher_reserve_fn reserve_event_seq;
    app_gateway_assignment_publisher_complete_fn batch_completed;
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
/*
 * Reserve the complete mapping without making any part visible to the host.
 * The caller may then perform its irreversible durable commit and either
 * publish or abort this exact prepared batch. A non-NULL hop_counts array
 * carries live, validated route depth into the reliable mapping events; cold
 * replay passes NULL because route evidence is intentionally not durable.
 */
int app_gateway_assignment_publisher_prepare_table(
    const struct gateway_command_event *base_event,
    const uint64_t *anchor_ids,
    const uint8_t *slots,
    const uint8_t *hop_counts,
    size_t anchor_count,
    uint64_t acknowledged_mask,
    uint16_t duplicate_count);
/* Mark an exact prepared batch as a reset reconstruction before exposure.
 * Every emitted mapping, aggregate, and terminal then carries REPLAY while
 * retaining its original semantic fields. */
int app_gateway_assignment_publisher_mark_prepared_replay(
    const struct gateway_command_event *base_event);
/* Return whether one fully materialized event belongs to the durable
 * assignment-publication batch.  This is the sole domain allowed to acquire
 * host-receipt custody; ordinary command observability remains best effort. */
bool app_gateway_assignment_publisher_event_is_reliable(
    const struct gateway_command_event *event);
int app_gateway_assignment_publisher_commit_prepared_batch(
    const struct gateway_command_event *base_event);
bool app_gateway_assignment_publisher_abort_prepared_batch(
    const struct gateway_command_event *base_event);
/* Explicitly abandon any active batch with this exact assignment identity.
 * This is reserved for a user-requested clean-slate RAM-only assignment,
 * where stale publication telemetry must not block the replacement run. */
bool app_gateway_assignment_publisher_discard_batch(
    const struct gateway_command_event *base_event);
void app_gateway_assignment_publisher_stage_table_ready(
    const struct gateway_command_event *event);
bool app_gateway_assignment_publisher_capture_terminal(
    const struct gateway_command_event *event);
void app_gateway_assignment_publisher_pump(void);
/*
 * Record one exact GUI-host-receipted publisher event without running
 * persistence callbacks in the Bluetooth receipt context. ATT notification
 * alone never advances the active publication. A positive return means this
 * exact event advanced (or had already advanced at the retry boundary), zero
 * means it belongs to another command stream, and a negative result means an
 * active assignment publication rejected the identity. Callers must not
 * retire BLE custody after a negative result.
 */
int app_gateway_assignment_publisher_note_host_receipt(
    const struct gateway_command_event *event);
/* True when owner work can emit or durably complete the active publication. */
bool app_gateway_assignment_publisher_work_pending(void);
/*
 * Complete one durable terminal debt from its owning workqueue. Returns one
 * after retiring the matching batch, zero when no completion is due, or a
 * negative callback error while retaining the exact debt for retry.
 */
int app_gateway_assignment_publisher_complete_pending(void);
void app_gateway_assignment_publisher_get_diagnostics(
    struct app_gateway_assignment_publisher_diagnostics *diagnostics);

#endif
