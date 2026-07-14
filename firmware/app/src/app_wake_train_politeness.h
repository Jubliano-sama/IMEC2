#ifndef APP_WAKE_TRAIN_POLITENESS_H
#define APP_WAKE_TRAIN_POLITENESS_H

#include "dwm3000_driver.h"
#include "mesh_radio_timing.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_WAKE_TRAIN_POLITE_SNIFF_MS \
    (MESH_RADIO_WAKE_POLITENESS_CHECK_US / 1000u)
#define APP_WAKE_TRAIN_POLITE_BACKOFF_MIN_MS 200u
#define APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS 2000u
#define APP_WAKE_TRAIN_POLITE_BACKOFF_CAPPED_BASE_MS 1600u
#define APP_WAKE_TRAIN_POLITE_MAX_RETRIES \
    (MESH_RADIO_WAKE_OPPORTUNITIES - 1u)

_Static_assert(MESH_RADIO_WAKE_POLITENESS_CHECK_US % 1000u == 0u,
               "wake politeness check must be whole milliseconds");
_Static_assert(MESH_RADIO_WAKE_OPPORTUNITIES > 0u,
               "wake train needs at least one opportunity");
_Static_assert(APP_WAKE_TRAIN_POLITE_BACKOFF_CAPPED_BASE_MS <
                   APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS,
               "capped wake retries need a nonzero jitter window");

bool app_wake_train_politeness_rx_activity(int rx_ret,
                                           enum dwm3000_rx_failure failure);
uint32_t app_wake_train_politeness_backoff_ms(uint8_t retry_index,
                                              uint32_t random_value);

#endif
