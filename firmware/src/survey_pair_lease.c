#include "survey_pair_lease.h"

#include <limits.h>
#include <string.h>

enum sequence_relation {
    SEQUENCE_OLDER = -1,
    SEQUENCE_EQUAL = 0,
    SEQUENCE_NEWER = 1,
};

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool pair_equal(const struct survey_pair *left,
                       const struct survey_pair *right)
{
    return left->operation_generation == right->operation_generation &&
           left->survey_id == right->survey_id &&
           left->initiator_id == right->initiator_id &&
           left->responder_id == right->responder_id &&
           left->sample_count == right->sample_count;
}

static bool pair_is_zero(const struct survey_pair *pair)
{
    return pair->operation_generation == 0u &&
           pair->survey_id == 0u && pair->initiator_id == 0u &&
           pair->responder_id == 0u && pair->sample_count == 0u;
}

static bool control_id_valid(const struct survey_pair_control_id *control_id)
{
    return control_id != NULL && control_id->session_id != 0u &&
           control_id->command_seq != 0u;
}

static bool control_id_equal(const struct survey_pair_control_id *left,
                             const struct survey_pair_control_id *right)
{
    return left->session_id == right->session_id &&
           left->command_seq == right->command_seq;
}

static bool round_binding_valid(
    const struct survey_pair *pair,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (pair == NULL) {
        return false;
    }
    if (pair->operation_generation == 0u) {
        return round_commitment == NULL;
    }
    return round_id != SURVEY_LEGACY_ROUND_ID &&
           round_commitment != NULL &&
           survey_operation_session_id(pair->operation_generation) != 0u;
}

static bool round_binding_equal(
    const struct survey_pair_lease *lease,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    return lease != NULL && lease->round_commitment_valid &&
           round_commitment != NULL &&
           semantic_digest_equal(lease->round_commitment,
                                 round_commitment,
                                 SEMANTIC_DIGEST_SHA256_LEN);
}

static enum sequence_relation sequence_compare(uint16_t candidate,
                                               uint16_t reference)
{
    uint16_t delta = (uint16_t)(candidate - reference);

    if (delta == 0u) {
        return SEQUENCE_EQUAL;
    }
    return delta < 0x8000u ? SEQUENCE_NEWER : SEQUENCE_OLDER;
}

static enum sequence_relation control_id_compare(
    const struct survey_pair_control_id *candidate,
    const struct survey_pair_control_id *reference)
{
    if (candidate->session_id != reference->session_id) {
        return SEQUENCE_OLDER;
    }
    return sequence_compare(candidate->command_seq, reference->command_seq);
}

static void clear_active(struct survey_pair_lease *lease)
{
    memset(&lease->pair, 0, sizeof(lease->pair));
    memset(&lease->prepare_id, 0, sizeof(lease->prepare_id));
    memset(&lease->start_id, 0, sizeof(lease->start_id));
    memset(lease->round_commitment, 0, sizeof(lease->round_commitment));
    lease->prepared_deadline_ms = 0u;
    lease->go_execution_deadline_ms = 0u;
    lease->round_id = SURVEY_LEGACY_ROUND_ID;
    lease->phase = SURVEY_PAIR_LEASE_IDLE;
    lease->prepare_id_valid = false;
    lease->start_id_valid = false;
    lease->round_commitment_valid = false;
    lease->start_released = false;
    lease->go_released = false;
}

