#ifndef APP_MESH_RADIO_HANDOFF_H
#define APP_MESH_RADIO_HANDOFF_H

#include "app_mesh_radio_owner_types.h"

#include <stdbool.h>

bool app_mesh_radio_owner_rx_scan_rearm_allowed(void);
int app_mesh_radio_owner_rx_scan_try_claim(
    const char *reason,
    struct app_mesh_radio_owner_lease *lease_out);
int app_mesh_radio_owner_rx_scan_release(
    struct app_mesh_radio_owner_lease *lease);

int app_mesh_radio_owner_scheduled_control_begin(
    bool *wait_for_scan_boundary);
bool app_mesh_radio_owner_scheduled_control_pending(void);
bool app_mesh_radio_owner_scheduled_control_ready(void);
bool app_mesh_radio_owner_scheduled_control_end(void);

int app_mesh_radio_owner_inline_control_begin(bool *abort_scan);
bool app_mesh_radio_owner_inline_control_ready(void);
void app_mesh_radio_owner_inline_control_end(void);

#endif
