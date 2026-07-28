#ifndef APP_MESH_RADIO_CLIENT_H
#define APP_MESH_RADIO_CLIENT_H

#include "app_mesh_radio_owner_types.h"

#include <stdbool.h>

int app_mesh_radio_owner_try_claim(
    enum app_mesh_radio_client client, const char *reason,
    struct app_mesh_radio_owner_lease *lease_out);
int app_mesh_radio_owner_release(struct app_mesh_radio_owner_lease *lease);
bool app_mesh_radio_owner_busy(void);
int app_mesh_radio_owner_abort_request(
    enum app_mesh_radio_abort_kind kind,
    struct app_mesh_radio_owner_abort_lease *lease_in_out);
int app_mesh_radio_owner_abort_release(
    struct app_mesh_radio_owner_abort_lease *lease_in_out);
typedef void (*app_mesh_radio_owner_schedule_failure_fn)(
    void *ctx, int error, uint32_t token);

#endif
