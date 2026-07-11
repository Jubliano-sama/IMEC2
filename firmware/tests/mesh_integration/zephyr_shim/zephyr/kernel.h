#ifndef ZEPHYR_KERNEL_H_
#define ZEPHYR_KERNEL_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define K_NO_WAIT 0
#define ARG_UNUSED(value) (void)(value)

struct k_work_q {
    uint32_t unused;
};

/* Native scenario shim for the two kernel operations used by app preemption. */
struct k_msgq {
    uint8_t *buffer;
    size_t msg_size;
    uint32_t max_msgs;
    uint32_t used_msgs;
};

struct k_work_delayable {
    int cancel_result;
    int reschedule_result;
    uint32_t cancel_calls;
    uint32_t reschedule_calls;
    struct k_work_q *last_queue;
};

static inline int k_msgq_put(struct k_msgq *msgq, const void *data, int timeout)
{
    size_t offset;

    (void)timeout;
    if (msgq == NULL || data == NULL || msgq->buffer == NULL ||
        msgq->msg_size == 0u || msgq->used_msgs >= msgq->max_msgs) {
        return -ENOMSG;
    }
    offset = (size_t)msgq->used_msgs * msgq->msg_size;
    memcpy(&msgq->buffer[offset], data, msgq->msg_size);
    msgq->used_msgs++;
    return 0;
}

static inline int k_work_cancel_delayable(struct k_work_delayable *work)
{
    if (work == NULL) {
        return -EINVAL;
    }
    work->cancel_calls++;
    return work->cancel_result;
}

static inline int k_work_reschedule(struct k_work_delayable *work, int timeout)
{
    (void)timeout;
    if (work == NULL) {
        return -EINVAL;
    }
    work->reschedule_calls++;
    work->last_queue = NULL;
    return work->reschedule_result;
}

static inline int k_work_reschedule_for_queue(struct k_work_q *queue,
                                              struct k_work_delayable *work,
                                              int timeout)
{
    (void)timeout;
    if (queue == NULL || work == NULL) {
        return -EINVAL;
    }
    work->reschedule_calls++;
    work->last_queue = queue;
    return work->reschedule_result;
}

#endif
