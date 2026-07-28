#include "app_anchor_host_command_policy.h"

#include <errno.h>
#include <stdint.h>

bool app_anchor_host_command_within_failure_cutoff(uint32_t admission_id,
                                                   uint32_t cutoff_id)
{
    if (admission_id == 0u || cutoff_id == 0u) {
        return false;
    }

    return cutoff_id - admission_id <= INT32_MAX;
}

bool app_anchor_host_command_submit_needs_retry(int submit_result)
{
    return submit_result < 0;
}

bool app_anchor_host_command_submit_is_contract_failure(int submit_result)
{
    return submit_result < 0 &&
           submit_result != -EAGAIN &&
           submit_result != -EBUSY &&
           submit_result != -ENOSPC;
}

int app_anchor_host_command_ingress_result(int submit_result)
{
    return submit_result < 0 ? -EAGAIN : submit_result;
}
