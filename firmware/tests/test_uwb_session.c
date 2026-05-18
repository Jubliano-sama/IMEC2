#include "uwb_session.h"

#include "report.h"

#include <assert.h>

static struct uwb_clicker_config clicker_config(void)
{
    const struct uwb_clicker_config config = {
        .network_id = 0x494D4543u,
        .clicker_id = UINT64_C(0x1111222233334444),
        .click_event_id = 77u,
        .nonce = UINT64_C(0xAABBCCDDEEFF0011),
        .min_anchor_count = 4u,
        .max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS,
        .max_attempts = UWB_SESSION_DEFAULT_MAX_ATTEMPTS,
        .samples_per_anchor = 2u,
        .wake_channel = 5u,
        .ranging_channel = 5u,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    return config;
}

static struct uwb_anchor_config anchor_config(uint64_t anchor_id, uint8_t slot)
{
    const struct uwb_anchor_config config = {
        .network_id = 0x494D4543u,
        .anchor_id = anchor_id,
        .anchor_slot = slot,
        .wake_channel = 5u,
        .ranging_channel = 5u,
    };
    return config;
}

static struct uwb_discovery_reply_frame reply_for(const struct uwb_clicker_session *session,
                                                  uint64_t anchor_id,
                                                  uint8_t slot,
                                                  uint8_t quality)
{
    const struct uwb_discovery_reply_frame reply = {
        .network_id = session->config.network_id,
        .anchor_id = anchor_id,
        .selected_clicker_id = session->config.clicker_id,
        .click_event_id = session->config.click_event_id,
        .attempt_index = session->attempt_index,
        .nonce = session->config.nonce,
        .anchor_slot = slot,
        .status = UWB_DISCOVERY_REPLY_PRESENT,
        .rx_quality = quality,
        .battery_mv = 3000u,
        .flags = session->config.flags,
    };
    return reply;
}

static void clicker_begin_discovery(struct uwb_clicker_session *session)
{
    struct uwb_wake_claim_frame claim;
    struct uwb_discover_frame discover;

    if (session->state == UWB_CLICKER_POLITENESS) {
        assert(uwb_clicker_build_wake_claim(session,
                                            UINT64_C(0x0102030405060708),
                                            430u,
                                            430u,
                                            1365u,
                                            &claim) == PROTO_OK);
    }
    assert(uwb_clicker_build_discover(session, &discover) == PROTO_OK);
}

static void add_reply(struct uwb_clicker_session *session,
                      uint64_t anchor_id,
                      uint8_t slot,
                      uint8_t quality)
{
    const struct uwb_discovery_reply_frame reply = reply_for(session, anchor_id, slot, quality);

    if (session->state != UWB_CLICKER_DISCOVERY) {
        clicker_begin_discovery(session);
    }
    assert(uwb_clicker_note_discovery_reply(session, &reply) == PROTO_OK);
}

static void anchor_mark_discovery_replied(struct uwb_anchor_session *anchor)
{
    struct uwb_discover_frame discover = {0};
    struct uwb_discovery_reply_frame reply;

    assert(anchor != NULL);
    assert(anchor->epoch.active);

    discover.network_id = anchor->epoch.network_id;
    discover.clicker_id = anchor->epoch.clicker_id;
    discover.click_event_id = anchor->epoch.click_event_id;
    discover.attempt_index = anchor->epoch.attempt_index;
    discover.nonce = anchor->epoch.nonce;
    discover.discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT;
    discover.flags = anchor->epoch.flags;

    assert(uwb_anchor_build_discovery_reply(anchor, &discover, 80u, 3000u, &reply) ==
           PROTO_OK);
}

static void set_schedule_burst_defaults(struct uwb_range_schedule_frame *schedule,
                                        uint8_t min_successful_unique_anchors)
{
    assert(schedule != NULL);

    schedule->burst_window_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS;
    schedule->exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US;
    schedule->max_exchanges = (uint8_t)(schedule->selected_count *
                                        schedule->samples_per_anchor);
    schedule->min_successful_unique_anchors = min_successful_unique_anchors;
    schedule->sts_mode = UWB_RANGE_SCHEDULE_STS_DISABLED;
    schedule->diagnostics_required = UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED;
}

static void test_clicker_builds_wake_claim_and_rejects_bad_timing(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();
    struct uwb_wake_claim_frame claim = {0};

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    assert(uwb_clicker_build_wake_claim(&session,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_OK);
    assert(session.state == UWB_CLICKER_WAKE);
    assert(claim.network_id == config.network_id);
    assert(claim.clicker_id == config.clicker_id);
    assert(claim.click_event_id == config.click_event_id);
    assert(claim.attempt_index == session.attempt_index);
    assert(claim.priority_id == UINT64_C(0x0102030405060708));
    assert(claim.wake_channel == config.wake_channel);
    assert(claim.ranging_channel == config.ranging_channel);
    assert(claim.wake_train_ends_in_ms == 430u);
    assert(claim.discovery_starts_in_ms == 430u);
    assert(claim.claimed_duration_ms == 1365u);
    assert(claim.min_anchor_count == config.min_anchor_count);
    assert(claim.max_anchor_count == config.max_anchor_count);
    assert(claim.nonce == config.nonce);
    assert(claim.flags == config.flags);

    assert(uwb_clicker_build_wake_claim(&session,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        429u,
                                        1365u,
                                        &claim) == PROTO_ERR_MALFORMED);
    assert(uwb_clicker_build_wake_claim(&session,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        430u,
                                        429u,
                                        &claim) == PROTO_ERR_MALFORMED);
    assert(uwb_clicker_build_wake_claim(&session,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        430u,
                                        UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS + 1u,
                                        &claim) == PROTO_ERR_MALFORMED);
    assert(uwb_clicker_build_wake_claim(&session,
                                        0u,
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_ERR_ARG);

    session.state = UWB_CLICKER_RANGING;
    assert(uwb_clicker_build_wake_claim(&session,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_ERR_BUSY);
}

static void test_clicker_contention_delay_bounds_and_diagnostics(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();

    assert(uwb_clicker_contention_window_slots(0u) ==
           UWB_CLICKER_CONTENTION_ATTEMPT1_SLOTS);
    assert(uwb_clicker_contention_window_slots(1u) ==
           UWB_CLICKER_CONTENTION_ATTEMPT1_SLOTS);
    assert(uwb_clicker_contention_window_slots(2u) ==
           UWB_CLICKER_CONTENTION_ATTEMPT2_SLOTS);
    assert(uwb_clicker_contention_window_slots(3u) ==
           UWB_CLICKER_CONTENTION_ATTEMPT3_PLUS_SLOTS);
    assert(uwb_clicker_contention_window_slots(6u) ==
           UWB_CLICKER_CONTENTION_ATTEMPT3_PLUS_SLOTS);
    assert(UWB_CLICKER_CONTENTION_SLOT_MS == 12u);
    assert(UWB_CLICKER_CONTENTION_ATTEMPT2_SLOTS >= 32u);
    assert(UWB_CLICKER_CONTENTION_ATTEMPT3_PLUS_SLOTS >= 64u);

    assert(uwb_clicker_contention_delay_ms(1u, 0u) == 0u);
    assert(uwb_clicker_contention_delay_ms(1u, 15u) == 180u);
    assert(uwb_clicker_contention_delay_ms(1u, 16u) == 0u);
    assert(uwb_clicker_contention_delay_ms(2u, 31u) == 372u);
    assert(uwb_clicker_contention_delay_ms(3u, 63u) == 756u);
    assert(uwb_clicker_contention_delay_ms(6u, UINT32_MAX) <= 756u);

    assert(uwb_clicker_wake_claim_jitter_us(0u) == 0u);
    assert(uwb_clicker_wake_claim_jitter_us(400u) == 400u);
    assert(uwb_clicker_wake_claim_jitter_us(401u) == 0u);

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    uwb_clicker_note_politeness_sample(&session, false);
    uwb_clicker_note_politeness_sample(&session, true);
    uwb_clicker_note_contention_delay(&session, 180u);
    uwb_clicker_note_retry_delay(&session, 522u);
    uwb_clicker_note_wake_claim_tx(&session, 3u);

    assert(session.diagnostics.politeness_samples == 2u);
    assert(session.diagnostics.politeness_activity_hits == 1u);
    assert(session.diagnostics.contention_delay_ms == 180u);
    assert(session.diagnostics.retry_delay_ms == 522u);
    assert(session.diagnostics.wake_claim_tx_count == 3u);

    session.diagnostics.contention_delay_ms = UINT32_MAX - 1u;
    session.diagnostics.retry_delay_ms = UINT32_MAX - 1u;
    session.diagnostics.wake_claim_tx_count = UINT32_MAX - 1u;
    uwb_clicker_note_contention_delay(&session, 2u);
    uwb_clicker_note_retry_delay(&session, 2u);
    uwb_clicker_note_wake_claim_tx(&session, 2u);
    assert(session.diagnostics.contention_delay_ms == UINT32_MAX);
    assert(session.diagnostics.retry_delay_ms == UINT32_MAX);
    assert(session.diagnostics.wake_claim_tx_count == UINT32_MAX);
}

static void test_clicker_politeness_decodes_relevant_uwb_packets(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint16_t wait_ms = 0u;
    uint8_t frame_type = 0u;
    struct uwb_wake_claim_frame claim = {
        .network_id = config.network_id,
        .clicker_id = UINT64_C(0x2222333344445555),
        .click_event_id = 99u,
        .attempt_index = 1u,
        .priority_id = UINT64_C(0x0102030405060708),
        .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .wake_train_ends_in_ms = 300u,
        .discovery_starts_in_ms = 300u,
        .claimed_duration_ms = 612u,
        .min_anchor_count = 4u,
        .max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS,
        .nonce = UINT64_C(0x1111222233334444),
        .flags = FLAG_COUNT_AS_CLICK,
    };
    struct uwb_range_schedule_frame schedule = {
        .network_id = config.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .selected_count = 4u,
        .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .first_poll_delay_ms = 8u,
        .poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
        .samples_per_anchor = 2u,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    struct uwb_range_header poll = {
        .type = MSG_UWB_POLL,
        .seq = 1u,
        .round_index = 0u,
        .network_id = config.network_id,
        .session_id = 1234u,
        .session_nonce = UINT64_C(0x5555666677778888),
        .initiator_id = claim.clicker_id,
        .responder_id = UINT64_C(0xABCDEF0012345678),
        .flags = FLAG_COUNT_AS_CLICK,
    };
    struct proto_packet mesh_packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = 0u,
        .src_id = UINT64_C(0xABCDEF0012345678),
        .dst_id = config.clicker_id,
        .session_id = 9001u,
        .seq = 1u,
        .ttl = 3u,
        .payload_len = 0u,
    };

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);

    assert(uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              &frame_type) == PROTO_OK);
    assert(frame_type == MSG_UWB_WAKE_CLAIM);
    assert(wait_ms == claim.claimed_duration_ms);

    claim.network_id++;
    assert(uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) == PROTO_OK);
    wait_ms = 123u;
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              NULL) == PROTO_OK);
    assert(wait_ms == 0u);
    claim.network_id = config.network_id;

    claim.clicker_id = config.clicker_id;
    assert(uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              NULL) == PROTO_OK);
    assert(wait_ms == 0u);
    claim.clicker_id = UINT64_C(0x2222333344445555);

    assert(uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) == PROTO_OK);
    frame[frame_len - 1u] ^= 0x01u;
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              NULL) == PROTO_ERR_BAD_CRC);

    for (uint8_t i = 0u; i < schedule.selected_count; i++) {
        schedule.entries[i].anchor_id = UINT64_C(0xABCDEF0012345600) + i;
        schedule.entries[i].seq = (uint8_t)(1u + (i * schedule.samples_per_anchor));
        schedule.entries[i].sample_count = schedule.samples_per_anchor;
    }
    set_schedule_burst_defaults(&schedule, 4u);
    schedule.first_poll_delay_ms = 8u;
    schedule.burst_window_ms = 240u;
    assert(uwb_encode_range_schedule(&schedule, frame, sizeof(frame), &frame_len) ==
           PROTO_OK);
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              &frame_type) == PROTO_OK);
    assert(frame_type == MSG_UWB_RANGE_SCHEDULE);
    assert(wait_ms == schedule.first_poll_delay_ms + schedule.burst_window_ms);

    poll.initiator_short_addr = uwb_session_short_addr_from_id(poll.initiator_id);
    poll.responder_short_addr = uwb_session_short_addr_from_id(poll.responder_id);
    assert(uwb_encode_poll(&poll, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              &frame_type) == PROTO_OK);
    assert(frame_type == MSG_UWB_POLL);
    assert(wait_ms == 250u);

    assert(uwb_mesh_frame_encode(config.network_id,
                                 mesh_packet.src_id,
                                 config.clicker_id,
                                 &mesh_packet,
                                 NULL,
                                 frame,
                                 sizeof(frame),
                                 &frame_len) == PROTO_OK);
    wait_ms = 250u;
    assert(uwb_clicker_decode_politeness_wait(&session,
                                              frame,
                                              frame_len,
                                              250u,
                                              &wait_ms,
                                              &frame_type) == PROTO_OK);
    assert(frame_type == MSG_UWB_MESH);
    assert(wait_ms == 0u);
}

