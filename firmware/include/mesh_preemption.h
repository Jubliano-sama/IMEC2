#ifndef MESH_PREEMPTION_H
#define MESH_PREEMPTION_H

#include "mesh.h"
#include "mesh_relay.h"

#include <stdbool.h>
#include <stdint.h>

struct mesh_click_preempt_plan {
    /*
     * A transferable local click has one atomic ownership transfer: report
     * queue admission and release of relay->pending. A local multi-hop report
     * with an exact HOP_ACK still receivable remains in relay->pending instead,
     * so click preemption cannot erase its receive priority. The application
     * must not split a transfer into a fallible queue operation followed by a
     * blind cancellation.
     */
    bool transfer_local_click;
    /* Non-local work stays in the relay and is paused at retry backoff. */
    bool defer_active_tx;
    bool schedule_timeout;
    struct mesh_outbound click_report;
};

int mesh_prepare_click_preemption(struct mesh_relay *relay,
                                  uint64_t local_id,
                                  uint32_t now_ms,
                                  struct mesh_click_preempt_plan *plan);

#endif
