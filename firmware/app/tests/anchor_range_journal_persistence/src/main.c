#include "app_mesh_persistence.h"
#include "mesh.h"
#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#define CLICKER_ID UINT64_C(0x1111222233334444)
#define ANCHOR_ID UINT64_C(0x5555666677778888)
#define GATEWAY_ID UINT64_C(0x9999aaaabbbbcccc)
#define EVENT_SEQ UINT32_C(0x12345678)

static void *anchor_range_journal_suite_setup(void)
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
    zassert_ok(app_mesh_persistence_init());
    return NULL;
}

static struct mesh_outbound make_fragment(uint8_t index)
{
    struct mesh_outbound outbound = {
        .packet = {
            .msg_type = MSG_CLICK_REPORT,
            .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
            .src_id = ANCHOR_ID,
            .dst_id = GATEWAY_ID,
            .session_id = EVENT_SEQ,
            .seq = (uint16_t)(0x4000u + index),
            .ttl = 5u,
            .payload_len = 6u,
            .message_age_ms = (uint16_t)(20u + index),
        },
        .payload_len = 6u,
        .radio_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .next_hop_id = GATEWAY_ID,
    };

    outbound.payload[0] = 0x90u;
    outbound.payload[1] = 4u;
    outbound.payload[2] = index;
    outbound.payload[3] = (uint8_t)(index + 1u);
    outbound.payload[4] = (uint8_t)(index + 2u);
    outbound.payload[5] = (uint8_t)(index + 3u);
    return outbound;
}

static void prepare_control(struct anchor_range_journal_control *control)
{
    struct anchor_range_journal_control existing;
    int ret;

    app_mesh_persistence_test_reset_faults();
    ret = app_mesh_persistence_restore_anchor_range_journal(&existing);
    zassert_true(ret == 0 || ret == 1);
    if (ret == 1) {
        zassert_ok(app_mesh_persistence_clear_anchor_range_journal(&existing));
    }
    zassert_ok(app_mesh_persistence_prepare_anchor_range_journal(
        CLICKER_ID,
        EVENT_SEQ,
        2u,
        ANCHOR_ID,
        GATEWAY_ID,
        control));
}

static void save_fragments(struct anchor_range_journal_control *control,
                           uint8_t count)
{
    for (uint8_t i = 0u; i < count; i++) {
        struct mesh_outbound fragment = make_fragment(i);
        enum anchor_range_fragment_persistence_observation observation =
            ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;

        zassert_ok(app_mesh_persistence_save_anchor_range_fragment(
            control, i, &fragment, &observation));
        zassert_equal(observation,
                      ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED);
    }
}

static void assert_fragment_equal(
    const struct anchor_range_journal_control *control,
    uint8_t index)
{
    struct mesh_outbound expected = make_fragment(index);
    struct mesh_outbound restored;

    zassert_ok(app_mesh_persistence_restore_anchor_range_fragment(
        control, index, &restored));
    zassert_mem_equal(&restored.packet,
                      &expected.packet,
                      sizeof(expected.packet));
    zassert_equal(restored.payload_len, expected.payload_len);
    zassert_mem_equal(restored.payload,
                      expected.payload,
                      expected.payload_len);
}

ZTEST(anchor_range_journal_persistence,
      test_fragments_remain_invisible_until_control_commit)
{
    struct anchor_range_journal_control control;
    struct anchor_range_journal_control restored;

    prepare_control(&control);
    save_fragments(&control, 2u);
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 0);
    zassert_ok(app_mesh_persistence_commit_anchor_range_journal(&control));
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 1);
    zassert_mem_equal(&restored, &control, sizeof(control));
    assert_fragment_equal(&restored, 0u);
    assert_fragment_equal(&restored, 1u);
}

ZTEST(anchor_range_journal_persistence,
      test_fragment_write_and_readback_failures_never_publish_control)
{
    struct anchor_range_journal_control control;
    struct anchor_range_journal_control restored;
    struct mesh_outbound fragment = make_fragment(0u);
    enum anchor_range_fragment_persistence_observation observation =
        ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;

    prepare_control(&control);
    app_mesh_persistence_test_fail_anchor_range_fragment_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_anchor_range_fragment(
                      &control, 0u, &fragment, &observation), -EIO);
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS);
    zassert_equal(control.fragment_count, 0u);
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 0);

    observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;
    app_mesh_persistence_test_fail_anchor_range_fragment_read(-EIO, 1u);
    zassert_equal(app_mesh_persistence_save_anchor_range_fragment(
                      &control, 0u, &fragment, &observation), -EIO);
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_AMBIGUOUS);
    zassert_equal(control.fragment_count, 0u);
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 0);

    app_mesh_persistence_test_reset_faults();
    observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;
    zassert_ok(app_mesh_persistence_save_anchor_range_fragment(
        &control, 0u, &fragment, &observation));
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED);
    zassert_ok(app_mesh_persistence_commit_anchor_range_journal(&control));
}

