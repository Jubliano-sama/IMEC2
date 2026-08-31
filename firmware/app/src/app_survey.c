#include "app_survey.h"

#include "app_config.h"
#include "app_board.h"
#include "app_radio_guard.h"
#include "app_radio_recovery.h"
#include "app_state.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "enumeration_response_lane.h"
#include "status.h"

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif

#define APP_SURVEY_ANCHOR_PREPARE_MS 100u
#define APP_SURVEY_GATEWAY_PREPARE_MS 40u
#define APP_SURVEY_RADIO_RETRY_MS 2u
#define APP_SURVEY_RANGE_RX_GUARD_MS 25u
#define APP_SURVEY_RANGE_TIMEOUT_MS 55u
#define APP_SURVEY_START_EDGE_SLOP_MS SURVEY_RADIO_GUARD_MS

_Static_assert(APP_SURVEY_RANGE_RX_GUARD_MS +
                   APP_SURVEY_RANGE_TIMEOUT_MS <=
                   SURVEY_RANGE_ATTEMPT_SPACING_MS,
               "a failed responder attempt must not overlap the next one");
_Static_assert(
    SURVEY_RESPONDER_HEAD_START_MS +
        ((SURVEY_RANGE_ATTEMPT_COUNT - 1u) *
         SURVEY_RANGE_ATTEMPT_SPACING_MS) +
        APP_SURVEY_RANGE_TIMEOUT_MS + SURVEY_RADIO_GUARD_MS <=
            SURVEY_RANGE_WAVE_MS,
    "the last range timeout and radio guard must fit inside its wave");
_Static_assert(SURVEY_HARD_CAP_MS < INT32_MAX,
               "survey RX ownership must fit a wrap-safe uptime deadline");

enum app_survey_gateway_stage {
    APP_SURVEY_GATEWAY_IDLE = 0,
    APP_SURVEY_GATEWAY_WAIT_START_RF,
    APP_SURVEY_GATEWAY_NEIGHBORS,
    APP_SURVEY_GATEWAY_WAIT_PLAN,
    APP_SURVEY_GATEWAY_WAIT_PLAN_RF,
    APP_SURVEY_GATEWAY_EXECUTING,
    APP_SURVEY_GATEWAY_CLEANUP,
    APP_SURVEY_GATEWAY_TERMINAL,
};

enum app_survey_anchor_action {
    APP_SURVEY_ANCHOR_ACTION_NONE = 0,
    APP_SURVEY_ANCHOR_ACTION_NEIGHBORS,
    APP_SURVEY_ANCHOR_ACTION_EXECUTE,
    APP_SURVEY_ANCHOR_ACTION_EXPIRE,
};

struct app_survey_gateway_state {
    struct survey_identity identity;
    struct survey_graph graph;
    struct survey_plan_build_result plan_build;
    union {
        struct survey_range_result results[SURVEY_MAX_PAIRS];
        struct survey_signal_record signals[SURVEY_MAX_SIGNAL_RECORDS];
    } records;
    struct survey_event last_event;
    uint64_t node_ids_by_slot[SURVEY_MAX_ANCHORS];
    uint64_t result_received_mask[2];
    uint64_t signal_received_mask[2];
    uint64_t operation_origin_ms;
    uint64_t hard_deadline_ms;
    uint64_t response_lane_start_ms;
    uint64_t response_lane_end_ms;
    uint64_t execution_start_ms;
    uint64_t self_stop_ms;
    uint64_t plan_deadline_ms;
    uint64_t cleanup_deadline_ms;
    uint64_t pending_control_deadline_ms;
    uint32_t control_delivery_ms;
    uint32_t pending_control_handle;
    uint16_t partial_reasons;
    uint8_t hop_counts[SURVEY_MAX_ANCHORS];
    uint8_t stride_index;
    uint8_t next_batch_index;
    uint8_t signal_count;
    enum survey_response_kind response_kind;
    enum app_survey_gateway_stage stage;
    bool active;
};

struct app_survey_anchor_state {
    struct survey_identity identity;
    struct survey_plan plan;
    struct survey_range_result local_results[SURVEY_MAX_DEGREE];
    uint64_t node_ids_by_slot[SURVEY_MAX_ANCHORS];
    uint64_t occupied_slot_mask;
    uint64_t neighbor_start_ms;
    uint64_t execution_start_ms;
    uint64_t self_stop_ms;
    uint64_t parent_id;
    uint32_t roster_assignment_epoch;
    uint32_t roster_table_command_seq;
    struct discovery_assignment_table_commitment roster_table_commitment;
    uint8_t own_slot;
    uint8_t slot_span;
    uint8_t hop_count;
    uint8_t local_result_count;
    uint8_t next_batch_index;
    enum app_survey_anchor_action action;
    bool roster_valid;
    bool active;
    bool plan_valid;
    bool aborted;
};

static struct app_survey_ops survey_ops;
static struct app_survey_gateway_state gateway_state;
static struct app_survey_anchor_state anchor_state;
static struct protocol_rx_lifecycle anchor_rx_lifecycle;
static struct k_work_delayable gateway_work;
static struct k_work_delayable anchor_work;
K_MUTEX_DEFINE(survey_lock);

