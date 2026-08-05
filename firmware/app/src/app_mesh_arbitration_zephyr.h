#ifndef APP_MESH_ARBITRATION_ZEPHYR_H
#define APP_MESH_ARBITRATION_ZEPHYR_H

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stdint.h>

/* Zephyr bindings for the shared, native-tested gateway priority seam. */
struct app_mesh_arbitration_zephyr_gateway_ops {
    bool gateway_role;
    struct k_work_q *priority_work_queue;
};

typedef void (*app_mesh_arbitration_zephyr_schedule_failure_fn)(void *ctx,
                                                               int error,
                                                               uint32_t generation,
                                                               uint32_t admission_cutoff);

int app_mesh_arbitration_zephyr_gateway_command_submit(
    const struct app_mesh_arbitration_zephyr_gateway_ops *ops,
    struct k_work_delayable *work);
int app_mesh_arbitration_zephyr_gateway_bind_admission_cutoff(
    struct k_work_delayable *work,
    uint32_t admission_cutoff);
int app_mesh_arbitration_zephyr_gateway_receive_abort_observed(void);
void app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
    app_mesh_arbitration_zephyr_schedule_failure_fn handler,
    void *ctx);

#endif
