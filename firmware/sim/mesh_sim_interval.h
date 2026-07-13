#ifndef MESH_SIM_INTERVAL_H
#define MESH_SIM_INTERVAL_H

#include <stdbool.h>
#include <stdint.h>

bool mesh_sim_interval_overlaps(uint64_t a_start,
                                uint64_t a_end,
                                uint64_t b_start,
                                uint64_t b_end);

#endif
