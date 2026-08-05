#include "app_anchor_survey_result_delivery.h"
#include "app_mesh_persistence.h"
#include "app_mesh_route_state_persistence.h"
#include "app_nvs_storage.h"
#include "app_node_comm.h"
#include "protocol.h"
#include "survey.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

LOG_MODULE_REGISTER(app_anchor, LOG_LEVEL_DBG);

#define LOCAL_ID UINT64_C(0x1111222233334444)
#define OTHER_LOCAL_ID UINT64_C(0x1111222233334445)
#define GATEWAY_ID UINT64_C(0x9999888877776666)

static struct {
    struct app_node_comm_durable_attempt_ops durable_ops;
    struct node_comm_terminal_event terminal;
    struct mesh_outbound committed_outbound;
    uint32_t committed_handle;
    uint32_t next_handle;
    uint32_t reserve_calls;
    uint32_t commit_calls;
    uint32_t abandon_calls;
    uint32_t cancel_calls;
    uint32_t schedule_calls;
    uint32_t active_owner_checks;
    uint32_t active_owner_resume_calls;
    uint32_t watchdog_stop_calls;
    uint32_t fail_cancel_call;
    uint16_t active_owner_seq;
    int active_owner_result;
    int commit_result;
    int abandon_result;
    int schedule_result;
    int reentrant_service_result;
    bool reenter_service_on_commit;
    bool terminal_ready;
} result_delivery_mock;

int mesh_report_active_owner_matches_outbound(
    const struct mesh_outbound *outbound)
{
    zassert_not_null(outbound);
    result_delivery_mock.active_owner_checks++;
    return result_delivery_mock.active_owner_seq != 0u &&
           outbound->packet.seq == result_delivery_mock.active_owner_seq ?
        result_delivery_mock.active_owner_result : 0;
}

void mesh_report_resume_restored_outbox(const char *reason)
{
    zassert_not_null(reason);
    result_delivery_mock.active_owner_resume_calls++;
}

void status_debug_printf(const char *fmt, ...)
{
    ARG_UNUSED(fmt);
}

void app_watchdog_stop_feeding(void)
{
    result_delivery_mock.watchdog_stop_calls++;
}

int app_node_comm_register_durable_attempt_ops(
    const struct app_node_comm_durable_attempt_ops *ops)
{
    zassert_not_null(ops);
    result_delivery_mock.durable_ops = *ops;
    return 0;
}

int app_node_comm_reserve_durable_reliable_uplinks(
    size_t reservation_count,
    uint32_t *reservation_tokens,
    size_t reservation_token_capacity)
{
    zassert_equal(reservation_count, 1u);
    zassert_true(reservation_token_capacity >= reservation_count);
    zassert_not_null(reservation_tokens);
    result_delivery_mock.reserve_calls++;
    reservation_tokens[0] = UINT32_C(0xCAFE0001);
    return 0;
}

int app_node_comm_commit_durable_reliable_uplink_reservation(
    uint32_t reservation_token,
    const app_node_comm_envelope *envelope,
    uint64_t absolute_deadline_ms,
    uint32_t client_token,
    uint32_t *handle_out)
{
    ARG_UNUSED(client_token);
    zassert_not_equal(reservation_token, 0u);
    zassert_not_null(envelope);
    zassert_not_null(handle_out);
    zassert_equal(absolute_deadline_ms, UINT64_MAX);
    zassert_false(envelope->queued_at_valid);
    zassert_false(envelope->earliest_tx_valid);
    result_delivery_mock.commit_calls++;
    result_delivery_mock.committed_outbound = *envelope;
    if (result_delivery_mock.reenter_service_on_commit) {
        result_delivery_mock.reenter_service_on_commit = false;
        result_delivery_mock.reentrant_service_result =
            app_anchor_survey_result_delivery_service();
    }
    if (result_delivery_mock.commit_result < 0) {
        return result_delivery_mock.commit_result;
    }
    result_delivery_mock.next_handle++;
    if (result_delivery_mock.next_handle == 0u) {
        result_delivery_mock.next_handle = 1u;
    }
    result_delivery_mock.committed_handle =
        result_delivery_mock.next_handle;
    *handle_out = result_delivery_mock.committed_handle;
    return 0;
}

int app_node_comm_cancel_durable_reliable_uplink_reservation(
    uint32_t reservation_token)
{
    zassert_not_equal(reservation_token, 0u);
    result_delivery_mock.cancel_calls++;
    if (result_delivery_mock.cancel_calls ==
        result_delivery_mock.fail_cancel_call) {
        return -EIO;
    }
    return 0;
}

int app_node_comm_abandon_delivery(uint32_t handle)
{
    zassert_not_equal(handle, 0u);
    result_delivery_mock.abandon_calls++;
    return result_delivery_mock.abandon_result;
}

bool app_node_comm_peek_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out)
{
    if (!result_delivery_mock.terminal_ready ||
        handle != result_delivery_mock.committed_handle) {
        return false;
    }
    *event_out = result_delivery_mock.terminal;
    return true;
}

bool app_node_comm_take_delivery_event_for(
    uint32_t handle,
    struct node_comm_terminal_event *event_out)
{
    if (!app_node_comm_peek_delivery_event_for(handle, event_out)) {
        return false;
    }
    result_delivery_mock.terminal_ready = false;
    return true;
}