static void test_clicker_discovers_50_and_schedules_best_6_only(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config = clicker_config();

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < UWB_DISCOVERY_SLOT_COUNT; i++) {
        add_reply(&session, UINT64_C(0xAA00000000000000) + i + 1u, i, i);
    }

    assert(session.candidate_count == UWB_DISCOVERY_SLOT_COUNT);
    assert(session.successful_unique_count == 0u);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(schedule.selected_count == UWB_RANGE_SCHEDULE_MAX_ANCHORS);
    assert(schedule.selected_count == 6u);

    for (uint8_t i = 0u; i < schedule.selected_count; i++) {
        uint64_t expected_anchor = UINT64_C(0xAA00000000000000) +
                                   UWB_DISCOVERY_SLOT_COUNT - i;

        assert(schedule.entries[i].anchor_id == expected_anchor);
        assert(schedule.entries[i].sample_count == config.samples_per_anchor);
    }
}

static void test_clicker_discovers_sparse_50_slots_with_6_present(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config = clicker_config();
    const uint8_t present_slots[] = {0u, 7u, 13u, 29u, 43u, 49u};
    const uint64_t present_base = UINT64_C(0xBB00000000000000);

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    clicker_begin_discovery(&session);
    for (uint8_t slot = 0u; slot < UWB_DISCOVERY_SLOT_COUNT; slot++) {
        struct uwb_discovery_reply_frame reply =
            reply_for(&session, UINT64_C(0xCC00000000000000) + slot + 1u, slot, 10u);
        bool present = false;
        uint8_t present_index = 0u;

        for (uint8_t i = 0u; i < sizeof(present_slots) / sizeof(present_slots[0]); i++) {
            if (slot == present_slots[i]) {
                present = true;
                present_index = i;
                break;
            }
        }

        if (present) {
            reply.anchor_id = present_base + present_index + 1u;
            reply.status = UWB_DISCOVERY_REPLY_PRESENT;
            reply.rx_quality = (uint8_t)(40u + present_index);
        } else {
            reply.status = (slot % 2u) == 0u ? UWB_DISCOVERY_REPLY_BUSY :
                                                UWB_DISCOVERY_REPLY_COLLISION;
        }
        assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);
    }

    assert(session.diagnostics.discovery_replies == UWB_DISCOVERY_SLOT_COUNT);
    assert(session.candidate_count == sizeof(present_slots) / sizeof(present_slots[0]));
    assert(session.successful_unique_count == 0u);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(schedule.selected_count == sizeof(present_slots) / sizeof(present_slots[0]));

    for (uint8_t i = 0u; i < schedule.selected_count; i++) {
        uint64_t expected_anchor = present_base + schedule.selected_count - i;

        assert(schedule.entries[i].anchor_id == expected_anchor);
        assert(schedule.entries[i].sample_count == config.samples_per_anchor);
    }
}

static void test_clicker_releases_replied_anchors_when_too_few_for_normal_click(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();
    struct uwb_range_release_frame release;
    struct uwb_range_schedule_frame schedule;

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    add_reply(&session, 1u, 0u, 80u);
    add_reply(&session, 2u, 1u, 90u);
    add_reply(&session, 3u, 2u, 70u);

    assert(session.candidate_count == config.min_anchor_count - 1u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            UWB_DS_TWR_REPLY_DELAY_US,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_ERR_NOT_FOUND);
    assert(uwb_clicker_build_range_release(
               &session,
               UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
               &release) == PROTO_OK);
    assert(release.network_id == config.network_id);
    assert(release.clicker_id == config.clicker_id);
    assert(release.click_event_id == config.click_event_id);
    assert(release.attempt_index == session.attempt_index);
    assert(release.nonce == config.nonce);
    assert(release.discovered_anchor_count == session.candidate_count);
    assert(release.min_anchor_count == config.min_anchor_count);
    assert(release.reason == UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS);
    assert(release.flags == config.flags);

    add_reply(&session, 4u, 3u, 60u);
    assert(uwb_clicker_build_range_release(
               &session,
               UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
               &release) == PROTO_ERR_MALFORMED);
}

static void test_clicker_rejects_success_history_overflow_config(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS;
    config.max_attempts = 9u;

    assert(uwb_clicker_session_start(&session, &config) == PROTO_ERR_MALFORMED);
    config = clicker_config();
    config.flags = 0u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_ERR_MALFORMED);
    config.flags = FLAG_COUNT_AS_CLICK | FLAG_GATEWAY_ACK_REQUIRED;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_ERR_MALFORMED);
}

static void test_clicker_serializes_failures_and_retries_without_counting_discovery(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(session.successful_unique_count == 0u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS - 1u,
                                            &schedule) == PROTO_ERR_MALFORMED);
    assert(session.state == UWB_CLICKER_DISCOVERY);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) ==
           PROTO_ERR_BUSY);

    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 1u);
    assert(session.diagnostics.sample_order_count == 1u);
    assert(uwb_clicker_next_range_step(&session, &(struct uwb_range_step){0}) ==
           PROTO_ERR_BUSY);
    assert(session.diagnostics.sample_order_count == 1u);
    {
        struct uwb_range_step bad_step = step;

        bad_step.seq++;
        assert(uwb_clicker_record_range_result(&session, &bad_step, RANGE_RX_TIMEOUT) ==
               PROTO_ERR_MALFORMED);
        bad_step = step;
        bad_step.round_index++;
        assert(uwb_clicker_record_range_result(&session, &bad_step, RANGE_RX_TIMEOUT) ==
               PROTO_ERR_MALFORMED);
    }
    assert(uwb_clicker_record_range_result(&session,
                                           &step,
                                           (enum range_status)(RANGE_TIMING_INVALID + 1u)) ==
           PROTO_ERR_MALFORMED);
    assert(uwb_clicker_record_range_result(&session,
                                           &step,
                                           RANGE_STS_QUALITY_FAIL) ==
           PROTO_ERR_MALFORMED);
    assert(uwb_clicker_next_range_step(&session, &(struct uwb_range_step){0}) ==
           PROTO_ERR_BUSY);
    assert(session.next_sample_index == step.sample_index);
    assert(session.diagnostics.ds_twr_failures == 0u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_RX_TIMEOUT) == PROTO_OK);
    assert(!session.range_step_active);
    assert(session.next_sample_index == step.sample_index + 1u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_RX_TIMEOUT) ==
           PROTO_ERR_MALFORMED);
    assert(session.diagnostics.ds_twr_failures == 1u);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 2u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 3u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 4u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);

    assert(session.successful_unique_count == 3u);
    assert(session.state == UWB_CLICKER_RANGING);

    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 1u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_TIMING_INVALID) == PROTO_OK);
    assert(session.diagnostics.timing_rejections == 1u);

    while (uwb_clicker_next_range_step(&session, &step) == PROTO_OK) {
        assert(step.anchor_id != 1u);
        assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    }
    assert(session.state == UWB_CLICKER_RETRY_WAIT);
    assert(session.successful_unique_count == 3u);

    assert(uwb_clicker_prepare_retry(&session) == PROTO_OK);
    assert(session.attempt_index == 2u);
    add_reply(&session, 5u, 4u, 100u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_ERR_NOT_FOUND);
    add_reply(&session, 6u, 5u, 90u);
    add_reply(&session, 7u, 6u, 80u);
    add_reply(&session, 8u, 7u, 70u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 5u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    assert(session.successful_unique_count == config.min_anchor_count);
    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(session.successful_unique_count == config.min_anchor_count);
    assert(session.diagnostics.retries == 1u);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) ==
           PROTO_ERR_BUSY);
}

static void test_retry_ignores_already_successful_anchors(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = 4u;
    config.samples_per_anchor = 1u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    for (uint8_t i = 0u; i < 3u; i++) {
        assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
        assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    }
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_RX_TIMEOUT) == PROTO_OK);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_ERR_NOT_FOUND);
    assert(session.state == UWB_CLICKER_RETRY_WAIT);
    assert(session.successful_unique_count == 3u);

    assert(uwb_clicker_prepare_retry(&session) == PROTO_OK);
    add_reply(&session, 1u, 0u, 100u);
    add_reply(&session, 2u, 1u, 100u);
    add_reply(&session, 3u, 2u, 100u);
    add_reply(&session, 5u, 4u, 70u);

    assert(session.candidate_count == 1u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_ERR_NOT_FOUND);
    add_reply(&session, 6u, 5u, 60u);
    add_reply(&session, 7u, 6u, 50u);
    add_reply(&session, 8u, 7u, 40u);
    assert(session.candidate_count == config.min_anchor_count);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == config.min_anchor_count);
    assert(schedule.entries[0].anchor_id == 5u);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 5u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(session.successful_unique_count == config.min_anchor_count);
}

static void test_clicker_abort_attempt_clears_active_step_for_retry(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(session.range_step_active);
    assert(session.diagnostics.sample_order_count == 1u);

    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(!session.range_step_active);
    assert(session.state == UWB_CLICKER_RETRY_WAIT);
    assert(session.diagnostics.ds_twr_failures == 0u);
    assert(session.diagnostics.timing_rejections == 0u);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_ERR_BUSY);

    assert(uwb_clicker_prepare_retry(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_POLITENESS);
    assert(session.attempt_index == 2u);
    assert(session.candidate_count == 0u);
    assert(session.diagnostics.retries == 1u);
}

