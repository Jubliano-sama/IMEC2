#ifndef APP_MESH_REPORT_ENCODE_H
#define APP_MESH_REPORT_ENCODE_H

#include <stdint.h>

struct mesh_outbound;

struct app_mesh_report_encode_ops {
    int (*queue_cir_fragment)(struct mesh_outbound *outbound,
                              uint32_t *queue_depth);
};

void app_mesh_report_encode_init(
    const struct app_mesh_report_encode_ops *ops);
int app_mesh_report_encode_queue_next_cir(void);

#endif
