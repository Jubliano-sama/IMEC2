#ifndef APP_MESH_RADIO_OWNER_POLICY_H
#define APP_MESH_RADIO_OWNER_POLICY_H

#include "app_mesh_radio_owner_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MESH_RADIO_ABORT_LEASE_CAPACITY 8u

enum app_mesh_radio_owner_phase {
    APP_MESH_RADIO_OWNER_IDLE = 0,
    APP_MESH_RADIO_OWNER_ACTIVE,
    APP_MESH_RADIO_OWNER_RELEASING,
};

enum app_mesh_radio_handoff_phase {
    APP_MESH_RADIO_HANDOFF_IDLE = 0,
    APP_MESH_RADIO_HANDOFF_WAIT_SAFE_BOUNDARY,
    APP_MESH_RADIO_HANDOFF_SCHEDULING,
    APP_MESH_RADIO_HANDOFF_GRANTED,
    APP_MESH_RADIO_HANDOFF_FAILURE_CALLBACK,
};

struct app_mesh_radio_owner_abort_slot {
    struct app_mesh_radio_owner_abort_lease lease;
};

/*
 * This policy is deliberately platform-independent and does not provide its
 * own synchronization. The runtime adapter must serialize every transition
 * and snapshot with one lock.
 */
struct app_mesh_radio_owner_policy {
    uint32_t next_generation;

    enum app_mesh_radio_owner_phase owner_phase;
    struct app_mesh_radio_owner_lease owner;

    struct app_mesh_radio_owner_pause_lease pause;
    bool pause_active;

    enum app_mesh_radio_handoff_phase handoff_phase;
    struct app_mesh_radio_owner_handoff_lease handoff;

    struct app_mesh_radio_owner_abort_slot
        aborts[APP_MESH_RADIO_ABORT_LEASE_CAPACITY];

    bool rx_scheduled_control_pending;
    bool rx_inline_control_active;
    bool rx_scan_active;
};

/*
 * The first reset requires a zero-initialized policy. Later resets invalidate
 * every live lease while preserving the generation cursor, so a pre-reset
 * token cannot own post-reset work.
 */
void app_mesh_radio_owner_policy_reset(
    struct app_mesh_radio_owner_policy *policy);

int app_mesh_radio_owner_policy_try_claim(
    struct app_mesh_radio_owner_policy *policy,
    enum app_mesh_radio_client client,
    struct app_mesh_radio_owner_lease *lease_out);
int app_mesh_radio_owner_policy_release_begin(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_lease *lease);
int app_mesh_radio_owner_policy_release_complete(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_lease *lease);
bool app_mesh_radio_owner_policy_busy(
    const struct app_mesh_radio_owner_policy *policy);
enum app_mesh_radio_owner_phase app_mesh_radio_owner_policy_phase(
    const struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_claim_snapshot(
    const struct app_mesh_radio_owner_policy *policy,
    struct app_mesh_radio_owner_lease *lease_out);

/*
 * Pass a zero generation to request a new pause lease. Reusing the exact live
 * lease is idempotent; any other nonzero generation is stale.
 */
int app_mesh_radio_owner_policy_pause(
    struct app_mesh_radio_owner_policy *policy,
    struct app_mesh_radio_owner_pause_lease *lease_in_out);
int app_mesh_radio_owner_policy_resume(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_pause_lease *lease);
bool app_mesh_radio_owner_policy_paused(
    const struct app_mesh_radio_owner_policy *policy);

/*
 * A handoff remains owned through scheduling and until the scheduled worker
 * consumes its exact grant. A duplicate request for the same identity returns
 * the existing lease; another identity is rejected while any phase is live.
 */
int app_mesh_radio_owner_policy_handoff_request(
    struct app_mesh_radio_owner_policy *policy,
    uintptr_t identity,
    struct app_mesh_radio_owner_handoff_lease *lease_out);
int app_mesh_radio_owner_policy_handoff_begin(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease);
int app_mesh_radio_owner_policy_handoff_schedule_complete(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease,
    bool success);
int app_mesh_radio_owner_policy_handoff_failure_complete(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease);
int app_mesh_radio_owner_policy_handoff_take_grant(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease);
int app_mesh_radio_owner_policy_handoff_cancel(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_handoff_lease *lease);
bool app_mesh_radio_owner_policy_handoff_waiting(
    const struct app_mesh_radio_owner_policy *policy);
enum app_mesh_radio_handoff_phase app_mesh_radio_owner_policy_handoff_phase(
    const struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_handoff_snapshot(
    const struct app_mesh_radio_owner_policy *policy,
    struct app_mesh_radio_owner_handoff_lease *lease_out);

/*
 * Pass token zero to allocate a new receive-abort lease. Reusing the exact
 * still-live token and kind is idempotent. Independent causes, including
 * causes of the same kind, occupy separate fixed-capacity slots.
 */
int app_mesh_radio_owner_policy_abort_request(
    struct app_mesh_radio_owner_policy *policy,
    enum app_mesh_radio_abort_kind kind,
    struct app_mesh_radio_owner_abort_lease *lease_in_out);
int app_mesh_radio_owner_policy_abort_release(
    struct app_mesh_radio_owner_policy *policy,
    const struct app_mesh_radio_owner_abort_lease *lease);
bool app_mesh_radio_owner_policy_abort_pending(
    const struct app_mesh_radio_owner_policy *policy);
size_t app_mesh_radio_owner_policy_abort_count(
    const struct app_mesh_radio_owner_policy *policy);

bool app_mesh_radio_owner_policy_rx_scheduled_control_request(
    struct app_mesh_radio_owner_policy *policy,
    bool *abort_scan);
bool app_mesh_radio_owner_policy_rx_scheduled_control_pending(
    const struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_rx_scheduled_control_ready(
    const struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_rx_scheduled_control_end(
    struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_rx_inline_control_begin(
    struct app_mesh_radio_owner_policy *policy,
    bool *abort_scan);
bool app_mesh_radio_owner_policy_rx_inline_control_ready(
    const struct app_mesh_radio_owner_policy *policy);
void app_mesh_radio_owner_policy_rx_inline_control_end(
    struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_rx_scan_try_begin(
    struct app_mesh_radio_owner_policy *policy);
void app_mesh_radio_owner_policy_rx_scan_end(
    struct app_mesh_radio_owner_policy *policy);
bool app_mesh_radio_owner_policy_rx_scan_rearm_allowed(
    const struct app_mesh_radio_owner_policy *policy);

#endif
