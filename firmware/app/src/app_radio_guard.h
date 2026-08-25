#ifndef APP_RADIO_GUARD_H
#define APP_RADIO_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The DWM3000 is one physical resource.  A lease identifies the exact
 * operation that owns it, so an old callback cannot release a newer owner.
 */
enum radio_guard_uwb_client {
    RADIO_GUARD_UWB_CLIENT_NONE = 0,
    RADIO_GUARD_UWB_CLIENT_LEGACY,
    RADIO_GUARD_UWB_CLIENT_MESH_RX,
    RADIO_GUARD_UWB_CLIENT_MESH_TX,
    RADIO_GUARD_UWB_CLIENT_CLICKER,
    RADIO_GUARD_UWB_CLIENT_ANCHOR_SCAN,
    RADIO_GUARD_UWB_CLIENT_ANCHOR_CLICK,
    RADIO_GUARD_UWB_CLIENT_SURVEY,
    RADIO_GUARD_UWB_CLIENT_COUNT,
};

enum radio_guard_uwb_phase {
    RADIO_GUARD_UWB_IDLE = 0,
    RADIO_GUARD_UWB_ACTIVE,
    RADIO_GUARD_UWB_RELEASING,
    RADIO_GUARD_UWB_POISONED,
};

struct radio_guard_uwb_lease {
    uint32_t generation;
    enum radio_guard_uwb_client client;
};

int radio_guard_uwb_claim(enum radio_guard_uwb_client client,
                          const char *reason,
                          struct radio_guard_uwb_lease *lease_out);
int radio_guard_uwb_release_begin(const struct radio_guard_uwb_lease *lease);
/*
 * Complete the second half of release after parking the DWM3000.  A failed
 * parking result retains the exact owner in POISONED state and is returned
 * unchanged; no later claimant or rearm may treat that radio as available.
 */
int radio_guard_uwb_release_finish(struct radio_guard_uwb_lease *lease,
                                   int parking_result);
bool radio_guard_uwb_busy(void);
enum radio_guard_uwb_client radio_guard_uwb_owner_client(void);
bool radio_guard_uwb_rearm_allowed(void);
bool radio_guard_uwb_poisoned(void);
int radio_guard_uwb_poison_error(void);
enum radio_guard_uwb_phase radio_guard_uwb_phase(void);
void radio_guard_uwb_admission_pause(void);
void radio_guard_uwb_admission_resume(void);
bool radio_guard_uwb_admission_paused(void);

/* Compatibility only: new paths must retain and release an explicit lease. */
int radio_guard_uwb_start(const char *reason);
void radio_guard_uwb_stop(void);

#endif