static void test_clicker_abort_scheduled_attempt_before_first_range_for_retry(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(session.state == UWB_CLICKER_RANGING);
    assert(!session.range_step_active);
    assert(session.diagnostics.sample_order_count == 0u);

    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_RETRY_WAIT);
    assert(!session.range_step_active);
    assert(session.diagnostics.ds_twr_failures == 0u);

    assert(uwb_clicker_prepare_retry(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_POLITENESS);
    assert(session.attempt_index == 2u);
    assert(session.candidate_count == 0u);
}

static void test_clicker_abort_after_successes_preserves_completed_ranges_only(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    for (uint8_t i = 0u; i < 3u; i++) {
        assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
        assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    }
    assert(session.successful_unique_count == 3u);
    assert(session.diagnostics.ds_twr_successes == 3u);
    assert(session.diagnostics.ds_twr_failures == 0u);

    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 4u);
    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_RETRY_WAIT);
    assert(session.successful_unique_count == 3u);
    assert(session.diagnostics.ds_twr_successes == 3u);
    assert(session.diagnostics.ds_twr_failures == 0u);

    assert(uwb_clicker_prepare_retry(&session) == PROTO_OK);
    add_reply(&session, 5u, 4u, 100u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_ERR_NOT_FOUND);
    add_reply(&session, 6u, 5u, 90u);
    add_reply(&session, 7u, 6u, 80u);
    add_reply(&session, 8u, 7u, 70u);
    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == config.min_anchor_count);
    assert(schedule.entries[0].anchor_id == 5u);

    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.anchor_id == 5u);
    assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(session.successful_unique_count == config.min_anchor_count);
    assert(session.diagnostics.ds_twr_successes == 4u);
    assert(session.diagnostics.ds_twr_failures == 0u);
    assert(session.diagnostics.retries == 1u);
}

static void test_clicker_abort_after_min_successes_finishes_without_failure(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();

    config.max_anchor_count = 4u;
    config.samples_per_anchor = 2u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session,
                                            900u,
                                            3u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(uwb_range_schedule_total_samples(&schedule) == 8u);

    for (uint8_t i = 0u; i < 4u; i++) {
        assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
        assert(step.round_index == 0u);
        assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
    }

    assert(session.successful_unique_count == config.min_anchor_count);
    assert(session.diagnostics.ds_twr_successes == config.min_anchor_count);
    assert(session.diagnostics.ds_twr_failures == 0u);

    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(step.round_index == 1u);
    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(!session.range_step_active);
    assert(session.successful_unique_count == config.min_anchor_count);
    assert(session.diagnostics.ds_twr_successes == config.min_anchor_count);
    assert(session.diagnostics.ds_twr_failures == 0u);
    assert(session.diagnostics.retries == 0u);
    assert(uwb_clicker_prepare_retry(&session) == PROTO_ERR_BUSY);
}

static void test_clicker_abort_last_attempt_fails_without_counting_ds_twr(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();

    config.max_attempts = 1u;
    config.max_anchor_count = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
    assert(uwb_clicker_abort_attempt(&session) == PROTO_OK);
    assert(!session.range_step_active);
    assert(session.state == UWB_CLICKER_FAILED);
    assert(session.diagnostics.ds_twr_failures == 0u);
    assert(uwb_clicker_prepare_retry(&session) == PROTO_ERR_BUSY);
}

static void test_failing_anchor_is_capped_and_others_continue(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();
    const uint64_t expected_anchors[] = {
        1u, 2u, 3u, 4u,
        1u, 2u, 3u, 4u,
        2u, 3u, 4u,
        2u, 3u, 4u,
    };

    config.min_anchor_count = 3u;
    config.max_anchor_count = 4u;
    config.samples_per_anchor = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, 80u);
    }

    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(uwb_range_schedule_total_samples(&schedule) == 16u);

    for (size_t i = 0u; i < sizeof(expected_anchors) / sizeof(expected_anchors[0]); i++) {
        assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
        assert(step.anchor_id == expected_anchors[i]);
        if (step.anchor_id == 1u) {
            assert(step.round_index < UWB_SESSION_MAX_FAILED_RANGING_PER_ANCHOR);
            assert(uwb_clicker_record_range_result(&session, &step, RANGE_RX_TIMEOUT) ==
                   PROTO_OK);
        } else {
            assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
        }
    }

    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_ERR_NOT_FOUND);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(session.successful_unique_count == config.min_anchor_count);
    assert(session.diagnostics.ds_twr_failures == UWB_SESSION_MAX_FAILED_RANGING_PER_ANCHOR);
    assert(session.diagnostics.ds_twr_successes == 12u);
    assert(session.diagnostics.sample_order_count == 14u);
}

static void test_round_robin_sample_order_completes_before_success(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();
    const uint64_t expected[] = {3u, 1u, 4u, 2u, 3u, 1u, 4u, 2u};
    const uint8_t expected_rounds[] = {0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u};

    config.max_anchor_count = 4u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    add_reply(&session, 1u, 0u, 90u);
    add_reply(&session, 2u, 1u, 60u);
    add_reply(&session, 3u, 2u, 100u);
    add_reply(&session, 4u, 3u, 80u);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) == PROTO_OK);
    assert(schedule.entries[0].anchor_id == 3u);
    assert(schedule.entries[1].anchor_id == 1u);
    assert(schedule.entries[2].anchor_id == 4u);
    assert(schedule.entries[3].anchor_id == 2u);

    for (size_t i = 0u; i < 8u; i++) {
        assert(uwb_clicker_next_range_step(&session, &step) == PROTO_OK);
        assert(step.anchor_id == expected[i]);
        assert(step.round_index == expected_rounds[i]);
        assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
        if (i < 7u) {
            assert(session.state == UWB_CLICKER_RANGING);
        }
    }
    assert(uwb_clicker_next_range_step(&session, &step) == PROTO_ERR_NOT_FOUND);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(session.successful_unique_count == config.min_anchor_count);
    assert(session.diagnostics.sample_order_count == 8u);
}

static void test_six_anchor_single_sample_runs_full_schedule_before_success(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    struct uwb_clicker_config config = clicker_config();
    size_t completed_samples = 0u;

    config.samples_per_anchor = 1u;
    config.max_anchor_count = 6u;
    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 6u; i++) {
        add_reply(&session, (uint64_t)i + 1u, i, (uint8_t)(100u - i));
    }

    assert(uwb_clicker_build_range_schedule(&session,
                                            UWB_DS_TWR_REPLY_DELAY_US,
                                            5u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == 6u);
    assert(uwb_range_schedule_total_samples(&schedule) == 6u);

    while (uwb_clicker_next_range_step(&session, &step) == PROTO_OK) {
        assert(uwb_clicker_record_range_result(&session, &step, RANGE_OK) == PROTO_OK);
        completed_samples++;
        if (completed_samples < 6u) {
            assert(session.state == UWB_CLICKER_RANGING);
        }
    }

    assert(completed_samples == 6u);
    assert(session.state == UWB_CLICKER_SUCCEEDED);
    assert(session.successful_unique_count == 6u);
    assert(session.diagnostics.ds_twr_successes == 6u);
}