static uint32_t bounded_wait_ms(uint64_t now_ms, uint64_t deadline_ms)
{
    uint64_t remaining = deadline_ms > now_ms ? deadline_ms - now_ms : 0u;

    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static bool close_u64(uint64_t left, uint64_t right, uint32_t slop_ms)
{
    uint64_t delta = left > right ? left - right : right - left;

    return delta <= slop_ms;
}

static void anchor_rx_terminate_locked(bool aborted)
{
    if (anchor_state.identity.generation != 0u) {
        (void)protocol_rx_lifecycle_terminate(
            &anchor_rx_lifecycle,
            PROTOCOL_RX_OPERATION_SURVEY,
            anchor_state.identity.generation);
    } else {
        protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
    }
    anchor_state.active = false;
    anchor_state.aborted = aborted;
    anchor_state.action = APP_SURVEY_ANCHOR_ACTION_NONE;
}

static bool anchor_rx_expire_locked(uint64_t now_ms)
{
    if (!anchor_state.active || now_ms < anchor_state.self_stop_ms) {
        return false;
    }
    anchor_rx_terminate_locked(false);
    return true;
}

static bool response_bit_get(const uint64_t bits[2], uint8_t index)
{
    return index < SURVEY_MAX_PAIRS &&
           (bits[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u;
}

static void response_bit_set(uint64_t bits[2], uint8_t index)
{
    if (index < SURVEY_MAX_PAIRS) {
        bits[index / 64u] |= UINT64_C(1) << (index % 64u);
    }
}

static bool signal_bit_get(const uint64_t bits[2], uint8_t index)
{
    return index < SURVEY_MAX_SIGNAL_RECORDS &&
           (bits[index / 64u] &
            (UINT64_C(1) << (index % 64u))) != 0u;
}

static void signal_bit_set(uint64_t bits[2], uint8_t index)
{
    if (index < SURVEY_MAX_SIGNAL_RECORDS) {
        bits[index / 64u] |= UINT64_C(1) << (index % 64u);
    }
}

static bool anchor_identity_matches_roster(
    const struct survey_assignment_identity *identity)
{
    return identity != NULL && anchor_state.roster_valid &&
           identity->assignment_epoch ==
               anchor_state.roster_assignment_epoch &&
           identity->table_command_seq ==
               anchor_state.roster_table_command_seq &&
           identity->slot_span == anchor_state.slot_span &&
           discovery_assignment_table_commitment_equal(
               &identity->table_commitment,
               &anchor_state.roster_table_commitment);
}

static bool anchor_generation_live(uint32_t generation)
{
    bool live;

    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked((uint64_t)k_uptime_get());
    live = anchor_state.active && !anchor_state.aborted &&
           anchor_state.identity.generation == generation;
    k_mutex_unlock(&survey_lock);
    return live;
}

static int anchor_work_reschedule(uint64_t due_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint32_t delay_ms = bounded_wait_ms(now_ms, due_ms);

    if (survey_ops.anchor_reschedule == NULL) {
        return -ENOTSUP;
    }
    return survey_ops.anchor_reschedule(&anchor_work, delay_ms);
#else
    ARG_UNUSED(due_ms);
    return -ENOTSUP;
#endif
}

static int gateway_work_reschedule(uint64_t due_ms)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();

    return k_work_reschedule(&gateway_work,
                             K_MSEC(bounded_wait_ms(now_ms, due_ms)));
}

static int anchor_radio_release(struct radio_guard_uwb_lease *lease,
                                const char *reason)
{
    int ret;
    int parking_ret;

    ret = radio_guard_uwb_release_begin(lease);
    if (ret < 0) {
        return ret;
    }
    parking_ret = app_radio_idle_with_bounded_recovery(reason);
    return radio_guard_uwb_release_finish(lease, parking_ret);
}

static int anchor_radio_claim(uint64_t deadline_ms,
                              struct radio_guard_uwb_lease *lease)
{
    int ret;

    dwm3000_driver_request_receive_abort(
        DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    while ((uint64_t)k_uptime_get() <= deadline_ms) {
        ret = radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_SURVEY,
                                    "scheduled anchor survey",
                                    lease);
        if (ret == 0) {
            dwm3000_driver_clear_receive_abort(
                DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
            return 0;
        }
        if (ret != -EBUSY && ret != -EAGAIN) {
            return ret;
        }
        k_sleep(K_MSEC(APP_SURVEY_RADIO_RETRY_MS));
        app_watchdog_note_radio_progress();
    }
    return -ETIMEDOUT;
}

static int survey_lane_try_tx(struct survey_response_lane *lane,
                              const struct enumeration_response_timing *timing)
{
    struct survey_response_bundle bundle;
    uint8_t encoded[UWB_SURVEY_BUNDLE_MAX_LEN];
    size_t encoded_len = 0u;
    int ret;

    ret = survey_response_lane_prepare_round(lane, timing->round,
                                             sys_rand32_get());
    if (ret == PROTO_OK) {
        ret = survey_response_lane_bundle_for_offset(lane, timing, &bundle);
    }
    if (ret == PROTO_ERR_NOT_FOUND) {
        return 0;
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = uwb_encode_survey_bundle(&bundle, encoded, sizeof(encoded),
                                   &encoded_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = dwm3000_driver_send_frame(encoded, encoded_len,
                                    UWB_CONTROL_TX_TIMEOUT_MS);
    return ret < 0 ? ret : 1;
}

static int survey_lane_handle_raw(
    struct survey_response_lane *lane,
    const uint8_t *frame,
    size_t frame_len,
    const struct enumeration_response_timing *timing,
    uint64_t round_deadline_ms)
{
    struct survey_response_bundle bundle;
    struct survey_response_hop_ack ack;
    uint8_t encoded[UWB_SURVEY_HOP_ACK_LEN];
    size_t encoded_len = 0u;
    bool added = false;
    int ret;

    ret = uwb_decode_survey_hop_ack(frame, frame_len, &ack);
    if (ret == PROTO_OK) {
        (void)survey_response_lane_note_ack(lane, &ack);
        return 1;
    }
    ret = uwb_decode_survey_bundle(frame, frame_len, &bundle);
    if (ret != PROTO_OK) {
        return 0;
    }
    if (timing->depth <= lane->hop_count) {
        return 1;
    }
    ret = survey_response_lane_merge_bundle(lane, &bundle, &added);
    if (ret != PROTO_OK) {
        return 1;
    }
    ack = (struct survey_response_hop_ack) {
        .network_id = lane->network_id,
        .generation = lane->generation,
        .parent_id = lane->local_id,
        .child_id = bundle.sender_id,
        .kind = lane->kind,
        .sequence = bundle.sequence,
    };
    ret = uwb_encode_survey_hop_ack(&ack, encoded, sizeof(encoded),
                                    &encoded_len);
    if (ret != PROTO_OK) {
        return 1;
    }
    (void)added;
    (void)dwm3000_driver_send_frame_tracked_until(
        encoded, encoded_len, UWB_CONTROL_TX_TIMEOUT_MS,
        round_deadline_ms, NULL);
    return 1;
}

static int anchor_run_response_lane(
    uint32_t generation,
    enum survey_response_kind kind,
    uint64_t start_ms,
    uint64_t parent_id,
    uint8_t hop_count,
    uint8_t max_hop_count,
    const struct survey_response_record *local_records,
    uint8_t local_record_count)
{
    struct survey_response_lane lane;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    int32_t all_acked_elapsed_ms = -1;
    int ret;

    ret = survey_response_lane_begin(&lane, NETWORK_ID, generation,
                                     DEVICE_ID, parent_id, kind,
                                     hop_count, max_hop_count, start_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    for (uint8_t i = 0u; i < local_record_count; i++) {
        ret = survey_response_lane_add_record(&lane, &local_records[i], NULL);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
    }
    sleep_until_ms((int64_t)start_ms);
    ret = dwm3000_driver_configure_wake_mesh_control_mode();
    if (ret < 0) {
        return ret;
    }
    while (anchor_generation_live(generation)) {
        struct enumeration_response_timing timing;
        uint64_t now_ms = (uint64_t)k_uptime_get();
        uint64_t round_start_ms;
        uint64_t round_deadline_ms;
        uint64_t receive_deadline_ms;
        uint8_t next_offset;
        size_t frame_len = 0u;
        enum dwm3000_rx_failure failure = DWM3000_RX_FAILURE_NONE;

        if (!enumeration_response_timing_at_depth(start_ms, now_ms,
                                                   max_hop_count, &timing)) {
            if (enumeration_response_lane_complete_depth(
                    start_ms, now_ms, max_hop_count)) {
                status_debug_printf(
                    "DBG_SURVEY_LANE_END g=%u k=%u h=%u rec=%u "
                    "ack=%d dur=%llu\n",
                    generation, (unsigned int)kind, hop_count,
                    lane.record_count, all_acked_elapsed_ms,
                    (unsigned long long)(now_ms - start_ms));
                return 0;
            }
            return -ESTALE;
        }
        round_start_ms = now_ms - timing.round_offset_ms;
        round_deadline_ms = round_start_ms + ENUMERATION_RESPONSE_ROUND_MS;
        ret = survey_lane_try_tx(&lane, &timing);
        if (ret < 0) {
            status_debug_printf(
                "DBG_SURVEY_LANE_TX_FAIL gen=%u kind=%u depth=%u round=%u ret=%d\n",
                generation, (unsigned int)kind, timing.depth, timing.round,
                ret);
        }
        next_offset = survey_response_lane_next_offset_ms(&lane, &timing);
        receive_deadline_ms = round_deadline_ms;
        if (next_offset != SURVEY_RESPONSE_NO_OFFSET) {
            uint64_t next_tx_ms = round_start_ms + next_offset;

            if (next_tx_ms > now_ms && next_tx_ms < receive_deadline_ms) {
                receive_deadline_ms = next_tx_ms;
            }
        }
        now_ms = (uint64_t)k_uptime_get();
        if (now_ms >= receive_deadline_ms) {
            continue;
        }
        ret = dwm3000_driver_receive_frame_continuous(
            bounded_wait_ms(now_ms, receive_deadline_ms),
            frame, sizeof(frame), &frame_len, NULL, NULL, &failure);
        if (ret == 0) {
            (void)survey_lane_handle_raw(&lane, frame, frame_len, &timing,
                                         round_deadline_ms);
            if (all_acked_elapsed_ms < 0 &&
                survey_response_lane_all_acked(&lane)) {
                all_acked_elapsed_ms = (int32_t)MIN(
                    (uint64_t)INT32_MAX,
                    (uint64_t)k_uptime_get() - start_ms);
                status_debug_printf(
                    "DBG_SURVEY_LANE_ACK g=%u k=%u h=%u rec=%u t=%d\n",
                    generation, (unsigned int)kind, hop_count,
                    lane.record_count, all_acked_elapsed_ms);
            }
        } else if (ret != -ETIMEDOUT && ret != -ECANCELED) {
            int recovery_ret = dwm3000_driver_force_recovery();

            if (recovery_ret < 0 ||
                dwm3000_driver_configure_wake_mesh_control_mode() < 0) {
                return recovery_ret < 0 ? recovery_ret : ret;
            }
        } else if (ret == -ECANCELED && !anchor_generation_live(generation)) {
            return -ECANCELED;
        }
        app_watchdog_note_radio_progress();
    }
    return -ECANCELED;
}

static int anchor_neighbor_sequence(
    const struct app_survey_anchor_state *snapshot)
{
#define LOCAL_SIGNAL_RECORD_CAPACITY \
    (1u + ((SURVEY_MAX_ANCHORS - 1u + \
            SURVEY_SIGNAL_LEVELS_PER_RECORD - 1u) / \
           SURVEY_SIGNAL_LEVELS_PER_RECORD))
    struct radio_guard_uwb_lease lease = {0};
    struct survey_neighbor_report report = {.own_slot = snapshot->own_slot};
    struct survey_response_record records[LOCAL_SIGNAL_RECORD_CAPACITY];
    struct survey_presence_frame presence = {
        .network_id = NETWORK_ID,
        .generation = snapshot->identity.generation,
        .sender_id = DEVICE_ID,
        .sender_slot = snapshot->own_slot,
    };
    uint8_t encoded_presence[UWB_SURVEY_PRESENCE_LEN];
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    int8_t rsl_samples[SURVEY_MAX_ANCHORS][SURVEY_NEIGHBOR_BEACON_COUNT] = {0};
    uint8_t rsl_sample_count[SURVEY_MAX_ANCHORS] = {0};
    uint8_t signal_levels[SURVEY_MAX_ANCHORS] = {0};
    uint8_t record_count = 1u;
    size_t encoded_presence_len = 0u;
    uint64_t sequence_end_ms;
    int ret;

    ret = anchor_radio_claim(snapshot->neighbor_start_ms +
                                 SURVEY_NEIGHBOR_QUIET_MARGIN_MS,
                             &lease);
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mesh_control_mode();
    if (ret < 0) {
        goto out;
    }
    ret = uwb_encode_survey_presence(&presence, encoded_presence,
                                     sizeof(encoded_presence),
                                     &encoded_presence_len);
    if (ret != PROTO_OK) {
        ret = mesh_errno_from_proto(ret);
        goto out;
    }
    sleep_until_ms((int64_t)snapshot->neighbor_start_ms);
    for (uint8_t slot = 0u; slot < snapshot->slot_span; slot++) {
        uint64_t slot_start_ms = snapshot->neighbor_start_ms +
            (uint64_t)slot * SURVEY_NEIGHBOR_SLOT_MS;
        uint64_t slot_end_ms = slot_start_ms + SURVEY_NEIGHBOR_SLOT_MS;

        if (!anchor_generation_live(snapshot->identity.generation)) {
            ret = -ECANCELED;
            goto out;
        }
        if (slot == snapshot->own_slot) {
            for (uint8_t beacon = 0u;
                 beacon < SURVEY_NEIGHBOR_BEACON_COUNT; beacon++) {
                sleep_until_ms((int64_t)(slot_start_ms +
                    survey_neighbor_beacon_offset_ms(beacon)));
                ret = dwm3000_driver_send_frame(encoded_presence,
                                                 encoded_presence_len,
                                                 UWB_CONTROL_TX_TIMEOUT_MS);
                if (ret < 0) {
                    int recovery_ret = dwm3000_driver_force_recovery();

                    if (recovery_ret < 0 ||
                        dwm3000_driver_configure_wake_mesh_control_mode() < 0) {
                        goto out;
                    }
                }
                app_watchdog_note_radio_progress();
            }
            sleep_until_ms((int64_t)slot_end_ms);
            continue;
        }
        while ((uint64_t)k_uptime_get() < slot_end_ms &&
               anchor_generation_live(snapshot->identity.generation)) {
            struct survey_presence_frame heard;
            size_t frame_len = 0u;
            uint64_t now_ms = (uint64_t)k_uptime_get();
            int8_t rsl_dbm = 0;

            ret = dwm3000_driver_receive_frame_continuous(
                bounded_wait_ms(now_ms, slot_end_ms), frame, sizeof(frame),
                &frame_len, NULL, &rsl_dbm, NULL);
            if (ret == 0 &&
                uwb_decode_survey_presence(frame, frame_len, &heard) ==
                    PROTO_OK &&
                heard.network_id == NETWORK_ID &&
                heard.generation == snapshot->identity.generation &&
                heard.sender_slot < snapshot->slot_span &&
                snapshot->node_ids_by_slot[heard.sender_slot] ==
                    heard.sender_id &&
                heard.sender_slot != snapshot->own_slot) {
                (void)survey_neighbor_bitmap_set(report.heard_bitmap,
                                                 heard.sender_slot);
                if (heard.sender_slot < snapshot->own_slot &&
                    rsl_dbm != 0 &&
                    rsl_sample_count[heard.sender_slot] <
                        SURVEY_NEIGHBOR_BEACON_COUNT) {
                    rsl_samples[heard.sender_slot]
                               [rsl_sample_count[heard.sender_slot]++] =
                        rsl_dbm;
                }
            } else if (ret < 0 && ret != -ETIMEDOUT && ret != -ECANCELED) {
                int recovery_ret = dwm3000_driver_force_recovery();

                if (recovery_ret < 0 ||
                    dwm3000_driver_configure_wake_mesh_control_mode() < 0) {
                    goto out;
                }
            }
            app_watchdog_note_radio_progress();
        }
    }
    if (survey_neighbor_report_encode(&report, records[0].bytes) == 0u) {
        ret = -EBADMSG;
        goto out;
    }
    for (uint8_t slot = 0u; slot < snapshot->own_slot; slot++) {
        uint8_t count = rsl_sample_count[slot];

        for (uint8_t i = 1u; i < count; i++) {
            int8_t value = rsl_samples[slot][i];
            uint8_t j = i;

            while (j > 0u && rsl_samples[slot][j - 1u] > value) {
                rsl_samples[slot][j] = rsl_samples[slot][j - 1u];
                j--;
            }
            rsl_samples[slot][j] = value;
        }
        if (count > 0u) {
            signal_levels[slot] = survey_rsl_quantize_dbm(
                rsl_samples[slot][count / 2u]);
        }
    }
    for (uint8_t chunk = 0u;
         chunk < survey_signal_record_count_for_slot(snapshot->own_slot);
         chunk++) {
        struct survey_signal_record signal;

        if (record_count >= ARRAY_SIZE(records) ||
            survey_signal_record_encode(snapshot->own_slot, chunk,
                                        signal_levels, &signal) == 0u) {
            ret = -EBADMSG;
            goto out;
        }
        memcpy(records[record_count++].bytes, signal.bytes,
               sizeof(signal.bytes));
    }
    sequence_end_ms = snapshot->neighbor_start_ms +
        survey_neighbor_sequence_duration_ms(snapshot->slot_span);
    ret = anchor_run_response_lane(
        snapshot->identity.generation, SURVEY_RESPONSE_NEIGHBORS,
        sequence_end_ms + SURVEY_RESULT_PREPARE_MS,
        snapshot->parent_id, snapshot->hop_count,
        snapshot->identity.assignment.max_hop_count, records, record_count);
out:
    {
        int release_ret = anchor_radio_release(&lease, "survey-neighbors");

        return release_ret < 0 ? release_ret : ret;
    }
#undef LOCAL_SIGNAL_RECORD_CAPACITY
}

static uint64_t survey_range_nonce(uint32_t generation,
                                   uint8_t batch_index,
                                   uint8_t pair_index)
{
    uint64_t nonce = ((uint64_t)generation << 32) |
                     ((uint64_t)batch_index << 8) |
                     ((uint64_t)pair_index + 1u);

    return nonce == 0u ? 1u : nonce;
}

static void survey_range_request_fill(
    struct dwm3000_range_request *request,
    const struct app_survey_anchor_state *snapshot,
    uint8_t pair_index,
    uint8_t attempt,
    uint64_t initiator_id,
    uint64_t responder_id)
{
    memset(request, 0, sizeof(*request));
    request->initiator_id = initiator_id;
    request->responder_id = responder_id;
    request->network_id = NETWORK_ID;
    request->session_nonce = survey_range_nonce(
        snapshot->identity.generation, snapshot->plan.batch_index,
        pair_index);
    request->responder_short_addr =
        uwb_session_short_addr_from_id(responder_id);
    request->session_id = snapshot->identity.generation;
    request->seq = (uint8_t)(pair_index + 1u);
    request->round_index = attempt;
    request->flags = FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
    request->timeout_ms = APP_SURVEY_RANGE_TIMEOUT_MS;
    request->reply_delay_uus = UWB_RANGE_REPLY_DELAY_UUS;
    request->skip_responder_report = true;
}

static int anchor_run_responder_pair(
    const struct app_survey_anchor_state *snapshot,
    uint8_t pair_index,
    uint64_t wave_start_ms,
    struct survey_range_result *survey_result)
{
    const struct survey_plan_pair *pair = &snapshot->plan.pairs[pair_index];
    uint64_t initiator_id = snapshot->node_ids_by_slot[pair->initiator_slot];
    int32_t samples[SURVEY_RANGE_ATTEMPT_COUNT];
    size_t sample_count = 0u;

    for (uint8_t attempt = 0u; attempt < SURVEY_RANGE_ATTEMPT_COUNT;
         attempt++) {
        struct dwm3000_range_request request;
        struct dwm3000_range_result result = {.status = RANGE_RX_TIMEOUT};
        uint64_t target_ms = wave_start_ms +
            survey_range_attempt_offset_ms(attempt);
        uint64_t listen_ms = attempt == 0u ? wave_start_ms :
            target_ms - APP_SURVEY_RANGE_RX_GUARD_MS;
        uint32_t timeout_ms = attempt == 0u ?
            bounded_wait_ms(wave_start_ms,
                            target_ms + APP_SURVEY_RANGE_RX_GUARD_MS) :
            APP_SURVEY_RANGE_RX_GUARD_MS + APP_SURVEY_RANGE_TIMEOUT_MS;
        int64_t call_start_ms;
        int64_t call_end_ms;
        int ret;

        sleep_until_ms((int64_t)listen_ms);
        survey_range_request_fill(&request, snapshot, pair_index, attempt,
                                  initiator_id, DEVICE_ID);
        request.timeout_ms = timeout_ms;
        call_start_ms = k_uptime_get();
        ret = dwm3000_driver_responder_poll_expected(
            DEVICE_ID, &request, timeout_ms, &result);
        call_end_ms = k_uptime_get();
        status_debug_printf(
            "DBG_SURVEY_PROFILE kind=attempt r=R p=%u a=%u "
            "sd=%lld dur=%lld fd=%lld to=%u ret=%d st=%u mm=%d\n",
            pair_index, attempt,
            (long long)(call_start_ms - (int64_t)target_ms),
            (long long)(call_end_ms - call_start_ms),
            (long long)(call_end_ms - (int64_t)target_ms),
            timeout_ms, ret, (unsigned int)result.status,
            result.distance_mm);
        if (ret == 0 && result.status == RANGE_OK &&
            result.distance_mm >= 0 &&
            sample_count < ARRAY_SIZE(samples)) {
            samples[sample_count++] = result.distance_mm;
        }
        app_watchdog_note_radio_progress();
    }
    return mesh_errno_from_proto(survey_range_result_from_samples(
        pair_index, pair->responder_slot, samples, sample_count,
        survey_result));
}

static void anchor_run_initiator_pair(
    const struct app_survey_anchor_state *snapshot,
    uint8_t pair_index,
    uint64_t wave_start_ms)
{
    const struct survey_plan_pair *pair = &snapshot->plan.pairs[pair_index];
    uint64_t responder_id = snapshot->node_ids_by_slot[pair->responder_slot];

    for (uint8_t attempt = 0u; attempt < SURVEY_RANGE_ATTEMPT_COUNT;
         attempt++) {
        struct dwm3000_range_request request;
        struct dwm3000_range_result result = {.status = RANGE_RX_TIMEOUT};

        sleep_until_ms((int64_t)(wave_start_ms +
            survey_range_attempt_offset_ms(attempt)));
        survey_range_request_fill(&request, snapshot, pair_index, attempt,
                                  DEVICE_ID, responder_id);
        {
            uint64_t target_ms = wave_start_ms +
                survey_range_attempt_offset_ms(attempt);
            int64_t call_start_ms = k_uptime_get();
            int ret = dwm3000_driver_range_initiator(&request, &result);
            int64_t call_end_ms = k_uptime_get();

            status_debug_printf(
                "DBG_SURVEY_PROFILE kind=attempt r=I p=%u a=%u "
                "sd=%lld dur=%lld fd=%lld ret=%d st=%u\n",
                pair_index, attempt,
                (long long)(call_start_ms - (int64_t)target_ms),
                (long long)(call_end_ms - call_start_ms),
                (long long)(call_end_ms - (int64_t)target_ms),
                ret, (unsigned int)result.status);
        }
        app_watchdog_note_radio_progress();
    }
}

static int anchor_execute_plan(struct app_survey_anchor_state *snapshot)
{
    struct radio_guard_uwb_lease lease = {0};
    uint32_t stride_ms = survey_wave_stride_ms(
        snapshot->identity.assignment.max_hop_count);
    uint8_t total_strides = snapshot->plan.wave_count +
                            SURVEY_EXTRA_DRAIN_STRIDES;
    int ret;

    ret = anchor_radio_claim(snapshot->execution_start_ms +
                                 SURVEY_RADIO_GUARD_MS,
                             &lease);
    if (ret < 0) {
        return ret;
    }
    for (uint8_t stride = 0u; stride < total_strides; stride++) {
        struct survey_response_record records[SURVEY_MAX_DEGREE];
        uint64_t wave_start_ms = snapshot->execution_start_ms +
            (uint64_t)stride * stride_ms;
        uint64_t lane_start_ms = wave_start_ms + SURVEY_RANGE_WAVE_MS +
                                 SURVEY_RESULT_PREPARE_MS;

        if (!anchor_generation_live(snapshot->identity.generation)) {
            ret = -ECANCELED;
            break;
        }
        sleep_until_ms((int64_t)wave_start_ms);
        if (stride < snapshot->plan.wave_count) {
            int64_t configure_start_ms = k_uptime_get();
            int64_t configure_end_ms;

            ret = dwm3000_driver_configure_range_mode();
            configure_end_ms = k_uptime_get();
            if (ret < 0) {
                break;
            }
            for (uint8_t pair_index = 0u;
                 pair_index < snapshot->plan.pair_count; pair_index++) {
                const struct survey_plan_pair *pair =
                    &snapshot->plan.pairs[pair_index];

                if (pair->wave_index != stride) {
                    continue;
                }
                if (pair->responder_slot == snapshot->own_slot) {
                    struct survey_range_result local_result;
                    int64_t pair_end_ms;

                    ret = anchor_run_responder_pair(snapshot, pair_index,
                                                    wave_start_ms,
                                                    &local_result);
                    pair_end_ms = k_uptime_get();
                    status_debug_printf(
                        "DBG_SURVEY_PROFILE kind=pair r=R p=%u "
                        "cfg=%lld cfg_end=%lld end=%lld slack=%lld\n",
                        pair_index,
                        (long long)(configure_end_ms - configure_start_ms),
                        (long long)(configure_end_ms -
                                    (int64_t)wave_start_ms),
                        (long long)(pair_end_ms - (int64_t)wave_start_ms),
                        (long long)((int64_t)(wave_start_ms +
                                             SURVEY_RANGE_WAVE_MS) -
                                    pair_end_ms));
                    if (ret == 0 && snapshot->local_result_count <
                                        SURVEY_MAX_DEGREE) {
                        snapshot->local_results[
                            snapshot->local_result_count++] = local_result;
                    }
                    break;
                }
                if (pair->initiator_slot == snapshot->own_slot) {
                    int64_t pair_end_ms;

                    anchor_run_initiator_pair(snapshot, pair_index,
                                              wave_start_ms);
                    pair_end_ms = k_uptime_get();
                    status_debug_printf(
                        "DBG_SURVEY_PROFILE kind=pair r=I p=%u "
                        "cfg=%lld cfg_end=%lld end=%lld slack=%lld\n",
                        pair_index,
                        (long long)(configure_end_ms - configure_start_ms),
                        (long long)(configure_end_ms -
                                    (int64_t)wave_start_ms),
                        (long long)(pair_end_ms - (int64_t)wave_start_ms),
                        (long long)((int64_t)(wave_start_ms +
                                             SURVEY_RANGE_WAVE_MS) -
                                    pair_end_ms));
                    break;
                }
            }
        }
        for (uint8_t i = 0u; i < snapshot->local_result_count; i++) {
            if (survey_range_result_encode(&snapshot->local_results[i],
                                           records[i].bytes) == 0u) {
                ret = -EBADMSG;
                goto out;
            }
        }
        ret = anchor_run_response_lane(
            snapshot->identity.generation, SURVEY_RESPONSE_RANGES,
            lane_start_ms, snapshot->parent_id, snapshot->hop_count,
            snapshot->identity.assignment.max_hop_count,
            records, snapshot->local_result_count);
        if (ret < 0 && ret != -ECANCELED) {
            break;
        }
    }
out:
    {
        int release_ret = anchor_radio_release(&lease, "survey-ranging");

        return release_ret < 0 ? release_ret : ret;
    }
}

static void anchor_work_handler(struct k_work *work)
{
    struct app_survey_anchor_state snapshot;
    enum app_survey_anchor_action action;
    int ret = 0;

    ARG_UNUSED(work);
    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked((uint64_t)k_uptime_get());
    snapshot = anchor_state;
    action = anchor_state.action;
    anchor_state.action = APP_SURVEY_ANCHOR_ACTION_NONE;
    if (snapshot.active && !snapshot.aborted &&
        (action == APP_SURVEY_ANCHOR_ACTION_NEIGHBORS ||
         action == APP_SURVEY_ANCHOR_ACTION_EXECUTE) &&
        !protocol_rx_lifecycle_rf_begin(
            &anchor_rx_lifecycle,
            PROTOCOL_RX_OPERATION_SURVEY,
            snapshot.identity.generation)) {
        anchor_rx_terminate_locked(true);
        snapshot.active = false;
    }
    k_mutex_unlock(&survey_lock);

    if (!snapshot.active || snapshot.aborted) {
        return;
    }
    if (action == APP_SURVEY_ANCHOR_ACTION_NEIGHBORS) {
        ret = anchor_neighbor_sequence(&snapshot);
        k_mutex_lock(&survey_lock, K_FOREVER);
        if (anchor_state.active &&
            anchor_state.identity.generation == snapshot.identity.generation) {
            if (protocol_rx_lifecycle_rf_end(
                    &anchor_rx_lifecycle,
                    PROTOCOL_RX_OPERATION_SURVEY,
                    snapshot.identity.generation)) {
                anchor_state.action = APP_SURVEY_ANCHOR_ACTION_EXPIRE;
                (void)anchor_work_reschedule(anchor_state.self_stop_ms);
            } else {
                anchor_rx_terminate_locked(true);
            }
        }
        k_mutex_unlock(&survey_lock);
    } else if (action == APP_SURVEY_ANCHOR_ACTION_EXECUTE) {
        ret = anchor_execute_plan(&snapshot);
        k_mutex_lock(&survey_lock, K_FOREVER);
        if (anchor_state.active &&
            anchor_state.identity.generation == snapshot.identity.generation) {
            if (ret < 0 && ret != -ECANCELED) {
                anchor_rx_terminate_locked(true);
            } else if (snapshot.plan.final_batch) {
                anchor_rx_terminate_locked(false);
            } else if (protocol_rx_lifecycle_rf_end(
                           &anchor_rx_lifecycle,
                           PROTOCOL_RX_OPERATION_SURVEY,
                           snapshot.identity.generation)) {
                anchor_state.plan_valid = false;
                anchor_state.local_result_count = 0u;
                anchor_state.next_batch_index =
                    (uint8_t)(snapshot.plan.batch_index + 1u);
                anchor_state.action = APP_SURVEY_ANCHOR_ACTION_EXPIRE;
                (void)anchor_work_reschedule(anchor_state.self_stop_ms);
            } else {
                anchor_rx_terminate_locked(true);
            }
        }
        k_mutex_unlock(&survey_lock);
    } else if (action == APP_SURVEY_ANCHOR_ACTION_EXPIRE) {
        k_mutex_lock(&survey_lock, K_FOREVER);
        if (anchor_state.identity.generation == snapshot.identity.generation &&
            (uint64_t)k_uptime_get() >= anchor_state.self_stop_ms) {
            anchor_rx_terminate_locked(false);
        }
        k_mutex_unlock(&survey_lock);
    }
    status_debug_printf("DBG_SURVEY_ANCHOR_WORK gen=%u action=%u ret=%d\n",
                        snapshot.identity.generation,
                        (unsigned int)action, ret);
    (void)anchor_uwb_scan_schedule_ms(0u);
}

static void gateway_build_progress_event_locked(
    struct survey_event *event,
    enum survey_event_kind kind)
{
    bool complete = kind == SURVEY_EVENT_BATCH_COMPLETE ||
                    kind == SURVEY_EVENT_TERMINAL;

    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->identity = gateway_state.identity;
    event->batch_index = gateway_state.plan_build.plan.batch_index;
    event->final_batch = gateway_state.plan_build.plan.final_batch;
    event->partial_reasons = gateway_state.partial_reasons;
    for (uint8_t pair = 0u;
         pair < gateway_state.plan_build.plan.pair_count; pair++) {
        if (response_bit_get(gateway_state.result_received_mask, pair)) {
            event->records.results[event->result_count++] =
                gateway_state.records.results[pair];
        } else if (complete) {
            event->records.results[event->result_count++] =
                (struct survey_range_result) {
                    .pair_index = pair,
                    .responder_slot = gateway_state.plan_build.plan
                        .pairs[pair].responder_slot,
                    .median_mm = SURVEY_NO_MEDIAN_MM,
                };
            event->partial_reasons |=
                SURVEY_PARTIAL_MISSING_RANGE_RESULT;
        }
    }
    for (uint8_t pair = 0u; pair < event->result_count; pair++) {
        if (survey_range_status(&event->records.results[pair]) !=
            SURVEY_RANGE_RESULT_USABLE) {
            event->partial_reasons |=
                event->records.results[pair].success_count == 0u ?
                    SURVEY_PARTIAL_MISSING_RANGE_RESULT :
                    SURVEY_PARTIAL_INSUFFICIENT_RANGE;
        }
    }
    event->status = event->partial_reasons == 0u ?
        SURVEY_TERMINAL_COMPLETE : SURVEY_TERMINAL_PARTIAL;
}

static void gateway_terminal_publish(struct survey_event *event)
{
    struct app_survey_ops ops;

    k_mutex_lock(&survey_lock, K_FOREVER);
    gateway_state.last_event = *event;
    gateway_state.active = false;
    gateway_state.stage = APP_SURVEY_GATEWAY_TERMINAL;
    ops = survey_ops;
    k_mutex_unlock(&survey_lock);
    if (ops.emit_event != NULL) {
        (void)ops.emit_event(event);
    }
    if (ops.gateway_terminal != NULL) {
        ops.gateway_terminal();
    }
}

static void gateway_begin_cleanup_locked(const struct survey_event *event)
{
    gateway_state.last_event = *event;
    gateway_state.stage = APP_SURVEY_GATEWAY_CLEANUP;
    /* Control submission is not an all-anchor delivery quorum. ABORT lets
     * receivers release early, but only the advertised self-stop proves that
     * every possible listener has relinquished survey ownership. */
    gateway_state.cleanup_deadline_ms = gateway_state.self_stop_ms;
    (void)gateway_work_reschedule(gateway_state.cleanup_deadline_ms);
    status_debug_printf(
        "DBG_SURVEY_CLEANUP g=%u release=%llu self=%llu\n",
        gateway_state.identity.generation,
        (unsigned long long)gateway_state.cleanup_deadline_ms,
        (unsigned long long)gateway_state.self_stop_ms);
}

static bool gateway_cleanup_event_if_due_locked(uint64_t now_ms,
                                                struct survey_event *event)
{
    if (gateway_state.stage != APP_SURVEY_GATEWAY_CLEANUP ||
        now_ms < gateway_state.cleanup_deadline_ms) {
        return false;
    }
    *event = gateway_state.last_event;
    return true;
}

static void gateway_work_handler(struct k_work *work)
{
    struct survey_control abort_control;
    struct survey_event event;
    struct app_survey_ops ops;
    struct survey_identity cleanup_identity;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t control_origin_ms = 0u;
    uint64_t pending_deadline_ms = 0u;
    uint32_t pending_handle = 0u;
    uint32_t abort_handle = 0u;
    enum app_survey_gateway_stage pending_stage =
        APP_SURVEY_GATEWAY_IDLE;
    bool queue_abort = false;
    bool publish = false;
    bool publish_signals = false;
    bool terminal = false;
    int abort_ret = -ENOTSUP;
    int control_ret = -ENOTSUP;

    ARG_UNUSED(work);
    memset(&abort_control, 0, sizeof(abort_control));
    memset(&event, 0, sizeof(event));
    memset(&cleanup_identity, 0, sizeof(cleanup_identity));
    k_mutex_lock(&survey_lock, K_FOREVER);
    ops = survey_ops;
    if (gateway_state.active &&
        (gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_START_RF ||
         gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN_RF)) {
        pending_stage = gateway_state.stage;
        pending_handle = gateway_state.pending_control_handle;
        pending_deadline_ms = gateway_state.pending_control_deadline_ms;
    }
    k_mutex_unlock(&survey_lock);
    if (pending_handle != 0u) {
        if (ops.control_origin != NULL) {
            control_ret = ops.control_origin(pending_handle,
                                             &control_origin_ms);
        }
        now_ms = (uint64_t)k_uptime_get();
        if (control_ret == -EAGAIN && now_ms < pending_deadline_ms) {
            (void)gateway_work_reschedule(now_ms + APP_SURVEY_RADIO_RETRY_MS);
            return;
        }
        if (control_ret == -EAGAIN) {
            control_ret = -ETIMEDOUT;
        }
        k_mutex_lock(&survey_lock, K_FOREVER);
        if (!gateway_state.active || gateway_state.stage != pending_stage ||
            gateway_state.pending_control_handle != pending_handle) {
            k_mutex_unlock(&survey_lock);
            if (control_ret != 0 && ops.control_abandon != NULL) {
                (void)ops.control_abandon(pending_handle);
            }
            return;
        }
        gateway_state.pending_control_handle = 0u;
        gateway_state.pending_control_deadline_ms = 0u;
        if (control_ret != 0 || control_origin_ms == 0u) {
            event.kind = SURVEY_EVENT_TERMINAL;
            event.status = SURVEY_TERMINAL_ABORTED;
            event.identity = gateway_state.identity;
            event.partial_reasons = gateway_state.partial_reasons;
            gateway_state.self_stop_ms = now_ms +
                                         gateway_state.control_delivery_ms;
            gateway_begin_cleanup_locked(&event);
            k_mutex_unlock(&survey_lock);
            if (ops.control_abandon != NULL) {
                (void)ops.control_abandon(pending_handle);
            }
            status_debug_printf(
                "DBG_SURVEY_CONTROL_RF_FAIL phase=%u gen=%u handle=%u "
                "ret=%d\n",
                pending_stage == APP_SURVEY_GATEWAY_WAIT_START_RF ?
                    SURVEY_PHASE_NEIGHBOR_START : SURVEY_PHASE_PLAN,
                event.identity.generation, pending_handle, control_ret);
            if (ops.wake_gateway_rx != NULL) {
                ops.wake_gateway_rx();
            }
            return;
        }
        status_debug_printf(
            "DBG_SURVEY_CONTROL_RF phase=%u gen=%u handle=%u rf=%llu\n",
            pending_stage == APP_SURVEY_GATEWAY_WAIT_START_RF ?
                SURVEY_PHASE_NEIGHBOR_START : SURVEY_PHASE_PLAN,
            gateway_state.identity.generation, pending_handle,
            (unsigned long long)control_origin_ms);
        if (pending_stage == APP_SURVEY_GATEWAY_WAIT_START_RF) {
            gateway_state.operation_origin_ms = control_origin_ms;
            gateway_state.hard_deadline_ms = control_origin_ms +
                                             SURVEY_HARD_CAP_MS;
            gateway_state.self_stop_ms = control_origin_ms +
                                          SURVEY_INITIAL_SELF_EXPIRY_MS;
            gateway_state.response_lane_start_ms = control_origin_ms +
                gateway_state.control_delivery_ms +
                survey_neighbor_sequence_duration_ms(
                    gateway_state.identity.assignment.slot_span) +
                SURVEY_RESULT_PREPARE_MS;
            gateway_state.response_lane_end_ms =
                gateway_state.response_lane_start_ms +
                survey_result_lane_duration_ms(
                    gateway_state.identity.assignment.max_hop_count);
            gateway_state.stage = APP_SURVEY_GATEWAY_NEIGHBORS;
            (void)gateway_work_reschedule(
                gateway_state.response_lane_end_ms);
        } else {
            const struct survey_plan *plan = &gateway_state.plan_build.plan;

            memset(&gateway_state.records, 0, sizeof(gateway_state.records));
            memset(gateway_state.result_received_mask, 0,
                   sizeof(gateway_state.result_received_mask));
            gateway_state.execution_start_ms = control_origin_ms +
                                                plan->execution_start_delay_ms;
            gateway_state.self_stop_ms = control_origin_ms +
                                         plan->self_stop_delay_ms;
            gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;
            gateway_state.stride_index = 0u;
            gateway_state.response_kind = SURVEY_RESPONSE_RANGES;
            gateway_state.response_lane_start_ms =
                gateway_state.execution_start_ms + SURVEY_RANGE_WAVE_MS +
                SURVEY_RESULT_PREPARE_MS;
            gateway_state.response_lane_end_ms =
                gateway_state.response_lane_start_ms +
                survey_result_lane_duration_ms(
                    gateway_state.identity.assignment.max_hop_count);
            if (gateway_state.plan_build.skipped_count != 0u) {
                gateway_state.partial_reasons |=
                    SURVEY_PARTIAL_SKIPPED_PLAN_ENTRY;
            }
            if (plan->pair_count == 0u) {
                gateway_state.partial_reasons |=
                    SURVEY_PARTIAL_NO_EXECUTABLE_PAIRS;
            }
            event.kind = SURVEY_EVENT_PLAN_ACCEPTED;
            event.identity = gateway_state.identity;
            event.batch_index = plan->batch_index;
            event.final_batch = plan->final_batch;
            event.plan = *plan;
            memcpy(event.skipped, gateway_state.plan_build.skipped,
                   (size_t)gateway_state.plan_build.skipped_count *
                       sizeof(event.skipped[0]));
            event.skipped_count = gateway_state.plan_build.skipped_count;
            event.partial_reasons = gateway_state.partial_reasons;
            event.status = event.partial_reasons == 0u ?
                SURVEY_TERMINAL_COMPLETE : SURVEY_TERMINAL_PARTIAL;
            gateway_state.last_event = event;
            (void)gateway_work_reschedule(
                gateway_state.response_lane_end_ms);
            publish = true;
        }
        k_mutex_unlock(&survey_lock);
        if (publish && ops.emit_event != NULL) {
            (void)ops.emit_event(&event);
        }
        if (ops.wake_gateway_rx != NULL) {
            ops.wake_gateway_rx();
        }
        return;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    ops = survey_ops;
    if (!gateway_state.active) {
        k_mutex_unlock(&survey_lock);
        return;
    }
    if (gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP) {
        terminal = gateway_cleanup_event_if_due_locked(now_ms, &event);
        if (!terminal) {
            (void)gateway_work_reschedule(
                gateway_state.cleanup_deadline_ms);
        }
        k_mutex_unlock(&survey_lock);
        if (terminal) {
            gateway_terminal_publish(&event);
        } else if (ops.wake_gateway_rx != NULL) {
            ops.wake_gateway_rx();
        }
        return;
    }
    if (now_ms >= gateway_state.hard_deadline_ms ||
        now_ms >= gateway_state.self_stop_ms) {
        gateway_build_progress_event_locked(&event, SURVEY_EVENT_TERMINAL);
        gateway_begin_cleanup_locked(&event);
        terminal = gateway_cleanup_event_if_due_locked(now_ms, &event);
        k_mutex_unlock(&survey_lock);
        if (terminal) {
            gateway_terminal_publish(&event);
        }
        return;
    }

    if (gateway_state.stage == APP_SURVEY_GATEWAY_NEIGHBORS &&
        now_ms >= gateway_state.response_lane_end_ms) {
        uint64_t occupied = gateway_state.graph.occupied_slot_mask;

        if ((gateway_state.graph.received_report_mask & occupied) != occupied) {
            gateway_state.partial_reasons |=
                SURVEY_PARTIAL_MISSING_NEIGHBOR_REPORT;
        }
        for (uint8_t first = 0u; first < SURVEY_MAX_ANCHORS; first++) {
            for (uint8_t second = (uint8_t)(first + 1u);
                 second < SURVEY_MAX_ANCHORS; second++) {
                bool forward = survey_graph_observed(
                    &gateway_state.graph, first, second);
                bool reverse = survey_graph_observed(
                    &gateway_state.graph, second, first);

                if (forward != reverse) {
                    gateway_state.partial_reasons |=
                        SURVEY_PARTIAL_ASYMMETRIC_NEIGHBOR;
                }
            }
        }
        event.kind = SURVEY_EVENT_NEIGHBOR_GRAPH;
        event.identity = gateway_state.identity;
        event.graph = gateway_state.graph;
        event.partial_reasons = gateway_state.partial_reasons;
        event.status = event.partial_reasons == 0u ?
            SURVEY_TERMINAL_COMPLETE : SURVEY_TERMINAL_PARTIAL;
        gateway_state.last_event = event;
        gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN;
        gateway_state.plan_deadline_ms = now_ms +
                                         SURVEY_HOST_PLAN_TIMEOUT_MS;
        gateway_state.self_stop_ms = MIN(
            gateway_state.operation_origin_ms + SURVEY_INITIAL_SELF_EXPIRY_MS,
            gateway_state.hard_deadline_ms);
        (void)gateway_work_reschedule(gateway_state.plan_deadline_ms);
        publish = true;
        publish_signals = gateway_state.signal_count > 0u;
    } else if (gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN &&
               now_ms >= gateway_state.plan_deadline_ms) {
        gateway_state.partial_reasons |=
            SURVEY_PARTIAL_NO_EXECUTABLE_PAIRS;
        event.kind = SURVEY_EVENT_TERMINAL;
        event.identity = gateway_state.identity;
        event.status = SURVEY_TERMINAL_PARTIAL;
        event.partial_reasons = gateway_state.partial_reasons;
        cleanup_identity = gateway_state.identity;
        abort_control.phase = SURVEY_PHASE_ABORT;
        abort_control.identity = cleanup_identity;
        gateway_begin_cleanup_locked(&event);
        queue_abort = true;
    } else if (gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING &&
               now_ms >= gateway_state.response_lane_end_ms) {
        uint8_t total_strides =
            gateway_state.plan_build.plan.wave_count +
            SURVEY_EXTRA_DRAIN_STRIDES;

        gateway_build_progress_event_locked(
            &event, SURVEY_EVENT_RANGE_PROGRESS);
        gateway_state.last_event = event;
        status_debug_printf(
            "DBG_SURVEY_STRIDE g=%u s=%u got=%u want=%u late=%lld\n",
            gateway_state.identity.generation, gateway_state.stride_index,
            event.result_count, gateway_state.plan_build.plan.pair_count,
            (long long)(now_ms - gateway_state.response_lane_end_ms));
        gateway_state.stride_index++;
        if (gateway_state.stride_index >= total_strides) {
            if (gateway_state.plan_build.plan.final_batch) {
                gateway_build_progress_event_locked(
                    &event, SURVEY_EVENT_TERMINAL);
                gateway_begin_cleanup_locked(&event);
            } else {
                gateway_build_progress_event_locked(
                    &event, SURVEY_EVENT_BATCH_COMPLETE);
                gateway_state.last_event = event;
                gateway_state.next_batch_index =
                    (uint8_t)(gateway_state.plan_build.plan.batch_index + 1u);
                gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN;
                gateway_state.plan_deadline_ms = now_ms +
                                                 SURVEY_HOST_PLAN_TIMEOUT_MS;
                (void)gateway_work_reschedule(
                    gateway_state.plan_deadline_ms);
                publish = true;
            }
        } else {
            uint32_t stride_ms = survey_wave_stride_ms(
                gateway_state.identity.assignment.max_hop_count);
            uint64_t wave_start = gateway_state.execution_start_ms +
                (uint64_t)gateway_state.stride_index * stride_ms;

            gateway_state.response_lane_start_ms = wave_start +
                SURVEY_RANGE_WAVE_MS + SURVEY_RESULT_PREPARE_MS;
            gateway_state.response_lane_end_ms =
                gateway_state.response_lane_start_ms +
                survey_result_lane_duration_ms(
                    gateway_state.identity.assignment.max_hop_count);
            (void)gateway_work_reschedule(
                gateway_state.response_lane_end_ms);
            publish = true;
        }
    }
    k_mutex_unlock(&survey_lock);

    if (queue_abort) {
        if (ops.send_control != NULL) {
            abort_ret = ops.send_control(&abort_control, &abort_handle);
        }
        if (abort_ret == 0 && ops.control_detach != NULL) {
            abort_ret = ops.control_detach(abort_handle);
        }
        if (abort_ret < 0 || abort_handle == 0u) {
            status_debug_printf(
                "DBG_SURVEY_CLEANUP_ABORT_FAIL g=%u ret=%d handle=%u\n",
                cleanup_identity.generation, abort_ret, abort_handle);
        }
        if (ops.wake_gateway_rx != NULL) {
            ops.wake_gateway_rx();
        }
        return;
    }
    if (publish && ops.emit_event != NULL) {
        int emit_ret = ops.emit_event(&event);

        if (emit_ret == 0 && publish_signals) {
            k_mutex_lock(&survey_lock, K_FOREVER);
            memset(&event, 0, sizeof(event));
            event.kind = SURVEY_EVENT_SIGNALS;
            event.identity = gateway_state.identity;
            event.status = SURVEY_TERMINAL_COMPLETE;
            for (uint8_t ordinal = 0u;
                 ordinal < SURVEY_MAX_SIGNAL_RECORDS; ordinal++) {
                if (signal_bit_get(gateway_state.signal_received_mask,
                                   ordinal)) {
                    event.records.signals[event.signal_count++] =
                        gateway_state.records.signals[ordinal];
                }
            }
            k_mutex_unlock(&survey_lock);
            (void)ops.emit_event(&event);
        }
    }
    if (ops.wake_gateway_rx != NULL) {
        ops.wake_gateway_rx();
    }
}

int app_survey_init(const struct app_survey_ops *ops)
{
    k_mutex_lock(&survey_lock, K_FOREVER);
    memset(&survey_ops, 0, sizeof(survey_ops));
    if (ops != NULL) {
        survey_ops = *ops;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    memset(&gateway_state, 0, sizeof(gateway_state));
#elif DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    memset(&anchor_state, 0, sizeof(anchor_state));
    protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
#endif
    k_mutex_unlock(&survey_lock);
#if DEVICE_ROLE == ROLE_GATEWAY
    k_work_init_delayable(&gateway_work, gateway_work_handler);
#elif DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    k_work_init_delayable(&anchor_work, anchor_work_handler);
#endif
    return 0;
}

int app_survey_gateway_start(
    const struct app_survey_gateway_roster *roster,
    struct survey_identity *identity_out)
{
    struct survey_control control;
    uint32_t delivery_handle = 0u;
    uint32_t generation;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || roster == NULL ||
        identity_out == NULL || roster->node_count == 0u ||
        roster->node_count > SURVEY_MAX_ANCHORS ||
        !survey_assignment_identity_valid(&roster->assignment) ||
        survey_ops.send_control == NULL ||
        survey_ops.control_origin == NULL ||
        survey_ops.control_abandon == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (gateway_state.active) {
        k_mutex_unlock(&survey_lock);
        return -EBUSY;
    }
    generation = gateway_state.identity.generation + 1u;
    if (generation == 0u) {
        generation = 1u;
    }
    memset(&gateway_state, 0, sizeof(gateway_state));
    gateway_state.identity.generation = generation;
    gateway_state.identity.assignment = roster->assignment;
    gateway_state.graph.occupied_slot_mask = 0u;
    for (size_t i = 0u; i < roster->node_count; i++) {
        uint8_t slot = roster->slots[i];

        if (roster->node_ids[i] == 0u ||
            slot >= roster->assignment.slot_span ||
            gateway_state.node_ids_by_slot[slot] != 0u) {
            memset(&gateway_state, 0, sizeof(gateway_state));
            k_mutex_unlock(&survey_lock);
            return -EINVAL;
        }
        gateway_state.node_ids_by_slot[slot] = roster->node_ids[i];
        gateway_state.hop_counts[slot] = roster->hop_counts[i];
        gateway_state.graph.occupied_slot_mask |= UINT64_C(1) << slot;
    }
    gateway_state.control_delivery_ms = survey_control_delivery_delay_ms(
        roster->assignment.max_hop_count);
    gateway_state.active = true;
    gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_START_RF;
    gateway_state.response_kind = SURVEY_RESPONSE_NEIGHBORS;
    *identity_out = gateway_state.identity;
    memset(&control, 0, sizeof(control));
    control.phase = SURVEY_PHASE_NEIGHBOR_START;
    control.identity = gateway_state.identity;
    control.start_delay_ms = gateway_state.control_delivery_ms;
    control.self_stop_delay_ms = SURVEY_INITIAL_SELF_EXPIRY_MS;
    control.start_delay_present = true;
    control.self_stop_delay_present = true;
    k_mutex_unlock(&survey_lock);

    ret = survey_ops.send_control(&control, &delivery_handle);
    if (ret < 0 || delivery_handle == 0u) {
        k_mutex_lock(&survey_lock, K_FOREVER);
        if (gateway_state.active &&
            gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_START_RF &&
            gateway_state.identity.generation == generation) {
            memset(&gateway_state, 0, sizeof(gateway_state));
        }
        k_mutex_unlock(&survey_lock);
        return ret < 0 ? ret : -EIO;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (!gateway_state.active ||
        gateway_state.stage != APP_SURVEY_GATEWAY_WAIT_START_RF ||
        gateway_state.identity.generation != generation) {
        k_mutex_unlock(&survey_lock);
        (void)survey_ops.control_abandon(delivery_handle);
        return -ESTALE;
    }
    gateway_state.pending_control_handle = delivery_handle;
    gateway_state.pending_control_deadline_ms =
        (uint64_t)k_uptime_get() + SURVEY_CONTROL_ORIGIN_BUDGET_MS;
    status_debug_printf(
        "DBG_SURVEY_BUDGET ph=1 g=%u ctrl=%u slots=%u prep=%u "
        "lane=%u total=%u\n",
        gateway_state.identity.generation, gateway_state.control_delivery_ms,
        survey_neighbor_sequence_duration_ms(roster->assignment.slot_span),
        SURVEY_RESULT_PREPARE_MS,
        survey_result_lane_duration_ms(roster->assignment.max_hop_count),
        gateway_state.control_delivery_ms +
            survey_neighbor_sequence_duration_ms(
                roster->assignment.slot_span) +
            SURVEY_RESULT_PREPARE_MS +
            survey_result_lane_duration_ms(
                roster->assignment.max_hop_count));
    (void)gateway_work_reschedule(
        (uint64_t)k_uptime_get() + APP_SURVEY_RADIO_RETRY_MS);
    k_mutex_unlock(&survey_lock);
    if (survey_ops.wake_gateway_rx != NULL) {
        survey_ops.wake_gateway_rx();
    }
    return 0;
}

int app_survey_gateway_submit_plan(
    const struct survey_host_plan_request *request,
    struct survey_plan_build_result *result_out)
{
    struct survey_plan_build_result built;
    struct survey_control control;
    uint32_t delivery_handle = 0u;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t elapsed_before_execution_ms;
    uint32_t execution_delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || request == NULL ||
        survey_ops.send_control == NULL ||
        survey_ops.control_origin == NULL ||
        survey_ops.control_abandon == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (!gateway_state.active ||
        gateway_state.stage != APP_SURVEY_GATEWAY_WAIT_PLAN) {
        k_mutex_unlock(&survey_lock);
        return -EBUSY;
    }
    if (!survey_identity_equal(&request->identity,
                               &gateway_state.identity)) {
        k_mutex_unlock(&survey_lock);
        return -ESTALE;
    }
    if (request->batch_index != gateway_state.next_batch_index ||
        request->batch_index >= SURVEY_MAX_BATCHES) {
        k_mutex_unlock(&survey_lock);
        return -ESTALE;
    }
    execution_delay_ms = gateway_state.control_delivery_ms;
    elapsed_before_execution_ms = now_ms -
        gateway_state.operation_origin_ms + execution_delay_ms;
    ret = survey_build_plan(&gateway_state.identity,
                            &gateway_state.graph,
                            gateway_state.hop_counts,
                            request->pairs,
                            request->pair_count,
                            execution_delay_ms,
                            request->batch_index,
                            request->final_batch,
                            &built);
    if (ret != PROTO_OK ||
        elapsed_before_execution_ms > UINT32_MAX ||
        !survey_plan_fits_hard_cap(
            (uint32_t)elapsed_before_execution_ms,
            built.plan.wave_count,
            gateway_state.identity.assignment.max_hop_count)) {
        k_mutex_unlock(&survey_lock);
        return ret == PROTO_OK ? -E2BIG : mesh_errno_from_proto(ret);
    }
    memset(&control, 0, sizeof(control));
    control.phase = SURVEY_PHASE_PLAN;
    control.identity = gateway_state.identity;
    control.plan = built.plan;
    control.plan_present = true;
    gateway_state.plan_build = built;
    gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN_RF;
    k_mutex_unlock(&survey_lock);

    ret = survey_ops.send_control(&control, &delivery_handle);
    if (ret < 0 || delivery_handle == 0u) {
        k_mutex_lock(&survey_lock, K_FOREVER);
        if (gateway_state.active &&
            gateway_state.stage == APP_SURVEY_GATEWAY_WAIT_PLAN_RF &&
            survey_identity_equal(&request->identity,
                                  &gateway_state.identity)) {
            gateway_state.stage = APP_SURVEY_GATEWAY_WAIT_PLAN;
            (void)gateway_work_reschedule(gateway_state.plan_deadline_ms);
        }
        k_mutex_unlock(&survey_lock);
        return ret < 0 ? ret : -EIO;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (!gateway_state.active ||
        gateway_state.stage != APP_SURVEY_GATEWAY_WAIT_PLAN_RF ||
        !survey_identity_equal(&request->identity,
                               &gateway_state.identity)) {
        k_mutex_unlock(&survey_lock);
        (void)survey_ops.control_abandon(delivery_handle);
        return -ESTALE;
    }
    gateway_state.pending_control_handle = delivery_handle;
    gateway_state.pending_control_deadline_ms =
        (uint64_t)k_uptime_get() + SURVEY_CONTROL_ORIGIN_BUDGET_MS;
    status_debug_printf(
        "DBG_SURVEY_BUDGET ph=2 g=%u ctrl=%u stride=%u waves=%u "
        "drain=%u total=%u\n",
        gateway_state.identity.generation, execution_delay_ms,
        survey_wave_stride_ms(
            gateway_state.identity.assignment.max_hop_count),
        built.plan.wave_count, SURVEY_EXTRA_DRAIN_STRIDES,
        execution_delay_ms + survey_execution_duration_ms(
            built.plan.wave_count,
            gateway_state.identity.assignment.max_hop_count));
    (void)gateway_work_reschedule(
        (uint64_t)k_uptime_get() + APP_SURVEY_RADIO_RETRY_MS);
    if (result_out != NULL) {
        *result_out = built;
    }
    k_mutex_unlock(&survey_lock);
    return 0;
}

int app_survey_gateway_abort(const struct survey_identity *identity)
{
    struct survey_control control;
    struct survey_event event;
    struct app_survey_ops ops;
    uint32_t pending_handle = 0u;
    uint32_t abort_handle = 0u;
    int ret = -ENOTSUP;

    if (DEVICE_ROLE != ROLE_GATEWAY || identity == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (!gateway_state.active) {
        k_mutex_unlock(&survey_lock);
        return -ENOENT;
    }
    if (!survey_identity_equal(identity, &gateway_state.identity)) {
        k_mutex_unlock(&survey_lock);
        return -ESTALE;
    }
    if (gateway_state.stage == APP_SURVEY_GATEWAY_CLEANUP) {
        k_mutex_unlock(&survey_lock);
        return 0;
    }
    memset(&control, 0, sizeof(control));
    control.phase = SURVEY_PHASE_ABORT;
    control.identity = gateway_state.identity;
    memset(&event, 0, sizeof(event));
    event.kind = SURVEY_EVENT_TERMINAL;
    event.status = SURVEY_TERMINAL_ABORTED;
    event.identity = gateway_state.identity;
    event.partial_reasons = gateway_state.partial_reasons;
    pending_handle = gateway_state.pending_control_handle;
    gateway_state.pending_control_handle = 0u;
    gateway_state.pending_control_deadline_ms = 0u;
    gateway_begin_cleanup_locked(&event);
    ops = survey_ops;
    k_mutex_unlock(&survey_lock);
    if (pending_handle != 0u && ops.control_abandon != NULL) {
        (void)ops.control_abandon(pending_handle);
    }
    if (ops.send_control != NULL) {
        ret = ops.send_control(&control, &abort_handle);
    }
    if (ret == 0 && ops.control_detach != NULL) {
        ret = ops.control_detach(abort_handle);
    }
    if (ret < 0 || abort_handle == 0u) {
        status_debug_printf(
            "DBG_SURVEY_CLEANUP_ABORT_FAIL g=%u ret=%d handle=%u\n",
            identity->generation, ret, abort_handle);
    }
    if (ops.wake_gateway_rx != NULL) {
        ops.wake_gateway_rx();
    }
    return 0;
}

int app_survey_gateway_status(struct survey_event *event_out)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || event_out == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (gateway_state.identity.generation == 0u ||
        gateway_state.last_event.kind == 0u) {
        k_mutex_unlock(&survey_lock);
        return -ENOENT;
    }
    *event_out = gateway_state.last_event;
    k_mutex_unlock(&survey_lock);
    return 0;
}

bool app_survey_gateway_active(void)
{
    bool active;

    k_mutex_lock(&survey_lock, K_FOREVER);
    active = gateway_state.active;
    k_mutex_unlock(&survey_lock);
    return active;
}

bool app_survey_gateway_response_window(uint64_t now_ms,
                                        uint64_t *round_deadline_ms)
{
    uint64_t prepare_ms;
    uint64_t deadline_ms;
    bool active = false;

    k_mutex_lock(&survey_lock, K_FOREVER);
    prepare_ms = gateway_state.response_lane_start_ms >
            APP_SURVEY_GATEWAY_PREPARE_MS ?
        gateway_state.response_lane_start_ms -
            APP_SURVEY_GATEWAY_PREPARE_MS : 0u;
    if (gateway_state.active &&
        (gateway_state.stage == APP_SURVEY_GATEWAY_NEIGHBORS ||
         gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING) &&
        now_ms >= prepare_ms && now_ms < gateway_state.response_lane_end_ms) {
        if (now_ms < gateway_state.response_lane_start_ms) {
            deadline_ms = gateway_state.response_lane_start_ms +
                          ENUMERATION_RESPONSE_ROUND_MS;
        } else {
            uint64_t elapsed = now_ms -
                               gateway_state.response_lane_start_ms;

            deadline_ms = now_ms -
                (elapsed % ENUMERATION_RESPONSE_ROUND_MS) +
                ENUMERATION_RESPONSE_ROUND_MS;
        }
        deadline_ms = MIN(deadline_ms, gateway_state.response_lane_end_ms);
        if (round_deadline_ms != NULL) {
            *round_deadline_ms = deadline_ms;
        }
        active = true;
    }
    k_mutex_unlock(&survey_lock);
    return active;
}

bool app_survey_gateway_response_pending_wait_ms(uint64_t now_ms,
                                                 uint32_t *wait_ms)
{
    uint64_t prepare_ms;
    bool pending = false;

    k_mutex_lock(&survey_lock, K_FOREVER);
    prepare_ms = gateway_state.response_lane_start_ms >
            APP_SURVEY_GATEWAY_PREPARE_MS ?
        gateway_state.response_lane_start_ms -
            APP_SURVEY_GATEWAY_PREPARE_MS : 0u;
    if (gateway_state.active &&
        (gateway_state.stage == APP_SURVEY_GATEWAY_NEIGHBORS ||
         gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING) &&
        now_ms < prepare_ms) {
        if (wait_ms != NULL) {
            *wait_ms = bounded_wait_ms(now_ms, prepare_ms);
        }
        pending = true;
    }
    k_mutex_unlock(&survey_lock);
    return pending;
}

bool app_survey_gateway_radio_quiet(uint64_t now_ms, uint32_t *wait_ms)
{
    uint64_t wake_ms = 0u;
    bool quiet = false;

    k_mutex_lock(&survey_lock, K_FOREVER);
    if (gateway_state.active &&
        (gateway_state.stage == APP_SURVEY_GATEWAY_NEIGHBORS ||
         gateway_state.stage == APP_SURVEY_GATEWAY_EXECUTING)) {
        uint64_t prepare_ms = gateway_state.response_lane_start_ms >
                APP_SURVEY_GATEWAY_PREPARE_MS ?
            gateway_state.response_lane_start_ms -
                APP_SURVEY_GATEWAY_PREPARE_MS : 0u;

        if (now_ms < prepare_ms) {
            quiet = true;
            wake_ms = prepare_ms;
        } else if (now_ms >= gateway_state.response_lane_end_ms) {
            quiet = true;
            wake_ms = now_ms + 1u;
        }
    }
    if (quiet && wait_ms != NULL) {
        *wait_ms = MAX(bounded_wait_ms(now_ms, wake_ms), 1u);
    }
    k_mutex_unlock(&survey_lock);
    return quiet;
}

int app_survey_gateway_handle_bundle(
    const struct survey_response_bundle *bundle,
    uint64_t received_at_ms,
    struct survey_response_hop_ack *ack)
{
    struct enumeration_response_timing timing;
    int ret = 0;

    if (DEVICE_ROLE != ROLE_GATEWAY || bundle == NULL || ack == NULL ||
        bundle->network_id != NETWORK_ID || bundle->parent_id != DEVICE_ID) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (!gateway_state.active || bundle->generation !=
            gateway_state.identity.generation ||
        bundle->kind != gateway_state.response_kind ||
        !enumeration_response_timing_at_depth(
            gateway_state.response_lane_start_ms, received_at_ms,
            gateway_state.identity.assignment.max_hop_count, &timing)) {
        ret = -ESTALE;
        goto out;
    }
    for (uint8_t i = 0u; i < bundle->record_count; i++) {
        if (bundle->kind == SURVEY_RESPONSE_NEIGHBORS) {
            if (bundle->records[i].bytes[0] < SURVEY_MAX_ANCHORS) {
                struct survey_neighbor_report report;

                if (survey_neighbor_report_decode(
                        bundle->records[i].bytes,
                        sizeof(bundle->records[i].bytes), &report) !=
                        PROTO_OK ||
                    survey_graph_note_report(&gateway_state.graph, &report) !=
                        PROTO_OK) {
                    ret = -EBADMSG;
                    goto out;
                }
            } else {
                struct survey_signal_record signal;
                uint8_t owner;
                uint8_t base;
                uint8_t levels[SURVEY_SIGNAL_LEVELS_PER_RECORD];
                uint8_t ordinal = (uint8_t)(
                    bundle->records[i].bytes[0] - SURVEY_MAX_ANCHORS);

                memcpy(signal.bytes, bundle->records[i].bytes,
                       sizeof(signal.bytes));
                if (ordinal >= SURVEY_MAX_SIGNAL_RECORDS ||
                    survey_signal_record_decode(&signal, &owner, &base,
                                                levels) != PROTO_OK ||
                    (gateway_state.graph.occupied_slot_mask &
                     (UINT64_C(1) << owner)) == 0u) {
                    ret = -EBADMSG;
                    goto out;
                }
                if (signal_bit_get(gateway_state.signal_received_mask,
                                   ordinal)) {
                    if (memcmp(&gateway_state.records.signals[ordinal],
                               &signal, sizeof(signal)) != 0) {
                        ret = -ESTALE;
                        goto out;
                    }
                } else {
                    gateway_state.records.signals[ordinal] = signal;
                    signal_bit_set(gateway_state.signal_received_mask,
                                   ordinal);
                    gateway_state.signal_count++;
                }
            }
        } else {
            struct survey_range_result result;
            const struct survey_plan_pair *pair;

            if (survey_range_result_decode(bundle->records[i].bytes,
                                           sizeof(bundle->records[i].bytes),
                                           &result) != PROTO_OK ||
                result.pair_index >=
                    gateway_state.plan_build.plan.pair_count) {
                ret = -EBADMSG;
                goto out;
            }
            pair = &gateway_state.plan_build.plan.pairs[result.pair_index];
            if (result.responder_slot != pair->responder_slot) {
                ret = -ESTALE;
                goto out;
            }
            if (response_bit_get(gateway_state.result_received_mask,
                                 result.pair_index)) {
                if (memcmp(&gateway_state.records.results[result.pair_index],
                           &result, sizeof(result)) != 0) {
                    ret = -ESTALE;
                    goto out;
                }
            } else {
                gateway_state.records.results[result.pair_index] = result;
                response_bit_set(gateway_state.result_received_mask,
                                 result.pair_index);
                status_debug_printf(
                    "DBG_SURVEY_RESULT_RX g=%u p=%u s=%u t=%llu "
                    "d=%u r=%u ok=%u\n",
                    gateway_state.identity.generation, result.pair_index,
                    gateway_state.stride_index,
                    (unsigned long long)(received_at_ms -
                        gateway_state.response_lane_start_ms),
                    timing.depth, timing.round, result.success_count);
            }
        }
    }
    *ack = (struct survey_response_hop_ack) {
        .network_id = NETWORK_ID,
        .generation = gateway_state.identity.generation,
        .parent_id = DEVICE_ID,
        .child_id = bundle->sender_id,
        .kind = bundle->kind,
        .sequence = bundle->sequence,
    };
out:
    k_mutex_unlock(&survey_lock);
    return ret;
}

int app_survey_anchor_note_ram_roster(
    const struct discovery_assignment_entry *entries,
    size_t entry_count,
    uint8_t table_slot_count,
    uint32_t assignment_epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment)
{
    if (DEVICE_ROLE != ROLE_ANCHOR || entries == NULL ||
        table_commitment == NULL || entry_count == 0u ||
        entry_count > SURVEY_MAX_ANCHORS || table_slot_count == 0u ||
        table_slot_count > SURVEY_MAX_ANCHORS || assignment_epoch == 0u ||
        table_command_seq == 0u) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (anchor_state.active) {
        k_mutex_unlock(&survey_lock);
        return -EBUSY;
    }
    memset(&anchor_state, 0, sizeof(anchor_state));
    protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
    anchor_state.roster_assignment_epoch = assignment_epoch;
    anchor_state.roster_table_command_seq = table_command_seq;
    anchor_state.roster_table_commitment = *table_commitment;
    for (size_t i = 0u; i < entry_count; i++) {
        uint8_t occupied_slot_span;

        if (entries[i].anchor_id == 0u ||
            entries[i].slot >= table_slot_count ||
            anchor_state.node_ids_by_slot[entries[i].slot] != 0u) {
            memset(&anchor_state, 0, sizeof(anchor_state));
            protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
            k_mutex_unlock(&survey_lock);
            return -EINVAL;
        }
        occupied_slot_span = survey_slot_span_include(
            anchor_state.slot_span, entries[i].slot);
        if (occupied_slot_span == 0u) {
            memset(&anchor_state, 0, sizeof(anchor_state));
            protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
            k_mutex_unlock(&survey_lock);
            return -EINVAL;
        }
        anchor_state.slot_span = occupied_slot_span;
        anchor_state.node_ids_by_slot[entries[i].slot] =
            entries[i].anchor_id;
        anchor_state.occupied_slot_mask |= UINT64_C(1) << entries[i].slot;
    }
    anchor_state.roster_valid = true;
    k_mutex_unlock(&survey_lock);
    return 0;
}

void app_survey_anchor_clear_ram_roster(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    if (!anchor_state.active) {
        memset(&anchor_state, 0, sizeof(anchor_state));
        protocol_rx_lifecycle_init(&anchor_rx_lifecycle);
    }
    k_mutex_unlock(&survey_lock);
}

int app_survey_anchor_apply_control(const struct proto_packet *packet,
                                    const struct survey_control *control)
{
    uint64_t parent_id = 0u;
    uint8_t hop_count = 0u;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint64_t start_ms = 0u;
    uint64_t stop_ms = 0u;
    int64_t starts_in_ms;
    int64_t stops_in_ms;
    uint32_t assignment_epoch = 0u;
    uint32_t table_seq = 0u;
    struct discovery_assignment_table_commitment commitment;
    uint8_t own_slot = 0u;
    uint8_t table_slot_count = 0u;
    int ret = 0;

    if (DEVICE_ROLE != ROLE_ANCHOR || packet == NULL || control == NULL ||
        packet->msg_type != MSG_COMMAND || packet->src_id != GATEWAY_ID) {
        return -EINVAL;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked(now_ms);
    if (!anchor_identity_matches_roster(&control->identity.assignment)) {
        k_mutex_unlock(&survey_lock);
        return -ESTALE;
    }
    if (control->phase == SURVEY_PHASE_NEIGHBOR_START) {
        if (!control->start_delay_present ||
            !control->self_stop_delay_present ||
            !local_anchor_discovery_assignment_identity_get(
                &assignment_epoch, &table_seq, &commitment,
                &own_slot, &table_slot_count) ||
            assignment_epoch != control->identity.assignment.assignment_epoch ||
            table_seq != control->identity.assignment.table_command_seq ||
            table_slot_count == 0u || own_slot >= table_slot_count ||
            !discovery_assignment_table_commitment_equal(
                &commitment,
                &control->identity.assignment.table_commitment) ||
            anchor_state.node_ids_by_slot[own_slot] != DEVICE_ID) {
            ret = -ESTALE;
            goto out;
        }
        if (survey_ops.anchor_upstream == NULL ||
            survey_ops.anchor_upstream(&parent_id, &hop_count) < 0 ||
            parent_id == 0u || hop_count == 0u ||
            hop_count > control->identity.assignment.max_hop_count) {
            ret = -EHOSTUNREACH;
            goto out;
        }
        if (!enumeration_response_claim_start(
                now_ms, packet->message_age_ms, control->start_delay_ms,
                &start_ms, &starts_in_ms) ||
            !enumeration_response_claim_start(
                now_ms, packet->message_age_ms,
                control->self_stop_delay_ms, &stop_ms, &stops_in_ms) ||
            stop_ms <= start_ms) {
            ret = -ESTALE;
            goto out;
        }
        if (anchor_state.active) {
            ret = survey_identity_equal(&anchor_state.identity,
                                        &control->identity) &&
                  close_u64(anchor_state.neighbor_start_ms, start_ms,
                            APP_SURVEY_START_EDGE_SLOP_MS) ? 0 : -EBUSY;
            goto out;
        }
        if (protocol_rx_lifecycle_begin(
                &anchor_rx_lifecycle,
                PROTOCOL_RX_OPERATION_SURVEY,
                control->identity.generation,
                (uint32_t)now_ms,
                (uint32_t)stop_ms) != PROTOCOL_RX_BEGIN_ACCEPTED) {
            ret = -EBUSY;
            goto out;
        }
        if (survey_ops.anchor_consume_enumeration_handoff == NULL) {
            anchor_rx_terminate_locked(false);
            ret = -ENOTSUP;
            goto out;
        }
        ret = survey_ops.anchor_consume_enumeration_handoff(
            control->identity.assignment.assignment_epoch);
        if (ret < 0) {
            anchor_rx_terminate_locked(false);
            goto out;
        }
        anchor_state.identity = control->identity;
        anchor_state.neighbor_start_ms = start_ms;
        anchor_state.self_stop_ms = stop_ms;
        anchor_state.parent_id = parent_id;
        anchor_state.hop_count = hop_count;
        anchor_state.own_slot = own_slot;
        anchor_state.active = true;
        anchor_state.aborted = false;
        anchor_state.action = APP_SURVEY_ANCHOR_ACTION_NEIGHBORS;
        status_debug_printf(
            "DBG_SURVEY_TIMING ph=1 g=%u sl=%u h=%u n=%llu age=%u "
            "o=%llu d=%u s=%llu in=%lld\n",
            control->identity.generation, own_slot, hop_count,
            (unsigned long long)now_ms, packet->message_age_ms,
            (unsigned long long)(now_ms - packet->message_age_ms),
            control->start_delay_ms, (unsigned long long)start_ms,
            (long long)starts_in_ms);
        ret = anchor_work_reschedule(start_ms > APP_SURVEY_ANCHOR_PREPARE_MS ?
            start_ms - APP_SURVEY_ANCHOR_PREPARE_MS : start_ms);
        if (ret < 0) {
            anchor_rx_terminate_locked(false);
        }
    } else if (!anchor_state.active ||
               !survey_identity_equal(&anchor_state.identity,
                                      &control->identity)) {
        ret = -ESTALE;
    } else if (control->phase == SURVEY_PHASE_PLAN) {
        if (!control->plan_present ||
            !survey_plan_commitment_valid(&control->plan)) {
            ret = -EBADMSG;
            goto out;
        }
        if (!enumeration_response_claim_start(
                now_ms, packet->message_age_ms,
                control->plan.execution_start_delay_ms,
                &start_ms, &starts_in_ms) ||
            !enumeration_response_claim_start(
                now_ms, packet->message_age_ms,
                control->plan.self_stop_delay_ms,
                &stop_ms, &stops_in_ms) || stop_ms <= start_ms) {
            ret = -ESTALE;
            goto out;
        }
        for (uint8_t i = 0u; i < control->plan.pair_count; i++) {
            if (anchor_state.node_ids_by_slot[
                    control->plan.pairs[i].initiator_slot] == 0u ||
                anchor_state.node_ids_by_slot[
                    control->plan.pairs[i].responder_slot] == 0u) {
                ret = -ESTALE;
                goto out;
            }
        }
        if (anchor_state.plan_valid) {
            ret = semantic_digest_equal(anchor_state.plan.commitment,
                                        control->plan.commitment,
                                        SEMANTIC_DIGEST_SHA256_LEN) &&
                  close_u64(anchor_state.execution_start_ms, start_ms,
                            APP_SURVEY_START_EDGE_SLOP_MS) ? 0 : -ESTALE;
            goto out;
        }
        if (control->plan.batch_index != anchor_state.next_batch_index) {
            ret = -ESTALE;
            goto out;
        }
        if (survey_ops.anchor_upstream == NULL ||
            survey_ops.anchor_upstream(&parent_id, &hop_count) < 0 ||
            parent_id == 0u || hop_count == 0u ||
            hop_count >
                anchor_state.identity.assignment.max_hop_count) {
            ret = -EHOSTUNREACH;
            goto out;
        }
        if (!protocol_rx_lifecycle_set_deadline(
                &anchor_rx_lifecycle,
                PROTOCOL_RX_OPERATION_SURVEY,
                control->identity.generation,
                (uint32_t)now_ms,
                (uint32_t)stop_ms)) {
            ret = -ESTALE;
            goto out;
        }
        anchor_state.plan = control->plan;
        anchor_state.execution_start_ms = start_ms;
        anchor_state.self_stop_ms = stop_ms;
        anchor_state.parent_id = parent_id;
        anchor_state.hop_count = hop_count;
        anchor_state.plan_valid = true;
        anchor_state.action = APP_SURVEY_ANCHOR_ACTION_EXECUTE;
        status_debug_printf(
            "DBG_SURVEY_TIMING ph=2 g=%u sl=%u h=%u n=%llu age=%u "
            "o=%llu d=%u s=%llu in=%lld p=%u w=%u\n",
            control->identity.generation, anchor_state.own_slot, hop_count,
            (unsigned long long)now_ms, packet->message_age_ms,
            (unsigned long long)(now_ms - packet->message_age_ms),
            control->plan.execution_start_delay_ms,
            (unsigned long long)start_ms, (long long)starts_in_ms,
            control->plan.pair_count, control->plan.wave_count);
        ret = anchor_work_reschedule(
            start_ms > APP_SURVEY_ANCHOR_PREPARE_MS ?
                start_ms - APP_SURVEY_ANCHOR_PREPARE_MS : start_ms);
        if (ret < 0) {
            anchor_rx_terminate_locked(true);
        }
    } else if (control->phase == SURVEY_PHASE_ABORT) {
        anchor_rx_terminate_locked(true);
        (void)k_work_cancel_delayable(&anchor_work);
        dwm3000_driver_request_receive_abort(
            DWM3000_RECEIVE_ABORT_GATEWAY_PRIORITY);
    } else {
        ret = -EINVAL;
    }
out:
    k_mutex_unlock(&survey_lock);
    return ret;
}

bool app_survey_anchor_active(void)
{
    bool active;

    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked((uint64_t)k_uptime_get());
    active = anchor_state.active && !anchor_state.aborted;
    k_mutex_unlock(&survey_lock);
    return active;
}

bool app_survey_anchor_rx_continuous(void)
{
    bool continuous;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked((uint64_t)k_uptime_get());
    continuous = anchor_state.active && !anchor_state.aborted &&
                 anchor_rx_lifecycle.operation ==
                     PROTOCOL_RX_OPERATION_SURVEY &&
                 anchor_rx_lifecycle.generation ==
                     anchor_state.identity.generation &&
                 anchor_rx_lifecycle.mode ==
                     PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5;
    k_mutex_unlock(&survey_lock);
    return continuous;
}

bool app_survey_anchor_radio_work_pending(uint64_t now_ms,
                                          uint32_t *wait_ms_out)
{
    uint64_t start_ms = 0u;
    uint64_t prepare_ms;
    bool pending = false;

    if (DEVICE_ROLE != ROLE_ANCHOR || wait_ms_out == NULL) {
        return false;
    }
    *wait_ms_out = 0u;
    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked(now_ms);
    if (anchor_state.active && !anchor_state.aborted) {
        if (anchor_state.action == APP_SURVEY_ANCHOR_ACTION_NEIGHBORS) {
            start_ms = anchor_state.neighbor_start_ms;
            pending = true;
        } else if (anchor_state.action == APP_SURVEY_ANCHOR_ACTION_EXECUTE) {
            start_ms = anchor_state.execution_start_ms;
            pending = true;
        }
    }
    if (pending) {
        prepare_ms = start_ms > APP_SURVEY_ANCHOR_PREPARE_MS ?
            start_ms - APP_SURVEY_ANCHOR_PREPARE_MS : start_ms;
        if (now_ms < prepare_ms) {
            uint64_t wait_ms = prepare_ms - now_ms;

            *wait_ms_out = wait_ms > UINT32_MAX ?
                UINT32_MAX : (uint32_t)wait_ms;
        }
    }
    k_mutex_unlock(&survey_lock);
    return pending;
}

enum protocol_rx_recovery_result app_survey_anchor_rx_note_recovery(
    bool recovered)
{
    enum protocol_rx_recovery_result result = PROTOCOL_RX_RECOVERY_INVALID;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return result;
    }
    k_mutex_lock(&survey_lock, K_FOREVER);
    (void)anchor_rx_expire_locked((uint64_t)k_uptime_get());
    if (anchor_state.active && !anchor_state.aborted) {
        result = protocol_rx_lifecycle_note_rx_recovery(
            &anchor_rx_lifecycle,
            PROTOCOL_RX_OPERATION_SURVEY,
            anchor_state.identity.generation,
            recovered);
        if (result == PROTOCOL_RX_RECOVERY_TERMINATED) {
            anchor_rx_terminate_locked(true);
            (void)k_work_cancel_delayable(&anchor_work);
        }
    }
    k_mutex_unlock(&survey_lock);
    return result;
}
