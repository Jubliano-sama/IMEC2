#ifndef APP_CLICKER_RADIO_POWER_H
#define APP_CLICKER_RADIO_POWER_H

#include "app_mesh_radio_owner_types.h"

int app_clicker_radio_self_test_preflight(void);
int app_clicker_radio_finish_scheduled_burst(
    struct app_mesh_radio_owner_lease *radio_lease);
int app_clicker_radio_enter_retained_standby(void);
int app_clicker_radio_prepare_systemoff(
    struct app_mesh_radio_owner_lease *terminal_lease_out);

#endif
