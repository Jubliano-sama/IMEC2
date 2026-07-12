#include "device_identity.h"

#include <stddef.h>

uint64_t device_identity_ficr_value(uint32_t deviceid_word0,
                                    uint32_t deviceid_word1)
{
    return ((uint64_t)deviceid_word1 << 32) | deviceid_word0;
}

bool device_identity_anchor_from_ficr(uint32_t deviceid_word0,
                                      uint32_t deviceid_word1,
                                      uint64_t *anchor_id)
{
    uint64_t hardware_id;
    uint64_t mapped_id;

    if (anchor_id == NULL) {
        return false;
    }
    hardware_id = device_identity_ficr_value(deviceid_word0, deviceid_word1);
    mapped_id = hardware_id ^ DEVICE_IDENTITY_ANCHOR_DOMAIN;
    if (hardware_id == 0u || hardware_id == UINT64_MAX ||
        mapped_id == 0u || mapped_id == UINT64_MAX) {
        *anchor_id = 0u;
        return false;
    }
    *anchor_id = mapped_id;
    return true;
}