static void accept_prepare(struct survey_pair_lease *lease,
                           const struct survey_pair *pair,
                           uint16_t round_id,
                           const uint8_t round_commitment[
                               SEMANTIC_DIGEST_SHA256_LEN],
                           const struct survey_pair_control_id *control_id,
                           uint32_t now_ms,
                           uint32_t lease_ms)
{
    lease->pair = *pair;
    lease->prepare_id = *control_id;
    memset(&lease->start_id, 0, sizeof(lease->start_id));
    lease->last_accepted_id = *control_id;
    if (round_commitment != NULL) {
        memcpy(lease->round_commitment,
               round_commitment,
               sizeof(lease->round_commitment));
    } else {
        memset(lease->round_commitment,
               0,
               sizeof(lease->round_commitment));
    }
    lease->last_accepted_operation_generation =
        pair->operation_generation;
    lease->prepared_deadline_ms = now_ms + lease_ms;
    lease->go_execution_deadline_ms = 0u;
    lease->round_id = round_id;
    lease->phase = SURVEY_PAIR_LEASE_PREPARED;
    lease->prepare_id_valid = true;
    lease->start_id_valid = false;
    lease->last_accepted_id_valid = true;
    lease->round_commitment_valid = round_commitment != NULL;
    lease->start_released = false;
    lease->go_released = false;
}

void survey_pair_lease_reset(struct survey_pair_lease *lease)
{
    if (lease != NULL) {
        memset(lease, 0, sizeof(*lease));
    }
}

bool survey_pair_lease_expire(struct survey_pair_lease *lease,
                              uint32_t now_ms)
{
    if (lease == NULL ||
        (lease->phase != SURVEY_PAIR_LEASE_PREPARED &&
         lease->phase != SURVEY_PAIR_LEASE_START_PENDING)) {
        return false;
    }
    if (deadline_reached(now_ms, lease->prepared_deadline_ms)) {
        clear_active(lease);
        return true;
    }

    /*
     * A missed GO window invalidates only that GO. START remains armed so the
     * gateway's bounded zero-RF-attempt recovery can issue a fresh GO with a
     * new future instant, as required by the synchronized-round contract.
     */
    if (lease->phase == SURVEY_PAIR_LEASE_START_PENDING &&
        lease->go_released &&
        deadline_reached(now_ms,
                         lease->go_execution_deadline_ms)) {
        lease->go_released = false;
        lease->go_execution_deadline_ms = 0u;
    }
    return false;
}

uint32_t survey_pair_lease_remaining_ms(const struct survey_pair_lease *lease,
                                        uint32_t now_ms)
{
    if (lease == NULL ||
        (lease->phase != SURVEY_PAIR_LEASE_PREPARED &&
         lease->phase != SURVEY_PAIR_LEASE_START_PENDING) ||
        deadline_reached(now_ms, lease->prepared_deadline_ms)) {
        return 0u;
    }
    return lease->prepared_deadline_ms - now_ms;
}

enum survey_pair_lease_decision survey_pair_lease_prepare(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms,
    uint32_t lease_ms)
{
    return survey_pair_lease_prepare_round(lease,
                                           pair,
                                           SURVEY_LEGACY_ROUND_ID,
                                           control_id,
                                           now_ms,
                                           lease_ms);
}

enum survey_pair_lease_decision survey_pair_lease_prepare_round(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint16_t round_id,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms,
    uint32_t lease_ms)
{
    return survey_pair_lease_prepare_round_bound(lease,
                                                 pair,
                                                 round_id,
                                                 NULL,
                                                 control_id,
                                                 now_ms,
                                                 lease_ms);
}

