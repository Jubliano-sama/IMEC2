#ifndef ZEPHYR_LOGGING_LOG_H_
#define ZEPHYR_LOGGING_LOG_H_

#define LOG_LEVEL_DBG 0
#define LOG_MODULE_REGISTER(name, level)

static inline void zephyr_shim_log_discard(const char *format, ...)
{
    (void)format;
}

#define LOG_ERR(...) zephyr_shim_log_discard(__VA_ARGS__)
#define LOG_WRN(...) zephyr_shim_log_discard(__VA_ARGS__)
#define LOG_INF(...) zephyr_shim_log_discard(__VA_ARGS__)

#endif
