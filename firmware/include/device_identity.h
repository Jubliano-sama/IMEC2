#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "IMECANC" plus the anchor role value. XOR keeps the 64-bit mapping bijective. */
#define DEVICE_IDENTITY_ANCHOR_DOMAIN UINT64_C(0x494d4543414e4302)

uint64_t device_identity_ficr_value(uint32_t deviceid_word0,
                                    uint32_t deviceid_word1);
bool device_identity_anchor_from_ficr(uint32_t deviceid_word0,
                                      uint32_t deviceid_word1,
                                      uint64_t *anchor_id);

#ifdef __cplusplus
}
#endif

#endif