enum survey_pair_lease_decision survey_pair_lease_prepare_round_bound(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN],
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms,
    uint32_t lease_ms)
{
    enum sequence_relation relation;

    if (lease == NULL || pair == NULL || !control_id_valid(control_id) ||
        survey_pair_validate(pair) != PROTO_OK ||
        (pair->operation_generation == 0u ?
             pair->survey_id != control_id->session_id :
             survey_operation_session_id(pair->operation_generation) !=
                 control_id->session_id) ||
        !round_binding_valid(pair, round_id, round_commitment) ||
        lease_ms == 0u ||
        lease_ms > (uint32_t)INT32_MAX || !survey_pair_lease_invariant(lease)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }

    (void)survey_pair_lease_expire(lease, now_ms);

    if (lease->phase == SURVEY_PAIR_LEASE_IDLE) {
        if (pair->operation_generation != 0u) {
            if (pair->operation_generation <
                lease->last_accepted_operation_generation) {
                return SURVEY_PAIR_LEASE_STALE;
            }
            if (pair->operation_generation ==
                    lease->last_accepted_operation_generation &&
                lease->last_accepted_id_valid &&
                control_id_compare(control_id,
                                   &lease->last_accepted_id) !=
                    SEQUENCE_NEWER) {
                return SURVEY_PAIR_LEASE_STALE;
            }
        } else if (lease->last_accepted_operation_generation != 0u) {
            return SURVEY_PAIR_LEASE_STALE;
        } else if (lease->last_accepted_id_valid &&
                   lease->last_accepted_id.session_id ==
                       control_id->session_id &&
                   control_id_compare(control_id,
                                      &lease->last_accepted_id) !=
                       SEQUENCE_NEWER) {
            return SURVEY_PAIR_LEASE_STALE;
        }
        accept_prepare(lease, pair, round_id, round_commitment,
                       control_id, now_ms, lease_ms);
        return SURVEY_PAIR_LEASE_ACCEPTED;
    }

    if (pair->operation_generation != lease->pair.operation_generation) {
        if (pair->operation_generation == 0u ||
            pair->operation_generation <=
                lease->pair.operation_generation) {
            return SURVEY_PAIR_LEASE_STALE;
        }
        if (lease->phase != SURVEY_PAIR_LEASE_PREPARED &&
            lease->phase != SURVEY_PAIR_LEASE_START_PENDING) {
            return SURVEY_PAIR_LEASE_BUSY;
        }
        accept_prepare(lease, pair, round_id, round_commitment,
                       control_id, now_ms, lease_ms);
        return SURVEY_PAIR_LEASE_SUPERSEDED;
    }
    if (pair->operation_generation != 0u &&
        !round_binding_equal(lease, round_commitment)) {
        return SURVEY_PAIR_LEASE_BUSY;
    }
    if (pair_equal(&lease->pair, pair) && lease->round_id == round_id &&
        lease->prepare_id_valid &&
        control_id_equal(&lease->prepare_id, control_id)) {
        return SURVEY_PAIR_LEASE_DUPLICATE;
    }

    if (lease->phase != SURVEY_PAIR_LEASE_PREPARED) {
        return pair_equal(&lease->pair, pair) && lease->round_id == round_id ?
                   SURVEY_PAIR_LEASE_STALE : SURVEY_PAIR_LEASE_BUSY;
    }
    if (control_id->session_id != lease->prepare_id.session_id) {
        return SURVEY_PAIR_LEASE_BUSY;
    }

    relation = control_id_compare(control_id, &lease->last_accepted_id);
    if (relation != SEQUENCE_NEWER) {
        return SURVEY_PAIR_LEASE_STALE;
    }

    if (pair_equal(&lease->pair, pair) && lease->round_id == round_id) {
        accept_prepare(lease, pair, round_id, round_commitment,
                       control_id, now_ms, lease_ms);
        return SURVEY_PAIR_LEASE_DUPLICATE;
    }

    accept_prepare(lease, pair, round_id, round_commitment,
                   control_id, now_ms, lease_ms);
    return SURVEY_PAIR_LEASE_SUPERSEDED;
}

enum survey_pair_lease_decision survey_pair_lease_start(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms)
{
    return survey_pair_lease_start_round(lease,
                                         pair,
                                         SURVEY_LEGACY_ROUND_ID,
                                         control_id,
                                         now_ms);
}

enum survey_pair_lease_decision survey_pair_lease_start_round(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint16_t round_id,
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms)
{
    return survey_pair_lease_start_round_bound(lease,
                                               pair,
                                               round_id,
                                               NULL,
                                               control_id,
                                               now_ms);
}

