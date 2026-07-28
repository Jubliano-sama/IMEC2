#ifndef APP_ANCHOR_HOST_COMMAND_POLICY_H
#define APP_ANCHOR_HOST_COMMAND_POLICY_H

#include <stdbool.h>
#include <stdint.h>

bool app_anchor_host_command_within_failure_cutoff(uint32_t admission_id,
                                                   uint32_t cutoff_id);
bool app_anchor_host_command_submit_needs_retry(int submit_result);
bool app_anchor_host_command_submit_is_contract_failure(int submit_result);
int app_anchor_host_command_ingress_result(int submit_result);

#endif
