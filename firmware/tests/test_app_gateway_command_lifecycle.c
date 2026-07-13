#include "app_gateway_command_lifecycle.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static struct app_gateway_command_ingress_item command_item(uint16_t seq,
                                                            uint32_t admission_id)
{
    return (struct app_gateway_command_ingress_item) {
        .packet = {
            .msg_type = MSG_COMMAND,
            .src_id = UINT64_C(0x1111),
            .dst_id = UINT64_C(0x9000),
            .session_id = 77u,
            .seq = seq,
        },
        .admission_id = admission_id,
        .command_id = CMD_FORCE_REDISCOVERY,
    };
}

static int physical_remove_fails(
    void *ctx,
    const struct app_gateway_command_identity *identity)
{
    unsigned int *calls = ctx;

    assert(identity != NULL);
    (*calls)++;
    return -EIO;
}

static void test_cancel_tombstone_blocks_dispatch_after_physical_remove_failure(void)
{
    struct app_gateway_command_lifecycle lifecycle;
    struct app_gateway_command_lifecycle_cancel_result cancel_result;
    enum app_gateway_command_lifecycle_dispatch dispatch;
    struct app_gateway_command_identity identity;
    struct app_gateway_command_ingress_item first = command_item(1u, 1u);
    struct app_gateway_command_ingress_item second = command_item(2u, 2u);
    unsigned int physical_remove_calls = 0u;

    assert(app_gateway_command_lifecycle_init(&lifecycle, 2u) == 0);
    assert(app_gateway_command_lifecycle_admit(&lifecycle, &first) == 0);
    assert(app_gateway_command_lifecycle_admit(&lifecycle, &second) == 0);
    assert(app_gateway_command_lifecycle_admit(&lifecycle, &first) == -EALREADY);
    assert(app_gateway_command_identity_from_item(&first, &identity) == 0);
    assert(app_gateway_command_lifecycle_cancel(&lifecycle,
                                                &identity,
                                                physical_remove_fails,
                                                &physical_remove_calls,
                                                &cancel_result) == 0);
    assert(cancel_result.authoritative_cancelled);
    assert(cancel_result.physical_remove_ret == -EIO);
    assert(physical_remove_calls == 1u);

    assert(app_gateway_command_lifecycle_begin_dispatch(&lifecycle,
                                                        &first,
                                                        &dispatch) == 0);
    assert(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED);
    assert(app_gateway_command_lifecycle_begin_dispatch(&lifecycle,
                                                        &second,
                                                        &dispatch) == 0);
    assert(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE);
    assert(app_gateway_command_lifecycle_requeue_retry(&lifecycle, &second) == 0);
    assert(app_gateway_command_lifecycle_begin_dispatch(&lifecycle,
                                                        &second,
                                                        &dispatch) == 0);
    assert(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE);
    assert(app_gateway_command_lifecycle_finish(&lifecycle, &second) == 0);
}

static void test_capacity_is_bounded_by_the_physical_queue_capacity(void)
{
    struct app_gateway_command_lifecycle lifecycle;

    assert(app_gateway_command_lifecycle_init(&lifecycle, 0u) == -EINVAL);
    assert(app_gateway_command_lifecycle_init(
               &lifecycle,
               APP_GATEWAY_COMMAND_LIFECYCLE_MAX_ITEMS + 1u) == -EINVAL);
    assert(app_gateway_command_lifecycle_init(&lifecycle, 1u) == 0);
    assert(app_gateway_command_lifecycle_admit(&lifecycle,
                                               &(struct app_gateway_command_ingress_item) {
                                                   .packet = {.msg_type = MSG_COMMAND},
                                                   .admission_id = 1u,
                                                   .command_id = CMD_PING,
                                               }) == 0);
    assert(app_gateway_command_lifecycle_admit(&lifecycle,
                                               &(struct app_gateway_command_ingress_item) {
                                                   .packet = {.msg_type = MSG_COMMAND},
                                                   .admission_id = 2u,
                                                   .command_id = CMD_PING,
                                               }) == -ENOSPC);
}


int main(void)
{
    test_cancel_tombstone_blocks_dispatch_after_physical_remove_failure();
    test_capacity_is_bounded_by_the_physical_queue_capacity();
    return 0;
}