enum survey_pair_lease_decision survey_pair_lease_start_round_bound(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN],
    const struct survey_pair_control_id *control_id,
    uint32_t now_ms)
{
    enum sequence_relation relation;
    bool expired;

    if (lease == NULL || pair == NULL || !control_id_valid(control_id) ||
        survey_pair_validate(pair) != PROTO_OK ||
        (pair->operation_generation == 0u ?
             pair->survey_id != control_id->session_id :
             survey_operation_session_id(pair->operation_generation) !=
                 control_id->session_id) ||
        !round_binding_valid(pair, round_id, round_commitment) ||
        !survey_pair_lease_invariant(lease)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }

    expired = survey_pair_lease_expire(lease, now_ms);
    if (expired) {
        return SURVEY_PAIR_LEASE_EXPIRED;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_IDLE) {
        if (pair->operation_generation <
            lease->last_accepted_operation_generation) {
            return SURVEY_PAIR_LEASE_STALE;
        }
        if (lease->last_accepted_id_valid &&
            pair->operation_generation ==
                lease->last_accepted_operation_generation &&
            control_id_compare(control_id, &lease->last_accepted_id) !=
                SEQUENCE_NEWER) {
            return SURVEY_PAIR_LEASE_STALE;
        }
        return SURVEY_PAIR_LEASE_INVALID_STATE;
    }
    if (pair->operation_generation != lease->pair.operation_generation) {
        return pair->operation_generation <
                       lease->pair.operation_generation ?
                   SURVEY_PAIR_LEASE_STALE :
                   SURVEY_PAIR_LEASE_INVALID_STATE;
    }
    if (pair->operation_generation != 0u &&
        !round_binding_equal(lease, round_commitment)) {
        return SURVEY_PAIR_LEASE_INVALID_STATE;
    }
    if (!pair_equal(&lease->pair, pair) || lease->round_id != round_id) {
        return lease->phase == SURVEY_PAIR_LEASE_PREPARED ?
                   SURVEY_PAIR_LEASE_INVALID_STATE : SURVEY_PAIR_LEASE_BUSY;
    }
    if (control_id->session_id != lease->prepare_id.session_id) {
        return SURVEY_PAIR_LEASE_INVALID_STATE;
    }

    if (lease->phase == SURVEY_PAIR_LEASE_PREPARED) {
        relation = control_id_compare(control_id, &lease->last_accepted_id);
        if (relation != SEQUENCE_NEWER) {
            return SURVEY_PAIR_LEASE_STALE;
        }
        lease->start_id = *control_id;
        lease->last_accepted_id = *control_id;
        lease->phase = SURVEY_PAIR_LEASE_START_PENDING;
        lease->start_id_valid = true;
        lease->start_released = false;
        lease->go_released = false;
        lease->go_execution_deadline_ms = 0u;
        return SURVEY_PAIR_LEASE_ACCEPTED;
    }

    if (!lease->start_id_valid) {
        return SURVEY_PAIR_LEASE_INVALID_STATE;
    }
    relation = control_id_compare(control_id, &lease->start_id);
    if (relation == SEQUENCE_OLDER) {
        return SURVEY_PAIR_LEASE_STALE;
    }
    if (relation == SEQUENCE_NEWER) {
        /*
         * Once RF owns the lease, a new START identity cannot safely replace
         * the one whose result already released the run.  The application
         * normally rejects this through its radio-busy gate, but the lease is
         * the authoritative state machine and must remain safe across that
         * check-to-lock race.
         */
        if (lease->phase == SURVEY_PAIR_LEASE_RUNNING ||
            lease->phase == SURVEY_PAIR_LEASE_ABORTING) {
            return SURVEY_PAIR_LEASE_BUSY;
        }
        /*
         * GO names only the survey and synchronized round, not one START
         * command identity. Once GO has released this round, accepting a
         * newer START would let that new command inherit authorization meant
         * for the old START. Only its exact duplicate remains admissible.
         */
        if (lease->round_id != SURVEY_LEGACY_ROUND_ID &&
            lease->go_released) {
            return SURVEY_PAIR_LEASE_STALE;
        }

        lease->start_id = *control_id;
        lease->last_accepted_id = *control_id;
        lease->start_released = false;
        lease->go_released = false;
        lease->go_execution_deadline_ms = 0u;
        return SURVEY_PAIR_LEASE_SUPERSEDED;
    }
    return SURVEY_PAIR_LEASE_DUPLICATE;
}

