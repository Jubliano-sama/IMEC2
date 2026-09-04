#ifndef ZEPHYR_KERNEL_H_
#define ZEPHYR_KERNEL_H_

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(value) (value)
#define ARG_UNUSED(value) (void)(value)

typedef atomic_int atomic_t;
typedef int k_timeout_t;

struct k_thread {
    struct {
        size_t size;
    } stack_info;
    const char *name;
    size_t unused;
};

typedef struct k_thread *k_tid_t;
typedef unsigned int k_spinlock_key_t;

struct k_spinlock {
    unsigned int unused;
};

k_tid_t zephyr_shim_current_thread(void);
void zephyr_shim_thread_foreach(void (*callback)(const struct k_thread *,
                                                  void *),
                                void *user_data);

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
    (void)lock;
    return 0u;
}

static inline void k_spin_unlock(struct k_spinlock *lock,
                                 k_spinlock_key_t key)
{
    (void)lock;
    (void)key;
}

static inline k_tid_t k_current_get(void)
{
    return zephyr_shim_current_thread();
}

static inline int k_thread_name_set(k_tid_t thread, const char *name)
{
    thread->name = name;
    return 0;
}

static inline const char *k_thread_name_get(k_tid_t thread)
{
    return thread->name;
}

static inline int k_thread_stack_space_get(const struct k_thread *thread,
                                           size_t *unused)
{
    *unused = thread->unused;
    return 0;
}

static inline void k_thread_foreach_unlocked(
    void (*callback)(const struct k_thread *, void *),
    void *user_data)
{
    zephyr_shim_thread_foreach(callback, user_data);
}

static inline int atomic_get(const atomic_t *target)
{
    return atomic_load(target);
}

static inline int atomic_set(atomic_t *target, int value)
{
    return atomic_exchange(target, value);
}

struct k_mutex {
    pthread_mutex_t guard;
    pthread_cond_t available;
    pthread_t owner;
    uint32_t lock_count;
};

#define K_MUTEX_DEFINE(name) \
    struct k_mutex name = { \
        .guard = PTHREAD_MUTEX_INITIALIZER, \
        .available = PTHREAD_COND_INITIALIZER, \
    }

static inline bool k_is_in_isr(void)
{
    return false;
}

/*
 * The native scenarios are single-stepped from the reschedule seam, so the
 * cooperative hand-back a target replay performs between records has no
 * scheduler to model here.
 */
static inline void k_yield(void)
{
}

static inline int32_t k_sleep(k_timeout_t timeout)
{
    (void)timeout;
    return 0;
}

static inline int k_mutex_lock(struct k_mutex *mutex, int timeout)
{
    pthread_t self;

    (void)timeout;
    if (mutex == NULL) {
        return -EINVAL;
    }
    self = pthread_self();
    (void)pthread_mutex_lock(&mutex->guard);
    while (mutex->lock_count != 0u &&
           !pthread_equal(mutex->owner, self)) {
        (void)pthread_cond_wait(&mutex->available, &mutex->guard);
    }
    mutex->owner = self;
    mutex->lock_count++;
    (void)pthread_mutex_unlock(&mutex->guard);
    return 0;
}

static inline void k_mutex_unlock(struct k_mutex *mutex)
{
    if (mutex == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&mutex->guard);
    if (mutex->lock_count > 0u &&
        pthread_equal(mutex->owner, pthread_self())) {
        mutex->lock_count--;
        if (mutex->lock_count == 0u) {
            (void)pthread_cond_signal(&mutex->available);
        }
    }
    (void)pthread_mutex_unlock(&mutex->guard);
}

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work {
    k_work_handler_t handler;
};

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
    struct k_work work;
    int cancel_result;
    int reschedule_result;
    uint32_t cancel_calls;
    uint32_t reschedule_calls;
    struct k_work_q *last_queue;
};

void zephyr_shim_note_work_reschedule(struct k_work_delayable *work,
                                      int timeout) __attribute__((weak));

int64_t k_uptime_get(void);

static inline uint32_t k_uptime_get_32(void)
{
    return (uint32_t)k_uptime_get();
}

static inline void k_work_init_delayable(struct k_work_delayable *work,
                                         k_work_handler_t handler)
{
    memset(work, 0, sizeof(*work));
    work->work.handler = handler;
}

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
    if (zephyr_shim_note_work_reschedule != NULL) {
        zephyr_shim_note_work_reschedule(work, timeout);
    }
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
    if (zephyr_shim_note_work_reschedule != NULL) {
        zephyr_shim_note_work_reschedule(work, timeout);
    }
    return work->reschedule_result;
}

#endif