static int schedule_result_delivery_work(uint32_t delay_ms)
{
    ARG_UNUSED(delay_ms);
    result_delivery_mock.schedule_calls++;
    return result_delivery_mock.schedule_result;
}

static int capture_local_delivery_snapshot(
    void *ctx,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    struct app_mesh_local_delivery_snapshot *captured = ctx;

    zassert_not_null(captured);
    zassert_not_null(snapshot);
    *captured = *snapshot;
    return 0;
}

static struct mesh_outbound pair_result_outbound(uint8_t slot)
{
    return (struct mesh_outbound) {
        .packet = {
            .msg_type = MSG_SURVEY_PAIR_RESULT,
            .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            .payload_len = 1u,
            .session_id = (uint32_t)(100u + slot),
            .seq = (uint16_t)(10u + slot),
            .src_id = LOCAL_ID + slot,
            .dst_id = GATEWAY_ID,
        },
        .payload = {slot},
        .payload_len = 1u,
    };
}

static struct route_candidate direct_gateway_route(void)
{
    return (struct route_candidate) {
        .next_hop_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = 13u,
        .last_seen_ms = 1000u,
        .hop_count = 0u,
        .link_quality = 90u,
        .valid = true,
    };
}

static void build_gateway_ack_for_packet(
    struct proto_packet *ack,
    uint8_t *ack_payload,
    size_t ack_payload_cap,
    size_t *ack_payload_len,
    const struct proto_packet *acknowledged_packet,
    const uint8_t *acknowledged_payload,
    size_t acknowledged_payload_len)
{
    *ack_payload_len = 0u;
    zassert_ok(mesh_append_requested_seq(ack_payload,
                                         ack_payload_cap,
                                         ack_payload_len,
                                         acknowledged_packet->seq));
    zassert_ok(mesh_append_ack_semantic_identity(
        ack_payload,
        ack_payload_cap,
        ack_payload_len,
        acknowledged_packet,
        acknowledged_payload,
        acknowledged_payload_len));
    zassert_ok(mesh_init_gateway_ack(ack,
                                     GATEWAY_ID,
                                     LOCAL_ID,
                                     acknowledged_packet->session_id,
                                     1u,
                                     (uint8_t)*ack_payload_len));
}

static struct app_mesh_local_delivery_snapshot pair_result_snapshot(
    uint8_t slot)
{
    struct app_mesh_local_delivery_snapshot snapshot = {0};
    struct app_mesh_local_delivery delivery;
    struct app_mesh_local_delivery_ops ops = {
        .save = capture_local_delivery_snapshot,
        .ctx = &snapshot,
    };
    struct mesh_outbound outbound = pair_result_outbound(slot);

    app_mesh_local_delivery_init(&delivery, &ops);
    zassert_ok(app_mesh_local_delivery_stage(
        &delivery, &outbound, outbound.packet.session_id));
    zassert_true(app_mesh_local_delivery_snapshot_valid(&snapshot));
    return snapshot;
}

static void finalize_survey_generation_snapshot(
    struct app_mesh_survey_generation_snapshot *snapshot)
{
    snapshot->checksum = 0u;
    snapshot->checksum =
        proto_crc16_ccitt_false((const uint8_t *)snapshot,
                                sizeof(*snapshot));
}

static struct app_mesh_survey_generation_snapshot generation_snapshot(
    uint64_t local_id,
    uint64_t generation,
    uint8_t role)
{
    struct app_mesh_survey_generation_snapshot snapshot = {
        .magic = APP_MESH_SURVEY_GENERATION_SNAPSHOT_MAGIC,
        .local_id = local_id,
        .gateway_id = GATEWAY_ID,
        .generation = generation,
        .version = APP_MESH_SURVEY_GENERATION_SNAPSHOT_VERSION,
        .size = sizeof(snapshot),
        .role = role,
        .valid = 1u,
    };

    finalize_survey_generation_snapshot(&snapshot);
    return snapshot;
}

static void *mesh_persistence_anchor_suite_setup(void)
{
    const struct flash_area *area = NULL;
    int ret;

    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
    zassert_ok(ret);
    zassert_not_null(area);
    if (ret < 0 || area == NULL) {
        return NULL;
    }
    ret = flash_area_erase(area, 0u, area->fa_size);
    flash_area_close(area);
    zassert_ok(ret);
    app_mesh_persistence_test_reset_faults();
    return NULL;
}

ZTEST(mesh_persistence_anchor,
      test_route_state_round_trip_binds_role_identity_and_checksum)
{
    struct mesh_relay relay;
    struct mesh_relay restored;
    uint8_t corrupt[APP_NVS_ROUTE_STATE_RECORD_SIZE];
    ssize_t io_len;

    zassert_ok(app_mesh_route_state_clear(MESH_RELAY_ROLE_ANCHOR));
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_ok(app_mesh_route_state_persist(&relay, 37u, 101u));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&restored), 1);
    zassert_equal(restored.upstream.current_epoch, 37u);
    zassert_equal(restored.gateway_route_adv_seq, 101u);
    zassert_is_null(route_selected(&restored.upstream));

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    OTHER_LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&restored), -ESTALE);
    zassert_equal(restored.upstream.current_epoch, 1u);
    zassert_equal(restored.gateway_route_adv_seq, 0u);

    io_len = nvs_read(app_nvs_storage_fs(),
                      APP_NVS_ID_MESH_ROUTE_STATE_ANCHOR,
                      corrupt,
                      sizeof(corrupt));
    zassert_equal(io_len, sizeof(corrupt));
    corrupt[20] ^= 1u;
    io_len = nvs_write(app_nvs_storage_fs(),
                       APP_NVS_ID_MESH_ROUTE_STATE_ANCHOR,
                       corrupt,
                       sizeof(corrupt));
    zassert_true(io_len == 0 || io_len == sizeof(corrupt));
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&restored), -EILSEQ);
    zassert_equal(restored.upstream.current_epoch, 1u);
    zassert_equal(restored.gateway_route_adv_seq, 0u);
    zassert_ok(app_mesh_route_state_clear(MESH_RELAY_ROLE_ANCHOR));
}

