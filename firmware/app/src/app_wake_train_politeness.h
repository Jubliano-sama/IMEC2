#ifndef APP_WAKE_TRAIN_POLITENESS_H
#define APP_WAKE_TRAIN_POLITENESS_H

#include "dwm3000_driver.h"

#include <stdbool.h>
#include <stdint.h>

#define APP_WAKE_TRAIN_POLITE_SNIFF_MS 5u
#define APP_WAKE_TRAIN_POLITE_BACKOFF_MIN_MS 200u
#define APP_WAKE_TRAIN_POLITE_BACKOFF_MAX_MS 2000u
#define APP_WAKE_TRAIN_POLITE_MAX_RETRIES 6u

bool app_wake_train_politeness_rx_activity(int rx_ret,
                                           enum dwm3000_rx_failure failure);
uint32_t app_wake_train_politeness_backoff_ms(uint8_t retry_index,
                                              uint32_t random_value);

#endif
