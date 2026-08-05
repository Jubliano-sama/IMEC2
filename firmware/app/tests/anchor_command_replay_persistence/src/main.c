#include "app_mesh_persistence.h"
#include "gateway_command.h"
#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#define LOCAL_ID UINT64_C(0x1111222233334444)
#define GATEWAY_ID UINT64_C(0x9999888877776666)

static void *anchor_command_replay_persistence_setup(void)
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

ZTEST(anchor_command_replay_persistence,
      test_replay_survives_reset_and_window_churn)
{
    struct gateway_command_rx_duplicate_cache replay = {0};
    struct gateway_command_rx_duplicate_cache restored;
    struct gateway_command_rx_duplicate_cache wrong_identity;
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_HEARD,
        .response_mode = CMD_RESPONSE_NONE,
        .flood_epoch_id = 88u,
        .command_expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S,
        .flood_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID,
        .dst_id = MESH_BROADCAST_ID,
    };
    const uint32_t first_command_seq = 5001u;

    zassert_ok(app_mesh_persistence_init());
    for (size_t i = 0u; i < 66u; i++) {
        options.command_seq = first_command_seq + (uint32_t)i;
        packet.session_id = options.command_seq;
        packet.seq = (uint16_t)(i + 1u);
        gateway_command_rx_duplicate_store(&replay,
                                           &packet,
                                           &options,
                                           1000u + (uint32_t)i);
    }
    zassert_true(gateway_command_rx_duplicate_seen(&replay,
                                                   first_command_seq,
                                                   2000u));
    zassert_ok(app_mesh_persistence_save_anchor_command_replay(
        LOCAL_ID, GATEWAY_ID, &replay));

    /*
     * Restoring into a fresh cache models an anchor reset. The command that
     * fell beyond the exact 64-bit window must remain stale, while the next
     * gateway serial remains admissible.
     */
    memset(&restored, 0, sizeof(restored));
    zassert_equal(app_mesh_persistence_restore_anchor_command_replay(
                      LOCAL_ID, GATEWAY_ID, &restored),
                  1);
    zassert_true(gateway_command_rx_duplicate_seen(&restored,
                                                   first_command_seq,
                                                   90000u));
    zassert_false(gateway_command_rx_duplicate_seen(
        &restored, first_command_seq + 66u, 90000u));

    memset(&wrong_identity, 0xA5, sizeof(wrong_identity));
    zassert_equal(app_mesh_persistence_restore_anchor_command_replay(
                      LOCAL_ID + 1u, GATEWAY_ID, &wrong_identity),
                  -EINVAL);
    zassert_false(wrong_identity.initialized);
}

ZTEST(anchor_command_replay_persistence,
      test_legacy_bitset_restores_as_fail_closed_high_watermark)
{
    struct gateway_command_rx_duplicate_cache legacy = {
        .committed = UINT64_C(0x8000000000000005),
        .newest_command_seq = 42u,
        .initialized = true,
    };
    struct gateway_command_rx_duplicate_cache restored = {0};

    /*
     * Schema 1 originally persisted an out-of-order bitset. Keep its byte
     * layout readable, but collapse every valid legacy record to the strict
     * high watermark so an uncommitted gap such as 41 cannot execute.
     */
    zassert_ok(app_mesh_persistence_save_anchor_command_replay(
        LOCAL_ID, GATEWAY_ID, &legacy));
    zassert_equal(app_mesh_persistence_restore_anchor_command_replay(
                      LOCAL_ID, GATEWAY_ID, &restored),
                  1);
    zassert_true(restored.initialized);
    zassert_equal(restored.newest_command_seq, 42u);
    zassert_equal(restored.committed, UINT64_C(1));
    zassert_true(gateway_command_rx_duplicate_seen(&restored, 42u, 1000u));
    zassert_true(gateway_command_rx_duplicate_seen(&restored, 41u, 1000u));
    zassert_true(gateway_command_rx_duplicate_seen(
        &restored, 42u + UINT32_C(0x80000000), 1000u));
    zassert_false(gateway_command_rx_duplicate_seen(&restored, 43u, 1000u));
}

ZTEST_SUITE(anchor_command_replay_persistence,
            NULL,
            anchor_command_replay_persistence_setup,
            NULL,
            NULL,
            NULL);
