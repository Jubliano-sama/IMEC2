#ifndef APP_MESH_TRANSPORT_GATE_H
#define APP_MESH_TRANSPORT_GATE_H

#include <stdbool.h>

bool app_mesh_transport_gate_paused(void);
int app_mesh_transport_gate_pause(void);
int app_mesh_transport_gate_request_abort(void);
int app_mesh_transport_gate_resume(void);

#endif