static void test_four_anchor_click_uses_shared_200_ms_burst_window(void)
{
    struct uwb_clicker_session clicker;
    struct uwb_anchor_session anchors[4];
    struct uwb_clicker_config clicker_cfg = clicker_config();
    struct uwb_wake_claim_frame claim;
    struct uwb_discover_frame discover;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    const uint64_t anchor_ids[] = {1u, 2u, 3u, 4u};
    uint8_t completed_samples = 0u;

    clicker_cfg.max_anchor_count = 4u;
    clicker_cfg.samples_per_anchor = 1u;
    assert(uwb_clicker_session_start(&clicker, &clicker_cfg) == PROTO_OK);
    assert(uwb_clicker_build_wake_claim(&clicker,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_OK);
    assert(uwb_clicker_build_discover(&clicker, &discover) == PROTO_OK);

    for (uint8_t i = 0u; i < 4u; i++) {
        struct uwb_discovery_reply_frame reply;

        assert(uwb_anchor_session_init(&anchors[i],
                                       &(struct uwb_anchor_config){
                                           .network_id = clicker_cfg.network_id,
                                           .anchor_id = anchor_ids[i],
                                           .anchor_slot = i,
                                           .wake_channel = UWB_CHANNEL_WAKE_CONTACT,
                                           .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
                                       }) == PROTO_OK);
        assert(uwb_anchor_accept_wake_claim(&anchors[i], &claim, 1000u, NULL) ==
               PROTO_OK);
        assert(uwb_anchor_build_discovery_reply(&anchors[i],
                                                &discover,
                                                (uint8_t)(90u - i),
                                                3000u,
                                                &reply) == PROTO_OK);
        assert(uwb_clicker_note_discovery_reply(&clicker, &reply) == PROTO_OK);
    }

    assert(uwb_clicker_build_range_schedule(&clicker,
                                            UWB_DS_TWR_REPLY_DELAY_US,
                                            5u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == 4u);
    assert(schedule.samples_per_anchor == 1u);
    assert(schedule.sts_mode == UWB_RANGE_SCHEDULE_STS_DISABLED);
    assert(schedule.diagnostics_required == UWB_RANGE_SCHEDULE_DIAGNOSTICS_REQUIRED);
    assert(schedule.exchange_stride_us == UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US);
    assert(schedule.max_exchanges == 4u);
    assert(schedule.burst_window_ms == UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS);

    for (uint8_t i = 0u; i < 4u; i++) {
        assert(uwb_anchor_accept_range_schedule(&anchors[i], &schedule, 1200u, 10u) ==
               PROTO_OK);
        assert(anchors[i].uwb_wait_deadline_ms ==
               1200u + schedule.first_poll_delay_ms + schedule.burst_window_ms + 10u);
    }

    while (uwb_clicker_next_range_step(&clicker, &step) == PROTO_OK) {
        struct uwb_range_exchange_identity identity = {
            .network_id = clicker_cfg.network_id,
            .clicker_id = clicker_cfg.clicker_id,
            .click_event_id = clicker_cfg.click_event_id,
            .attempt_index = clicker.attempt_index,
            .nonce = clicker_cfg.nonce,
            .anchor_id = step.anchor_id,
            .ranging_channel = UWB_CHANNEL_WAKE_CONTACT,
            .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
            .seq = step.seq,
            .flags = clicker_cfg.flags,
        };
        uint8_t accepted = 0u;

        for (uint8_t i = 0u; i < 4u; i++) {
            if (uwb_anchor_accepts_range_exchange(&anchors[i], &identity)) {
                accepted++;
                assert(uwb_anchor_note_range_result(&anchors[i], RANGE_OK) == PROTO_OK);
                uwb_anchor_note_sample_order(&anchors[i]);
            }
        }
        assert(accepted == 1u);
        assert(uwb_clicker_record_range_result(&clicker, &step, RANGE_OK) == PROTO_OK);
        completed_samples++;
    }

    assert(completed_samples == 4u);
    assert(clicker.state == UWB_CLICKER_SUCCEEDED);
    assert(clicker.successful_unique_count == 4u);
}

static void test_complete_four_anchor_click_flow_reports_after_selected_ds_twr(void)
{
    struct uwb_clicker_session clicker;
    struct uwb_anchor_session anchors[4];
    struct uwb_clicker_config clicker_cfg = clicker_config();
    const uint64_t anchor_ids[] = {
        UINT64_C(0xAA00000000000001),
        UINT64_C(0xAA00000000000002),
        UINT64_C(0xAA00000000000003),
        UINT64_C(0xAA00000000000004),
    };
    uint8_t accepted_samples[4] = {0};
    struct uwb_wake_claim_frame claim;
    struct uwb_discover_frame discover;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    size_t completed_samples = 0u;

    clicker_cfg.max_anchor_count = 4u;
    clicker_cfg.samples_per_anchor = 2u;
    assert(uwb_clicker_session_start(&clicker, &clicker_cfg) == PROTO_OK);
    assert(uwb_clicker_build_wake_claim(&clicker,
                                        UINT64_C(0x0102030405060708),
                                        430u,
                                        430u,
                                        1365u,
                                        &claim) == PROTO_OK);

    for (uint8_t i = 0u; i < 4u; i++) {
        struct uwb_anchor_config anchor_cfg = anchor_config(anchor_ids[i], i);
        enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_REJECTED_BUSY;

        assert(uwb_anchor_session_init(&anchors[i], &anchor_cfg) == PROTO_OK);
        assert(uwb_anchor_accept_wake_claim(&anchors[i], &claim, 1000u, &decision) ==
               PROTO_OK);
        assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
        assert(anchors[i].epoch.clicker_id == clicker_cfg.clicker_id);
        assert(anchors[i].epoch.click_event_id == clicker_cfg.click_event_id);
    }

    assert(uwb_clicker_build_discover(&clicker, &discover) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        struct uwb_discovery_reply_frame reply;

        assert(uwb_anchor_build_discovery_reply(&anchors[i],
                                                &discover,
                                                (uint8_t)(80u + i),
                                                3000u,
                                                &reply) == PROTO_OK);
        assert(reply.selected_clicker_id == clicker_cfg.clicker_id);
        assert(reply.click_event_id == clicker_cfg.click_event_id);
        assert(uwb_clicker_note_discovery_reply(&clicker, &reply) == PROTO_OK);
    }

    assert(clicker.candidate_count == 4u);
    assert(clicker.successful_unique_count == 0u);
    assert(uwb_clicker_build_range_schedule(&clicker,
                                            UWB_DS_TWR_REPLY_DELAY_US,
                                            5u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == 4u);
    assert(uwb_range_schedule_total_samples(&schedule) == 8u);

    for (uint8_t i = 0u; i < 4u; i++) {
        assert(uwb_anchor_accept_range_schedule(&anchors[i], &schedule, 1200u, 10u) ==
               PROTO_OK);
        assert(anchors[i].scheduled);
        assert(anchors[i].schedule_entry.anchor_id == anchors[i].config.anchor_id);
    }

    while (uwb_clicker_next_range_step(&clicker, &step) == PROTO_OK) {
        struct uwb_range_exchange_identity identity = {
            .network_id = clicker_cfg.network_id,
            .clicker_id = clicker_cfg.clicker_id,
            .click_event_id = clicker_cfg.click_event_id,
            .attempt_index = clicker.attempt_index,
            .nonce = clicker_cfg.nonce,
            .anchor_id = step.anchor_id,
            .ranging_channel = clicker_cfg.ranging_channel,
            .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
            .seq = step.seq,
            .flags = clicker_cfg.flags,
        };
        uint8_t accept_count = 0u;
        uint8_t selected_anchor = 0xffu;

        for (uint8_t i = 0u; i < 4u; i++) {
            if (uwb_anchor_accepts_range_exchange(&anchors[i], &identity)) {
                accept_count++;
                selected_anchor = i;
            }
        }
        assert(accept_count == 1u);
        assert(selected_anchor < 4u);
        assert(anchors[selected_anchor].config.anchor_id == step.anchor_id);
        assert(uwb_anchor_note_range_result(&anchors[selected_anchor], RANGE_OK) ==
               PROTO_OK);
        uwb_anchor_note_sample_order(&anchors[selected_anchor]);
        accepted_samples[selected_anchor]++;
        completed_samples++;
        assert(uwb_clicker_record_range_result(&clicker, &step, RANGE_OK) == PROTO_OK);
    }

    assert(clicker.state == UWB_CLICKER_SUCCEEDED);
    assert(clicker.successful_unique_count == clicker_cfg.min_anchor_count);
    assert(clicker.diagnostics.ds_twr_successes == 8u);
    assert(clicker.diagnostics.ds_twr_failures == 0u);
    assert(clicker.diagnostics.sample_order_count == 8u);
    assert(completed_samples == 8u);

    for (uint8_t i = 0u; i < 4u; i++) {
        uint8_t payload[96];
        size_t payload_len = 0u;
        struct proto_packet packet;
        struct range_report_fields fields = {
            .clicker_id = clicker_cfg.clicker_id,
            .anchor_id = anchors[i].config.anchor_id,
            .event_seq = clicker_cfg.click_event_id,
            .timestamp_ms = 2000u + i,
            .distance_mm = 1000 + i,
            .quality = 90u,
            .range_status = RANGE_OK,
            .omit_rsl = true,
            .omit_cir = true,
        };

        assert(accepted_samples[i] == clicker_cfg.samples_per_anchor);
        assert(anchors[i].diagnostics.claims == 1u);
        assert(anchors[i].diagnostics.discovery_replies == 1u);
        assert(anchors[i].diagnostics.schedules == 1u);
        assert(anchors[i].diagnostics.ds_twr_successes == clicker_cfg.samples_per_anchor);
        assert(anchors[i].diagnostics.ds_twr_failures == 0u);
        assert(anchors[i].diagnostics.sample_order_count == clicker_cfg.samples_per_anchor);
        assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) ==
               PROTO_OK);
        assert(report_init_click_packet(&packet,
                                        fields.anchor_id,
                                        UINT64_C(0x9999888877776666),
                                        clicker_cfg.click_event_id,
                                        (uint16_t)(i + 1u),
                                        (uint8_t)payload_len) == PROTO_OK);
        assert(packet.msg_type == MSG_CLICK_REPORT);
        assert((packet.flags & FLAG_COUNT_AS_CLICK) != 0u);
        assert((packet.flags & FLAG_DIAGNOSTIC) == 0u);
        assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    }
}

static void test_four_simultaneous_clickers_converge_across_six_anchors(void)
{
    struct uwb_clicker_session clickers[4];
    struct uwb_clicker_config clicker_cfgs[4];
    struct uwb_wake_claim_frame claims[4];
    struct uwb_anchor_session anchors[UWB_RANGE_SCHEDULE_MAX_ANCHORS];
    struct uwb_discover_frame winner_discover;
    struct uwb_range_schedule_frame schedule;
    struct uwb_range_step step;
    uint8_t samples_per_anchor[UWB_RANGE_SCHEDULE_MAX_ANCHORS] = {0};
    const uint8_t winner = 3u;

    for (uint8_t i = 0u; i < 4u; i++) {
        clicker_cfgs[i] = clicker_config();
        clicker_cfgs[i].clicker_id = UINT64_C(0x4400000000000000) + i + 1u;
        clicker_cfgs[i].click_event_id = 400u + i;
        clicker_cfgs[i].nonce = UINT64_C(0x6600000000000000) + i + 1u;
        clicker_cfgs[i].max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS;
        clicker_cfgs[i].samples_per_anchor = 2u;
        assert(uwb_clicker_session_start(&clickers[i], &clicker_cfgs[i]) == PROTO_OK);
        assert(uwb_clicker_build_wake_claim(&clickers[i],
                                            4u - i,
                                            430u,
                                            430u,
                                            1365u,
                                            &claims[i]) == PROTO_OK);
    }

    for (uint8_t anchor_index = 0u; anchor_index < UWB_RANGE_SCHEDULE_MAX_ANCHORS; anchor_index++) {
        struct uwb_anchor_config anchor_cfg =
            anchor_config(UINT64_C(0xAA55000000000000) + anchor_index + 1u,
                          anchor_index);

        assert(uwb_anchor_session_init(&anchors[anchor_index], &anchor_cfg) == PROTO_OK);
        for (uint8_t clicker_index = 0u; clicker_index < 4u; clicker_index++) {
            enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_REJECTED_BUSY;

            assert(uwb_anchor_accept_wake_claim(&anchors[anchor_index],
                                                &claims[clicker_index],
                                                1000u + clicker_index,
                                                &decision) == PROTO_OK);
        }

        assert(anchors[anchor_index].epoch.clicker_id == clicker_cfgs[winner].clicker_id);
        assert(anchors[anchor_index].epoch.click_event_id ==
               clicker_cfgs[winner].click_event_id);
        assert(anchors[anchor_index].epoch.nonce == clicker_cfgs[winner].nonce);
        assert(anchors[anchor_index].diagnostics.claims == 4u);
        assert(anchors[anchor_index].diagnostics.collisions == 3u);
        assert(anchors[anchor_index].diagnostics.arbitration_wins == 4u);
        assert(anchors[anchor_index].diagnostics.arbitration_losses == 0u);
    }

    for (uint8_t loser = 0u; loser < winner; loser++) {
        struct uwb_discover_frame loser_discover;
        struct uwb_discovery_reply_frame reply;

        assert(uwb_clicker_build_discover(&clickers[loser], &loser_discover) == PROTO_OK);
        assert(uwb_anchor_build_discovery_reply(&anchors[0],
                                                &loser_discover,
                                                90u,
                                                3000u,
                                                &reply) == PROTO_ERR_MALFORMED);
        assert(uwb_clicker_build_range_schedule(&clickers[loser],
                                                UWB_DS_TWR_REPLY_DELAY_US,
                                                5u,
                                                UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                                &schedule) == PROTO_ERR_NOT_FOUND);
        assert(uwb_clicker_prepare_retry(&clickers[loser]) == PROTO_OK);
        assert(clickers[loser].attempt_index == 2u);
        assert(clickers[loser].diagnostics.retries == 1u);
    }

    assert(uwb_clicker_build_discover(&clickers[winner], &winner_discover) == PROTO_OK);
    for (uint8_t anchor_index = 0u; anchor_index < UWB_RANGE_SCHEDULE_MAX_ANCHORS; anchor_index++) {
        struct uwb_discovery_reply_frame reply;

        assert(uwb_anchor_build_discovery_reply(&anchors[anchor_index],
                                                &winner_discover,
                                                (uint8_t)(70u + anchor_index),
                                                3000u,
                                                &reply) == PROTO_OK);
        assert(reply.selected_clicker_id == clicker_cfgs[winner].clicker_id);
        assert(uwb_clicker_note_discovery_reply(&clickers[winner], &reply) == PROTO_OK);
    }

    assert(clickers[winner].candidate_count == UWB_RANGE_SCHEDULE_MAX_ANCHORS);
    assert(uwb_clicker_build_range_schedule(&clickers[winner],
                                            UWB_DS_TWR_REPLY_DELAY_US,
                                            5u,
                                            UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS,
                                            &schedule) == PROTO_OK);
    assert(schedule.selected_count == UWB_RANGE_SCHEDULE_MAX_ANCHORS);
    assert(uwb_range_schedule_total_samples(&schedule) ==
           UWB_RANGE_SCHEDULE_MAX_ANCHORS * clicker_cfgs[winner].samples_per_anchor);

    for (uint8_t anchor_index = 0u; anchor_index < UWB_RANGE_SCHEDULE_MAX_ANCHORS; anchor_index++) {
        assert(uwb_anchor_accept_range_schedule(&anchors[anchor_index],
                                                &schedule,
                                                1200u,
                                                10u) == PROTO_OK);
    }

    while (uwb_clicker_next_range_step(&clickers[winner], &step) == PROTO_OK) {
        struct uwb_range_exchange_identity winner_identity = {
            .network_id = clicker_cfgs[winner].network_id,
            .clicker_id = clicker_cfgs[winner].clicker_id,
            .click_event_id = clicker_cfgs[winner].click_event_id,
            .attempt_index = clickers[winner].attempt_index,
            .nonce = clicker_cfgs[winner].nonce,
            .anchor_id = step.anchor_id,
            .ranging_channel = clicker_cfgs[winner].ranging_channel,
            .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
            .seq = step.seq,
            .flags = clicker_cfgs[winner].flags,
        };
        struct uwb_range_exchange_identity hidden_identity = winner_identity;
        uint8_t accepting_anchor = 0xffu;
        uint8_t accept_count = 0u;

        hidden_identity.clicker_id = clicker_cfgs[0].clicker_id;
        hidden_identity.click_event_id = clicker_cfgs[0].click_event_id;
        hidden_identity.nonce = clicker_cfgs[0].nonce;

        for (uint8_t anchor_index = 0u; anchor_index < UWB_RANGE_SCHEDULE_MAX_ANCHORS; anchor_index++) {
            assert(!uwb_anchor_accepts_range_exchange(&anchors[anchor_index],
                                                      &hidden_identity));
            if (uwb_anchor_accepts_range_exchange(&anchors[anchor_index],
                                                  &winner_identity)) {
                accepting_anchor = anchor_index;
                accept_count++;
            }
        }

        assert(accept_count == 1u);
        assert(accepting_anchor < UWB_RANGE_SCHEDULE_MAX_ANCHORS);
        assert(anchors[accepting_anchor].config.anchor_id == step.anchor_id);
        assert(uwb_anchor_note_range_result(&anchors[accepting_anchor], RANGE_OK) ==
               PROTO_OK);
        uwb_anchor_note_sample_order(&anchors[accepting_anchor]);
        samples_per_anchor[accepting_anchor]++;
        assert(uwb_clicker_record_range_result(&clickers[winner], &step, RANGE_OK) ==
               PROTO_OK);
    }

    assert(clickers[winner].state == UWB_CLICKER_SUCCEEDED);
    assert(clickers[winner].successful_unique_count == UWB_RANGE_SCHEDULE_MAX_ANCHORS);
    assert(clickers[winner].diagnostics.ds_twr_successes ==
           UWB_RANGE_SCHEDULE_MAX_ANCHORS * clicker_cfgs[winner].samples_per_anchor);
    assert(clickers[winner].diagnostics.ds_twr_failures == 0u);
    assert(clickers[winner].diagnostics.sample_order_count ==
           UWB_RANGE_SCHEDULE_MAX_ANCHORS * clicker_cfgs[winner].samples_per_anchor);

    for (uint8_t anchor_index = 0u; anchor_index < UWB_RANGE_SCHEDULE_MAX_ANCHORS; anchor_index++) {
        assert(samples_per_anchor[anchor_index] == 2u);
        assert(anchors[anchor_index].diagnostics.discovery_replies == 1u);
        assert(anchors[anchor_index].diagnostics.schedules == 1u);
        assert(anchors[anchor_index].diagnostics.ds_twr_successes == 2u);
        assert(anchors[anchor_index].diagnostics.ds_twr_failures == 0u);
        assert(anchors[anchor_index].diagnostics.sample_order_count == 2u);
    }
}

static struct uwb_wake_claim_frame claim_for(uint64_t clicker_id,
                                             uint32_t event_id,
                                             uint64_t priority_id)
{
    const struct uwb_wake_claim_frame claim = {
        .network_id = 0x494D4543u,
        .clicker_id = clicker_id,
        .click_event_id = event_id,
        .attempt_index = 1u,
        .priority_id = priority_id,
        .wake_channel = 5u,
        .ranging_channel = 5u,
        .wake_train_ends_in_ms = 120u,
        .discovery_starts_in_ms = 150u,
        .claimed_duration_ms = 400u,
        .min_anchor_count = 4u,
        .max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS,
        .nonce = UINT64_C(0xCAFEBABE00000000) + clicker_id,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    return claim;
}

static void test_anchor_bad_first_wake_claim_does_not_create_epoch(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(UINT64_C(0xAA00000000000003), 3u);
    struct uwb_wake_claim_frame claim =
        claim_for(UINT64_C(0x1000000000000001), 101u, 10u);
    struct uwb_wake_claim_frame decoded = {0};
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) == PROTO_OK);
    assert(frame_len == UWB_WAKE_CLAIM_LEN);

    frame[12] ^= 0x01u;
    assert(uwb_decode_wake_claim(frame, frame_len, &decoded) == PROTO_ERR_BAD_CRC);
    uwb_anchor_note_wake_decode_failure(&anchor, UWB_WAKE_DECODE_CRC_FAILURE);
    assert(!anchor.epoch.active);
    assert(anchor.state == UWB_ANCHOR_IDLE);
    assert(anchor.diagnostics.crc_failures == 1u);
    assert(anchor.diagnostics.claims == 0u);
    assert(anchor.diagnostics.arbitration_wins == 0u);
    assert(anchor.diagnostics.collisions == 0u);

    claim.flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK;
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(!anchor.epoch.active);
    assert(anchor.state == UWB_ANCHOR_IDLE);
    assert(anchor.diagnostics.claims == 0u);
    assert(anchor.diagnostics.arbitration_wins == 0u);
}