ZTEST(mesh_persistence_anchor,
      test_route_adv_crash_cut_and_capacity_failure_never_regress_freshness)
{
    struct mesh_relay gateway;
    struct mesh_relay anchor;
    struct mesh_relay rebooted;
    struct mesh_outbound seq_99;
    struct mesh_outbound seq_100;
    struct mesh_outbound seq_101;
    struct mesh_relay_result result;

    zassert_ok(app_mesh_route_state_clear(MESH_RELAY_ROLE_ANCHOR));
    mesh_relay_init(&gateway,
                    MESH_RELAY_ROLE_GATEWAY,
                    GATEWAY_ID,
                    GATEWAY_ID,
                    37u);
    zassert_ok(mesh_relay_build_gateway_route_adv(
        &gateway, 99u, 1000u, &seq_99));
    zassert_ok(mesh_relay_build_gateway_route_adv(
        &gateway, 100u, 1001u, &seq_100));
    zassert_ok(mesh_relay_build_gateway_route_adv(
        &gateway, 101u, 1002u, &seq_101));

    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_ok(app_mesh_route_state_persist(&anchor, 37u, 99u));
    zassert_equal(app_mesh_route_state_restore(&anchor), 1);
    zassert_ok(mesh_relay_handle_rx(
        &anchor,
        &seq_101.packet,
        seq_101.payload,
        seq_101.payload_len,
        GATEWAY_ID,
        90u,
        1010u,
        &result));
    zassert_equal(result.status, PROTO_OK);
    zassert_true(result.route_state_changed);
    zassert_false(result.route_state_durable);
    zassert_equal(anchor.gateway_route_adv_seq, 101u);

    /*
     * Power loss before the NVS write rolls back the entire provisional
     * admission: no policy or forward action crossed the application commit
     * boundary, so the intermediate sequence remains legitimately retryable.
     */
    mesh_relay_init(&rebooted,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&rebooted), 1);
    zassert_equal(rebooted.gateway_route_adv_seq, 99u);
    zassert_is_null(route_selected(&rebooted.upstream));
    zassert_ok(mesh_relay_handle_rx(
        &rebooted,
        &seq_100.packet,
        seq_100.payload,
        seq_100.payload_len,
        GATEWAY_ID,
        90u,
        1020u,
        &result));
    zassert_equal(result.status, PROTO_OK);
    zassert_ok(app_mesh_route_state_save(&rebooted));
    zassert_ok(mesh_relay_mark_route_state_durable(&rebooted, &result));

    /*
     * Model a post-commit forwarding-capacity failure. Even an attempted
     * transport rollback cannot regress core state after the durable marker.
     */
    zassert_ok(mesh_relay_rollback_forward_admission(
        &rebooted,
        &seq_100.packet,
        seq_100.payload,
        seq_100.payload_len,
        &result));
    zassert_equal(rebooted.gateway_route_adv_seq, 100u);
    zassert_ok(mesh_relay_handle_rx(
        &rebooted,
        &seq_99.packet,
        seq_99.payload,
        seq_99.payload_len,
        GATEWAY_ID,
        90u,
        1021u,
        &result));
    zassert_equal(result.status, PROTO_ERR_STALE);
    zassert_equal(rebooted.gateway_route_adv_seq, 100u);

    mesh_relay_init(&anchor,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&anchor), 1);
    zassert_equal(anchor.gateway_route_adv_seq, 100u);
    zassert_ok(mesh_relay_handle_rx(
        &anchor,
        &seq_99.packet,
        seq_99.payload,
        seq_99.payload_len,
        GATEWAY_ID,
        90u,
        1030u,
        &result));
    zassert_equal(result.status, PROTO_ERR_STALE);
    zassert_equal(anchor.gateway_route_adv_seq, 100u);
    zassert_ok(app_mesh_route_state_clear(MESH_RELAY_ROLE_ANCHOR));
}

