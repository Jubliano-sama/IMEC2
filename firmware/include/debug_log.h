#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int debug_log_format_prefix(char *buffer,
                            size_t buffer_len,
                            uint32_t uptime_ms,
                            const char *role,
                            uint64_t device_id,
                            int stage,
                            const char *event);

#ifdef __cplusplus
}
#endif

#endif
