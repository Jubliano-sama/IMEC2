#include "survey_anchor_deadline.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(SURVEY_ANCHOR_DEADLINE_OWNER_COUNT <= 8,
               "anchor deadline owner mask exceeds one byte");
_Static_assert(sizeof(struct survey_anchor_deadline_registry) <= 64u,
               "anchor deadline registry exceeded its RAM budget");
_Static_assert(sizeof(struct survey_anchor_deadline_events) <= 40u,
               "anchor deadline event snapshot exceeded its stack budget");

static bool owner_valid(enum survey_anchor_deadline_owner owner)
{
    return owner >= SURVEY_ANCHOR_DEADLINE_OPERATION &&
           owner < SURVEY_ANCHOR_DEADLINE_OWNER_COUNT;
}

static uint8_t owner_bit(enum survey_anchor_deadline_owner owner)
{
    return (uint8_t)(UINT8_C(1) << (unsigned int)owner);
}

static int semantic_generation_index(
    enum survey_anchor_deadline_owner owner)
{
    switch (owner) {
    case SURVEY_ANCHOR_DEADLINE_OPERATION:
        return 0;
    case SURVEY_ANCHOR_DEADLINE_PHASE_SAFETY:
        return 1;
    case SURVEY_ANCHOR_DEADLINE_DISCOVERY_CUSTODY:
        return 2;
    case SURVEY_ANCHOR_DEADLINE_PAIR_ADMISSION:
        return 3;
    case SURVEY_ANCHOR_DEADLINE_RESULT_DELIVERY:
    case SURVEY_ANCHOR_DEADLINE_OWNER_COUNT:
        return -1;
    }
    return -1;
}

static bool deadline_reached(uint32_t now_ms, uint32_t due_ms)
{
    return (int32_t)(now_ms - due_ms) >= 0;
}

void survey_anchor_deadline_registry_init(
    struct survey_anchor_deadline_registry *registry)
{
    if (registry != NULL) {
        memset(registry, 0, sizeof(*registry));
    }
}

int survey_anchor_deadline_schedule_after(
    struct survey_anchor_deadline_registry *registry,
    enum survey_anchor_deadline_owner owner,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t delay_ms)
{
    uint8_t bit;
    int generation_index;

    if (registry == NULL || !owner_valid(owner) || delay_ms > INT32_MAX) {
        return -EINVAL;
    }
    bit = owner_bit(owner);
    generation_index = semantic_generation_index(owner);
    if (generation_index < 0) {
        if (generation != 0u) {
            return -EINVAL;
        }
    } else {
        uint64_t *cursor = &registry->semantic_generation[generation_index];

        if (generation == 0u || generation < *cursor) {
            return -ESTALE;
        }
        if (generation > *cursor) {
            registry->valid_mask &= (uint8_t)~bit;
        }
        *cursor = generation;
    }
    if ((registry->valid_mask & bit) != 0u) {
        const uint32_t retained_delay_ms = deadline_reached(
            now_ms, registry->due_ms[owner]) ?
                                               0u :
                                               registry->due_ms[owner] - now_ms;

        /* One logical owner may have several level-triggered producers. */
        if (retained_delay_ms <= delay_ms) {
            return 0;
        }
    }
    registry->due_ms[owner] = now_ms + delay_ms;
    registry->valid_mask |= bit;
    return 0;
}

bool survey_anchor_deadline_cancel(
    struct survey_anchor_deadline_registry *registry,
    enum survey_anchor_deadline_owner owner,
    uint64_t generation)
{
    uint8_t bit;
    int generation_index;

    if (registry == NULL || !owner_valid(owner)) {
        return false;
    }
    bit = owner_bit(owner);
    generation_index = semantic_generation_index(owner);
    if ((generation_index < 0 && generation != 0u) ||
        (generation_index >= 0 &&
         registry->semantic_generation[generation_index] != generation)) {
        return false;
    }
    registry->valid_mask &= (uint8_t)~bit;
    return true;
}

void survey_anchor_deadline_clear(
    struct survey_anchor_deadline_registry *registry,
    enum survey_anchor_deadline_owner owner)
{
    if (registry != NULL && owner_valid(owner)) {
        registry->valid_mask &= (uint8_t)~owner_bit(owner);
    }
}

bool survey_anchor_deadline_next(
    const struct survey_anchor_deadline_registry *registry,
    uint32_t now_ms,
    uint32_t *delay_ms)
{
    uint32_t selected_delay_ms = 0u;
    bool found = false;

    if (registry == NULL || delay_ms == NULL) {
        return false;
    }
    for (size_t i = 0u; i < SURVEY_ANCHOR_DEADLINE_OWNER_COUNT; i++) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << i);
        uint32_t candidate_delay_ms;

        if ((registry->valid_mask & bit) == 0u) {
            continue;
        }
        candidate_delay_ms = deadline_reached(now_ms,
                                              registry->due_ms[i]) ?
                                 0u : registry->due_ms[i] - now_ms;
        if (!found || candidate_delay_ms < selected_delay_ms) {
            selected_delay_ms = candidate_delay_ms;
            found = true;
        }
    }
    if (found) {
        *delay_ms = selected_delay_ms;
    }
    return found;
}

void survey_anchor_deadline_take_due(
    struct survey_anchor_deadline_registry *registry,
    uint32_t now_ms,
    struct survey_anchor_deadline_events *events)
{
    if (events != NULL) {
        memset(events, 0, sizeof(*events));
    }
    if (registry == NULL || events == NULL) {
        return;
    }
    for (size_t i = 0u; i < SURVEY_ANCHOR_DEADLINE_OWNER_COUNT; i++) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << i);

        if ((registry->valid_mask & bit) != 0u &&
            deadline_reached(now_ms, registry->due_ms[i])) {
            registry->valid_mask &= (uint8_t)~bit;
            events->due_mask |= bit;
            if (semantic_generation_index(
                    (enum survey_anchor_deadline_owner)i) >= 0) {
                const int generation_index = semantic_generation_index(
                    (enum survey_anchor_deadline_owner)i);

                events->semantic_generation[generation_index] =
                    registry->semantic_generation[generation_index];
            }
        }
    }
}

bool survey_anchor_deadline_event_matches(
    const struct survey_anchor_deadline_events *events,
    enum survey_anchor_deadline_owner owner,
    uint64_t generation)
{
    uint8_t bit;
    int generation_index;

    if (events == NULL || !owner_valid(owner)) {
        return false;
    }
    bit = owner_bit(owner);
    generation_index = semantic_generation_index(owner);
    return (events->due_mask & bit) != 0u &&
           ((generation_index < 0 && generation == 0u) ||
            (generation_index >= 0 &&
             events->semantic_generation[generation_index] == generation));
}
