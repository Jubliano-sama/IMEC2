#ifndef MESH_PREEMPTION_H
#define MESH_PREEMPTION_H

#include "mesh.h"
#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

struct mesh_click_preempt_plan {
    bool requeue_click_report;
    bool save_outbox;
    bool clear_outbox;
    bool cancel_timeout;
    bool schedule_timeout;
    struct mesh_outbound click_report;
};

int mesh_prepare_click_preemption(struct mesh_relay *relay,
                                  uint64_t local_id,
                                  uint32_t now_ms,
                                  struct mesh_click_preempt_plan *plan);

#endif
