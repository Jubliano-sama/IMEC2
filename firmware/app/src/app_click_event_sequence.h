#ifndef APP_CLICK_EVENT_SEQUENCE_H
#define APP_CLICK_EVENT_SEQUENCE_H

#include <stdint.h>

/*
 * Reserve enough IDs per flash write to keep normal click traffic from
 * turning into a flash-wear workload. A production boot consumes a fresh
 * block even if the preceding boot left IDs unused, so reset can never reuse
 * an identity that the gateway may still remember.
 */
#define APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE 256u
#define APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR UINT32_C(0x01000000)
#define APP_CLICK_EVENT_SEQUENCE_MAX_BOOT_RESERVATIONS \
    ((UINT32_MAX - APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR) / \
     APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE)
int app_click_event_sequence_init(void);
int app_click_event_sequence_next(uint32_t *event_seq);

#endif
