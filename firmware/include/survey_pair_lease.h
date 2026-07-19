#ifndef SURVEY_PAIR_LEASE_H
#define SURVEY_PAIR_LEASE_H

#include "survey.h"
#include "survey_round_control.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum survey_pair_lease_phase {
    SURVEY_PAIR_LEASE_IDLE = 0,
    SURVEY_PAIR_LEASE_PREPARED,
    SURVEY_PAIR_LEASE_START_PENDING,
    SURVEY_PAIR_LEASE_RUNNING,
    SURVEY_PAIR_LEASE_ABORTING,
};

enum survey_pair_lease_decision {
    SURVEY_PAIR_LEASE_ACCEPTED = 0,
    SURVEY_PAIR_LEASE_DUPLICATE,
    SURVEY_PAIR_LEASE_SUPERSEDED,
    SURVEY_PAIR_LEASE_BUSY,
    SURVEY_PAIR_LEASE_INVALID_STATE,
    SURVEY_PAIR_LEASE_STALE,
    SURVEY_PAIR_LEASE_EXPIRED,
    SURVEY_PAIR_LEASE_INVALID_ARGUMENT,
};

struct survey_pair_control_id {
    uint32_t session_id;
    uint16_t command_seq;
};

struct survey_pair_lease {
    struct survey_pair pair;
    struct survey_pair_control_id prepare_id;
    struct survey_pair_control_id start_id;
    struct survey_pair_control_id last_accepted_id;
    uint32_t prepared_deadline_ms;
    uint16_t round_id;
    enum survey_pair_lease_phase phase;
    bool prepare_id_valid;
    bool start_id_valid;
    bool last_accepted_id_valid;
    /* Exact START command-result delivery has reached gateway confirmation. */
    bool start_released;
    /* Matching nonzero-round GO has arrived; legacy round zero needs no GO. */
    bool go_released;
};

/* Reset is the only operation that intentionally forgets accepted command IDs. */
void survey_pair_lease_reset(struct survey_pair_lease *lease);

/*
 * Exact duplicate PREPAREs are idempotent and do not extend the deadline.
 * A newer command sequence may refresh or supersede a still-prepared pair.
 */
enum survey_pair_lease_decision survey_pair_lease_prepare(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms,
    uint32_t lease_ms);
enum survey_pair_lease_decision survey_pair_lease_prepare_round(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint16_t round_id,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms,
    uint32_t lease_ms);

/*
 * START must name the prepared pair and have a newer command sequence. An
 * exact retry is DUPLICATE; a newer command identity for the same pending pair
 * is SUPERSEDED because its command-result delivery replaces the prior START
 * custody. The original prepared deadline remains active until execution
 * actually starts, so a permanently blocked local radio cannot leave
 * START_PENDING forever.
 */
enum survey_pair_lease_decision survey_pair_lease_start(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms);
enum survey_pair_lease_decision survey_pair_lease_start_round(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint16_t round_id,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms);

/*
 * A GO releases only the matching nonzero survey round. Repeated or late GO
 * calls are harmless and never release a different prepared pair.
 */
enum survey_pair_lease_decision survey_pair_lease_go(
    struct survey_pair_lease *lease,
    uint32_t survey_id,
    uint16_t round_id,
    uint32_t now_ms);

bool survey_pair_lease_pending_snapshot(const struct survey_pair_lease *lease,
                                        struct survey_pair *pair);
bool survey_pair_lease_release_start(
    struct survey_pair_lease *lease,
    const struct survey_pair_control_id *control_id);
bool survey_pair_lease_ready_snapshot(const struct survey_pair_lease *lease,
                                      struct survey_pair *pair);
/*
 * Atomically claims the ready lease for RF execution and snapshots its exact
 * pair and synchronized-round generation. Either output may be NULL.
 */
bool survey_pair_lease_mark_running(struct survey_pair_lease *lease,
                                    struct survey_pair *pair,
                                    uint16_t *round_id);
bool survey_pair_lease_finish(struct survey_pair_lease *lease);
bool survey_pair_lease_abort(struct survey_pair_lease *lease);
bool survey_pair_lease_abort_matching(struct survey_pair_lease *lease,
                                      const struct survey_pair *pair,
                                      uint32_t session_id);
bool survey_pair_lease_expire(struct survey_pair_lease *lease,
                              uint32_t now_ms);
uint32_t survey_pair_lease_remaining_ms(const struct survey_pair_lease *lease,
                                        uint32_t now_ms);
bool survey_pair_lease_invariant(const struct survey_pair_lease *lease);

#ifdef __cplusplus
}
#endif

#endif
