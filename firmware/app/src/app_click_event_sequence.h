#ifndef APP_CLICK_EVENT_SEQUENCE_H
#define APP_CLICK_EVENT_SEQUENCE_H

#include <stdint.h>

/*
 * Semantic event numbers are intentionally volatile and clicker-local. The
 * click-report transport session separately binds the full clicker ID to this
 * counter; do not add flash writes merely to preserve it across uncommon
 * restarts.
 */
int app_click_event_sequence_init(void);
int app_click_event_sequence_next(uint32_t *event_seq);

#endif