ZTEST(anchor_range_journal_persistence,
      test_prewrite_lock_contention_is_typed_retryable_without_mutation)
{
    struct anchor_range_journal_control control;
    struct mesh_outbound fragment = make_fragment(0u);
    enum anchor_range_fragment_persistence_observation observation =
        ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;

    prepare_control(&control);
    app_mesh_persistence_test_set_deferred_busy(true);
    zassert_equal(app_mesh_persistence_save_anchor_range_fragment(
                      &control, 0u, &fragment, &observation), -EBUSY);
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_RETRYABLE_PREWRITE);
    zassert_equal(control.fragment_count, 0u);

    app_mesh_persistence_test_set_deferred_busy(false);
    observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;
    zassert_ok(app_mesh_persistence_save_anchor_range_fragment(
        &control, 0u, &fragment, &observation));
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED);
}

ZTEST(anchor_range_journal_persistence,
      test_existing_equal_control_never_confirms_unlisted_next_fragment)
{
    struct anchor_range_journal_control control;
    struct mesh_outbound first = make_fragment(0u);
    struct mesh_outbound next = make_fragment(1u);
    enum anchor_range_fragment_persistence_observation observation =
        ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED;

    prepare_control(&control);
    zassert_ok(app_mesh_persistence_save_anchor_range_fragment(
        &control, 0u, &first, &observation));
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED);
    zassert_ok(app_mesh_persistence_commit_anchor_range_journal(&control));

    observation = ANCHOR_RANGE_FRAGMENT_PERSISTENCE_CONFIRMED;
    zassert_equal(app_mesh_persistence_save_anchor_range_fragment(
                      &control, 1u, &next, &observation), -ESTALE);
    zassert_equal(observation,
                  ANCHOR_RANGE_FRAGMENT_PERSISTENCE_REJECTED);
    zassert_equal(control.fragment_count, 1u);
}

ZTEST(anchor_range_journal_persistence,
      test_control_write_and_readback_failures_are_retryable)
{
    struct anchor_range_journal_control control;
    struct anchor_range_journal_control restored;

    prepare_control(&control);
    save_fragments(&control, 3u);
    app_mesh_persistence_test_fail_anchor_range_control_write(-EIO, 1u);
    zassert_equal(app_mesh_persistence_commit_anchor_range_journal(
                      &control), -EIO);
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 0);

    app_mesh_persistence_test_fail_anchor_range_control_readback(-EIO, 1u);
    zassert_equal(app_mesh_persistence_commit_anchor_range_journal(
                      &control), -EIO);
    /*
     * The marker write reached NVS. The exact retry must recognize that
     * owner and succeed instead of leaving the queue permanently invisible.
     */
    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_commit_anchor_range_journal(&control));
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 1);
    zassert_mem_equal(&restored, &control, sizeof(control));
}

ZTEST(anchor_range_journal_persistence,
      test_control_delete_failure_keeps_replay_owner_until_retry)
{
    struct anchor_range_journal_control control;
    struct anchor_range_journal_control restored;

    prepare_control(&control);
    save_fragments(&control, 2u);
    zassert_ok(app_mesh_persistence_commit_anchor_range_journal(&control));

    app_mesh_persistence_test_fail_anchor_range_control_delete(-EIO, 1u);
    zassert_equal(app_mesh_persistence_clear_anchor_range_journal(
                      &control), -EIO);
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 1);
    zassert_mem_equal(&restored, &control, sizeof(control));

    app_mesh_persistence_test_reset_faults();
    zassert_ok(app_mesh_persistence_clear_anchor_range_journal(&control));
    zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                      &restored), 0);
}

ZTEST(anchor_range_journal_persistence,
      test_max_fragment_cycles_survive_nvs_gc_and_exact_restore)
{
    for (uint8_t cycle = 0u; cycle < 20u; cycle++) {
        struct anchor_range_journal_control control;
        struct anchor_range_journal_control restored;

        ARG_UNUSED(cycle);
        prepare_control(&control);
        save_fragments(&control, RANGE_REPORT_MAX_PACKET_FRAGMENTS);
        zassert_ok(app_mesh_persistence_commit_anchor_range_journal(&control));
        zassert_equal(app_mesh_persistence_restore_anchor_range_journal(
                          &restored), 1);
        zassert_mem_equal(&restored, &control, sizeof(control));
        for (uint8_t i = 0u; i < RANGE_REPORT_MAX_PACKET_FRAGMENTS; i++) {
            assert_fragment_equal(&restored, i);
        }
        zassert_ok(app_mesh_persistence_clear_anchor_range_journal(&control));
    }
}

ZTEST_SUITE(anchor_range_journal_persistence,
            NULL,
            anchor_range_journal_suite_setup,
            NULL,
            NULL,
            NULL);
