#ifndef APP_MESH_PERSISTENCE_H
#define APP_MESH_PERSISTENCE_H

#include "mesh_relay.h"

#include <stdint.h>

int app_mesh_persistence_init(void);
int app_mesh_persistence_restore_outbox(struct mesh_relay *relay, uint32_t now_ms);
int app_mesh_persistence_save_outbox(struct mesh_relay *relay, uint32_t now_ms);
void app_mesh_persistence_clear_outbox(void);

#endif
