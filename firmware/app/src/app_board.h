#ifndef APP_BOARD_H
#define APP_BOARD_H

#include "status.h"

#include <stdbool.h>
#include <stdint.h>

int status_leds_init(void);
void status_leds_set(bool red, bool green, bool blue);
void status_led0_set(bool red, bool green, bool blue);
void status_led1_set(bool red, bool green, bool blue);
void status_leds_disconnect(void);
void status_apply(const struct status_inputs *inputs);
int battery_adc_divider_disable(void);
int battery_sample_lithium_mv(uint16_t *battery_mv);
int debug_serial_init(void);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
int debug_serial_poll_in(unsigned char *byte);
#endif

#endif