bool survey_pair_lease_pending_snapshot(const struct survey_pair_lease *lease,
                                        struct survey_pair *pair)
{
    if (lease == NULL || lease->phase != SURVEY_PAIR_LEASE_START_PENDING) {
        return false;
    }
    if (pair != NULL) {
        *pair = lease->pair;
    }
    return true;
}

bool survey_pair_lease_release_start(
    struct survey_pair_lease *lease,
    const struct survey_pair_control_id *control_id)
{
    if (lease == NULL || !control_id_valid(control_id) ||
        lease->phase != SURVEY_PAIR_LEASE_START_PENDING ||
        !lease->start_id_valid ||
        !control_id_equal(&lease->start_id, control_id)) {
        return false;
    }
    lease->start_released = true;
    return true;
}

enum survey_pair_lease_decision survey_pair_lease_go(
    struct survey_pair_lease *lease,
    uint32_t survey_id,
    uint16_t round_id,
    uint32_t now_ms)
{
    return survey_pair_lease_go_until(
        lease,
        survey_id,
        round_id,
        now_ms,
        now_ms + SURVEY_PAIR_START_SKEW_MARGIN_MS);
}

enum survey_pair_lease_decision survey_pair_lease_go_until(
    struct survey_pair_lease *lease,
    uint32_t survey_id,
    uint16_t round_id,
    uint32_t now_ms,
    uint32_t execution_deadline_ms)
{
    return survey_pair_lease_go_until_bound(
        lease,
        0u,
        survey_id,
        round_id,
        NULL,
        now_ms,
        execution_deadline_ms);
}

