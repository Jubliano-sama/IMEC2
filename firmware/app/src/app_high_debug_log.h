#ifndef APP_HIGH_DEBUG_LOG_H
#define APP_HIGH_DEBUG_LOG_H

#if defined(CONFIG_IMEC_HIGH_DEBUG)
void high_debug_log_event(const char *event, const char *fmt, ...);
#else
static inline void high_debug_log_event(const char *event, const char *fmt, ...)
{
    (void)event;
    (void)fmt;
}
#endif

#endif
