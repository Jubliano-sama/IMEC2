#ifndef DWM3000_DRIVER_H
#define DWM3000_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

#define DWM3000_BROADCAST_ID UINT64_C(0xffffffffffffffff)

struct dwm3000_range_request {
    uint64_t initiator_id;
    uint64_t responder_id;
    uint32_t session_id;
    uint8_t seq;
    uint8_t flags;
    uint32_t timeout_ms;
};

struct dwm3000_range_result {
    uint64_t responder_id;
    int32_t distance_mm;
    uint8_t quality;
    enum range_status status;
};

int dwm3000_driver_probe(uint32_t *dev_id);
int dwm3000_driver_initialise(bool idle_after_init);
int dwm3000_driver_configure_default(void);
int dwm3000_driver_standby(void);
int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
                                   struct dwm3000_range_result *result);
int dwm3000_driver_responder_poll_once(uint64_t local_anchor_id,
                                       uint32_t timeout_ms,
                                       struct dwm3000_range_result *result);

#endif