static void test_anchor_arbitrates_four_clickers_and_rejects_hidden_ds_twr(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(UINT64_C(0xAA00000000000003), 3u);
    struct uwb_discover_frame discover;
    struct uwb_discovery_reply_frame discovery_reply;
    struct uwb_range_schedule_frame schedule = {0};
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    for (uint8_t i = 0u; i < 4u; i++) {
        struct uwb_wake_claim_frame claim =
            claim_for(UINT64_C(0x1000000000000000) + i, 100u + i, 4u - i);

        assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u + i, &decision) ==
               PROTO_OK);
    }
    assert(anchor.epoch.priority_id == 1u);
    assert(anchor.epoch.clicker_id == UINT64_C(0x1000000000000003));
    assert(anchor.epoch.flags == FLAG_COUNT_AS_CLICK);
    assert(anchor.diagnostics.claims == 4u);
    assert(anchor.diagnostics.collisions == 3u);
    assert(anchor.diagnostics.arbitration_wins == 4u);
    assert(anchor.diagnostics.arbitration_losses == 0u);

    discover.network_id = config.network_id;
    discover.clicker_id = anchor.epoch.clicker_id;
    discover.click_event_id = anchor.epoch.click_event_id;
    discover.attempt_index = anchor.epoch.attempt_index;
    discover.nonce = anchor.epoch.nonce;
    discover.discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT;
    discover.flags = FLAG_COUNT_AS_CLICK;
    assert(uwb_anchor_build_discovery_reply(&anchor,
                                            &discover,
                                            90u,
                                            2990u,
                                            &discovery_reply) == PROTO_OK);
    assert(discovery_reply.selected_clicker_id == anchor.epoch.clicker_id);
    assert(discovery_reply.click_event_id == anchor.epoch.click_event_id);

    discover.flags = FLAG_DIAGNOSTIC;
    assert(uwb_anchor_build_discovery_reply(&anchor,
                                            &discover,
                                            90u,
                                            2990u,
                                            &discovery_reply) == PROTO_ERR_MALFORMED);
    discover.flags = FLAG_COUNT_AS_CLICK;

    schedule.network_id = config.network_id;
    schedule.clicker_id = anchor.epoch.clicker_id;
    schedule.click_event_id = anchor.epoch.click_event_id;
    schedule.attempt_index = anchor.epoch.attempt_index;
    schedule.nonce = anchor.epoch.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = 900u;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = FLAG_COUNT_AS_CLICK;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 7u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    schedule.flags = FLAG_DIAGNOSTIC;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_MALFORMED);
    schedule.flags = FLAG_COUNT_AS_CLICK;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) == PROTO_OK);

    assert(uwb_anchor_accepts_range_exchange(
        &anchor,
        &(struct uwb_range_exchange_identity){
            .network_id = config.network_id,
            .clicker_id = anchor.epoch.clicker_id,
            .click_event_id = anchor.epoch.click_event_id,
            .attempt_index = anchor.epoch.attempt_index,
            .nonce = anchor.epoch.nonce,
            .anchor_id = config.anchor_id,
            .ranging_channel = config.ranging_channel,
            .reply_delay_us = 900u,
            .seq = 7u,
            .flags = FLAG_COUNT_AS_CLICK,
        }));
    assert(!uwb_anchor_accepts_range_exchange(
        &anchor,
        &(struct uwb_range_exchange_identity){
            .network_id = config.network_id,
            .clicker_id = anchor.epoch.clicker_id,
            .click_event_id = anchor.epoch.click_event_id,
            .attempt_index = anchor.epoch.attempt_index,
            .nonce = anchor.epoch.nonce,
            .anchor_id = config.anchor_id,
            .ranging_channel = config.ranging_channel,
            .reply_delay_us = 900u,
            .seq = 7u,
            .flags = FLAG_DIAGNOSTIC,
        }));
    assert(!uwb_anchor_accepts_range_exchange(
        &anchor,
        &(struct uwb_range_exchange_identity){
            .network_id = config.network_id,
            .clicker_id = UINT64_C(0x1000000000000001),
            .click_event_id = 101u,
            .attempt_index = 1u,
            .nonce = UINT64_C(0xCAFEBABE00000001),
            .anchor_id = config.anchor_id,
            .ranging_channel = config.ranging_channel,
            .reply_delay_us = 900u,
            .seq = 7u,
            .flags = FLAG_COUNT_AS_CLICK,
        }));
}