enum survey_pair_lease_decision survey_pair_lease_go_until_bound(
    struct survey_pair_lease *lease,
    uint64_t operation_generation,
    uint32_t survey_id,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN],
    uint32_t now_ms,
    uint32_t execution_deadline_ms)
{
    if (lease == NULL || survey_id == 0u ||
        round_id == SURVEY_LEGACY_ROUND_ID ||
        (operation_generation == 0u ?
             round_commitment != NULL :
             survey_operation_session_id(operation_generation) == 0u ||
                 round_commitment == NULL) ||
        deadline_reached(now_ms, execution_deadline_ms) ||
        (uint32_t)(execution_deadline_ms - now_ms) > (uint32_t)INT32_MAX ||
        !survey_pair_lease_invariant(lease)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    if (survey_pair_lease_expire(lease, now_ms)) {
        return SURVEY_PAIR_LEASE_EXPIRED;
    }
    if (operation_generation != lease->pair.operation_generation) {
        return operation_generation < lease->pair.operation_generation ?
                   SURVEY_PAIR_LEASE_STALE :
                   SURVEY_PAIR_LEASE_INVALID_STATE;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_IDLE ||
        lease->pair.survey_id != survey_id || lease->round_id != round_id ||
        (operation_generation != 0u &&
         !round_binding_equal(lease, round_commitment))) {
        return SURVEY_PAIR_LEASE_STALE;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_RUNNING ||
        lease->phase == SURVEY_PAIR_LEASE_ABORTING) {
        return SURVEY_PAIR_LEASE_DUPLICATE;
    }
    if (lease->phase != SURVEY_PAIR_LEASE_START_PENDING ||
        !lease->start_id_valid) {
        return SURVEY_PAIR_LEASE_INVALID_STATE;
    }
    if (lease->go_released) {
        return SURVEY_PAIR_LEASE_DUPLICATE;
    }
    lease->go_released = true;
    lease->go_execution_deadline_ms = execution_deadline_ms;
    return SURVEY_PAIR_LEASE_ACCEPTED;
}

bool survey_pair_lease_revoke_go(struct survey_pair_lease *lease,
                                 uint32_t survey_id,
                                 uint16_t round_id)
{
    return survey_pair_lease_revoke_go_bound(lease,
                                             0u,
                                             survey_id,
                                             round_id,
                                             NULL);
}

bool survey_pair_lease_revoke_go_bound(
    struct survey_pair_lease *lease,
    uint64_t operation_generation,
    uint32_t survey_id,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (lease == NULL || survey_id == 0u ||
        round_id == SURVEY_LEGACY_ROUND_ID ||
        lease->pair.operation_generation != operation_generation ||
        (operation_generation != 0u &&
         !round_binding_equal(lease, round_commitment)) ||
        lease->phase != SURVEY_PAIR_LEASE_START_PENDING ||
        lease->pair.survey_id != survey_id ||
        lease->round_id != round_id || !lease->go_released) {
        return false;
    }
    lease->go_released = false;
    lease->go_execution_deadline_ms = 0u;
    return true;
}

bool survey_pair_lease_ready_snapshot(const struct survey_pair_lease *lease,
                                      struct survey_pair *pair)
{
    if (lease == NULL || lease->phase != SURVEY_PAIR_LEASE_START_PENDING ||
        !lease->start_released ||
        (lease->round_id != SURVEY_LEGACY_ROUND_ID && !lease->go_released)) {
        return false;
    }
    if (pair != NULL) {
        *pair = lease->pair;
    }
    return true;
}

bool survey_pair_lease_mark_running(struct survey_pair_lease *lease,
                                    struct survey_pair *pair,
                                    uint16_t *round_id)
{
    if (lease == NULL || lease->phase != SURVEY_PAIR_LEASE_START_PENDING ||
        !lease->start_released ||
        (lease->round_id != SURVEY_LEGACY_ROUND_ID && !lease->go_released)) {
        return false;
    }
    if (pair != NULL) {
        *pair = lease->pair;
    }
    if (round_id != NULL) {
        *round_id = lease->round_id;
    }
    lease->prepared_deadline_ms = 0u;
    lease->go_execution_deadline_ms = 0u;
    lease->phase = SURVEY_PAIR_LEASE_RUNNING;
    lease->start_released = false;
    lease->go_released = false;
    return true;
}

bool survey_pair_lease_mark_running_at(struct survey_pair_lease *lease,
                                       uint32_t now_ms,
                                       struct survey_pair *pair,
                                       uint16_t *round_id)
{
    if (survey_pair_lease_expire(lease, now_ms)) {
        return false;
    }
    return survey_pair_lease_mark_running(lease, pair, round_id);
}

bool survey_pair_lease_finish(struct survey_pair_lease *lease)
{
    if (lease == NULL ||
        (lease->phase != SURVEY_PAIR_LEASE_RUNNING &&
         lease->phase != SURVEY_PAIR_LEASE_ABORTING)) {
        return false;
    }
    clear_active(lease);
    return true;
}

bool survey_pair_lease_abort(struct survey_pair_lease *lease)
{
    if (lease == NULL || lease->phase == SURVEY_PAIR_LEASE_IDLE ||
        lease->phase == SURVEY_PAIR_LEASE_ABORTING) {
        return false;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_RUNNING) {
        lease->phase = SURVEY_PAIR_LEASE_ABORTING;
    } else {
        clear_active(lease);
    }
    return true;
}

bool survey_pair_lease_abort_matching(struct survey_pair_lease *lease,
                                      const struct survey_pair *pair,
                                      uint32_t session_id)
{
    const uint32_t expected_session =
        pair == NULL || pair->operation_generation == 0u ?
            (pair == NULL ? 0u : pair->survey_id) :
            survey_operation_session_id(pair->operation_generation);

    if (lease == NULL || pair == NULL || session_id == 0u ||
        lease->phase == SURVEY_PAIR_LEASE_IDLE ||
        session_id != expected_session ||
        !pair_equal(&lease->pair, pair)) {
        return false;
    }
    return survey_pair_lease_abort(lease);
}

bool survey_pair_lease_abort_matching_round(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint32_t session_id,
    uint16_t round_id)
{
    return survey_pair_lease_abort_matching_round_bound(lease,
                                                        pair,
                                                        session_id,
                                                        round_id,
                                                        NULL);
}

bool survey_pair_lease_abort_matching_round_bound(
    struct survey_pair_lease *lease,
    const struct survey_pair *pair,
    uint32_t session_id,
    uint16_t round_id,
    const uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN])
{
    if (lease == NULL || pair == NULL || lease->round_id != round_id ||
        (pair->operation_generation == 0u ?
             round_id == SURVEY_LEGACY_ROUND_ID ||
                 round_commitment != NULL :
             round_commitment == NULL ||
                 !round_binding_equal(lease, round_commitment))) {
        return false;
    }
    return survey_pair_lease_abort_matching(lease, pair, session_id);
}

