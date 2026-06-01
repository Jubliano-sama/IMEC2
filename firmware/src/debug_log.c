#include "debug_log.h"

#include <inttypes.h>
#include <stdio.h>

int debug_log_format_prefix(char *buffer,
                            size_t buffer_len,
                            uint32_t uptime_ms,
                            const char *role,
                            uint64_t device_id,
                            int stage,
                            const char *event)
{
    int written;

    if (buffer == NULL || buffer_len == 0u || role == NULL || event == NULL ||
        stage < 0) {
        return -1;
    }

    written = snprintf(buffer,
                       buffer_len,
                       "[%" PRIu32 "][%s][0x%016" PRIx64 "][stage%d][%s]",
                       uptime_ms,
                       role,
                       device_id,
                       stage,
                       event);
    if (written < 0 || (size_t)written >= buffer_len) {
        return -1;
    }

    return written;
}
