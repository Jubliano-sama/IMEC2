#ifndef MESH_SIM_INVARIANTS_H
#define MESH_SIM_INVARIANTS_H

#include "mesh_sim.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mesh_sim_invariant_code {
    MESH_SIM_INVARIANT_NONE = 0,
    MESH_SIM_INVARIANT_WORLD_ERROR,
    MESH_SIM_INVARIANT_CAPACITY,
    MESH_SIM_INVARIANT_QUEUE_COUNT,
    MESH_SIM_INVARIANT_QUEUE_ENTRY,
    MESH_SIM_INVARIANT_RELAY_STATE,
    MESH_SIM_INVARIANT_RUNTIME_STATE,
    MESH_SIM_INVARIANT_CONNECTION_OWNER,
    MESH_SIM_INVARIANT_WORKER_ACCOUNTING,
    MESH_SIM_INVARIANT_STALE_WORK,
    MESH_SIM_INVARIANT_DUPLICATE_DELIVERY,
    MESH_SIM_INVARIANT_SEMANTIC_COUNT,
    MESH_SIM_INVARIANT_PENDING_EVENT,
    MESH_SIM_INVARIANT_NOT_SETTLED,
};

struct mesh_sim_invariant_report {
    enum mesh_sim_invariant_code code;
    size_t node_index;
    size_t object_index;
    uint64_t detail;
    const char *description;
};

/* Validate ownership, bounds, identities, and operation-generation state. */
int mesh_sim_check_invariants(const struct mesh_sim_world *world,
                              struct mesh_sim_invariant_report *report);

/* Validate invariants plus complete release of finite-scenario work. */
int mesh_sim_check_settled(const struct mesh_sim_world *world,
                           struct mesh_sim_invariant_report *report);

const char *mesh_sim_invariant_name(enum mesh_sim_invariant_code code);

#ifdef __cplusplus
}
#endif

#endif
