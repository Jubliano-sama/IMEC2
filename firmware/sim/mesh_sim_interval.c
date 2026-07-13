#include "mesh_sim_interval.h"

bool mesh_sim_interval_overlaps(uint64_t a_start,
                                uint64_t a_end,
                                uint64_t b_start,
                                uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}
