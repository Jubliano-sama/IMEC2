#ifndef APP_DEVICE_IDENTITY_H
#define APP_DEVICE_IDENTITY_H

#include <stdint.h>

#ifndef IMEC_USE_HARDWARE_DEVICE_ID
#define IMEC_USE_HARDWARE_DEVICE_ID 0
#endif

int app_device_identity_init(void);
uint64_t app_device_hardware_id(void);
uint64_t app_device_id(void);

#endif
