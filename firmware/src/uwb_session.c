#include "uwb_session.h"

#include <string.h>

static bool flags_valid(uint8_t flags)
{
    return flags == FLAG_DIAGNOSTIC ||
           flags == FLAG_COUNT_AS_CLICK;
}

static bool discovery_reply_status_valid(uint8_t status)
{
    return status == UWB_DISCOVERY_REPLY_PRESENT ||
           status == UWB_DISCOVERY_REPLY_BUSY ||
           status == UWB_DISCOVERY_REPLY_COLLISION;
}

static void diagnostics_add_saturated(uint32_t *counter, uint32_t delta)
{
    if (counter == NULL) {
        return;
    }
    if (UINT32_MAX - *counter < delta) {
        *counter = UINT32_MAX;
    } else {
        *counter += delta;
    }
}

static void diagnostics_add_awake_time(struct uwb_session_diagnostics *diagnostics,
                                       uint32_t awake_us)
{
    if (diagnostics == NULL) {
        return;
    }

    diagnostics_add_saturated(&diagnostics->awake_time_us, awake_us);
}

uint8_t uwb_clicker_contention_window_slots(uint8_t attempt_index)
{
    if (attempt_index <= 1u) {
        return UWB_CLICKER_CONTENTION_ATTEMPT1_SLOTS;
    }
    if (attempt_index == 2u) {
        return UWB_CLICKER_CONTENTION_ATTEMPT2_SLOTS;
    }
    return UWB_CLICKER_CONTENTION_ATTEMPT3_PLUS_SLOTS;
}

uint32_t uwb_clicker_contention_delay_ms(uint8_t attempt_index,
                                         uint32_t random_value)
{
    uint8_t slots = uwb_clicker_contention_window_slots(attempt_index);

    return (random_value % slots) * UWB_CLICKER_CONTENTION_SLOT_MS;
}

uint32_t uwb_clicker_wake_claim_jitter_us(uint32_t random_value)
{
    return random_value % (UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US + 1u);
}

void uwb_clicker_note_politeness_sample(struct uwb_clicker_session *session,
                                        bool activity_detected)
{
    if (session == NULL) {
        return;
    }

    diagnostics_add_saturated(&session->diagnostics.politeness_samples, 1u);
    if (activity_detected) {
        diagnostics_add_saturated(&session->diagnostics.politeness_activity_hits, 1u);
    }
}

void uwb_clicker_note_contention_delay(struct uwb_clicker_session *session,
                                       uint32_t delay_ms)
{
    if (session == NULL) {
        return;
    }

    diagnostics_add_saturated(&session->diagnostics.contention_delay_ms, delay_ms);
}

void uwb_clicker_note_retry_delay(struct uwb_clicker_session *session,
                                  uint32_t delay_ms)
{
    if (session == NULL) {
        return;
    }

    diagnostics_add_saturated(&session->diagnostics.retry_delay_ms, delay_ms);
}

void uwb_clicker_note_wake_claim_tx(struct uwb_clicker_session *session,
                                    uint32_t tx_count)
{
    if (session == NULL) {
        return;
    }

    diagnostics_add_saturated(&session->diagnostics.wake_claim_tx_count, tx_count);
}

