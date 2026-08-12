#ifndef APP_CLICK_EVENT_SEQUENCE_H
#define APP_CLICK_EVENT_SEQUENCE_H

#include <stdint.h>

#define APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_INITIAL_MS \
    (UINT32_C(60) * UINT32_C(60) * UINT32_C(1000))
#define APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_MAX_MS \
    (UINT32_C(8) * UINT32_C(60) * UINT32_C(60) * UINT32_C(1000))
#define APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_HORIZON_MS \
    ((uint64_t)APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_INITIAL_MS + \
     (UINT64_C(2) * \
      (uint64_t)APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_INITIAL_MS) + \
     (UINT64_C(4) * \
      (uint64_t)APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_INITIAL_MS) + \
     (uint64_t)APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_MAX_MS)

/*
 * Production clickers reserve durable semantic identities in 65536-value
 * blocks. The active block and one prefetched standby block live only in RAM,
 * so click admission never performs storage I/O. A reboot consumes a fresh
 * block, so no identity remembered by the gateway can be reused.
 * Compatibility-only non-durable images retain a boot-local counter.
 */
int app_click_event_sequence_init(void);
int app_click_event_sequence_next(uint32_t *event_seq);

/*
 * Refill the RAM standby block after the active block reaches its half-block
 * threshold. Call only from a non-admission maintenance seam. Transient
 * storage errors retry at a bounded backoff; terminal exhaustion is retained.
 */
int app_click_event_sequence_maintain(void);

#if defined(APP_CLICK_EVENT_SEQUENCE_TESTING)
void app_click_event_sequence_test_reset(void);
#endif

#endif
