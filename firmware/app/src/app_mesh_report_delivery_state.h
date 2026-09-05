#ifndef APP_MESH_REPORT_DELIVERY_STATE_H
#define APP_MESH_REPORT_DELIVERY_STATE_H

#include "mesh_relay.h"

#define MESH_REPORT_DELIVERY_CAPACITY 4u

struct mesh_report_delivery_entry {
    struct mesh_outbound outbound;
    /* A received ACK remains terminal while local completion is retried. */
    bool acked;
};

struct mesh_report_delivery_state {
    struct mesh_report_delivery_entry entries[MESH_REPORT_DELIVERY_CAPACITY];
    uint64_t next_hop_id;
    uint8_t count;
};

#endif