static void test_anchor_rejects_foreign_claim_without_clearing_epoch(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(UINT64_C(0xAA00000000000003), 3u);
    struct uwb_wake_claim_frame accepted =
        claim_for(UINT64_C(0x1000000000000001), 101u, 10u);
    struct uwb_wake_claim_frame foreign = accepted;
    struct uwb_wake_claim_frame malformed = accepted;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &accepted, 1000u, &decision) ==
           PROTO_OK);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.state == UWB_ANCHOR_CLAIMED);

    foreign.network_id = accepted.network_id + 1u;
    assert(uwb_anchor_accept_wake_claim(&anchor, &foreign, 1001u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.epoch.click_event_id == accepted.click_event_id);
    assert(anchor.state == UWB_ANCHOR_CLAIMED);

    foreign = accepted;
    foreign.ranging_channel = accepted.ranging_channel + 1u;
    assert(uwb_anchor_accept_wake_claim(&anchor, &foreign, 1002u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);

    malformed.flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK;
    assert(uwb_anchor_accept_wake_claim(&anchor, &malformed, 1003u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.diagnostics.claims == 1u);
    assert(anchor.diagnostics.collisions == 0u);
    assert(anchor.diagnostics.arbitration_losses == 0u);

    foreign = accepted;
    foreign.clicker_id = accepted.clicker_id + 1u;
    foreign.click_event_id = accepted.click_event_id + 1u;
    foreign.priority_id = accepted.priority_id + 1u;
    foreign.nonce = accepted.nonce + 1u;
    assert(uwb_anchor_accept_wake_claim(&anchor, &foreign, 1004u, &decision) ==
           PROTO_ERR_BUSY);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.diagnostics.claims == 1u);
    assert(anchor.diagnostics.collisions == 1u);
    assert(anchor.diagnostics.arbitration_losses == 1u);
}

static void test_anchor_accepts_newer_retry_claim_for_same_event(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(UINT64_C(0xAA00000000000003), 3u);
    struct uwb_wake_claim_frame accepted =
        claim_for(UINT64_C(0x1000000000000001), 101u, 10u);
    struct uwb_wake_claim_frame retry = accepted;
    struct uwb_wake_claim_frame stale = accepted;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_REJECTED_BUSY;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &accepted, 1000u, &decision) ==
           PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);

    retry.attempt_index = (uint8_t)(accepted.attempt_index + 1u);
    retry.priority_id = accepted.priority_id + 100u;
    assert(uwb_anchor_accept_wake_claim(&anchor, &retry, 1010u, &decision) ==
           PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.epoch.click_event_id == accepted.click_event_id);
    assert(anchor.epoch.attempt_index == retry.attempt_index);
    assert(anchor.epoch.priority_id == retry.priority_id);

    assert(uwb_anchor_accept_wake_claim(&anchor, &stale, 1020u, &decision) ==
           PROTO_ERR_STALE);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_STALE);
    assert(anchor.epoch.attempt_index == retry.attempt_index);
    assert(anchor.epoch.priority_id == retry.priority_id);
    assert(anchor.diagnostics.claims == 2u);
    assert(anchor.diagnostics.collisions == 0u);
    assert(anchor.diagnostics.arbitration_losses == 0u);
}

static void test_anchor_epoch_expiry_handles_ms_wrap(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(UINT64_C(0xAA00000000000004), 4u);
    struct uwb_wake_claim_frame accepted =
        claim_for(UINT64_C(0x1000000000000001), 101u, 10u);
    struct uwb_wake_claim_frame foreign =
        claim_for(UINT64_C(0x1000000000000002), 102u, 20u);
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    const uint32_t start_ms = UINT32_MAX - 50u;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &accepted, start_ms, &decision) ==
           PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(anchor.epoch.epoch_ends_at_ms == 349u);

    assert(uwb_anchor_accept_wake_claim(&anchor, &foreign, start_ms + 10u, &decision) ==
           PROTO_ERR_BUSY);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);

    assert(uwb_anchor_accept_wake_claim(&anchor, &foreign, 350u, &decision) ==
           PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == foreign.clicker_id);
}

static void test_anchor_rejects_same_event_mismatched_session_identity(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(UINT64_C(0xAA00000000000003), 3u);
    struct uwb_wake_claim_frame accepted =
        claim_for(UINT64_C(0x1000000000000001), 101u, 10u);
    struct uwb_wake_claim_frame changed = accepted;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &accepted, 1000u, &decision) ==
           PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);

    changed.priority_id = accepted.priority_id - 1u;
    assert(uwb_anchor_accept_wake_claim(&anchor, &changed, 1005u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.epoch.click_event_id == accepted.click_event_id);
    assert(anchor.epoch.attempt_index == accepted.attempt_index);
    assert(anchor.epoch.priority_id == accepted.priority_id);
    assert(anchor.epoch.nonce == accepted.nonce);
    assert(anchor.epoch.flags == accepted.flags);

    changed = accepted;
    changed.attempt_index = (uint8_t)(accepted.attempt_index + 1u);
    changed.priority_id = accepted.priority_id - 1u;
    changed.nonce = accepted.nonce + 1u;
    assert(uwb_anchor_accept_wake_claim(&anchor, &changed, 1010u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(anchor.epoch.active);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.epoch.click_event_id == accepted.click_event_id);
    assert(anchor.epoch.attempt_index == accepted.attempt_index);
    assert(anchor.epoch.priority_id == accepted.priority_id);
    assert(anchor.epoch.nonce == accepted.nonce);
    assert(anchor.epoch.flags == accepted.flags);

    changed = accepted;
    changed.attempt_index = (uint8_t)(accepted.attempt_index + 1u);
    changed.priority_id = accepted.priority_id - 1u;
    changed.flags = FLAG_DIAGNOSTIC;
    assert(uwb_anchor_accept_wake_claim(&anchor, &changed, 1020u, &decision) ==
           PROTO_ERR_MALFORMED);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_MALFORMED);
    assert(anchor.epoch.attempt_index == accepted.attempt_index);
    assert(anchor.epoch.priority_id == accepted.priority_id);
    assert(anchor.epoch.nonce == accepted.nonce);
    assert(anchor.epoch.flags == accepted.flags);
    assert(anchor.diagnostics.claims == 1u);
    assert(anchor.diagnostics.collisions == 0u);
    assert(anchor.diagnostics.arbitration_losses == 0u);
}

static void test_anchor_schedule_validation_and_presence_only_discovery(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(42u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_discover_frame discover = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT,
        .flags = claim.flags,
    };
    struct uwb_discovery_reply_frame reply;
    struct uwb_range_schedule_frame schedule = {0};

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u, NULL) == PROTO_OK);

    discover.discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT + 1u;
    assert(uwb_anchor_build_discovery_reply(&anchor, &discover, 77u, 3010u, &reply) ==
           PROTO_ERR_MALFORMED);
    assert(anchor.diagnostics.discovery_replies == 0u);
    discover.discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT;

    assert(uwb_anchor_build_discovery_reply(&anchor, &discover, 77u, 3010u, &reply) ==
           PROTO_OK);
    assert(reply.status == UWB_DISCOVERY_REPLY_PRESENT);
    assert(anchor.diagnostics.ds_twr_successes == 0u);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = 9u;
    schedule.reply_delay_us = 900u;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 1u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    schedule.ranging_channel = config.ranging_channel;
    schedule.samples_per_anchor = 2u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_MALFORMED);
    schedule.samples_per_anchor = 1u;

    schedule.ranging_channel = 9u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_MALFORMED);

    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US + 1u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_MALFORMED);

    schedule.reply_delay_us = 900u;
    schedule.flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_MALFORMED);

    schedule.flags = claim.flags;
    schedule.samples_per_anchor = 3u;
    schedule.entries[0].seq = 254u;
    schedule.entries[0].sample_count = 3u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_MALFORMED);

    schedule.samples_per_anchor = 1u;
    schedule.entries[0].seq = 1u;
    schedule.entries[0].sample_count = 1u;
    schedule.entries[0].anchor_id = 99u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) ==
           PROTO_ERR_NOT_FOUND);
    assert(anchor.state == UWB_ANCHOR_IDLE);
    assert(!anchor.epoch.active);
    assert(!anchor.scheduled);
    assert(anchor.schedule_entry.anchor_id == 0u);
    assert(anchor.reply_delay_us == 0u);
    assert(anchor.expected_ranging_channel == 0u);
    assert(anchor.uwb_wait_deadline_ms == 0u);
}

static void test_anchor_accepts_range_release_after_discovery_reply(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(42u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_range_release_frame release = {
        .network_id = claim.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .discovered_anchor_count = 3u,
        .min_anchor_count = 4u,
        .reason = UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS,
        .flags = claim.flags,
    };

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_range_release(&anchor, &release) == PROTO_ERR_BUSY);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u, NULL) == PROTO_OK);
    anchor_mark_discovery_replied(&anchor);
    assert(anchor.state == UWB_ANCHOR_DISCOVERY_REPLIED);
    assert(anchor.epoch.active);

    release.flags = FLAG_DIAGNOSTIC;
    assert(uwb_anchor_accept_range_release(&anchor, &release) == PROTO_ERR_MALFORMED);
    assert(anchor.state == UWB_ANCHOR_DISCOVERY_REPLIED);
    assert(anchor.epoch.active);

    release.flags = claim.flags;
    assert(uwb_anchor_accept_range_release(&anchor, &release) == PROTO_OK);
    assert(anchor.state == UWB_ANCHOR_IDLE);
    assert(!anchor.epoch.active);
    assert(!anchor.scheduled);
    assert(anchor.schedule_entry.anchor_id == 0u);
    assert(anchor.reply_delay_us == 0u);
    assert(anchor.expected_ranging_channel == 0u);
    assert(anchor.uwb_wait_deadline_ms == 0u);
}

static void test_anchor_rejects_out_of_schedule_range_exchange_identity(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(42u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_range_schedule_frame schedule = {0};
    struct uwb_range_exchange_identity identity;
    uint8_t round_index = 0xffu;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u, NULL) == PROTO_OK);
    anchor_mark_discovery_replied(&anchor);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 2u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 7u;
    schedule.entries[0].sample_count = 2u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) == PROTO_OK);
    identity = (struct uwb_range_exchange_identity){
        .network_id = config.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_id = config.anchor_id,
        .ranging_channel = config.ranging_channel,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .seq = 7u,
        .flags = claim.flags,
    };

    assert(uwb_anchor_accepts_range_exchange(&anchor, &identity));
    assert(uwb_anchor_range_round_index(&anchor, &identity, &round_index) == PROTO_OK);
    assert(round_index == 0u);
    identity.network_id = config.network_id + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.network_id = config.network_id;
    identity.clicker_id = claim.clicker_id + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.clicker_id = claim.clicker_id;
    identity.click_event_id = claim.click_event_id + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.click_event_id = claim.click_event_id;
    identity.attempt_index = claim.attempt_index + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.attempt_index = claim.attempt_index;
    identity.nonce = claim.nonce + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.nonce = claim.nonce;
    identity.flags = FLAG_DIAGNOSTIC;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.flags = claim.flags;
    identity.seq = 8u;
    assert(uwb_anchor_accepts_range_exchange(&anchor, &identity));
    assert(uwb_anchor_range_round_index(&anchor, &identity, &round_index) == PROTO_OK);
    assert(round_index == 1u);
    identity.seq = 6u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.seq = 9u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.seq = 7u;
    identity.anchor_id = config.anchor_id + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.anchor_id = config.anchor_id;
    identity.ranging_channel = config.ranging_channel + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    identity.ranging_channel = config.ranging_channel;
    identity.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US + 1u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
}