ZTEST(mesh_persistence_anchor,
      test_route_state_must_restore_before_epoch_bound_outbox)
{
    const uint8_t report_payload[] = {0x51u, 0x52u};
    struct route_candidate route = direct_gateway_route();
    struct proto_packet report = {
        .msg_type = MSG_SELF_TEST_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = LOCAL_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x12344321),
        .seq = 17u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(report_payload),
    };
    struct mesh_relay source;
    struct mesh_relay wrong_order;
    struct mesh_relay restored;
    struct mesh_outbound tx;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_outbox());
    zassert_ok(app_mesh_route_state_clear(MESH_RELAY_ROLE_ANCHOR));
    mesh_relay_init(&source,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    route.route_epoch);
    zassert_ok(route_upsert_candidate(&source.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&source,
                                   &report,
                                   report_payload,
                                   sizeof(report_payload),
                                   2000u,
                                   &tx));
    zassert_ok(app_mesh_persistence_save_outbox(&source, 2001u));

    mesh_relay_init(&wrong_order,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&wrong_order), 0);
    zassert_equal(app_mesh_persistence_restore_outbox(
                      &wrong_order, 3000u),
                  -EINVAL);
    zassert_false(mesh_relay_tx_active(&wrong_order));

    /*
     * The failed restore retains the sole-custody bytes. An explicit,
     * identity-bound migration can install the independently established
     * epoch and make the same record restorable; boot never infers that epoch
     * from the outbox itself.
     */
    zassert_ok(app_mesh_route_state_save(&source));
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    1u);
    zassert_equal(app_mesh_route_state_restore(&restored), 1);
    zassert_equal(restored.upstream.current_epoch, route.route_epoch);
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 3001u));
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(restored.pending.packet.session_id, report.session_id);

    zassert_ok(app_mesh_persistence_clear_outbox());
    zassert_ok(app_mesh_route_state_clear(MESH_RELAY_ROLE_ANCHOR));
}

ZTEST(mesh_persistence_anchor,
      test_anchor_generation_high_water_and_corruption)
{
    struct app_mesh_survey_generation_snapshot snapshot;
    uint64_t generation = UINT64_MAX;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_test_delete_survey_generation_snapshot());
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        0);
    zassert_equal(generation, 0u);

    zassert_ok(app_mesh_persistence_advance_anchor_survey_generation(
        LOCAL_ID, GATEWAY_ID, 5u));
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        1);
    zassert_equal(generation, 5u);
    zassert_ok(app_mesh_persistence_advance_anchor_survey_generation(
        LOCAL_ID, GATEWAY_ID, 5u));
    zassert_equal(
        app_mesh_persistence_advance_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, 4u),
        -ESTALE);
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        1);
    zassert_equal(generation, 5u);
    zassert_equal(
        app_mesh_persistence_advance_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, UINT64_C(0x100000000)),
        -EINVAL);
    zassert_ok(app_mesh_persistence_advance_anchor_survey_generation(
        LOCAL_ID, GATEWAY_ID, UINT64_C(0x100000001)));
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        1);
    zassert_equal(generation, UINT64_C(0x100000001));

    snapshot = generation_snapshot(LOCAL_ID, 7u, 3u);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    snapshot = generation_snapshot(OTHER_LOCAL_ID, 7u, DEVICE_ROLE);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    snapshot = generation_snapshot(LOCAL_ID, 7u, DEVICE_ROLE);
    snapshot.gateway_id ^= 1u;
    finalize_survey_generation_snapshot(&snapshot);
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    snapshot = generation_snapshot(LOCAL_ID, 7u, DEVICE_ROLE);
    snapshot.checksum ^= 1u;
    zassert_ok(app_mesh_persistence_test_write_survey_generation_snapshot(
        &snapshot, sizeof(snapshot)));
    generation = UINT64_MAX;
    zassert_equal(
        app_mesh_persistence_restore_anchor_survey_generation(
            LOCAL_ID, GATEWAY_ID, &generation),
        -EINVAL);
    zassert_equal(generation, 0u);

    zassert_ok(app_mesh_persistence_test_delete_survey_generation_snapshot());
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_four_slot_custody_and_corruption_fail_closed)
{
    struct app_mesh_local_delivery_snapshot expected[
        APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS];
    struct app_mesh_local_delivery_snapshot restored;
    const uint32_t corrupt_record = UINT32_C(0xBAD0C0DE);

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
        expected[slot] = pair_result_snapshot(slot);
        zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
            slot, &expected[slot]));
    }

    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        memset(&restored, 0xA5, sizeof(restored));
        zassert_equal(
            app_mesh_persistence_restore_survey_pair_result_delivery(
                slot, &restored),
            1);
        zassert_mem_equal(&restored, &expected[slot], sizeof(restored));
    }
    zassert_equal(
        app_mesh_persistence_save_survey_pair_result_delivery(
            APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS, &expected[0]),
        -EINVAL);
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS, &restored),
        -EINVAL);
    zassert_equal(
        app_mesh_persistence_clear_survey_pair_result_delivery(
            APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS),
        -EINVAL);

    zassert_ok(
        app_mesh_persistence_test_write_survey_pair_result_delivery_raw(
            2u, &corrupt_record, sizeof(corrupt_record)));
    memset(&restored, 0xA5, sizeof(restored));
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            2u, &restored),
        -EBADMSG);
    zassert_mem_equal(
        &restored,
        &(struct app_mesh_local_delivery_snapshot){0},
        sizeof(restored));
    /*
     * Restore validation must never erase the bad bytes: a second boot must
     * fail closed in exactly the same way until an explicit repair occurs.
     */
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            2u, &restored),
        -EBADMSG);

    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_clear_survey_pair_result_delivery(slot));
        memset(&restored, 0xA5, sizeof(restored));
        zassert_equal(
            app_mesh_persistence_restore_survey_pair_result_delivery(
                slot, &restored),
            0);
        zassert_mem_equal(
            &restored,
            &(struct app_mesh_local_delivery_snapshot){0},
            sizeof(restored));
    }
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_stage_blocks_reentrant_second_admission)
{
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    struct mesh_outbound outbound = pair_result_outbound(0u);
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));
    result_delivery_mock.next_handle = 100u;
    result_delivery_mock.reenter_service_on_commit = true;

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    zassert_ok(app_anchor_survey_result_delivery_stage_reserved(
        UINT32_C(0xABCDEF01), &outbound));
    zassert_equal(result_delivery_mock.reentrant_service_result, 0);
    zassert_equal(result_delivery_mock.commit_calls, 1u);
    zassert_equal(result_delivery_mock.abandon_calls, 0u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);

    /*
     * The reconstructed original callback is allowed only after the generic
     * ACK_CONFIRM reaches its own terminal gateway ACK.  It persists the
     * outer acknowledgement, while terminal polling owns the later delete.
     */
    zassert_true(mesh_packet_semantic_digest(
        &outbound.packet,
        outbound.payload,
        outbound.payload_len,
        semantic_digest));
    semantic_digest[0] ^= 1u;
    zassert_equal(
        app_anchor_survey_result_delivery_gateway_confirmed(
            &outbound.packet, semantic_digest),
        -EBADMSG);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
    semantic_digest[0] ^= 1u;
    zassert_ok(app_anchor_survey_result_delivery_gateway_confirmed(
        &outbound.packet, semantic_digest));
    result_delivery_mock.terminal = (struct node_comm_terminal_event) {
        .handle = result_delivery_mock.committed_handle,
        .reason = NODE_COMM_TERMINAL_DELIVERED,
        .attempts_started = 1u,
    };
    result_delivery_mock.terminal_ready = true;
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_false(result_delivery_mock.terminal_ready);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 0u);
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            0u,
            &(struct app_mesh_local_delivery_snapshot){0}),
        0);
}