bool survey_pair_lease_invariant(const struct survey_pair_lease *lease)
{
    if (lease == NULL || lease->phase < SURVEY_PAIR_LEASE_IDLE ||
        lease->phase > SURVEY_PAIR_LEASE_ABORTING) {
        return false;
    }
    if (lease->last_accepted_id_valid &&
        !control_id_valid(&lease->last_accepted_id)) {
        return false;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_IDLE) {
        return !lease->prepare_id_valid && !lease->start_id_valid &&
               !lease->start_released && !lease->go_released &&
               lease->prepared_deadline_ms == 0u &&
               lease->go_execution_deadline_ms == 0u &&
               lease->round_id == SURVEY_LEGACY_ROUND_ID &&
               !lease->round_commitment_valid &&
               pair_is_zero(&lease->pair) &&
               lease->prepare_id.session_id == 0u &&
               lease->prepare_id.command_seq == 0u &&
               lease->start_id.session_id == 0u &&
               lease->start_id.command_seq == 0u;
    }
    const uint32_t expected_session =
        lease->pair.operation_generation == 0u ?
            lease->pair.survey_id :
            survey_operation_session_id(
                lease->pair.operation_generation);

    if (survey_pair_validate(&lease->pair) != PROTO_OK ||
        !lease->prepare_id_valid || !control_id_valid(&lease->prepare_id) ||
        expected_session == 0u ||
        expected_session != lease->prepare_id.session_id ||
        (lease->pair.operation_generation == 0u ?
             lease->round_commitment_valid :
             lease->round_id == SURVEY_LEGACY_ROUND_ID ||
                 !lease->round_commitment_valid ||
                 lease->last_accepted_operation_generation !=
                     lease->pair.operation_generation) ||
        !lease->last_accepted_id_valid) {
        return false;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_PREPARED) {
        return !lease->start_id_valid && !lease->start_released &&
               !lease->go_released &&
               lease->go_execution_deadline_ms == 0u &&
               control_id_equal(&lease->prepare_id,
                                &lease->last_accepted_id);
    }
    if ((lease->phase == SURVEY_PAIR_LEASE_RUNNING ||
         lease->phase == SURVEY_PAIR_LEASE_ABORTING) &&
        (lease->prepared_deadline_ms != 0u || lease->start_released ||
         lease->go_released ||
         lease->go_execution_deadline_ms != 0u)) {
        return false;
    }
    return lease->start_id_valid &&
           control_id_valid(&lease->start_id) &&
           lease->start_id.session_id == expected_session &&
           (lease->go_released ?
                lease->round_id != SURVEY_LEGACY_ROUND_ID :
                lease->go_execution_deadline_ms == 0u) &&
           control_id_equal(&lease->start_id, &lease->last_accepted_id);
}
