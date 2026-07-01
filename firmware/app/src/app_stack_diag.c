#include "app_stack_diag.h"

#include "app_board.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define STACK_DIAG_FIRST_DELAY_MS 3000u
#define STACK_DIAG_INTERVAL_MS 30000u

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && defined(CONFIG_USE_SEGGER_RTT) && \
    defined(CONFIG_THREAD_MONITOR) && defined(CONFIG_THREAD_STACK_INFO) && \
    defined(CONFIG_INIT_STACKS)

static struct k_work_delayable stack_diag_work;
static bool stack_diag_started;

static const char *stack_diag_thread_name(const struct k_thread *thread)
{
#if defined(CONFIG_THREAD_NAME)
    const char *name = k_thread_name_get((k_tid_t)thread);

    if (name != NULL && name[0] != '\0') {
        return name;
    }
#else
    ARG_UNUSED(thread);
#endif

    return "unknown";
}

static void stack_diag_thread_cb(const struct k_thread *thread, void *user_data)
{
    size_t unused = 0u;
    uint32_t stack_size;
    uint32_t stack_used = 0u;
    int ret;

    ARG_UNUSED(user_data);

    ret = k_thread_stack_space_get(thread, &unused);
    stack_size = (uint32_t)thread->stack_info.size;
    if (ret == 0 && stack_size > unused) {
        stack_used = stack_size - (uint32_t)unused;
    }

    status_debug_printf("DBG_STACK name=%s tid=%p used=%u free=%u size=%u ret=%d\n",
                        stack_diag_thread_name(thread),
                        thread,
                        stack_used,
                        ret == 0 ? (uint32_t)unused : 0u,
                        stack_size,
                        ret);
}

static void stack_diag_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    status_debug_printf("DBG_STACK_BEGIN uptime=%u\n", k_uptime_get_32());
    k_thread_foreach_unlocked(stack_diag_thread_cb, NULL);
    status_debug_note("DBG_STACK_END\n");
    (void)k_work_reschedule(&stack_diag_work,
                            K_MSEC(STACK_DIAG_INTERVAL_MS));
}

#endif

void app_stack_diag_start(void)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && defined(CONFIG_USE_SEGGER_RTT) && \
    defined(CONFIG_THREAD_MONITOR) && defined(CONFIG_THREAD_STACK_INFO) && \
    defined(CONFIG_INIT_STACKS)
    if ((DEVICE_ROLE != ROLE_GATEWAY &&
         !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) ||
        stack_diag_started) {
        return;
    }

    stack_diag_started = true;
    k_work_init_delayable(&stack_diag_work, stack_diag_work_handler);
    (void)k_work_reschedule(&stack_diag_work,
                            K_MSEC(STACK_DIAG_FIRST_DELAY_MS));
#endif
}
