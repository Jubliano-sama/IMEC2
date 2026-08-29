#ifndef UWB_RF_SCOPE_H
#define UWB_RF_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#define UWB_RF_SCOPE_WIRE_LEN 1u
#define UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS 8u

enum uwb_rf_scope_role {
    UWB_RF_SCOPE_ROLE_GATEWAY = 0,
    UWB_RF_SCOPE_ROLE_ANCHOR = 1,
    UWB_RF_SCOPE_ROLE_CLICKER = 2,
};

struct uwb_rf_scope {
    uint8_t layer;
    bool clicker;
};

/*
 * Project the bench-only forced-hop depth onto physical RF layers:
 * gateway=0, ordinary anchor=1, and forced-N anchor=N+1. Clickers are mobile
 * and deliberately exempt from the layer boundary in both directions.
 */
int uwb_rf_scope_build(enum uwb_rf_scope_role role,
                       uint8_t forced_relay_hops,
                       struct uwb_rf_scope *out);
int uwb_rf_scope_encode(const struct uwb_rf_scope *scope, uint8_t *wire);
int uwb_rf_scope_decode(uint8_t wire, struct uwb_rf_scope *out);
bool uwb_rf_scope_visible(const struct uwb_rf_scope *local,
                          const struct uwb_rf_scope *remote);

#endif