ZTEST(mesh_persistence_anchor,
      test_ack_confirm_restore_supersedes_original_click_handoff)
{
    const uint8_t report_payload[] = {0x42u, 0x17u, 0xA5u};
    struct route_candidate route = direct_gateway_route();
    struct proto_packet report = {0};
    struct proto_packet gateway_ack = {0};
    struct proto_packet restored_original = {0};
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_relay after_terminal;
    struct mesh_outbound tx;
    struct mesh_relay_result ack_result;
    uint8_t gateway_ack_payload[MESH_ACK_SINGLE_PAYLOAD_LEN];
    uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN];
    size_t gateway_ack_payload_len = 0u;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_outbox());
    zassert_ok(app_mesh_persistence_rollback_click_handoff());
    report = (struct proto_packet) {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = LOCAL_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x33445566),
        .seq = 7u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(report_payload),
    };
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&relay,
                                   &report,
                                   report_payload,
                                   sizeof(report_payload),
                                   1000u,
                                   &tx));
    zassert_ok(app_mesh_persistence_save_outbox(&relay, 1001u));
    zassert_ok(app_mesh_persistence_stage_click_handoff(&relay, 1002u));
    zassert_ok(app_mesh_persistence_commit_click_handoff(&relay, 1003u));

    build_gateway_ack_for_packet(&gateway_ack,
                                 gateway_ack_payload,
                                 sizeof(gateway_ack_payload),
                                 &gateway_ack_payload_len,
                                 &report,
                                 report_payload,
                                 sizeof(report_payload));
    zassert_ok(mesh_relay_handle_rx(&relay,
                                    &gateway_ack,
                                    gateway_ack_payload,
                                    gateway_ack_payload_len,
                                    GATEWAY_ID,
                                    90u,
                                    1010u,
                                    &ack_result));
    zassert_true((ack_result.actions &
                  MESH_RELAY_ACTION_GATEWAY_ACK_CONFIRM_PENDING) != 0u);
    zassert_equal(relay.pending.packet.msg_type, MSG_GATEWAY_ACK_CONFIRM);
    zassert_ok(app_mesh_persistence_save_outbox(&relay, 1011u));

    /* Reset at this cut point must restore the successor confirmation while
     * retaining the older producer journal until terminal proof arrives. */
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 2000u));
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(restored.pending.packet.msg_type,
                  MSG_GATEWAY_ACK_CONFIRM);
    zassert_ok(mesh_gateway_ack_confirm_identity_packet(
        &restored.pending.packet,
        restored.pending.payload,
        restored.pending.payload_len,
        &restored_original,
        original_digest));
    zassert_mem_equal(&restored_original, &report, sizeof(report));
    zassert_ok(app_mesh_persistence_complete_confirmed_producer(
        &restored_original, original_digest));
    zassert_ok(app_mesh_persistence_complete_confirmed_producer(
        &restored_original, original_digest));

    /* Terminal primary clear must not uncover the already-confirmed original
     * on the following boot. */
    zassert_ok(app_mesh_persistence_clear_outbox());
    mesh_relay_init(&after_terminal,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(app_mesh_persistence_restore_outbox(&after_terminal, 3000u));
    zassert_false(mesh_relay_tx_active(&after_terminal));
}

