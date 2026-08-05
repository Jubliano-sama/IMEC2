#ifndef APP_RADIO_RECOVERY_H
#define APP_RADIO_RECOVERY_H

int app_radio_idle_with_bounded_recovery(const char *reason);
int app_radio_standby_with_bounded_recovery(const char *reason);

#endif
