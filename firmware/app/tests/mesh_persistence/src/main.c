#include "app_mesh_persistence.h"
#include "mesh.h"
#include "protocol.h"

#include <string.h>
#include <zephyr/ztest.h>

#define LOCAL_ID 0x1111222233334444ull
#define GATEWAY_ID 0x9999888877776666ull

static struct app_mesh_collection_result_snapshot make_snapshot(void)
{
    return (struct app_mesh_collection_result_snapshot) {
        .version = APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION,
        .local_id = LOCAL_ID,
        .gateway_id = GATEWAY_ID,
        .command = {
            .msg_type = MSG_COMMAND,
            .src_id = GATEWAY_ID,
            .dst_id = LOCAL_ID,
            .session_id = 1001u,
            .seq = 7u,
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = 3u,
        },
        .result_id = {
            .gateway_id = GATEWAY_ID,
            .gateway_epoch = 13u,
            .command_seq = 1001u,
            .node_id = LOCAL_ID,
            .node_boot_counter = 55u,
            .result_seq = 56u,
        },
        .collection_epoch_id = 3003u,
        .delay_ms = 12345u,
        .command_id = CMD_GET_STATUS,
        .status = COMMAND_OK,
        .reason = 9u,
        .force_rediscovery_after_result = true,
        .reboot_after_result = false,
        .valid = true,
    };
}

ZTEST(mesh_persistence, test_collection_result_snapshot_round_trip_and_clear)
{
    struct app_mesh_collection_result_snapshot saved = make_snapshot();
    struct app_mesh_collection_result_snapshot restored;

    zassert_ok(app_mesh_persistence_init());
    app_mesh_persistence_clear_collection_result();

    zassert_ok(app_mesh_persistence_save_collection_result(&saved));
    memset(&restored, 0, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_mem_equal(&restored, &saved, sizeof(saved));

    app_mesh_persistence_clear_collection_result();
    memset(&restored, 0xA5, sizeof(restored));
    zassert_ok(app_mesh_persistence_restore_collection_result(&restored));
    zassert_false(restored.valid);
}

ZTEST(mesh_persistence, test_collection_result_snapshot_rejects_invalid_save)
{
    struct app_mesh_collection_result_snapshot snapshot = make_snapshot();

    zassert_ok(app_mesh_persistence_init());

    snapshot.valid = false;
    zassert_equal(app_mesh_persistence_save_collection_result(&snapshot), -EINVAL);

    snapshot = make_snapshot();
    snapshot.version++;
    zassert_equal(app_mesh_persistence_save_collection_result(&snapshot), -EINVAL);

    zassert_equal(app_mesh_persistence_save_collection_result(NULL), -EINVAL);
}

ZTEST_SUITE(mesh_persistence, NULL, NULL, NULL, NULL, NULL);