static void test_anchor_accepts_schedule_sequence_range_ending_at_255(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(42u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_range_schedule_frame schedule = {0};
    struct uwb_range_exchange_identity identity;
    uint8_t round_index = 0xffu;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u, NULL) == PROTO_OK);
    anchor_mark_discovery_replied(&anchor);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 2u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 254u;
    schedule.entries[0].sample_count = 2u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) == PROTO_OK);
    identity = (struct uwb_range_exchange_identity){
        .network_id = config.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_id = config.anchor_id,
        .ranging_channel = config.ranging_channel,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .seq = 254u,
        .flags = claim.flags,
    };

    assert(uwb_anchor_accepts_range_exchange(&anchor, &identity));
    assert(uwb_anchor_range_round_index(&anchor, &identity, &round_index) == PROTO_OK);
    assert(round_index == 0u);
    identity.seq = 255u;
    assert(uwb_anchor_accepts_range_exchange(&anchor, &identity));
    assert(uwb_anchor_range_round_index(&anchor, &identity, &round_index) == PROTO_OK);
    assert(round_index == 1u);
    identity.seq = 253u;
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
}

static void test_anchor_rejects_duplicate_discover_after_schedule(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(42u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_discover_frame discover = {0};
    struct uwb_discovery_reply_frame reply = {0};
    struct uwb_range_schedule_frame schedule = {0};

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1000u, NULL) == PROTO_OK);

    discover.network_id = claim.network_id;
    discover.clicker_id = claim.clicker_id;
    discover.click_event_id = claim.click_event_id;
    discover.attempt_index = claim.attempt_index;
    discover.nonce = claim.nonce;
    discover.discovery_slot_count = UWB_DISCOVERY_SLOT_COUNT;
    discover.flags = claim.flags;
    assert(uwb_anchor_build_discovery_reply(&anchor, &discover, 80u, 3000u, &reply) ==
           PROTO_OK);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 9u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) == PROTO_OK);
    assert(anchor.state == UWB_ANCHOR_SCHEDULED);
    assert(anchor.scheduled);

    assert(uwb_anchor_build_discovery_reply(&anchor, &discover, 80u, 3000u, &reply) ==
           PROTO_ERR_BUSY);
    assert(anchor.state == UWB_ANCHOR_SCHEDULED);
    assert(anchor.scheduled);
    assert(anchor.epoch.active);
    assert(anchor.schedule_entry.anchor_id == config.anchor_id);
}

static void test_anchor_rejects_new_claim_while_scheduled_until_epoch_expiry(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(42u, 2u);
    struct uwb_wake_claim_frame accepted = claim_for(100u, 200u, 10u);
    struct uwb_wake_claim_frame duplicate = accepted;
    struct uwb_wake_claim_frame hidden = claim_for(101u, 201u, 1u);
    struct uwb_range_schedule_frame schedule = {0};
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &accepted, 1000u, &decision) ==
           PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    anchor_mark_discovery_replied(&anchor);

    schedule.network_id = accepted.network_id;
    schedule.clicker_id = accepted.clicker_id;
    schedule.click_event_id = accepted.click_event_id;
    schedule.attempt_index = accepted.attempt_index;
    schedule.nonce = accepted.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = accepted.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 9u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1200u, 10u) == PROTO_OK);
    assert(anchor.state == UWB_ANCHOR_SCHEDULED);
    assert(anchor.scheduled);

    assert(uwb_anchor_accept_wake_claim(&anchor, &duplicate, 1201u, &decision) ==
           PROTO_ERR_BUSY);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_BUSY);
    assert(anchor.state == UWB_ANCHOR_SCHEDULED);
    assert(anchor.scheduled);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.schedule_entry.anchor_id == config.anchor_id);
    assert(anchor.diagnostics.claims == 1u);
    assert(anchor.diagnostics.collisions == 1u);
    assert(anchor.diagnostics.arbitration_losses == 1u);

    assert(uwb_anchor_accept_wake_claim(&anchor, &hidden, 1202u, &decision) ==
           PROTO_ERR_BUSY);
    assert(decision == UWB_ANCHOR_CLAIM_REJECTED_BUSY);
    assert(anchor.state == UWB_ANCHOR_SCHEDULED);
    assert(anchor.scheduled);
    assert(anchor.epoch.clicker_id == accepted.clicker_id);
    assert(anchor.schedule_entry.anchor_id == config.anchor_id);
    assert(anchor.diagnostics.claims == 1u);
    assert(anchor.diagnostics.collisions == 2u);
    assert(anchor.diagnostics.arbitration_losses == 2u);

    assert(uwb_anchor_accept_wake_claim(&anchor, &hidden, 1401u, &decision) == PROTO_OK);
    assert(decision == UWB_ANCHOR_CLAIM_ACCEPTED);
    assert(anchor.state == UWB_ANCHOR_CLAIMED);
    assert(!anchor.scheduled);
    assert(anchor.schedule_entry.anchor_id == 0u);
    assert(anchor.epoch.clicker_id == hidden.clicker_id);
    assert(anchor.diagnostics.claims == 2u);
}

static void test_clicker_rejects_discovery_reply_with_wrong_mode_flags(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();
    struct uwb_discovery_reply_frame reply;

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    reply = reply_for(&session, 1u, 0u, 80u);
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_ERR_BUSY);
    assert(session.candidate_count == 0u);
    assert(session.diagnostics.discovery_replies == 0u);

    clicker_begin_discovery(&session);
    reply.flags = FLAG_DIAGNOSTIC;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_ERR_MALFORMED);
    assert(session.candidate_count == 0u);
    assert(session.diagnostics.discovery_replies == 0u);

    reply.flags = FLAG_COUNT_AS_CLICK;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);
    assert(session.candidate_count == 1u);
    assert(session.diagnostics.discovery_replies == 1u);
}

static void test_clicker_treats_non_present_replies_as_presence_only(void)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = clicker_config();
    struct uwb_discovery_reply_frame reply;

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    clicker_begin_discovery(&session);
    reply = reply_for(&session, 1u, 0u, 80u);

    reply.status = 0xffu;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_ERR_MALFORMED);
    assert(session.candidate_count == 0u);
    assert(session.successful_unique_count == 0u);
    assert(session.diagnostics.discovery_replies == 0u);

    reply.status = UWB_DISCOVERY_REPLY_BUSY;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);
    assert(session.candidate_count == 0u);
    assert(session.successful_unique_count == 0u);
    assert(session.diagnostics.discovery_replies == 1u);

    reply.status = UWB_DISCOVERY_REPLY_COLLISION;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);
    assert(session.candidate_count == 0u);
    assert(session.successful_unique_count == 0u);
    assert(session.diagnostics.discovery_replies == 2u);

    reply.status = UWB_DISCOVERY_REPLY_PRESENT;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);
    assert(session.candidate_count == 1u);
    assert(session.successful_unique_count == 0u);
    assert(session.diagnostics.discovery_replies == 3u);
}

static void test_clicker_retries_after_discovery_without_present_candidates(void)
{
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config = clicker_config();
    struct uwb_discovery_reply_frame reply;

    assert(uwb_clicker_session_start(&session, &config) == PROTO_OK);
    clicker_begin_discovery(&session);

    reply = reply_for(&session, 1u, 0u, 80u);
    reply.status = UWB_DISCOVERY_REPLY_BUSY;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);

    reply = reply_for(&session, 2u, 1u, 70u);
    reply.status = UWB_DISCOVERY_REPLY_COLLISION;
    assert(uwb_clicker_note_discovery_reply(&session, &reply) == PROTO_OK);

    assert(session.state == UWB_CLICKER_DISCOVERY);
    assert(session.candidate_count == 0u);
    assert(session.successful_unique_count == 0u);
    assert(session.diagnostics.discovery_replies == 2u);
    assert(uwb_clicker_build_range_schedule(&session, 900u, 3u, UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS, &schedule) ==
           PROTO_ERR_NOT_FOUND);
    assert(session.state == UWB_CLICKER_DISCOVERY);
    assert(session.diagnostics.ds_twr_failures == 0u);

    assert(uwb_clicker_prepare_retry(&session) == PROTO_OK);
    assert(session.state == UWB_CLICKER_POLITENESS);
    assert(session.attempt_index == 2u);
    assert(session.candidate_count == 0u);
    assert(session.diagnostics.retries == 1u);
    assert(session.diagnostics.discovery_replies == 2u);
}

static void test_anchor_deadline_covers_last_round_robin_sample(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(3u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_range_schedule_frame schedule = {0};
    const uint32_t now_ms = 1200u;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, now_ms, NULL) == PROTO_OK);
    anchor_mark_discovery_replied(&anchor);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 4u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = 900u;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 2u;
    schedule.flags = claim.flags;
    for (uint8_t i = 0u; i < schedule.selected_count; i++) {
        schedule.entries[i].anchor_id = (uint64_t)i + 1u;
        schedule.entries[i].seq = (uint8_t)(i + 1u);
        schedule.entries[i].sample_count = 2u;
    }
    set_schedule_burst_defaults(&schedule, 4u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, now_ms, 10u) == PROTO_OK);
    assert(anchor.uwb_wait_deadline_ms == now_ms + 3u + schedule.burst_window_ms + 10u);
}

static void test_anchor_rejects_wrong_schedule_identity_without_clearing_epoch(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(3u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_range_schedule_frame schedule = {0};
    struct uwb_range_exchange_identity expected_identity;
    const uint32_t now_ms = 1200u;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, now_ms, NULL) == PROTO_OK);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = 900u;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 9u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, now_ms, 10u) ==
           PROTO_ERR_BUSY);
    assert(!anchor.scheduled);
    assert(anchor.state == UWB_ANCHOR_CLAIMED);
    assert(anchor.epoch.clicker_id == claim.clicker_id);

    anchor_mark_discovery_replied(&anchor);

    schedule.attempt_index = claim.attempt_index + 1u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, now_ms, 10u) ==
           PROTO_ERR_MALFORMED);
    assert(!anchor.scheduled);
    assert(anchor.state == UWB_ANCHOR_DISCOVERY_REPLIED);
    assert(anchor.epoch.clicker_id == claim.clicker_id);

    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce + 1u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, now_ms, 10u) ==
           PROTO_ERR_MALFORMED);
    assert(!anchor.scheduled);
    assert(anchor.epoch.clicker_id == claim.clicker_id);

    schedule.nonce = claim.nonce;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, now_ms, 10u) == PROTO_OK);
    assert(anchor.scheduled);

    expected_identity = (struct uwb_range_exchange_identity){
        .network_id = config.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_id = config.anchor_id,
        .ranging_channel = config.ranging_channel,
        .reply_delay_us = 900u,
        .seq = 9u,
        .flags = claim.flags,
    };
    assert(uwb_anchor_accepts_range_exchange(&anchor, &expected_identity));

    schedule.clicker_id = claim.clicker_id + 1u;
    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, now_ms, 10u) ==
           PROTO_ERR_BUSY);
    assert(anchor.scheduled);
    assert(uwb_anchor_accepts_range_exchange(&anchor, &expected_identity));
}

