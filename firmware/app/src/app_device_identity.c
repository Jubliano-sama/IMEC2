#include "app_device_identity.h"

#include "device_identity.h"

#include <zephyr/kernel.h>

#include <errno.h>

#if IMEC_USE_HARDWARE_ANCHOR_ID
#include <hal/nrf_ficr.h>
#endif

static uint64_t hardware_id;
static uint64_t network_id;
static bool identity_ready;

int app_device_identity_init(void)
{
#if IMEC_USE_HARDWARE_ANCHOR_ID
    uint32_t word0 = nrf_ficr_deviceid_get(NRF_FICR, 0u);
    uint32_t word1 = nrf_ficr_deviceid_get(NRF_FICR, 1u);

    hardware_id = device_identity_ficr_value(word0, word1);
    if (!device_identity_anchor_from_ficr(word0, word1, &network_id)) {
        hardware_id = 0u;
        network_id = 0u;
        identity_ready = false;
        return -EINVAL;
    }
    identity_ready = true;
#else
    hardware_id = 0u;
    network_id = 0u;
    identity_ready = true;
#endif
    return 0;
}

uint64_t app_device_hardware_id(void)
{
    return identity_ready ? hardware_id : 0u;
}

uint64_t app_device_id(void)
{
    if (!identity_ready && app_device_identity_init() < 0) {
        k_panic();
    }
    return network_id;
}
