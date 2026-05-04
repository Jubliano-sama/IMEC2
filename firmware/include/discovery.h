#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_COMPANY_ID 0xFFFFu
#define BLE_DISCOVERY_REQ_LEN 31u
#define BLE_DISCOVERY_READY_LEN 38u

#define BLE_READY_STATUS_ADMITTED 0x01u
#define BLE_READY_STATUS_BUSY 0x02u
#define BLE_READY_STATUS_FULL 0x03u

struct ble_discovery_req {
    uint64_t clicker_id;
    uint64_t priority_id;
    uint32_t event_seq;
    uint16_t ready_scan_starts_in_ms;
    uint16_t ready_scan_duration_ms;
    uint8_t flags;
    uint8_t attempt_index;
    uint8_t min_anchor_count;
};

struct ble_discovery_ready {
    uint64_t anchor_id;
    uint64_t target_clicker_id;
    uint64_t priority_id_seen;
    uint32_t target_event_seq;
    uint16_t uwb_short_addr;
    uint8_t flags;
    uint8_t attempt_index;
    int8_t rssi_hint;
    uint8_t status;
};

bool ble_flags_are_diagnostic(uint8_t flags);
bool ble_flags_count_as_click(uint8_t flags);
uint8_t ble_flags_for_click(void);
uint8_t ble_flags_for_diagnostic(void);

int ble_discovery_req_encode(const struct ble_discovery_req *request,
                                  uint8_t *out,
                                  size_t out_cap,
                                  size_t *written);
int ble_discovery_req_decode(const uint8_t *data,
                                  size_t len,
                                  struct ble_discovery_req *request);
int ble_discovery_ready_encode(const struct ble_discovery_ready *ready,
                                    uint8_t *out,
                                    size_t out_cap,
                                    size_t *written);
int ble_discovery_ready_decode(const uint8_t *data,
                                    size_t len,
                                    struct ble_discovery_ready *ready);

#ifdef __cplusplus
}
#endif

#endif