static void test_anchor_new_claim_clears_stale_schedule(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(3u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_wake_claim_frame retry = claim;
    struct uwb_range_schedule_frame schedule = {0};
    struct uwb_range_exchange_identity identity;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1200u, NULL) == PROTO_OK);
    anchor_mark_discovery_replied(&anchor);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 9u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1300u, 10u) == PROTO_OK);
    identity = (struct uwb_range_exchange_identity){
        .network_id = config.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_id = config.anchor_id,
        .ranging_channel = config.ranging_channel,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .seq = 9u,
        .flags = claim.flags,
    };
    assert(uwb_anchor_accepts_range_exchange(&anchor, &identity));

    retry.attempt_index = (uint8_t)(claim.attempt_index + 1u);
    assert(uwb_anchor_accept_wake_claim(&anchor, &retry, 1400u, NULL) == PROTO_OK);
    assert(anchor.state == UWB_ANCHOR_CLAIMED);
    assert(!anchor.scheduled);
    assert(anchor.schedule_entry.anchor_id == 0u);
    assert(anchor.reply_delay_us == 0u);
    assert(anchor.expected_ranging_channel == 0u);
    assert(anchor.uwb_wait_deadline_ms == 0u);
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
}

static void test_anchor_abort_clears_epoch_and_scheduled_window(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(3u, 2u);
    struct uwb_wake_claim_frame claim = claim_for(100u, 200u, 1u);
    struct uwb_range_schedule_frame schedule = {0};
    struct uwb_range_exchange_identity identity;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1200u, NULL) == PROTO_OK);
    anchor_mark_discovery_replied(&anchor);

    schedule.network_id = claim.network_id;
    schedule.clicker_id = claim.clicker_id;
    schedule.click_event_id = claim.click_event_id;
    schedule.attempt_index = claim.attempt_index;
    schedule.nonce = claim.nonce;
    schedule.selected_count = 1u;
    schedule.ranging_channel = config.ranging_channel;
    schedule.reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US;
    schedule.first_poll_delay_ms = 3u;
    schedule.poll_spacing_ms = UWB_RANGE_SCHEDULE_MIN_POLL_SPACING_MS;
    schedule.samples_per_anchor = 1u;
    schedule.flags = claim.flags;
    schedule.entries[0].anchor_id = config.anchor_id;
    schedule.entries[0].seq = 9u;
    schedule.entries[0].sample_count = 1u;
    set_schedule_burst_defaults(&schedule, 1u);

    assert(uwb_anchor_accept_range_schedule(&anchor, &schedule, 1300u, 10u) == PROTO_OK);
    identity = (struct uwb_range_exchange_identity){
        .network_id = config.network_id,
        .clicker_id = claim.clicker_id,
        .click_event_id = claim.click_event_id,
        .attempt_index = claim.attempt_index,
        .nonce = claim.nonce,
        .anchor_id = config.anchor_id,
        .ranging_channel = config.ranging_channel,
        .reply_delay_us = UWB_DS_TWR_REPLY_DELAY_US,
        .seq = 9u,
        .flags = claim.flags,
    };
    assert(uwb_anchor_accepts_range_exchange(&anchor, &identity));

    uwb_anchor_abort_epoch(&anchor);
    assert(anchor.state == UWB_ANCHOR_ABORTED);
    assert(!anchor.epoch.active);
    assert(!anchor.scheduled);
    assert(anchor.schedule_entry.anchor_id == 0u);
    assert(anchor.reply_delay_us == 0u);
    assert(anchor.expected_ranging_channel == 0u);
    assert(anchor.uwb_wait_deadline_ms == 0u);
    assert(!uwb_anchor_accepts_range_exchange(&anchor, &identity));
    assert(uwb_anchor_accept_wake_claim(&anchor, &claim, 1400u, NULL) == PROTO_OK);
    assert(anchor.epoch.active);
    assert(anchor.state == UWB_ANCHOR_CLAIMED);
}

static void test_anchor_scan_accounting_and_timing_rejection(void)
{
    struct uwb_anchor_session anchor;
    struct uwb_anchor_config config = anchor_config(1u, 1u);
    uint32_t status_bits;

    assert(uwb_anchor_session_init(&anchor, &config) == PROTO_OK);
    assert(uwb_session_status_bits_from_diagnostics(NULL) == 0u);
    assert(uwb_session_status_bits_from_diagnostics(&anchor.diagnostics) == 0u);
    for (uint8_t i = 0u; i < 10u; i++) {
        uwb_anchor_note_idle_scan(&anchor, 1000u, 170u, 600u, i == 0u);
    }
    uwb_anchor_note_wake_decode_failure(&anchor, UWB_WAKE_DECODE_SFD_TIMEOUT);
    uwb_anchor_note_wake_decode_failure(&anchor, UWB_WAKE_DECODE_FRAME_TIMEOUT);
    uwb_anchor_note_wake_decode_failure(&anchor, UWB_WAKE_DECODE_CRC_FAILURE);
    uwb_anchor_note_false_wake_cooldown(&anchor);
    uwb_anchor_note_timing_rejection(&anchor);
    assert(uwb_anchor_note_range_result(&anchor, RANGE_OK) == PROTO_OK);
    assert(uwb_anchor_note_range_result(&anchor, RANGE_TIMING_INVALID) == PROTO_OK);
    assert(uwb_anchor_note_range_result(&anchor, RANGE_STS_QUALITY_FAIL) ==
           PROTO_ERR_MALFORMED);
    assert(uwb_anchor_note_range_result(&anchor, (enum range_status)99) ==
           PROTO_ERR_MALFORMED);
    uwb_anchor_note_mesh_packet(&anchor);
    uwb_anchor_note_sample_order(&anchor);

    assert(anchor.diagnostics.scans == 10u);
    assert(anchor.diagnostics.preambles == 1u);
    assert(anchor.diagnostics.sfd_timeouts == 1u);
    assert(anchor.diagnostics.frame_timeouts == 1u);
    assert(anchor.diagnostics.crc_failures == 1u);
    assert(anchor.diagnostics.false_wake_cooldowns == 1u);
    assert(anchor.diagnostics.timing_rejections == 2u);
    assert(anchor.diagnostics.ds_twr_successes == 1u);
    assert(anchor.diagnostics.ds_twr_failures == 1u);
    assert(anchor.diagnostics.uwb_mesh_packets == 1u);
    assert(anchor.diagnostics.sample_order_count == 1u);
    assert(anchor.diagnostics.scan_startup_time_us == 10000u);
    assert(anchor.diagnostics.scan_pll_time_us == 1700u);
    assert(anchor.diagnostics.scan_rx_time_us == 6000u);
    assert(anchor.diagnostics.awake_time_us == 17700u);
    uwb_anchor_note_awake_time(&anchor, 2300u);
    assert(anchor.diagnostics.awake_time_us == 20000u);
    assert(anchor.diagnostics.awake_time_us / 3u < 10000u);
    anchor.diagnostics.scan_startup_time_us = UINT32_MAX - 1u;
    anchor.diagnostics.scan_pll_time_us = UINT32_MAX - 1u;
    anchor.diagnostics.scan_rx_time_us = UINT32_MAX - 1u;
    uwb_anchor_note_idle_scan(&anchor, 2u, 2u, 2u, false);
    assert(anchor.diagnostics.scan_startup_time_us == UINT32_MAX);
    assert(anchor.diagnostics.scan_pll_time_us == UINT32_MAX);
    assert(anchor.diagnostics.scan_rx_time_us == UINT32_MAX);
    anchor.diagnostics.awake_time_us = UINT32_MAX - 1u;
    uwb_anchor_note_awake_time(&anchor, 2u);
    assert(anchor.diagnostics.awake_time_us == UINT32_MAX);
    status_bits = uwb_session_status_bits_from_diagnostics(&anchor.diagnostics);
    assert((status_bits & STATUS_BIT_UWB_SCAN_ACTIVE) != 0u);
    assert((status_bits & STATUS_BIT_UWB_WAKE_DECODE_FAILURE) != 0u);
    assert((status_bits & STATUS_BIT_UWB_CLAIM_COLLISION) == 0u);
    assert((status_bits & STATUS_BIT_UWB_DS_TWR_FAILURE) != 0u);
    assert((status_bits & STATUS_BIT_UWB_TIMING_REJECTION) != 0u);
    assert((status_bits & STATUS_BIT_UWB_MESH_RX) != 0u);

    anchor.diagnostics.collisions = 1u;
    status_bits = uwb_session_status_bits_from_diagnostics(&anchor.diagnostics);
    assert((status_bits & STATUS_BIT_UWB_CLAIM_COLLISION) != 0u);

    assert(uwb_session_validate_reply_timing(900u, 910u, 900u, 50u) == PROTO_OK);
    assert(uwb_session_validate_reply_timing(900u, 1300u, 900u, 50u) ==
           PROTO_ERR_MALFORMED);
    assert(uwb_session_validate_reply_timing(1800u, 1800u, 1800u, 50u) ==
           PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_clicker_builds_wake_claim_and_rejects_bad_timing();
    test_clicker_contention_delay_bounds_and_diagnostics();
    test_clicker_politeness_decodes_relevant_uwb_packets();
    test_clicker_discovers_50_and_schedules_best_6_only();
    test_clicker_discovers_sparse_50_slots_with_6_present();
    test_clicker_releases_replied_anchors_when_too_few_for_normal_click();
    test_clicker_rejects_success_history_overflow_config();
    test_clicker_serializes_failures_and_retries_without_counting_discovery();
    test_retry_ignores_already_successful_anchors();
    test_clicker_abort_attempt_clears_active_step_for_retry();
    test_clicker_abort_scheduled_attempt_before_first_range_for_retry();
    test_clicker_abort_after_successes_preserves_completed_ranges_only();
    test_clicker_abort_after_min_successes_finishes_without_failure();
    test_clicker_abort_last_attempt_fails_without_counting_ds_twr();
    test_failing_anchor_is_capped_and_others_continue();
    test_round_robin_sample_order_completes_before_success();
    test_six_anchor_single_sample_runs_full_schedule_before_success();
    test_four_anchor_click_uses_shared_200_ms_burst_window();
    test_complete_four_anchor_click_flow_reports_after_selected_ds_twr();
    test_four_simultaneous_clickers_converge_across_six_anchors();
    test_anchor_bad_first_wake_claim_does_not_create_epoch();
    test_anchor_arbitrates_four_clickers_and_rejects_hidden_ds_twr();
    test_anchor_rejects_foreign_claim_without_clearing_epoch();
    test_anchor_accepts_newer_retry_claim_for_same_event();
    test_anchor_epoch_expiry_handles_ms_wrap();
    test_anchor_rejects_same_event_mismatched_session_identity();
    test_anchor_schedule_validation_and_presence_only_discovery();
    test_anchor_rejects_out_of_schedule_range_exchange_identity();
    test_anchor_accepts_schedule_sequence_range_ending_at_255();
    test_anchor_rejects_duplicate_discover_after_schedule();
    test_anchor_rejects_new_claim_while_scheduled_until_epoch_expiry();
    test_clicker_rejects_discovery_reply_with_wrong_mode_flags();
    test_clicker_treats_non_present_replies_as_presence_only();
    test_clicker_retries_after_discovery_without_present_candidates();
    test_anchor_deadline_covers_last_round_robin_sample();
    test_anchor_accepts_range_release_after_discovery_reply();
    test_anchor_rejects_wrong_schedule_identity_without_clearing_epoch();
    test_anchor_new_claim_clears_stale_schedule();
    test_anchor_abort_clears_epoch_and_scheduled_window();
    test_anchor_scan_accounting_and_timing_rejection();
    return 0;
}
