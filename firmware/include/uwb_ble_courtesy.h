#ifndef UWB_BLE_COURTESY_H
#define UWB_BLE_COURTESY_H

#include "uwb.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UWB_BLE_COURTESY_COMPANY_ID 0xffffu
#define UWB_BLE_COURTESY_MARKER_VERSION 0xc1u
#define UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN 28u

struct uwb_ble_courtesy_frame {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t priority_id;
};

int uwb_ble_courtesy_encode(const struct uwb_ble_courtesy_frame *frame,
                            uint8_t *out,
                            size_t out_cap,
                            size_t *written);
int uwb_ble_courtesy_decode(const uint8_t *data,
                            size_t len,
                            struct uwb_ble_courtesy_frame *frame);

#ifdef __cplusplus
}
#endif

#endif
