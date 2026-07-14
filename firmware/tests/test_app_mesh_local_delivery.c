#include "app_mesh_local_delivery.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct journal_store {
    struct app_mesh_local_delivery_snapshot persisted;
    unsigned int save_count;
    unsigned int clear_count;
    int save_result;
    int clear_result;
    bool present;
};

static int journal_save(void *ctx,
                        const struct app_mesh_local_delivery_snapshot *snapshot)
{
    struct journal_store *store = ctx;

    store->save_count++;
    if (store->save_result != 0) {
        return store->save_result;
    }
    if (!app_mesh_local_delivery_snapshot_valid(snapshot)) {
        return -EINVAL;
    }
    store->persisted = *snapshot;
    store->present = true;
    return 0;
}

static int journal_clear(void *ctx)
{
    struct journal_store *store = ctx;

    store->clear_count++;
    if (store->clear_result != 0) {
        return store->clear_result;
    }
    memset(&store->persisted, 0, sizeof(store->persisted));
    store->present = false;
    return 0;
}

static struct app_mesh_local_delivery make_delivery(struct journal_store *store)
{
    struct app_mesh_local_delivery delivery;
    const struct app_mesh_local_delivery_ops ops = {
        .save = journal_save,
        .clear = journal_clear,
        .ctx = store,
    };

    app_mesh_local_delivery_init(&delivery, &ops);
    return delivery;
}

static struct mesh_outbound make_report(uint32_t survey_id, uint16_t seq)
{
    struct mesh_outbound outbound = {0};

    outbound.packet.msg_type = MSG_SURVEY_DISCOVERY_REPORT;
    outbound.packet.src_id = UINT64_C(0x1020304050607080);
    outbound.packet.dst_id = UINT64_C(0x8877665544332211);
    outbound.packet.session_id = survey_id;
    outbound.packet.seq = seq;
    outbound.packet.ttl = 4u;
    outbound.packet.payload_len = 5u;
    outbound.payload_len = 5u;
    outbound.payload[0] = 0x55u;
    outbound.payload[1] = 0xa5u;
    outbound.payload[2] = 0x11u;
    outbound.payload[3] = 0x22u;
    outbound.payload[4] = 0x33u;
    outbound.radio_channel = 9u;
    outbound.next_hop_id = UINT64_C(0x0102030405060708);
    outbound.queued_at_ms = 1234u;
    outbound.earliest_tx_ms = 1275u;
    return outbound;
}

static void test_transactional_stage_and_back_to_back_rejection(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound first = make_report(101u, 7u);
    const struct mesh_outbound second = make_report(102u, 8u);
    struct app_mesh_local_delivery_snapshot saved;

    store.save_result = -EIO;
    assert(app_mesh_local_delivery_stage(&delivery, &first, 101u) == -EIO);
    assert(!app_mesh_local_delivery_active(&delivery));
    assert(!store.present);

    store.save_result = 0;
    assert(app_mesh_local_delivery_stage(&delivery, &first, 101u) == 0);
    assert(store.present);
    assert(app_mesh_local_delivery_snapshot_valid(&store.persisted));
    assert(memcmp(&store.persisted.outbound, &first, sizeof(first)) == 0);
    saved = store.persisted;

    assert(app_mesh_local_delivery_stage(&delivery, &second, 102u) == -EBUSY);
    assert(memcmp(&store.persisted, &saved, sizeof(saved)) == 0);
}

static void test_reboot_and_exact_ack_identity(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct mesh_outbound outbound = make_report(201u, 19u);
    struct proto_packet wrong;
    unsigned int saves_before;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 201u) == 0);
    assert(app_mesh_local_delivery_note_state(
               &delivery, APP_MESH_LOCAL_DELIVERY_ROUTE_WAIT) == 0);
    assert(app_mesh_local_delivery_note_tracked(&delivery) == 0);

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(memcmp(app_mesh_local_delivery_outbound(&rebooted), &outbound,
                  sizeof(outbound)) == 0);
    assert(rebooted.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS - 1u);

    saves_before = store.save_count;
    wrong = outbound.packet;
    wrong.seq++;
    assert(app_mesh_local_delivery_note_ack(&rebooted, &wrong) ==
           -EKEYREJECTED);
    assert(app_mesh_local_delivery_note_ack(&rebooted, NULL) == -EINVAL);
    assert(store.save_count == saves_before);
    assert(store.clear_count == 0u);

    assert(app_mesh_local_delivery_note_ack(&rebooted, &outbound.packet) == 0);
    assert(!store.present);
    assert(!app_mesh_local_delivery_active(&rebooted));
    assert(store.clear_count == 1u);
}

static void test_blocked_start_does_not_consume_attempt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(251u, 24u);
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 251u) == 0);
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_ROUTE_WAIT) == 0);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);

    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, attempt_token) == 0);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS - 1u);
}

