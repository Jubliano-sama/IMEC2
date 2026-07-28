#include "app_mesh_transport_gate.h"

#include "app_mesh_radio_owner.h"
#include "app_watchdog.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_mesh_transport_gate, LOG_LEVEL_DBG);

static struct app_mesh_radio_owner_pause_lease pause_lease;
static struct app_mesh_radio_owner_abort_lease abort_lease;

static int fail_closed(const char *operation, int error)
{
    LOG_ERR("mesh transport exact %s failed: %d", operation, error);
    app_watchdog_stop_feeding();
    return error;
}

bool app_mesh_transport_gate_paused(void)
{
    return app_mesh_radio_owner_paused();
}

int app_mesh_transport_gate_pause(void)
{
    int ret = app_mesh_radio_owner_pause(&pause_lease);

    return ret < 0 ? fail_closed("pause", ret) : 0;
}

int app_mesh_transport_gate_request_abort(void)
{
    int ret = app_mesh_radio_owner_abort_request(
        APP_MESH_RADIO_ABORT_TRANSPORT_PAUSE, &abort_lease);

    return ret < 0 ? fail_closed("abort request", ret) : 0;
}

int app_mesh_transport_gate_resume(void)
{
    int ret = app_mesh_radio_owner_abort_release(&abort_lease);

    if (ret < 0) {
        return fail_closed("abort release", ret);
    }
    ret = app_mesh_radio_owner_resume(&pause_lease);
    return ret < 0 ? fail_closed("resume", ret) : 0;
}
