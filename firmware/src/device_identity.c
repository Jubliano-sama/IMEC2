#include "device_identity.h"

#include <stddef.h>

uint64_t device_identity_ficr_value(uint32_t deviceid_word0,
                                    uint32_t deviceid_word1)
{
    return ((uint64_t)deviceid_word1 << 32) | deviceid_word0;
}

bool device_identity_node_from_ficr(uint32_t deviceid_word0,
                                    uint32_t deviceid_word1,
                                    uint64_t *node_id)
{
    uint64_t hardware_id;
    uint64_t mapped_id;

    if (node_id == NULL) {
        return false;
    }
    hardware_id = device_identity_ficr_value(deviceid_word0, deviceid_word1);
    mapped_id = hardware_id ^ DEVICE_IDENTITY_NODE_DOMAIN;
    if (hardware_id == 0u || hardware_id == UINT64_MAX ||
        mapped_id == 0u || mapped_id == UINT64_MAX ||
        mapped_id == DEVICE_IDENTITY_LEGACY_FIXED_CLICKER_ID ||
        mapped_id == DEVICE_IDENTITY_DEFAULT_GATEWAY_ID) {
        *node_id = 0u;
        return false;
    }
    *node_id = mapped_id;
    return true;
}

bool device_identity_anchor_from_ficr(uint32_t deviceid_word0,
                                      uint32_t deviceid_word1,
                                      uint64_t *anchor_id)
{
    return device_identity_node_from_ficr(deviceid_word0,
                                          deviceid_word1,
                                          anchor_id);
}
