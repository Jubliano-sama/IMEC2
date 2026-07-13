#ifndef APP_DISCOVERY_ASSIGNMENT_STACK_H
#define APP_DISCOVERY_ASSIGNMENT_STACK_H

#include "discovery_assignment.h"
#include "mesh_relay.h"

#define APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_BYTES \
    (sizeof(struct mesh_outbound) + \
     sizeof(struct discovery_assignment_claim) * UWB_DISCOVERY_SLOT_COUNT + \
     sizeof(struct discovery_assignment_entry) * UWB_DISCOVERY_SLOT_COUNT)
#define APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_LIMIT_BYTES 4096u

#endif
