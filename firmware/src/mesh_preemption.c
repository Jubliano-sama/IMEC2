#include "mesh_preemption.h"

#include "protocol.h"

#include <string.h>

static void copy_pending_click_report(const struct mesh_pending_tx *pending,
                                      struct mesh_click_preempt_plan *plan)
{
    if (pending == NULL || plan == NULL) {
        return;
    }

    plan->click_report.packet = pending->packet;
    plan->click_report.payload_len = pending->payload_len;
    if (plan->click_report.payload_len > 0u) {
        memcpy(plan->click_report.payload,
               pending->payload,
               plan->click_report.payload_len);
    }
    plan->requeue_click_report = true;
}

int mesh_prepare_click_preemption(struct mesh_relay *relay,
                                  uint64_t local_id,
                                  uint32_t now_ms,
                                  struct mesh_click_preempt_plan *plan)
{
    bool pending_local_click_report;
    bool pending_transit_click_report;

    if (relay == NULL || plan == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(plan, 0, sizeof(*plan));
    if (!mesh_relay_tx_active(relay)) {
        return PROTO_OK;
    }

    pending_local_click_report =
        relay->pending.packet.msg_type == MSG_CLICK_REPORT &&
        relay->pending.packet.src_id == local_id;
    pending_transit_click_report =
        relay->pending.packet.msg_type == MSG_CLICK_REPORT &&
        relay->pending.packet.src_id != local_id;

    if (pending_local_click_report) {
        copy_pending_click_report(&relay->pending, plan);
    }

    if (!pending_transit_click_report &&
        mesh_relay_defer_tx(relay, now_ms)) {
        plan->save_outbox = true;
        plan->schedule_timeout = true;
        return PROTO_OK;
    }

    mesh_relay_cancel_tx(relay);
    plan->clear_outbox = true;
    plan->cancel_timeout = true;
    return PROTO_OK;
}
