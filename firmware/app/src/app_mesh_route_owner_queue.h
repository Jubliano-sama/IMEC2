#ifndef APP_MESH_ROUTE_OWNER_QUEUE_H
#define APP_MESH_ROUTE_OWNER_QUEUE_H

#include <zephyr/kernel.h>

/*
 * State-owner lifecycle work must remain runnable while transport admission is
 * paused so terminal cleanup cannot be stranded. These helpers select the
 * mesh-route queue in production presets and intentionally bypass only the
 * transport-paused admission check.
 */
void mesh_route_owner_work_queue_start(void);
struct k_work_q *mesh_route_owner_work_queue(void);
int mesh_route_owner_work_reschedule(struct k_work_delayable *work,
                                     k_timeout_t delay);
int mesh_route_owner_work_submit(struct k_work *work);

#endif
