#ifndef WATCHDOG_ADOPTION_H
#define WATCHDOG_ADOPTION_H

#include <stdbool.h>
#include <stdint.h>

#define WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS 8u

enum watchdog_adoption_mode {
    WATCHDOG_ADOPTION_INVALID = 0,
    WATCHDOG_ADOPTION_FRESH,
    WATCHDOG_ADOPTION_INHERITED,
};

struct watchdog_adoption_plan {
    enum watchdog_adoption_mode mode;
    uint32_t reload_request_mask;
    uint8_t reload_request_count;
};

typedef void (*watchdog_adoption_feed_fn)(uint8_t reload_request, void *ctx);

int watchdog_adoption_plan(bool hardware_running,
                           uint32_t enabled_reload_request_mask,
                           uint8_t available_reload_requests,
                           struct watchdog_adoption_plan *plan);

uint8_t watchdog_adoption_feed_mask(uint32_t reload_request_mask,
                                    uint8_t available_reload_requests,
                                    bool feeding_allowed,
                                    watchdog_adoption_feed_fn feed,
                                    void *ctx);

#endif
