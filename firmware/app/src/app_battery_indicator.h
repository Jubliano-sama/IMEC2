#ifndef APP_BATTERY_INDICATOR_H
#define APP_BATTERY_INDICATOR_H

#if defined(CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR)
int app_battery_indicator_init(void);
void app_battery_indicator_resume(void);
void app_battery_indicator_suspend(void);
#else
static inline int app_battery_indicator_init(void)
{
    return 0;
}

static inline void app_battery_indicator_resume(void)
{
}

static inline void app_battery_indicator_suspend(void)
{
}
#endif

#endif
