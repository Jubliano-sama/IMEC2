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
    return left->survey_id == right->survey_id &&
           left->initiator_id == right->initiator_id &&
           left->responder_id == right->responder_id &&
           left->sample_count == right->sample_count;
}

static bool pair_is_zero(const struct survey_pair *pair)
{
    return pair->survey_id == 0u && pair->initiator_id == 0u &&
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
    lease->prepared_deadline_ms = 0u;
    lease->round_id = SURVEY_LEGACY_ROUND_ID;
    lease->phase = SURVEY_PAIR_LEASE_IDLE;
    lease->prepare_id_valid = false;
    lease->start_id_valid = false;
    lease->start_released = false;
    lease->go_released = false;
}

static void accept_prepare(struct survey_pair_lease *lease,
                           const struct survey_pair *pair,
                           uint16_t round_id,
                           const struct survey_pair_control_id *control_id,
                           uint32_t now_ms,
                           uint32_t lease_ms)
{
    lease->pair = *pair;
    lease->prepare_id = *control_id;
    memset(&lease->start_id, 0, sizeof(lease->start_id));
    lease->last_accepted_id = *control_id;
    lease->prepared_deadline_ms = now_ms + lease_ms;
    lease->round_id = round_id;
    lease->phase = SURVEY_PAIR_LEASE_PREPARED;
    lease->prepare_id_valid = true;
    lease->start_id_valid = false;
    lease->last_accepted_id_valid = true;
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
         lease->phase != SURVEY_PAIR_LEASE_START_PENDING) ||
        !deadline_reached(now_ms, lease->prepared_deadline_ms)) {
        return false;
    }

    clear_active(lease);
    return true;
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
    enum sequence_relation relation;

    if (lease == NULL || pair == NULL || !control_id_valid(control_id) ||
        survey_pair_validate(pair) != PROTO_OK ||
        pair->survey_id != control_id->session_id || lease_ms == 0u ||
        lease_ms > (uint32_t)INT32_MAX || !survey_pair_lease_invariant(lease)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }

    (void)survey_pair_lease_expire(lease, now_ms);

    if (lease->phase == SURVEY_PAIR_LEASE_IDLE) {
        if (lease->last_accepted_id_valid &&
            lease->last_accepted_id.session_id == control_id->session_id &&
            control_id_compare(control_id, &lease->last_accepted_id) !=
                SEQUENCE_NEWER) {
            return SURVEY_PAIR_LEASE_STALE;
        }
        accept_prepare(lease, pair, round_id, control_id, now_ms, lease_ms);
        return SURVEY_PAIR_LEASE_ACCEPTED;
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
        accept_prepare(lease, pair, round_id, control_id, now_ms, lease_ms);
        return SURVEY_PAIR_LEASE_DUPLICATE;
    }

    accept_prepare(lease, pair, round_id, control_id, now_ms, lease_ms);
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
    enum sequence_relation relation;
    bool expired;

    if (lease == NULL || pair == NULL || !control_id_valid(control_id) ||
        survey_pair_validate(pair) != PROTO_OK ||
        pair->survey_id != control_id->session_id ||
        !survey_pair_lease_invariant(lease)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }

    expired = survey_pair_lease_expire(lease, now_ms);
    if (expired) {
        return SURVEY_PAIR_LEASE_EXPIRED;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_IDLE) {
        if (lease->last_accepted_id_valid &&
            lease->last_accepted_id.session_id == control_id->session_id &&
            control_id_compare(control_id, &lease->last_accepted_id) !=
                SEQUENCE_NEWER) {
            return SURVEY_PAIR_LEASE_STALE;
        }
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
        const bool retain_go_release =
            lease->round_id != SURVEY_LEGACY_ROUND_ID &&
            lease->go_released;

        lease->start_id = *control_id;
        lease->last_accepted_id = *control_id;
        lease->start_released = false;
        lease->go_released = retain_go_release;
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
    if (lease == NULL || survey_id == 0u ||
        round_id == SURVEY_LEGACY_ROUND_ID ||
        !survey_pair_lease_invariant(lease)) {
        return SURVEY_PAIR_LEASE_INVALID_ARGUMENT;
    }
    if (survey_pair_lease_expire(lease, now_ms)) {
        return SURVEY_PAIR_LEASE_EXPIRED;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_IDLE ||
        lease->pair.survey_id != survey_id || lease->round_id != round_id) {
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
    return SURVEY_PAIR_LEASE_ACCEPTED;
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
    lease->phase = SURVEY_PAIR_LEASE_RUNNING;
    lease->start_released = false;
    lease->go_released = false;
    return true;
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
    if (lease == NULL || pair == NULL || session_id == 0u ||
        lease->phase == SURVEY_PAIR_LEASE_IDLE ||
        session_id != pair->survey_id ||
        session_id != lease->pair.survey_id ||
        !pair_equal(&lease->pair, pair)) {
        return false;
    }
    return survey_pair_lease_abort(lease);
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
               lease->round_id == SURVEY_LEGACY_ROUND_ID &&
               pair_is_zero(&lease->pair) &&
               lease->prepare_id.session_id == 0u &&
               lease->prepare_id.command_seq == 0u &&
               lease->start_id.session_id == 0u &&
               lease->start_id.command_seq == 0u;
    }
    if (survey_pair_validate(&lease->pair) != PROTO_OK ||
        !lease->prepare_id_valid || !control_id_valid(&lease->prepare_id) ||
        lease->pair.survey_id != lease->prepare_id.session_id ||
        !lease->last_accepted_id_valid) {
        return false;
    }
    if (lease->phase == SURVEY_PAIR_LEASE_PREPARED) {
        return !lease->start_id_valid && !lease->start_released &&
               !lease->go_released &&
               control_id_equal(&lease->prepare_id,
                                &lease->last_accepted_id);
    }
    if ((lease->phase == SURVEY_PAIR_LEASE_RUNNING ||
         lease->phase == SURVEY_PAIR_LEASE_ABORTING) &&
        (lease->prepared_deadline_ms != 0u || lease->start_released ||
         lease->go_released)) {
        return false;
    }
    return lease->start_id_valid &&
           control_id_valid(&lease->start_id) &&
           lease->start_id.session_id == lease->pair.survey_id &&
           control_id_equal(&lease->start_id, &lease->last_accepted_id);
}
