#ifndef MESH_RELAY_CHANNEL9_H
#define MESH_RELAY_CHANNEL9_H

#include "mesh_relay.h"

int relay_channel9_timing_index(const struct mesh_relay *relay,
                                uint64_t next_hop_id);

bool relay_channel9_plan_misses_event(enum mesh_event_plan_action action);

#endif
