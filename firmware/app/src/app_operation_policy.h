#ifndef APP_OPERATION_POLICY_H
#define APP_OPERATION_POLICY_H

#include "operation_policy.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_OPERATION_POLICY_ASSIGNMENT_MASK (1u << 0)
#define APP_OPERATION_POLICY_ALL_MASK APP_OPERATION_POLICY_ASSIGNMENT_MASK

struct app_operation_policy_candidate {
    struct operation_policy_set updates;
    struct operation_policy_set resolved;
};

/* The active assignment profile is RAM-only and replaced atomically. */
void app_operation_policy_reset_defaults(void);
int app_operation_policy_prepare_payload(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t required_mask,
    uint8_t allowed_mask,
    struct app_operation_policy_candidate *candidate);
void app_operation_policy_commit_prepared(
    const struct app_operation_policy_candidate *candidate);
int app_operation_policy_install(const struct operation_policy_set *set,
                                 uint8_t required_mask);
void app_operation_policy_snapshot(struct operation_policy_set *set);

#ifdef __cplusplus
}
#endif

#endif