uint16_t uwb_session_short_addr_from_id(uint64_t device_id)
{
    uint16_t short_addr = (uint16_t)(device_id & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

uint32_t uwb_session_status_bits_from_diagnostics(const struct uwb_session_diagnostics *diagnostics)
{
    uint32_t status_bits = 0u;

    if (diagnostics == NULL) {
        return 0u;
    }

    if (diagnostics->scans > 0u) {
        status_bits |= STATUS_BIT_UWB_SCAN_ACTIVE;
    }
    if (diagnostics->sfd_timeouts > 0u ||
        diagnostics->frame_timeouts > 0u ||
        diagnostics->crc_failures > 0u ||
        diagnostics->false_wake_cooldowns > 0u) {
        status_bits |= STATUS_BIT_UWB_WAKE_DECODE_FAILURE;
    }
    if (diagnostics->collisions > 0u ||
        diagnostics->arbitration_losses > 0u) {
        status_bits |= STATUS_BIT_UWB_CLAIM_COLLISION;
    }
    if (diagnostics->ds_twr_failures > 0u) {
        status_bits |= STATUS_BIT_UWB_DS_TWR_FAILURE;
    }
    if (diagnostics->timing_rejections > 0u) {
        status_bits |= STATUS_BIT_UWB_TIMING_REJECTION;
    }
    if (diagnostics->uwb_mesh_packets > 0u) {
        status_bits |= STATUS_BIT_UWB_MESH_RX;
    }

    return status_bits;
}

static int validate_clicker_config(const struct uwb_clicker_config *config)
{
    if (config == NULL) {
        return PROTO_ERR_ARG;
    }
    if (config->network_id == 0u ||
        config->clicker_id == 0u ||
        config->click_event_id == 0u ||
        config->nonce == 0u ||
        config->min_anchor_count == 0u ||
        config->max_anchor_count < config->min_anchor_count ||
        config->max_anchor_count > UWB_RANGE_SCHEDULE_MAX_ANCHORS ||
        config->max_attempts == 0u ||
        ((uint16_t)config->max_attempts * config->max_anchor_count) >
        UWB_SESSION_DISCOVERY_CAPACITY ||
        config->samples_per_anchor == 0u ||
        config->samples_per_anchor > UWB_RANGING_REQUESTS_MAX_PER_ANCHOR ||
        config->wake_channel == 0u ||
        config->ranging_channel == 0u ||
        !flags_valid(config->flags)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static int validate_anchor_config(const struct uwb_anchor_config *config)
{
    if (config == NULL) {
        return PROTO_ERR_ARG;
    }
    if (config->network_id == 0u ||
        config->anchor_id == 0u ||
        config->anchor_slot >= UWB_DISCOVERY_SLOT_COUNT ||
        config->wake_channel == 0u ||
        config->ranging_channel == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static bool successful_anchor_seen(const struct uwb_clicker_session *session,
                                   uint64_t anchor_id)
{
    for (uint8_t i = 0u; i < session->successful_unique_count; i++) {
        if (session->successful_anchor_ids[i] == anchor_id) {
            return true;
        }
    }
    return false;
}

static int candidate_index(const struct uwb_clicker_session *session, uint64_t anchor_id)
{
    for (uint8_t i = 0u; i < session->candidate_count; i++) {
        if (session->candidates[i].anchor_id == anchor_id) {
            return (int)i;
        }
    }
    return -1;
}

static bool range_status_valid(enum range_status status)
{
    return status >= RANGE_OK && status <= RANGE_TIMING_INVALID;
}

static int schedule_entry_index(const struct uwb_range_schedule_frame *schedule, uint64_t anchor_id)
{
    for (uint8_t i = 0u; i < schedule->selected_count; i++) {
        if (schedule->entries[i].anchor_id == anchor_id) {
            return (int)i;
        }
    }
    return -1;
}

static bool schedule_last_sample_index_for_anchor(const struct uwb_range_schedule_frame *schedule,
                                                  uint64_t anchor_id,
                                                  size_t *last_sample_index)
{
    size_t sample_index = 0u;
    bool found = false;

    if (schedule == NULL || last_sample_index == NULL) {
        return false;
    }

    for (uint8_t round = 0u; round < schedule->samples_per_anchor; round++) {
        for (uint8_t i = 0u; i < schedule->selected_count; i++) {
            if (round < schedule->entries[i].sample_count) {
                if (schedule->entries[i].anchor_id == anchor_id) {
                    *last_sample_index = sample_index;
                    found = true;
                }
                sample_index++;
            }
        }
    }
    return found;
}

static void clear_anchor_schedule(struct uwb_anchor_session *session)
{
    memset(&session->schedule_entry, 0, sizeof(session->schedule_entry));
    session->uwb_wait_deadline_ms = 0u;
    session->reply_delay_us = 0u;
    session->expected_ranging_channel = 0u;
    session->scheduled = false;
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool claim_has_current_event_ids(const struct uwb_anchor_session *session,
                                        const struct uwb_wake_claim_frame *claim)
{
    return session->epoch.active &&
           session->epoch.network_id == claim->network_id &&
           session->epoch.clicker_id == claim->clicker_id &&
           session->epoch.click_event_id == claim->click_event_id;
}

static bool claim_is_newer_attempt_for_current_event(const struct uwb_anchor_session *session,
                                                     const struct uwb_wake_claim_frame *claim)
{
    return claim_has_current_event_ids(session, claim) &&
           claim->attempt_index > session->epoch.attempt_index;
}

static void clear_attempt_discovery(struct uwb_clicker_session *session)
{
    memset(session->candidates, 0, sizeof(session->candidates));
    memset(&session->schedule, 0, sizeof(session->schedule));
    session->candidate_count = 0u;
    session->next_sample_index = 0u;
    session->range_step_active = false;
}

int uwb_clicker_session_start(struct uwb_clicker_session *session,
                              const struct uwb_clicker_config *config)
{
    int ret;

    if (session == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_clicker_config(config);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(session, 0, sizeof(*session));
    session->config = *config;
    session->attempt_index = 1u;
    session->state = UWB_CLICKER_POLITENESS;
    return PROTO_OK;
}

int uwb_clicker_build_wake_claim(struct uwb_clicker_session *session,
                                 uint64_t priority_id,
                                 uint16_t wake_train_ends_in_ms,
                                 uint16_t discovery_starts_in_ms,
                                 uint16_t claimed_duration_ms,
                                 struct uwb_wake_claim_frame *claim)
{
    int ret;

    if (session == NULL || claim == NULL || priority_id == 0u) {
        return PROTO_ERR_ARG;
    }
    if (session->state != UWB_CLICKER_POLITENESS &&
        session->state != UWB_CLICKER_WAKE) {
        return PROTO_ERR_BUSY;
    }

    claim->network_id = session->config.network_id;
    claim->clicker_id = session->config.clicker_id;
    claim->click_event_id = session->config.click_event_id;
    claim->attempt_index = session->attempt_index;
    claim->priority_id = priority_id;
    claim->wake_channel = session->config.wake_channel;
    claim->ranging_channel = session->config.ranging_channel;
    claim->wake_train_ends_in_ms = wake_train_ends_in_ms;
    claim->discovery_starts_in_ms = discovery_starts_in_ms;
    claim->claimed_duration_ms = claimed_duration_ms;
    claim->min_anchor_count = session->config.min_anchor_count;
    claim->max_anchor_count = session->config.max_anchor_count;
    claim->nonce = session->config.nonce;
    claim->flags = session->config.flags;
    ret = uwb_validate_wake_claim(claim);
    if (ret == PROTO_OK) {
        session->state = UWB_CLICKER_WAKE;
    }
    return ret;
}

int uwb_clicker_build_discover(struct uwb_clicker_session *session,
                               struct uwb_discover_frame *discover)
{
    if (session == NULL || discover == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state != UWB_CLICKER_WAKE &&
        session->state != UWB_CLICKER_DISCOVERY) {
        return PROTO_ERR_BUSY;
    }

    discover->network_id = session->config.network_id;
    discover->clicker_id = session->config.clicker_id;
    discover->click_event_id = session->config.click_event_id;
    discover->attempt_index = session->attempt_index;
    discover->nonce = session->config.nonce;
    discover->discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT;
    discover->flags = session->config.flags;
    session->state = UWB_CLICKER_DISCOVERY;
    return PROTO_OK;
}

int uwb_clicker_note_discovery_reply(struct uwb_clicker_session *session,
                                     const struct uwb_discovery_reply_frame *reply)
{
    struct uwb_anchor_candidate *candidate;
    int index;

    if (session == NULL || reply == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state != UWB_CLICKER_DISCOVERY) {
        return PROTO_ERR_BUSY;
    }
    if (reply->network_id != session->config.network_id ||
        reply->selected_clicker_id != session->config.clicker_id ||
        reply->click_event_id != session->config.click_event_id ||
        reply->attempt_index != session->attempt_index ||
        reply->nonce != session->config.nonce ||
        reply->anchor_id == 0u ||
        reply->anchor_slot >= UWB_DISCOVERY_SLOT_COUNT ||
        !discovery_reply_status_valid(reply->status) ||
        reply->rx_quality > 100u ||
        reply->flags != session->config.flags) {
        return PROTO_ERR_MALFORMED;
    }

    session->diagnostics.discovery_replies++;
    session->state = UWB_CLICKER_DISCOVERY;

    if (reply->status != UWB_DISCOVERY_REPLY_PRESENT ||
        successful_anchor_seen(session, reply->anchor_id)) {
        return PROTO_OK;
    }

    index = candidate_index(session, reply->anchor_id);
    if (index >= 0) {
        candidate = &session->candidates[index];
        if (reply->rx_quality > candidate->rx_quality) {
            candidate->rx_quality = reply->rx_quality;
        }
        candidate->anchor_slot = reply->anchor_slot;
        return PROTO_OK;
    }

    if (session->candidate_count >= UWB_SESSION_DISCOVERY_CAPACITY) {
        return PROTO_ERR_NO_SPACE;
    }

    candidate = &session->candidates[session->candidate_count];
    candidate->anchor_id = reply->anchor_id;
    candidate->anchor_slot = reply->anchor_slot;
    candidate->rx_quality = reply->rx_quality;
    candidate->sample_count = session->config.samples_per_anchor;
    session->candidate_count++;
    return PROTO_OK;
}

static int select_next_candidate(const struct uwb_clicker_session *session,
                                 const bool *used)
{
    int selected = -1;

    for (uint8_t i = 0u; i < session->candidate_count; i++) {
        const struct uwb_anchor_candidate *candidate = &session->candidates[i];

        if (used[i]) {
            continue;
        }
        if (selected < 0 ||
            candidate->rx_quality > session->candidates[selected].rx_quality ||
            (candidate->rx_quality == session->candidates[selected].rx_quality &&
             candidate->anchor_id < session->candidates[selected].anchor_id)) {
            selected = (int)i;
        }
    }
    return selected;
}

int uwb_clicker_build_range_schedule(struct uwb_clicker_session *session,
                                     uint16_t reply_delay_us,
                                     uint16_t first_poll_delay_ms,
                                     uint16_t poll_spacing_ms,
                                     struct uwb_range_schedule_frame *schedule)
{
    bool used[UWB_SESSION_DISCOVERY_CAPACITY] = {0};
    uint8_t selected_count;

    if (session == NULL || schedule == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->candidate_count == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }
    if (session->state != UWB_CLICKER_DISCOVERY) {
        return PROTO_ERR_BUSY;
    }
    if (reply_delay_us != UWB_DS_TWR_REPLY_DELAY_US ||
        first_poll_delay_ms == 0u ||
        poll_spacing_ms < UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS) {
        return PROTO_ERR_MALFORMED;
    }

    selected_count = session->candidate_count < session->config.max_anchor_count ?
                     session->candidate_count : session->config.max_anchor_count;
    memset(schedule, 0, sizeof(*schedule));
    schedule->network_id = session->config.network_id;
    schedule->clicker_id = session->config.clicker_id;
    schedule->click_event_id = session->config.click_event_id;
    schedule->attempt_index = session->attempt_index;
    schedule->nonce = session->config.nonce;
    schedule->selected_count = selected_count;
    schedule->ranging_channel = session->config.ranging_channel;
    schedule->reply_delay_us = reply_delay_us;
    schedule->first_poll_delay_ms = first_poll_delay_ms;
    schedule->poll_spacing_ms = poll_spacing_ms;
    schedule->samples_per_anchor = session->config.samples_per_anchor;
    schedule->flags = session->config.flags;

    for (uint8_t i = 0u; i < selected_count; i++) {
        int selected = select_next_candidate(session, used);

        if (selected < 0) {
            return PROTO_ERR_MALFORMED;
        }
        used[selected] = true;
        schedule->entries[i].anchor_id = session->candidates[selected].anchor_id;
        schedule->entries[i].seq = (uint8_t)(1u + i);
        schedule->entries[i].sample_count = session->candidates[selected].sample_count;
    }

    session->schedule = *schedule;
    session->next_sample_index = 0u;
    session->range_step_active = false;
    session->state = UWB_CLICKER_RANGING;
    session->diagnostics.schedules++;
    return PROTO_OK;
}

int uwb_clicker_next_range_step(struct uwb_clicker_session *session,
                                struct uwb_range_step *step)
{
    size_t total_samples;

    if (session == NULL || step == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state != UWB_CLICKER_RANGING) {
        return PROTO_ERR_BUSY;
    }
    if (session->range_step_active) {
        return PROTO_ERR_BUSY;
    }

    total_samples = uwb_range_schedule_total_samples(&session->schedule);
    while (session->next_sample_index < total_samples) {
        uint64_t anchor_id = 0u;
        uint8_t seq = 0u;
        int index;
        int schedule_index;
        int ret;

        ret = uwb_range_schedule_sample_at(&session->schedule,
                                           session->next_sample_index,
                                           &anchor_id,
                                           &seq);
        if (ret != PROTO_OK) {
            return ret;
        }
        index = candidate_index(session, anchor_id);
        if (index < 0) {
            return PROTO_ERR_MALFORMED;
        }
        schedule_index = schedule_entry_index(&session->schedule, anchor_id);
        if (schedule_index < 0) {
            return PROTO_ERR_MALFORMED;
        }

        if (session->candidates[index].failure_count >=
            UWB_SESSION_MAX_FAILED_RANGING_PER_ANCHOR) {
            session->next_sample_index++;
            continue;
        }

        step->anchor_id = anchor_id;
        step->anchor_index = (uint8_t)index;
        step->round_index = (uint8_t)(seq - session->schedule.entries[schedule_index].seq);
        step->seq = seq;
        step->sample_index = session->next_sample_index;
        session->range_step_active = true;
        session->diagnostics.sample_order_count++;
        return PROTO_OK;
    }

    if (session->successful_unique_count >= session->config.min_anchor_count) {
        session->state = UWB_CLICKER_SUCCEEDED;
        return PROTO_ERR_NOT_FOUND;
    }
    if (session->attempt_index < session->config.max_attempts) {
        session->state = UWB_CLICKER_RETRY_WAIT;
    } else {
        session->state = UWB_CLICKER_FAILED;
    }
    return PROTO_ERR_NOT_FOUND;
}

int uwb_clicker_record_range_result(struct uwb_clicker_session *session,
                                    const struct uwb_range_step *step,
                                    enum range_status status)
{
    struct uwb_anchor_candidate *candidate;
    uint64_t expected_anchor_id = 0u;
    uint8_t expected_seq = 0u;
    int schedule_index;
    int ret;

    if (session == NULL || step == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!range_status_valid(status)) {
        return PROTO_ERR_MALFORMED;
    }
    if (session->state != UWB_CLICKER_RANGING ||
        !session->range_step_active ||
        step->sample_index != session->next_sample_index ||
        step->anchor_index >= session->candidate_count ||
        session->candidates[step->anchor_index].anchor_id != step->anchor_id) {
        return PROTO_ERR_MALFORMED;
    }
    ret = uwb_range_schedule_sample_at(&session->schedule,
                                       session->next_sample_index,
                                       &expected_anchor_id,
                                       &expected_seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    schedule_index = schedule_entry_index(&session->schedule, expected_anchor_id);
    if (schedule_index < 0 ||
        expected_anchor_id != step->anchor_id ||
        expected_seq != step->seq ||
        step->round_index !=
        (uint8_t)(expected_seq - session->schedule.entries[schedule_index].seq)) {
        return PROTO_ERR_MALFORMED;
    }

    candidate = &session->candidates[step->anchor_index];
    if (status == RANGE_OK) {
        session->diagnostics.ds_twr_successes++;
        if (!candidate->ranged_ok && !successful_anchor_seen(session, step->anchor_id)) {
            candidate->ranged_ok = true;
            session->successful_anchor_ids[session->successful_unique_count] = step->anchor_id;
            session->successful_unique_count++;
        }
    } else {
        session->diagnostics.ds_twr_failures++;
        if (status == RANGE_TIMING_INVALID) {
            session->diagnostics.timing_rejections++;
        }
        if (candidate->failure_count < UINT8_MAX) {
            candidate->failure_count++;
        }
    }

    session->next_sample_index++;
    session->range_step_active = false;
    return PROTO_OK;
}

int uwb_clicker_abort_attempt(struct uwb_clicker_session *session)
{
    if (session == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state == UWB_CLICKER_IDLE ||
        session->state == UWB_CLICKER_SUCCEEDED ||
        session->state == UWB_CLICKER_FAILED) {
        return PROTO_ERR_BUSY;
    }

    session->range_step_active = false;
    if (session->successful_unique_count >= session->config.min_anchor_count) {
        session->state = UWB_CLICKER_SUCCEEDED;
        return PROTO_OK;
    }
    if (session->attempt_index < session->config.max_attempts) {
        session->state = UWB_CLICKER_RETRY_WAIT;
    } else {
        session->state = UWB_CLICKER_FAILED;
    }
    return PROTO_OK;
}

int uwb_clicker_prepare_retry(struct uwb_clicker_session *session)
{
    if (session == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state == UWB_CLICKER_IDLE ||
        session->state == UWB_CLICKER_RANGING ||
        session->state == UWB_CLICKER_SUCCEEDED ||
        session->state == UWB_CLICKER_FAILED) {
        return PROTO_ERR_BUSY;
    }
    if (session->attempt_index >= session->config.max_attempts) {
        session->state = UWB_CLICKER_FAILED;
        return PROTO_ERR_BUSY;
    }

    session->attempt_index++;
    clear_attempt_discovery(session);
    session->diagnostics.retries++;
    session->state = UWB_CLICKER_POLITENESS;
    return PROTO_OK;
}

int uwb_anchor_session_init(struct uwb_anchor_session *session,
                            const struct uwb_anchor_config *config)
{
    int ret;

    if (session == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_anchor_config(config);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(session, 0, sizeof(*session));
    session->config = *config;
    session->state = UWB_ANCHOR_IDLE;
    return PROTO_OK;
}

void uwb_anchor_note_idle_scan(struct uwb_anchor_session *session,
                               uint16_t startup_us,
                               uint16_t pll_us,
                               uint16_t rx_us,
                               bool preamble_detected)
{
    if (session == NULL) {
        return;
    }

    session->diagnostics.scans++;
    diagnostics_add_saturated(&session->diagnostics.scan_startup_time_us, startup_us);
    diagnostics_add_saturated(&session->diagnostics.scan_pll_time_us, pll_us);
    diagnostics_add_saturated(&session->diagnostics.scan_rx_time_us, rx_us);
    diagnostics_add_awake_time(&session->diagnostics,
                               (uint32_t)startup_us + (uint32_t)pll_us +
                               (uint32_t)rx_us);
    if (preamble_detected) {
        session->diagnostics.preambles++;
    }
}

void uwb_anchor_note_awake_time(struct uwb_anchor_session *session,
                                uint32_t awake_us)
{
    if (session == NULL) {
        return;
    }

    diagnostics_add_awake_time(&session->diagnostics, awake_us);
}

void uwb_anchor_note_wake_decode_failure(struct uwb_anchor_session *session,
                                         enum uwb_wake_decode_failure failure)
{
    if (session == NULL) {
        return;
    }

    switch (failure) {
    case UWB_WAKE_DECODE_PREAMBLE_ONLY:
        session->diagnostics.preambles++;
        break;
    case UWB_WAKE_DECODE_SFD_TIMEOUT:
        session->diagnostics.sfd_timeouts++;
        break;
    case UWB_WAKE_DECODE_FRAME_TIMEOUT:
        session->diagnostics.frame_timeouts++;
        break;
    case UWB_WAKE_DECODE_CRC_FAILURE:
        session->diagnostics.crc_failures++;
        break;
    default:
        break;
    }
}

void uwb_anchor_note_false_wake_cooldown(struct uwb_anchor_session *session)
{
    if (session == NULL) {
        return;
    }

    session->diagnostics.false_wake_cooldowns++;
}

void uwb_anchor_note_timing_rejection(struct uwb_anchor_session *session)
{
    if (session == NULL) {
        return;
    }

    session->diagnostics.timing_rejections++;
}

int uwb_anchor_note_range_result(struct uwb_anchor_session *session,
                                 enum range_status status)
{
    if (session == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!range_status_valid(status)) {
        return PROTO_ERR_MALFORMED;
    }

    if (status == RANGE_OK) {
        session->diagnostics.ds_twr_successes++;
        return PROTO_OK;
    }

    session->diagnostics.ds_twr_failures++;
    if (status == RANGE_TIMING_INVALID) {
        uwb_anchor_note_timing_rejection(session);
    }
    return PROTO_OK;
}

void uwb_anchor_note_mesh_packet(struct uwb_anchor_session *session)
{
    if (session == NULL) {
        return;
    }

    session->diagnostics.uwb_mesh_packets++;
}

void uwb_anchor_note_sample_order(struct uwb_anchor_session *session)
{
    if (session == NULL) {
        return;
    }

    session->diagnostics.sample_order_count++;
}

static bool claim_decision_is_collision(enum uwb_anchor_claim_decision decision)
{
    return decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY ||
           decision == UWB_ANCHOR_CLAIM_REJECTED_BUSY ||
           decision == UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION;
}

int uwb_anchor_accept_wake_claim(struct uwb_anchor_session *session,
                                 const struct uwb_wake_claim_frame *claim,
                                 uint32_t now_ms,
                                 enum uwb_anchor_claim_decision *decision)
{
    enum uwb_anchor_claim_decision local_decision =
        UWB_ANCHOR_CLAIM_REJECTED_MALFORMED;
    int ret;

    if (session == NULL || claim == NULL) {
        return PROTO_ERR_ARG;
    }
    if (claim->network_id != session->config.network_id ||
        claim->wake_channel != session->config.wake_channel ||
        claim->ranging_channel != session->config.ranging_channel) {
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REJECTED_MALFORMED;
        }
        return PROTO_ERR_MALFORMED;
    }
    ret = uwb_validate_wake_claim(claim);
    if (ret != PROTO_OK) {
        if (decision != NULL) {
            *decision = UWB_ANCHOR_CLAIM_REJECTED_MALFORMED;
        }
        return ret;
    }
    if (session->state != UWB_ANCHOR_IDLE &&
        session->state != UWB_ANCHOR_CLAIMED &&
        session->state != UWB_ANCHOR_ABORTED) {
        if (session->epoch.active &&
            deadline_reached(now_ms, session->epoch.epoch_ends_at_ms)) {
            clear_anchor_schedule(session);
            uwb_anchor_epoch_clear(&session->epoch);
            session->state = UWB_ANCHOR_IDLE;
        } else if (!claim_is_newer_attempt_for_current_event(session, claim)) {
            local_decision = UWB_ANCHOR_CLAIM_REJECTED_BUSY;
            if (decision != NULL) {
                *decision = local_decision;
            }
            session->diagnostics.collisions++;
            session->diagnostics.arbitration_losses++;
            return PROTO_ERR_BUSY;
        }
    }

    ret = uwb_anchor_epoch_consider_claim(&session->epoch,
                                          claim,
                                          now_ms,
                                          &local_decision);
    if (decision != NULL) {
        *decision = local_decision;
    }
    if (ret == PROTO_OK) {
        clear_anchor_schedule(session);
        session->state = UWB_ANCHOR_CLAIMED;
        session->diagnostics.claims++;
        if (local_decision == UWB_ANCHOR_CLAIM_ACCEPTED ||
            local_decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY) {
            session->diagnostics.arbitration_wins++;
        }
    }
    if (claim_decision_is_collision(local_decision)) {
        session->diagnostics.collisions++;
    }
    if (ret != PROTO_OK && claim_decision_is_collision(local_decision)) {
        session->diagnostics.arbitration_losses++;
    }
    return ret;
}

int uwb_anchor_build_discovery_reply(struct uwb_anchor_session *session,
                                     const struct uwb_discover_frame *discover,
                                     uint8_t rx_quality,
                                     uint16_t battery_mv,
                                     struct uwb_discovery_reply_frame *reply)
{
    if (session == NULL || discover == NULL || reply == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state != UWB_ANCHOR_CLAIMED &&
        session->state != UWB_ANCHOR_DISCOVERY_REPLIED) {
        return PROTO_ERR_BUSY;
    }
    if (!uwb_anchor_epoch_matches(&session->epoch,
                                  discover->network_id,
                                  discover->clicker_id,
                                  discover->click_event_id,
                                  discover->attempt_index,
                                  discover->nonce) ||
        discover->discovery_slot_count == 0u ||
        discover->discovery_slot_count > UWB_DISCOVERY_SLOT_COUNT ||
        session->config.anchor_slot >= discover->discovery_slot_count ||
        rx_quality > 100u ||
        !flags_valid(discover->flags) ||
        discover->flags != session->epoch.flags) {
        return PROTO_ERR_MALFORMED;
    }

    reply->network_id = discover->network_id;
    reply->anchor_id = session->config.anchor_id;
    reply->selected_clicker_id = discover->clicker_id;
    reply->click_event_id = discover->click_event_id;
    reply->attempt_index = discover->attempt_index;
    reply->nonce = discover->nonce;
    reply->anchor_slot = session->config.anchor_slot;
    reply->status = UWB_DISCOVERY_REPLY_PRESENT;
    reply->rx_quality = rx_quality;
    reply->battery_mv = battery_mv;
    reply->flags = discover->flags;

    session->state = UWB_ANCHOR_DISCOVERY_REPLIED;
    session->diagnostics.discovery_replies++;
    return PROTO_OK;
}

int uwb_anchor_accept_range_schedule(struct uwb_anchor_session *session,
                                     const struct uwb_range_schedule_frame *schedule,
                                     uint32_t now_ms,
                                     uint16_t guard_ms)
{
    if (session == NULL || schedule == NULL) {
        return PROTO_ERR_ARG;
    }
    if (session->state != UWB_ANCHOR_DISCOVERY_REPLIED) {
        return PROTO_ERR_BUSY;
    }
    if (!uwb_anchor_epoch_matches(&session->epoch,
                                  schedule->network_id,
                                  schedule->clicker_id,
                                  schedule->click_event_id,
                                  schedule->attempt_index,
                                  schedule->nonce) ||
        schedule->ranging_channel != session->config.ranging_channel ||
        schedule->flags != session->epoch.flags) {
        return PROTO_ERR_MALFORMED;
    }
    if (uwb_validate_range_schedule(schedule) != PROTO_OK ||
        uwb_range_schedule_total_samples(schedule) == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    for (uint8_t i = 0u; i < schedule->selected_count; i++) {
        if (schedule->entries[i].anchor_id != session->config.anchor_id) {
            continue;
        }
        size_t last_sample_index = 0u;

        if (!schedule_last_sample_index_for_anchor(schedule,
                                                   session->config.anchor_id,
                                                   &last_sample_index)) {
            return PROTO_ERR_MALFORMED;
        }

        session->schedule_entry = schedule->entries[i];
        session->reply_delay_us = schedule->reply_delay_us;
        session->expected_ranging_channel = schedule->ranging_channel;
        session->uwb_wait_deadline_ms = now_ms + schedule->first_poll_delay_ms +
                                        ((uint32_t)(last_sample_index + 1u) *
                                         schedule->poll_spacing_ms) +
                                        guard_ms;
        session->scheduled = true;
        session->state = UWB_ANCHOR_SCHEDULED;
        session->diagnostics.schedules++;
        return PROTO_OK;
    }

    clear_anchor_schedule(session);
    uwb_anchor_epoch_clear(&session->epoch);
    session->state = UWB_ANCHOR_IDLE;
    return PROTO_ERR_NOT_FOUND;
}

bool uwb_anchor_accepts_range_exchange(const struct uwb_anchor_session *session,
                                       const struct uwb_range_exchange_identity *identity)
{
    uint8_t round_index = 0u;

    return uwb_anchor_range_round_index(session, identity, &round_index) == PROTO_OK;
}

int uwb_anchor_range_round_index(const struct uwb_anchor_session *session,
                                 const struct uwb_range_exchange_identity *identity,
                                 uint8_t *round_index)
{
    uint16_t first_seq;
    uint16_t seq_after_last;
    uint16_t identity_seq;

    if (session == NULL || identity == NULL ||
        round_index == NULL ||
        !session->scheduled ||
        session->state != UWB_ANCHOR_SCHEDULED) {
        return PROTO_ERR_ARG;
    }

    first_seq = session->schedule_entry.seq;
    seq_after_last = first_seq + session->schedule_entry.sample_count;
    identity_seq = identity->seq;

    if (identity->network_id != session->config.network_id ||
        identity->clicker_id != session->epoch.clicker_id ||
        identity->click_event_id != session->epoch.click_event_id ||
        identity->attempt_index != session->epoch.attempt_index ||
        identity->nonce != session->epoch.nonce ||
        identity->anchor_id != session->config.anchor_id ||
        identity->ranging_channel != session->expected_ranging_channel ||
        identity->reply_delay_us != session->reply_delay_us ||
        identity->flags != session->epoch.flags ||
        identity_seq < first_seq ||
        identity_seq >= seq_after_last) {
        return PROTO_ERR_MALFORMED;
    }

    *round_index = (uint8_t)(identity_seq - first_seq);
    return PROTO_OK;
}

void uwb_anchor_abort_epoch(struct uwb_anchor_session *session)
{
    if (session == NULL) {
        return;
    }

    uwb_anchor_epoch_clear(&session->epoch);
    clear_anchor_schedule(session);
    session->state = UWB_ANCHOR_ABORTED;
}

int uwb_session_validate_reply_timing(uint16_t poll_to_resp_us,
                                      uint16_t resp_to_final_us,
                                      uint16_t expected_reply_delay_us,
                                      uint16_t tolerance_us)
{
    uint16_t delta;

    if (expected_reply_delay_us != UWB_DS_TWR_REPLY_DELAY_US) {
        return PROTO_ERR_MALFORMED;
    }
    if (tolerance_us == 0u) {
        tolerance_us = UWB_SESSION_REPLY_DELAY_TOLERANCE_US;
    }

    delta = poll_to_resp_us > expected_reply_delay_us ?
            poll_to_resp_us - expected_reply_delay_us :
            expected_reply_delay_us - poll_to_resp_us;
    if (delta > tolerance_us) {
        return PROTO_ERR_MALFORMED;
    }

    delta = resp_to_final_us > expected_reply_delay_us ?
            resp_to_final_us - expected_reply_delay_us :
            expected_reply_delay_us - resp_to_final_us;
    if (delta > tolerance_us) {
        return PROTO_ERR_MALFORMED;
    }

    delta = poll_to_resp_us > resp_to_final_us ?
            poll_to_resp_us - resp_to_final_us :
            resp_to_final_us - poll_to_resp_us;
    if (delta > tolerance_us) {
        return PROTO_ERR_MALFORMED;
    }

    return PROTO_OK;
}
