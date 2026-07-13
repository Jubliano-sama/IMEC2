#ifndef APP_BOARD_H
#define APP_BOARD_H

#include "status.h"

#include <stdbool.h>
#include <stdint.h>

int status_leds_init(void);
void status_leds_set(bool red, bool green, bool blue);
void status_led0_set(bool red, bool green, bool blue);
void status_led1_set(bool red, bool green, bool blue);
void status_power_indicator_set(bool enabled);
void status_debug_tx_boot_test(void);
void status_debug_gateway_boot_test(void);
void status_debug_anchor_boot_test(void);
void status_debug_uwb_rx_channel_pulse(uint8_t uwb_channel);
void status_debug_gateway_uwb_rx_channel_pulse(uint8_t uwb_channel);
void status_debug_uwb_tx_channel_pulse(uint8_t uwb_channel);
void status_debug_tx_packet_sent_pulse(void);
void status_debug_tx_wake_claim_sent_pulse(void);
void status_debug_tx_mesh_frame_sent_pulse(void);
void status_debug_tx_gateway_ack_rx_pulse(void);
void status_debug_gateway_ack_tx_pulse(void);
void status_debug_note(const char *text);
void status_debug_printf(const char *fmt, ...);
int status_stack_diag_transaction_begin(void);
int status_stack_diag_note(const char *text);
void status_stack_diag_transaction_end(void);
void status_leds_disconnect(void);
void status_apply(const struct status_inputs *inputs);
int battery_adc_divider_disable(void);
int battery_sample_lithium_mv(uint16_t *battery_mv);
int debug_serial_init(void);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
int debug_serial_poll_in(unsigned char *byte);
#endif

#endif
