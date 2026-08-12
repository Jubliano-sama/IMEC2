#ifndef SURVEY_ANCHOR_DEADLINE_H
#define SURVEY_ANCHOR_DEADLINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One anchor survey worker services several independent clocks.  A slot owns
 * an absolute uptime deadline and the semantic operation generation that
 * published it.  The validity bit is separate because uptime zero is a legal
 * wrapped deadline.
 */
enum survey_anchor_deadline_owner {
    SURVEY_ANCHOR_DEADLINE_OPERATION = 0,
    SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY,
    SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY,
    SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY,
    SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION,
    SURVEY_ANCHOR_DEADLINE_OWNER_COUNT,
};

#define SURVEY_ANCHOR_DEADLINE_SEMANTIC_OWNER_COUNT 4u

struct survey_anchor_deadline_registry {
    uint64_t semantic_generation[
        SURVEY_ANCHOR_DEADLINE_SEMANTIC_OWNER_COUNT];
    uint32_t due_ms[SURVEY_ANCHOR_DEADLINE_OWNER_COUNT];
    uint8_t valid_mask;
};

struct survey_anchor_deadline_events {
    uint64_t semantic_generation[
        SURVEY_ANCHOR_DEADLINE_SEMANTIC_OWNER_COUNT];
    uint8_t due_mask;
};

void survey_anchor_deadline_registry_init(
    struct survey_anchor_deadline_registry *registry);

/*
 * Durations must stay inside the signed 32-bit uptime comparison horizon.
 * A lower nonzero semantic generation can never replace a newer owner.
 */
int survey_anchor_deadline_schedule_after(
    struct survey_anchor_deadline_registry *registry,
    enum survey_anchor_deadline_owner owner,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t delay_ms);

/* Exact cancellation cannot erase a successor generation. */
bool survey_anchor_deadline_cancel(
    struct survey_anchor_deadline_registry *registry,
    enum survey_anchor_deadline_owner owner,
    uint64_t generation);

/* Clears the owner's live deadline while retaining its generation cursor. */
void survey_anchor_deadline_clear(
    struct survey_anchor_deadline_registry *registry,
    enum survey_anchor_deadline_owner owner);

bool survey_anchor_deadline_next(
    const struct survey_anchor_deadline_registry *registry,
    uint32_t now_ms,
    uint32_t *delay_ms);

void survey_anchor_deadline_take_due(
    struct survey_anchor_deadline_registry *registry,
    uint32_t now_ms,
    struct survey_anchor_deadline_events *events);

bool survey_anchor_deadline_event_matches(
    const struct survey_anchor_deadline_events *events,
    enum survey_anchor_deadline_owner owner,
    uint64_t generation);

#ifdef __cplusplus
}
#endif

#endif