ZTEST(mesh_persistence_anchor,
      test_ack_confirm_digest_mismatch_retains_original_click_handoff)
{
    const uint8_t report_payload[] = {0x61u, 0x62u, 0x63u};
    struct route_candidate route = direct_gateway_route();
    struct proto_packet report = {0};
    struct proto_packet restored_original = {0};
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_outbound tx;
    uint8_t original_digest[SEMANTIC_DIGEST_SHA256_LEN];

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_outbox());
    zassert_ok(app_mesh_persistence_rollback_click_handoff());
    report = (struct proto_packet) {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = LOCAL_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x66778899),
        .seq = 9u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(report_payload),
    };
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&relay,
                                   &report,
                                   report_payload,
                                   sizeof(report_payload),
                                   4000u,
                                   &tx));
    zassert_ok(app_mesh_persistence_stage_click_handoff(&relay, 4001u));
    zassert_ok(app_mesh_persistence_commit_click_handoff(&relay, 4002u));
    zassert_true(mesh_packet_semantic_digest(&report,
                                             report_payload,
                                             sizeof(report_payload),
                                             original_digest));
    restored_original = report;
    original_digest[0] ^= 0x80u;
    zassert_equal(app_mesh_persistence_complete_confirmed_producer(
                      &restored_original, original_digest),
                  -EBADMSG);

    /* A failed proof leaves the producer owner available as reset fallback. */
    zassert_ok(app_mesh_persistence_clear_outbox());
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 5000u));
    zassert_true(mesh_relay_tx_active(&restored));
    zassert_equal(restored.pending.packet.msg_type, MSG_CLICK_REPORT);
    zassert_ok(app_mesh_persistence_rollback_click_handoff());
}

