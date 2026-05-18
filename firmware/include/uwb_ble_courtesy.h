#ifndef UWB_BLE_COURTESY_H
#define UWB_BLE_COURTESY_H

#include "uwb.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UWB_BLE_COURTESY_COMPANY_ID 0xffffu
#define UWB_BLE_COURTESY_MARKER_VERSION 0xc2u
#define UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN 29u
#define UWB_BLE_COURTESY_DURATION_UNIT_MS 10u
#define UWB_BLE_COURTESY_MAX_DURATION_MS \
    (UINT8_MAX * UWB_BLE_COURTESY_DURATION_UNIT_MS)

struct uwb_ble_courtesy_frame {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t priority_id;
    uint8_t defer_duration_units;
};

uint8_t uwb_ble_courtesy_duration_units_from_ms(uint32_t duration_ms);
uint32_t uwb_ble_courtesy_duration_ms(uint8_t duration_units);

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