static void test_ack_commit_survives_clear_failure(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    const struct mesh_outbound outbound = make_report(301u, 29u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 301u) == 0);
    store.clear_result = -EIO;
    assert(app_mesh_local_delivery_note_ack(&delivery, &outbound.packet) == -EIO);
    assert(store.present);
    assert(store.persisted.state == APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED);
    assert(!app_mesh_local_delivery_active(&delivery));

    store.clear_result = 0;
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(!store.present);
    assert(!app_mesh_local_delivery_active(&rebooted));
}

static void test_last_inflight_attempt_can_still_be_acked(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(351u, 34u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 351u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    store.persisted = delivery.snapshot;
    assert(app_mesh_local_delivery_note_tracked(&delivery) == 0);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_note_ack(&delivery, &outbound.packet) == 0);
    assert(!store.present);
}

static void test_corruption_and_bounded_attempts_fail_closed(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    const struct mesh_outbound outbound = make_report(401u, 39u);
    struct app_mesh_local_delivery_snapshot corrupt;
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 401u) == 0);
    corrupt = store.persisted;
    corrupt.outbound.payload[0] ^= 1u;
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &corrupt) == -EINVAL);
    assert(!app_mesh_local_delivery_active(&rebooted));

    corrupt = store.persisted;
    corrupt.version++;
    assert(!app_mesh_local_delivery_snapshot_valid(&corrupt));
    corrupt = store.persisted;
    corrupt.size--;
    assert(!app_mesh_local_delivery_snapshot_valid(&corrupt));

    for (unsigned int i = 0u;
         i < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS; i++) {
        assert(app_mesh_local_delivery_begin_attempt(
                   &delivery, &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &delivery, attempt_token) == 0);
        if (i + 1u < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS) {
            assert(app_mesh_local_delivery_note_attempt_released(
                       &delivery, attempt_token,
                       APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
        }
    }
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_snapshot_valid(&store.persisted));
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == -EINPROGRESS);
    assert(app_mesh_local_delivery_note_attempt_released(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == -ETIMEDOUT);
    assert(app_mesh_local_delivery_note_failed(&delivery) == 0);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_FAILED);

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(rebooted.snapshot.state == APP_MESH_LOCAL_DELIVERY_FAILED);

    store.clear_result = -EIO;
    assert(app_mesh_local_delivery_discard_failed(&rebooted) == -EIO);
    assert(app_mesh_local_delivery_active(&rebooted));
    assert(store.present);

    store.clear_result = 0;
    assert(app_mesh_local_delivery_discard_failed(&rebooted) == 0);
    assert(!app_mesh_local_delivery_active(&rebooted));
    assert(!store.present);
    assert(app_mesh_local_delivery_stage(&rebooted, &outbound, 402u) == 0);
}

static void test_synchronous_ack_wins_post_send_resolution(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(451u, 44u);
    uint8_t attempt_token;
    unsigned int saves_after_ack;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 451u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_snapshot_valid(&store.persisted));
    assert(app_mesh_local_delivery_note_ack(&delivery, &outbound.packet) == 0);
    saves_after_ack = store.save_count;
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, attempt_token) == -EALREADY);
    assert(store.save_count == saves_after_ack);
    assert(!app_mesh_local_delivery_active(&delivery));
}

static void test_reset_after_start_does_not_refund_attempt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    const struct mesh_outbound outbound = make_report(501u, 49u);
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 501u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(store.persisted.state == APP_MESH_LOCAL_DELIVERY_STARTING);
    assert(store.persisted.attempts_remaining == 0u);

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(app_mesh_local_delivery_note_attempt_released(
               &rebooted, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(rebooted.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_begin_attempt(
               &rebooted, &attempt_token) == -ETIMEDOUT);
}

static void test_stale_attempt_callback_cannot_overwrite_new_attempt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(551u, 54u);
    uint8_t first_token;
    uint8_t second_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 551u) == 0);
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &first_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, first_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &second_token) == 0);
    assert(second_token != first_token);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, first_token) == -ESTALE);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, second_token) == 0);
}

static void test_recovery_preserves_custody_on_transient_read_failure(void)
{
    const int transient_errors[] = {-EAGAIN, -EIO};

    for (size_t i = 0u; i < sizeof(transient_errors) /
                                  sizeof(transient_errors[0]); ++i) {
        struct journal_store store = {0};
        struct app_mesh_local_delivery delivery = make_delivery(&store);
        struct app_mesh_local_delivery_recovery recovery;

        assert(app_mesh_local_delivery_recover(
                   &delivery, NULL, transient_errors[i], &recovery) == 0);
        assert(!recovery.restored);
        assert(recovery.retry_required);
        assert(!recovery.quarantined);
        assert(recovery.source_error == transient_errors[i]);
        assert(recovery.clear_error == 0);
        assert(store.clear_count == 0u);
        assert(app_mesh_local_delivery_active(&delivery));

        assert(app_mesh_local_delivery_recover(&delivery, NULL, -ENOENT,
                                               &recovery) == 0);
        assert(!recovery.restored);
        assert(!recovery.retry_required);
        assert(!recovery.quarantined);
        assert(recovery.source_error == 0);
        assert(store.clear_count == 0u);
        assert(!app_mesh_local_delivery_active(&delivery));
    }
}