ZTEST(mesh_persistence_anchor,
      test_terminal_self_test_retires_only_exact_producer_handoff)
{
    const uint8_t report_payload[] = {0x71u, 0x72u, 0x73u, 0x74u};
    uint8_t wrong_payload[sizeof(report_payload)];
    struct route_candidate route = direct_gateway_route();
    struct proto_packet report = {
        .msg_type = MSG_SELF_TEST_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = LOCAL_ID,
        .dst_id = GATEWAY_ID,
        .session_id = UINT32_C(0x88776655),
        .seq = 11u,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = sizeof(report_payload),
    };
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_outbound tx;

    zassert_ok(app_mesh_persistence_init());
    zassert_ok(app_mesh_persistence_clear_outbox());
    zassert_ok(app_mesh_persistence_rollback_click_handoff());
    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_tx(&relay,
                                   &report,
                                   report_payload,
                                   sizeof(report_payload),
                                   6000u,
                                   &tx));
    zassert_ok(app_mesh_persistence_save_outbox(&relay, 6001u));
    zassert_ok(app_mesh_persistence_stage_click_handoff(&relay, 6002u));
    zassert_ok(app_mesh_persistence_commit_click_handoff(&relay, 6003u));

    memcpy(wrong_payload, report_payload, sizeof(wrong_payload));
    wrong_payload[0] ^= 0x80u;
    zassert_equal(app_mesh_persistence_complete_terminal_producer(
                      &report, wrong_payload, sizeof(wrong_payload)),
                  -EBADMSG);
    zassert_ok(app_mesh_persistence_complete_terminal_producer(
        &report, report_payload, sizeof(report_payload)));
    zassert_ok(app_mesh_persistence_complete_terminal_producer(
        &report, report_payload, sizeof(report_payload)));

    zassert_ok(app_mesh_persistence_clear_outbox());
    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    13u);
    zassert_ok(app_mesh_persistence_restore_outbox(&restored, 7000u));
    zassert_false(mesh_relay_tx_active(&restored));
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_restore_blocks_reentrant_second_admission)
{
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    const struct app_mesh_local_delivery_snapshot snapshot =
        pair_result_snapshot(0u);
    const struct mesh_outbound outbound = snapshot.outbound;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool restored = false;

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
        0u, &snapshot));
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));
    result_delivery_mock.next_handle = 200u;

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    result_delivery_mock.reenter_service_on_commit = true;
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(result_delivery_mock.reentrant_service_result, 0);
    zassert_equal(result_delivery_mock.reserve_calls, 1u);
    zassert_equal(result_delivery_mock.commit_calls, 1u);
    zassert_equal(result_delivery_mock.abandon_calls, 0u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);

    zassert_true(mesh_packet_semantic_digest(
        &outbound.packet,
        outbound.payload,
        outbound.payload_len,
        semantic_digest));
    zassert_ok(app_anchor_survey_result_delivery_gateway_confirmed(
        &outbound.packet, semantic_digest));
    result_delivery_mock.terminal = (struct node_comm_terminal_event) {
        .handle = result_delivery_mock.committed_handle,
        .reason = NODE_COMM_TERMINAL_DELIVERED,
        .attempts_started = 1u,
    };
    result_delivery_mock.terminal_ready = true;
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 0u);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_prequeue_schedule_failure_retains_boot_custody)
{
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    const struct app_mesh_local_delivery_snapshot snapshot =
        pair_result_snapshot(0u);
    bool restored = false;

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
        0u, &snapshot));
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));
    result_delivery_mock.schedule_result = -ENODEV;

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    zassert_equal(app_anchor_survey_result_delivery_start(), -ENODEV);
    zassert_equal(result_delivery_mock.commit_calls, 0u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);

    result_delivery_mock.schedule_result = 0;
    zassert_ok(app_anchor_survey_result_delivery_start());
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(result_delivery_mock.commit_calls, 1u);
    zassert_mem_equal(&result_delivery_mock.committed_outbound,
                      &snapshot.outbound,
                      sizeof(snapshot.outbound));
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_restore_suppresses_only_exact_active_owner)
{
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    struct app_mesh_local_delivery_snapshot snapshots[
        APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS];
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    bool restored = false;

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
        snapshots[slot] = pair_result_snapshot(slot);
        zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
            slot, &snapshots[slot]));
    }
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));
    result_delivery_mock.next_handle = 300u;
    result_delivery_mock.active_owner_seq =
        snapshots[2].outbound.packet.seq;
    result_delivery_mock.active_owner_result = 1;

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(result_delivery_mock.reserve_calls, 3u);
    zassert_equal(result_delivery_mock.commit_calls, 3u);
    zassert_equal(result_delivery_mock.active_owner_resume_calls, 1u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(),
                  APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS);

    zassert_true(mesh_packet_semantic_digest(
        &snapshots[2].outbound.packet,
        snapshots[2].outbound.payload,
        snapshots[2].outbound.payload_len,
        semantic_digest));
    zassert_ok(app_anchor_survey_result_delivery_gateway_confirmed(
        &snapshots[2].outbound.packet, semantic_digest));
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(),
                  APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS - 1u);

    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_transport_release_requires_exact_durable_owner)
{
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    struct app_mesh_local_delivery_snapshot snapshot =
        pair_result_snapshot(0u);
    struct app_mesh_local_delivery_snapshot persisted;
    struct proto_packet collision = snapshot.outbound.packet;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t attempt_token = 0u;
    bool restored = false;

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
        0u, &snapshot));
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    zassert_ok(result_delivery_mock.durable_ops.begin(
        &snapshot.outbound.packet, &attempt_token));
    zassert_not_equal(attempt_token, 0u);
    zassert_true(mesh_packet_semantic_digest(
        &snapshot.outbound.packet,
        snapshot.outbound.payload,
        snapshot.outbound.payload_len,
        semantic_digest));

    semantic_digest[0] ^= 1u;
    zassert_equal(
        app_anchor_survey_result_delivery_transport_released(
            &snapshot.outbound.packet, semantic_digest, true),
        -EBADMSG);
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            0u, &persisted),
        1);
    zassert_equal(persisted.state, APP_MESH_LOCAL_DELIVERY_STARTING);

    semantic_digest[0] ^= 1u;
    zassert_ok(app_anchor_survey_result_delivery_transport_released(
        &snapshot.outbound.packet, semantic_digest, true));
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            0u, &persisted),
        1);
    zassert_equal(persisted.state, APP_MESH_LOCAL_DELIVERY_PREEMPTED);

    collision.seq++;
    zassert_equal(
        app_anchor_survey_result_delivery_transport_released(
            &collision, semantic_digest, false),
        -ESTALE);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_retry_cancel_failure_forces_recovery)
{
    const struct app_anchor_survey_result_delivery_ops ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    const struct app_mesh_local_delivery_snapshot snapshot =
        pair_result_snapshot(0u);
    bool restored = false;

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
        0u, &snapshot));
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));
    result_delivery_mock.commit_result = -EIO;
    result_delivery_mock.fail_cancel_call = 1u;

    zassert_ok(app_anchor_survey_result_delivery_init(&ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    zassert_equal(app_anchor_survey_result_delivery_service(), -EIO);
    zassert_equal(result_delivery_mock.reserve_calls, 1u);
    zassert_equal(result_delivery_mock.commit_calls, 1u);
    zassert_equal(result_delivery_mock.cancel_calls, 1u);
    zassert_equal(result_delivery_mock.watchdog_stop_calls, 1u);
    zassert_equal(result_delivery_mock.abandon_calls, 0u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_failed_restore_rearms_nonexpiring_custody)
{
    const struct app_anchor_survey_result_delivery_ops result_ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    struct app_mesh_local_delivery_snapshot snapshot =
        pair_result_snapshot(0u);
    struct app_mesh_local_delivery delivery;
    struct app_mesh_local_delivery_ops snapshot_ops = {
        .save = capture_local_delivery_snapshot,
        .ctx = &snapshot,
    };
    bool restored = false;

    app_mesh_local_delivery_init(&delivery, &snapshot_ops);
    zassert_ok(app_mesh_local_delivery_restore(&delivery, &snapshot));
    zassert_ok(app_mesh_local_delivery_note_failed(&delivery));
    zassert_equal(snapshot.state, APP_MESH_LOCAL_DELIVERY_FAILED);

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
        0u, &snapshot));
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));

    zassert_ok(app_anchor_survey_result_delivery_init(&result_ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(result_delivery_mock.reserve_calls, 1u);
    zassert_equal(result_delivery_mock.commit_calls, 1u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            0u,
            &(struct app_mesh_local_delivery_snapshot){0}),
        1);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_transport_exhaustion_retains_exact_source)
{
    const struct app_anchor_survey_result_delivery_ops result_ops = {
        .schedule_work_ms = schedule_result_delivery_work,
        .active_owner_matches_outbound =
            mesh_report_active_owner_matches_outbound,
        .resume_restored_outbox = mesh_report_resume_restored_outbox,
    };
    const struct app_mesh_local_delivery_snapshot snapshot =
        pair_result_snapshot(0u);
    struct app_mesh_local_delivery_snapshot persisted;
    bool restored = false;

    zassert_ok(app_mesh_persistence_init());
    for (uint8_t slot = 0u;
         slot < APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS;
         slot++) {
        zassert_ok(
            app_mesh_persistence_test_delete_survey_pair_result_delivery(
                slot));
    }
    zassert_ok(app_mesh_persistence_save_survey_pair_result_delivery(
        0u, &snapshot));
    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));

    zassert_ok(app_anchor_survey_result_delivery_init(&result_ops));
    zassert_ok(app_anchor_survey_result_delivery_restore(&restored));
    zassert_true(restored);
    zassert_ok(app_anchor_survey_result_delivery_service());
    result_delivery_mock.terminal = (struct node_comm_terminal_event) {
        .handle = result_delivery_mock.committed_handle,
        .reason = NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
        .attempts_started = APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS,
    };
    result_delivery_mock.terminal_ready = true;
    zassert_equal(app_anchor_survey_result_delivery_service(), -EAGAIN);
    zassert_false(result_delivery_mock.terminal_ready);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
    zassert_equal(
        app_mesh_persistence_restore_survey_pair_result_delivery(
            0u, &persisted),
        1);
    zassert_equal(persisted.state, APP_MESH_LOCAL_DELIVERY_RETRY);

    zassert_ok(app_anchor_survey_result_delivery_service());
    zassert_equal(result_delivery_mock.commit_calls, 2u);
    zassert_equal(app_anchor_survey_result_delivery_occupied_count(), 1u);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_first_rf_stamp_and_restored_retry_preserve_age)
{
    const uint32_t first_admission_ms = 500000u;
    const uint32_t snapshot_ms = first_admission_ms + 123u;
    const uint32_t restore_ms = 900000u;
    struct survey_sample sample = {
        .pair = {
            .initiator_id = LOCAL_ID,
            .responder_id = OTHER_LOCAL_ID,
            .operation_generation = UINT64_C(0x100000001),
            .survey_id = 1u,
            .sample_count = 1u,
        },
        .round_id = 1u,
        .sample_index = 0u,
        .distance_mm = 3210,
        .quality = 95u,
        .range_status = RANGE_OK,
    };
    struct route_candidate route = direct_gateway_route();
    struct mesh_relay_outbox_snapshot snapshot;
    struct mesh_relay_result tick_result;
    struct mesh_relay relay;
    struct mesh_relay restored;
    struct mesh_outbound tx;
    struct proto_packet packet;
    uint8_t payload[SURVEY_SAMPLE_TLV_MAX_LEN];
    size_t payload_len = 0u;

    zassert_ok(survey_append_sample_tlvs(
        payload, sizeof(payload), &payload_len, &sample));
    zassert_ok(survey_init_result_packet_from_reporter(
        &packet,
        &sample,
        LOCAL_ID,
        GATEWAY_ID,
        77u,
        (uint8_t)payload_len));

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    route.route_epoch);
    zassert_ok(route_upsert_candidate(&relay.upstream, &route));
    zassert_ok(mesh_relay_start_tx(
        &relay,
        &packet,
        payload,
        payload_len,
        first_admission_ms,
        &tx));
    zassert_equal(relay.pending.queued_at_ms, first_admission_ms);
    zassert_equal(relay.pending.packet.message_age_ms, 0u);
    mesh_relay_note_tx_sent(&relay, &tx, first_admission_ms);

    zassert_ok(mesh_relay_export_outbox_snapshot(
        &relay, snapshot_ms, &snapshot));
    zassert_true(snapshot.valid);
    zassert_equal(snapshot.record.age_ms_saturating, 123u);
    zassert_equal(snapshot.pending.packet.message_age_ms, 123u);

    mesh_relay_init(&restored,
                    MESH_RELAY_ROLE_ANCHOR,
                    LOCAL_ID,
                    GATEWAY_ID,
                    route.route_epoch);
    zassert_ok(route_upsert_candidate(&restored.upstream, &route));
    zassert_ok(mesh_relay_restore_outbox_snapshot(
        &restored, &snapshot, restore_ms));
    zassert_equal(restored.pending.packet.message_age_ms, 123u);
    zassert_equal(restored.pending.queued_at_ms, restore_ms);
    zassert_ok(mesh_relay_tick(
        &restored,
        restore_ms + RELAY_BUSY_RETRY_MIN_MS,
        &tick_result));
    zassert_true((tick_result.actions &
                  MESH_RELAY_ACTION_RETRANSMIT) != 0u);
    zassert_equal(
        tick_result.retransmit.packet.message_age_ms,
        123u + RELAY_BUSY_RETRY_MIN_MS);
}

