#include "app_stack_workload_diag.h"

int main(void)
{
    struct proto_packet packet = {
        .src_id = 1u,
        .dst_id = 2u,
        .session_id = 3u,
        .seq = 4u,
        .msg_type = MSG_CLICK_REPORT,
    };

    app_stack_workload_diag_click_admit(&packet, 2u, 2u);
    app_stack_workload_diag_click_sample(&packet, 2u, 2u);
    app_stack_workload_diag_click_release(&packet, 0, 0u, 0u);
    app_stack_workload_diag_ble_release_all(-1, 0u, 0u);
    return 0;
}
