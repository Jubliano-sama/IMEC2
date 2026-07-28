#ifndef APP_CLICKER_BLE_COURTESY_H
#define APP_CLICKER_BLE_COURTESY_H

#include <stdint.h>

int app_clicker_ble_courtesy_start(uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint64_t priority_id,
                                   uint32_t peer_finish_ms);
uint32_t app_clicker_ble_courtesy_higher_wait_ms(void);
void app_clicker_ble_courtesy_stop(void);
int app_clicker_ble_courtesy_low_power_stop(void);

#endif