static void test_recovery_quarantines_bad_message_and_old_schema(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct app_mesh_local_delivery_recovery recovery;
    const struct mesh_outbound outbound = make_report(601u, 59u);
    struct app_mesh_local_delivery_snapshot corrupt;

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(&rebooted, NULL, -EBADMSG,
                                           &recovery) == 0);
    assert(recovery.quarantined);
    assert(recovery.source_error == -EBADMSG);
    assert(recovery.clear_error == 0);
    assert(store.clear_count == 1u);
    assert(!app_mesh_local_delivery_active(&rebooted));

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 601u) == 0);
    corrupt = store.persisted;
    corrupt.version--;
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(&rebooted, &corrupt, 1,
                                           &recovery) == 0);
    assert(recovery.quarantined);
    assert(recovery.source_error == -EBADMSG);
    assert(recovery.clear_error == 0);
    assert(store.clear_count == 2u);
    assert(!app_mesh_local_delivery_active(&rebooted));
}

static void test_recovery_reports_quarantine_clear_failure(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery_recovery recovery;
    const struct mesh_outbound outbound = make_report(602u, 60u);
    struct app_mesh_local_delivery_snapshot corrupt;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 602u) == 0);
    corrupt = store.persisted;
    corrupt.checksum ^= 1u;
    store.clear_result = -EIO;
    delivery = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(&delivery, &corrupt, 1,
                                           &recovery) == 0);
    assert(recovery.quarantined);
    assert(recovery.source_error == -EBADMSG);
    assert(recovery.clear_error == -EIO);
    assert(store.clear_count == 1u);
    assert(store.present);
    assert(!app_mesh_local_delivery_active(&delivery));
}

static void test_persistence_mock_rejects_invalid_snapshot_like_production(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(651u, 64u);
    struct app_mesh_local_delivery_snapshot invalid;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 651u) == 0);
    invalid = store.persisted;
    invalid.checksum ^= 1u;
    store.present = false;
    assert(journal_save(&store, &invalid) == -EINVAL);
    assert(!store.present);
}

static void test_sustained_pre_rf_contention_has_constant_write_bound(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(701u, 69u);
    uint8_t attempt_token;
    unsigned int writes_after_reservation;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 701u) == 0);
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &attempt_token) == 0);
    writes_after_reservation = store.save_count;
    assert(app_mesh_local_delivery_note_attempt_blocked(
               &delivery, attempt_token) == 0);

    for (unsigned int i = 0u; i < 10000u; i++) {
        uint8_t resumed_token = 0u;

        assert(app_mesh_local_delivery_attempts_available(&delivery) ==
               APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);
        assert(app_mesh_local_delivery_resume_blocked_attempt(
                   &delivery, &resumed_token) == 0);
        assert(resumed_token == attempt_token);
        assert(app_mesh_local_delivery_note_attempt_blocked(
                   &delivery, resumed_token) == 0);
    }
    assert(store.save_count == writes_after_reservation);
    assert(app_mesh_local_delivery_attempts_available(&delivery) ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);

    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(store.save_count == writes_after_reservation + 1u);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);
}

static void test_last_blocked_token_can_retry_send_and_ack(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(751u, 74u);
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 751u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &attempt_token) == 0);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_note_attempt_blocked(
               &delivery, attempt_token) == 0);
    assert(app_mesh_local_delivery_attempts_available(&delivery) == 1u);

    for (unsigned int i = 0u; i < 32u; i++) {
        uint8_t resumed_token = 0u;

        assert(app_mesh_local_delivery_resume_blocked_attempt(
                   &delivery, &resumed_token) == 0);
        assert(resumed_token == attempt_token);
        assert(app_mesh_local_delivery_note_attempt_blocked(
                   &delivery, resumed_token) == 0);
        assert(app_mesh_local_delivery_attempts_available(&delivery) == 1u);
    }

    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(delivery.snapshot.attempts_remaining == 1u);
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_blocked(
               &delivery, attempt_token) == 0);
    assert(app_mesh_local_delivery_resume_blocked_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, attempt_token) == 0);
    assert(app_mesh_local_delivery_attempts_available(&delivery) == 0u);
    assert(app_mesh_local_delivery_note_ack(&delivery, &outbound.packet) == 0);
    assert(!app_mesh_local_delivery_active(&delivery));
}

