#include "uwb_rf_scope.h"

#include <errno.h>
#include <stddef.h>

#define UWB_RF_SCOPE_MARKER_MASK 0xc0u
#define UWB_RF_SCOPE_MARKER 0xc0u
#define UWB_RF_SCOPE_CLICKER_BIT 0x20u
#define UWB_RF_SCOPE_LAYER_MASK 0x1fu
#define UWB_RF_SCOPE_MAX_LAYER (UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS + 1u)

int uwb_rf_scope_build(enum uwb_rf_scope_role role,
                       uint8_t forced_relay_hops,
                       struct uwb_rf_scope *out)
{
    struct uwb_rf_scope scope = {0};

    if (out == NULL || forced_relay_hops >
                           UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS) {
        return -EINVAL;
    }

    switch (role) {
    case UWB_RF_SCOPE_ROLE_GATEWAY:
        if (forced_relay_hops != 0u) {
            return -EINVAL;
        }
        break;
    case UWB_RF_SCOPE_ROLE_ANCHOR:
        scope.layer = (uint8_t)(forced_relay_hops + 1u);
        break;
    case UWB_RF_SCOPE_ROLE_CLICKER:
        if (forced_relay_hops != 0u) {
            return -EINVAL;
        }
        scope.clicker = true;
        break;
    default:
        return -EINVAL;
    }

    *out = scope;
    return 0;
}

int uwb_rf_scope_encode(const struct uwb_rf_scope *scope, uint8_t *wire)
{
    if (scope == NULL || wire == NULL || scope->layer > UWB_RF_SCOPE_MAX_LAYER ||
        (scope->clicker && scope->layer != 0u)) {
        return -EINVAL;
    }

    *wire = UWB_RF_SCOPE_MARKER | scope->layer |
            (scope->clicker ? UWB_RF_SCOPE_CLICKER_BIT : 0u);
    return 0;
}

int uwb_rf_scope_decode(uint8_t wire, struct uwb_rf_scope *out)
{
    struct uwb_rf_scope scope;

    if (out == NULL ||
        (wire & UWB_RF_SCOPE_MARKER_MASK) != UWB_RF_SCOPE_MARKER) {
        return -EINVAL;
    }

    scope.layer = wire & UWB_RF_SCOPE_LAYER_MASK;
    scope.clicker = (wire & UWB_RF_SCOPE_CLICKER_BIT) != 0u;
    if (scope.layer > UWB_RF_SCOPE_MAX_LAYER ||
        (scope.clicker && scope.layer != 0u)) {
        return -EINVAL;
    }

    *out = scope;
    return 0;
}

bool uwb_rf_scope_visible(const struct uwb_rf_scope *local,
                          const struct uwb_rf_scope *remote)
{
    uint8_t distance;

    if (local == NULL || remote == NULL) {
        return false;
    }
    if (local->clicker || remote->clicker) {
        return true;
    }

    distance = local->layer >= remote->layer ?
        (uint8_t)(local->layer - remote->layer) :
        (uint8_t)(remote->layer - local->layer);
    return distance <= 1u;
}
