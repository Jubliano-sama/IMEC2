#include "app_mesh_route_owner_queue.h"

#include "app_config.h"

#include <errno.h>

#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
K_THREAD_STACK_DEFINE(mesh_route_work_q_stack, MESH_ROUTE_WORKQUEUE_STACK_SIZE);
static struct k_work_q mesh_route_work_q;
static const struct k_work_queue_config mesh_route_work_q_config = {
    .name = "mesh_route",
};
#endif

void mesh_route_owner_work_queue_start(void)
{
#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
    k_work_queue_start(&mesh_route_work_q,
                       mesh_route_work_q_stack,
                       K_THREAD_STACK_SIZEOF(mesh_route_work_q_stack),
                       MESH_ROUTE_WORKQUEUE_PRIORITY,
                       &mesh_route_work_q_config);
#endif
}

struct k_work_q *mesh_route_owner_work_queue(void)
{
#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
    return &mesh_route_work_q;
#else
    return NULL;
#endif
}

int mesh_route_owner_work_reschedule(struct k_work_delayable *work,
                                     k_timeout_t delay)
{
    struct k_work_q *queue = mesh_route_owner_work_queue();

    if (work == NULL) {
        return -EINVAL;
    }
    return queue == NULL ? k_work_reschedule(work, delay) :
           k_work_reschedule_for_queue(queue, work, delay);
}

int mesh_route_owner_work_submit(struct k_work *work)
{
    struct k_work_q *queue = mesh_route_owner_work_queue();

    if (work == NULL) {
        return -EINVAL;
    }
    return queue == NULL ? k_work_submit(work) :
           k_work_submit_to_queue(queue, work);
}
