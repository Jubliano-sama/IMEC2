#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable network-node domain. This retains the original anchor-domain value
 * so existing anchor wire identities do not move as clickers join the same
 * hardware-derived namespace. XOR keeps the 64-bit mapping bijective.
 */
#define DEVICE_IDENTITY_NODE_DOMAIN UINT64_C(0x494d4543414e4302)
#define DEVICE_IDENTITY_ANCHOR_DOMAIN DEVICE_IDENTITY_NODE_DOMAIN
#define DEVICE_IDENTITY_LEGACY_FIXED_CLICKER_ID UINT64_C(0x1111111111111111)
#define DEVICE_IDENTITY_DEFAULT_GATEWAY_ID UINT64_C(0x9999888877776666)

uint64_t device_identity_ficr_value(uint32_t deviceid_word0,
                                    uint32_t deviceid_word1);
bool device_identity_node_from_ficr(uint32_t deviceid_word0,
                                    uint32_t deviceid_word1,
                                    uint64_t *node_id);
/* Compatibility name for callers that still reason specifically about anchors. */
bool device_identity_anchor_from_ficr(uint32_t deviceid_word0,
                                      uint32_t deviceid_word1,
                                      uint64_t *anchor_id);

#ifdef __cplusplus
}
#endif

#endif