ZTEST(mesh_persistence_anchor,
      test_pair_result_reservation_cancel_fault_releases_remaining_tokens)
{
    uint32_t tokens[APP_MESH_SURVEY_PAIR_RESULT_JOURNAL_SLOTS] = {
        11u, 12u, 13u, 14u,
    };

    memset(&result_delivery_mock, 0, sizeof(result_delivery_mock));
    result_delivery_mock.fail_cancel_call = 2u;
    zassert_equal(
        app_anchor_survey_result_delivery_cancel_reservations(
            tokens, ARRAY_SIZE(tokens), "fault-injection"),
        -EIO);
    zassert_equal(result_delivery_mock.cancel_calls, ARRAY_SIZE(tokens));
    zassert_equal(result_delivery_mock.watchdog_stop_calls, 1u);
    for (size_t i = 0u; i < ARRAY_SIZE(tokens); i++) {
        zassert_equal(tokens[i], 0u);
    }
    zassert_equal(
        app_anchor_survey_result_delivery_cancel_reservations(
            tokens, ARRAY_SIZE(tokens), "already-cleared"),
        0);
    zassert_equal(result_delivery_mock.cancel_calls, ARRAY_SIZE(tokens));
    zassert_equal(result_delivery_mock.watchdog_stop_calls, 1u);
}

ZTEST_SUITE(mesh_persistence_anchor,
            NULL,
            mesh_persistence_anchor_suite_setup,
            NULL,
            NULL,
            NULL);
