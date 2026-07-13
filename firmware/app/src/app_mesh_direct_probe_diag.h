#ifndef APP_MESH_DIRECT_PROBE_DIAG_H
#define APP_MESH_DIRECT_PROBE_DIAG_H

#include <stdint.h>

enum app_mesh_direct_probe_phase {
    APP_MESH_DIRECT_PROBE_PHASE_NONE = 0,
    APP_MESH_DIRECT_PROBE_PHASE_SCRATCH_ACQUIRE,
    APP_MESH_DIRECT_PROBE_PHASE_SCAN_GUARD_TRANSITION,
    APP_MESH_DIRECT_PROBE_PHASE_CH9_CONFIGURE_RECOVERY,
    APP_MESH_DIRECT_PROBE_PHASE_PAYLOAD_MODE_COMPLETE,
};

void app_mesh_direct_probe_breadcrumb_note(enum app_mesh_direct_probe_phase phase,
                                           uint8_t attempt,
                                           uint16_t sequence);
void app_mesh_direct_probe_breadcrumb_boot_diagnostics(void);

#endif
