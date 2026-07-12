#include "app_gateway_command_lifecycle.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>

static struct app_gateway_command_ingress_item command_item(uint16_t seq,
                                                            uint32_t admission_id)
{
    return (struct app_gateway_command_ingress_item) {
        .packet = {
            .msg_type = MSG_COMMAND,
            .src_id = 1u,
            .dst_id = 2u,
            .session_id = 3u,
            .seq = seq,
        },
        .admission_id = admission_id,
        .command_id = CMD_FORCE_REDISCOVERY,
    };
}

static void generated_single_owner_terminal_release_invariant(void)
{
    for (uint16_t seed = 1u; seed <= 128u; seed++) {
        struct app_gateway_command_lifecycle lifecycle;
        struct app_gateway_command_ingress_item active = command_item(seed, seed);
        struct app_gateway_command_ingress_item later = command_item(
            (uint16_t)(seed + 1000u), (uint32_t)seed + 1000u);
        enum app_gateway_command_lifecycle_dispatch dispatch;
        unsigned int retry_count = seed % 5u;

        assert(app_gateway_command_lifecycle_init(&lifecycle, 2u) == 0);
        assert(app_gateway_command_lifecycle_admit(&lifecycle, &active) == 0);
        for (unsigned int retry = 0u; retry < retry_count; retry++) {
            assert(app_gateway_command_lifecycle_begin_dispatch(
                       &lifecycle, &active, &dispatch) == 0);
            assert(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE);
            assert(app_gateway_command_lifecycle_requeue_retry(
                       &lifecycle, &active) == 0);
            assert(app_gateway_command_lifecycle_admit(&lifecycle, &active) == -EALREADY);
        }
        assert(app_gateway_command_lifecycle_begin_dispatch(
                   &lifecycle, &active, &dispatch) == 0);
        assert(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE);
        assert(app_gateway_command_lifecycle_finish(&lifecycle, &active) == 0);
        assert(app_gateway_command_lifecycle_finish(&lifecycle, &active) == -ENOENT);
        assert(app_gateway_command_lifecycle_admit(&lifecycle, &later) == 0);
        assert(app_gateway_command_lifecycle_begin_dispatch(
                   &lifecycle, &later, &dispatch) == 0);
        assert(dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_EXECUTE);
        assert(app_gateway_command_lifecycle_finish(&lifecycle, &later) == 0);
    }
}

int main(void)
{
    generated_single_owner_terminal_release_invariant();
    return 0;
}
