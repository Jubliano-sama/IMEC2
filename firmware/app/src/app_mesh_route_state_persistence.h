#ifndef APP_MESH_ROUTE_STATE_PERSISTENCE_H
#define APP_MESH_ROUTE_STATE_PERSISTENCE_H

#include <stdint.h>

struct mesh_relay;

/*
 * Persist an explicitly validated next route generation before committing it
 * to the live relay. The caller must still serialize the subsequent relay
 * mutation with mesh RX.
 */
int app_mesh_route_state_persist(const struct mesh_relay *relay,
                                 uint32_t route_epoch,
                                 uint32_t gateway_route_adv_seq);

/* Persist the live relay ordering state after a wire-learned transition. */
int app_mesh_route_state_save(const struct mesh_relay *relay);

/*
 * Restore ordering state into a newly initialized, otherwise empty relay.
 * Returns one when a valid record was restored, zero when none exists, or a
 * negative errno when stored state cannot be trusted.
 */
int app_mesh_route_state_restore(struct mesh_relay *relay);

/* Used by native reboot/corruption tests and explicit reprovisioning flows. */
int app_mesh_route_state_clear(uint8_t role);

#endif