static void test_supersede_is_generation_exact_and_persistence_safe(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound old_report = make_report(801u, 81u);
    const struct mesh_outbound new_report = make_report(802u, 82u);

    assert(app_mesh_local_delivery_stage(&delivery, &old_report, 801u) == 0);
    assert(app_mesh_local_delivery_supersede(&delivery, 801u) == -EALREADY);
    assert(app_mesh_local_delivery_active(&delivery));
    assert(store.present);

    store.clear_result = -EIO;
    assert(app_mesh_local_delivery_supersede(&delivery, 802u) == -EIO);
    assert(app_mesh_local_delivery_active(&delivery));
    assert(store.present);
    assert(app_mesh_local_delivery_stage(&delivery, &new_report, 802u) == -EBUSY);

    store.clear_result = 0;
    assert(app_mesh_local_delivery_supersede(&delivery, 802u) == 0);
    assert(!app_mesh_local_delivery_active(&delivery));
    assert(!store.present);
    assert(app_mesh_local_delivery_stage(&delivery, &new_report, 802u) == 0);
}

static void test_fifty_anchors_survive_back_to_back_survey_and_lost_acks(void)
{
    enum { ANCHOR_COUNT = 50 };
    static struct journal_store stores[ANCHOR_COUNT];
    static struct app_mesh_local_delivery deliveries[ANCHOR_COUNT];
    const uint32_t old_survey_id = 901u;
    const uint32_t new_survey_id = 902u;

    memset(stores, 0, sizeof(stores));
    memset(deliveries, 0, sizeof(deliveries));
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        struct mesh_outbound old_report =
            make_report(old_survey_id, (uint16_t)(100u + i));
        struct mesh_outbound new_report =
            make_report(new_survey_id, (uint16_t)(200u + i));
        uint8_t attempt_token;
        unsigned int destructive_collisions =
            (unsigned int)(i % APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);

        deliveries[i] = make_delivery(&stores[i]);
        old_report.packet.src_id += i;
        new_report.packet.src_id += i;
        assert(app_mesh_local_delivery_stage(
                   &deliveries[i], &old_report, old_survey_id) == 0);
        assert(app_mesh_local_delivery_begin_attempt(
                   &deliveries[i], &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &deliveries[i], attempt_token) == 0);

        if ((i % 4u) == 0u) {
            assert(app_mesh_local_delivery_note_ack(
                       &deliveries[i], &old_report.packet) == 0);
        }
        assert(app_mesh_local_delivery_supersede(
                   &deliveries[i], new_survey_id) == 0);
        assert(app_mesh_local_delivery_stage(
                   &deliveries[i], &new_report, new_survey_id) == 0);
        assert(app_mesh_local_delivery_note_ack(
                   &deliveries[i], &old_report.packet) == -EKEYREJECTED);

        for (unsigned int collision = 0u;
             collision < destructive_collisions;
             collision++) {
            assert(app_mesh_local_delivery_begin_attempt(
                       &deliveries[i], &attempt_token) == 0);
            assert(app_mesh_local_delivery_note_attempt_sent(
                       &deliveries[i], attempt_token) == 0);
            assert(app_mesh_local_delivery_note_attempt_released(
                       &deliveries[i], attempt_token,
                       APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
        }
        assert(app_mesh_local_delivery_begin_attempt(
                   &deliveries[i], &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &deliveries[i], attempt_token) == 0);
        assert(app_mesh_local_delivery_note_ack(
                   &deliveries[i], &new_report.packet) == 0);
        assert(!app_mesh_local_delivery_active(&deliveries[i]));
        assert(!stores[i].present);
    }
}

int main(void)
{
    test_transactional_stage_and_back_to_back_rejection();
    test_reboot_and_exact_ack_identity();
    test_blocked_start_does_not_consume_attempt();
    test_ack_commit_survives_clear_failure();
    test_last_inflight_attempt_can_still_be_acked();
    test_corruption_and_bounded_attempts_fail_closed();
    test_synchronous_ack_wins_post_send_resolution();
    test_reset_after_start_does_not_refund_attempt();
    test_stale_attempt_callback_cannot_overwrite_new_attempt();
    test_recovery_preserves_custody_on_transient_read_failure();
    test_recovery_quarantines_bad_message_and_old_schema();
    test_recovery_reports_quarantine_clear_failure();
    test_persistence_mock_rejects_invalid_snapshot_like_production();
    test_sustained_pre_rf_contention_has_constant_write_bound();
    test_last_blocked_token_can_retry_send_and_ack();
    test_supersede_is_generation_exact_and_persistence_safe();
    test_fifty_anchors_survive_back_to_back_survey_and_lost_acks();
    puts("app mesh local delivery tests passed");
    return 0;
}
