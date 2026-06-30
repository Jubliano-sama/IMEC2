#ifndef APP_HIGH_DEBUG_H
#define APP_HIGH_DEBUG_H

#include "app_clicker.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct uwb_anchor_session;
struct uwb_wake_claim_frame;

enum stage1_led_phase {
    STAGE1_LED_PHASE_IDLE,
    STAGE1_LED_PHASE_SCAN,
    STAGE1_LED_PHASE_WAKE,
    STAGE1_LED_PHASE_DISCOVERY,
    STAGE1_LED_PHASE_SCHEDULE,
    STAGE1_LED_PHASE_RANGE,
};

enum stage1_led_result {
    STAGE1_LED_RESULT_OFF,
    STAGE1_LED_RESULT_ACTIVE,
    STAGE1_LED_RESULT_OK,
    STAGE1_LED_RESULT_TIMEOUT,
    STAGE1_LED_RESULT_ERROR,
};

#if defined(CONFIG_IMEC_HIGH_DEBUG)
typedef bool (*app_high_debug_bool_handler_t)(void);
typedef int (*app_high_debug_command_handler_t)(const char *command);

struct app_high_debug_callbacks {
    app_high_debug_bool_handler_t command_poll_enabled;
    app_high_debug_command_handler_t handle_command;
};

enum high_debug_counter_field {
    HIGH_DEBUG_COUNTER_boot_count,
    HIGH_DEBUG_COUNTER_dwm_dev_id_successes,
    HIGH_DEBUG_COUNTER_dwm_dev_id_failures,
    HIGH_DEBUG_COUNTER_wake_claim_tx,
    HIGH_DEBUG_COUNTER_wake_claim_rx,
    HIGH_DEBUG_COUNTER_wake_claim_accepted,
    HIGH_DEBUG_COUNTER_wake_claim_rejected,
    HIGH_DEBUG_COUNTER_discovery_tx,
    HIGH_DEBUG_COUNTER_discovery_rx,
    HIGH_DEBUG_COUNTER_discovery_reply_tx,
    HIGH_DEBUG_COUNTER_discovery_reply_rx,
    HIGH_DEBUG_COUNTER_schedules_tx,
    HIGH_DEBUG_COUNTER_schedules_rx,
    HIGH_DEBUG_COUNTER_schedules_accepted,
    HIGH_DEBUG_COUNTER_schedules_rejected,
    HIGH_DEBUG_COUNTER_ds_twr_attempts,
    HIGH_DEBUG_COUNTER_ds_twr_successes,
    HIGH_DEBUG_COUNTER_ds_twr_failures,
    HIGH_DEBUG_COUNTER_ds_twr_timing_rejects,
    HIGH_DEBUG_COUNTER_mesh_tx,
    HIGH_DEBUG_COUNTER_mesh_rx,
    HIGH_DEBUG_COUNTER_mesh_ack,
    HIGH_DEBUG_COUNTER_mesh_retry,
    HIGH_DEBUG_COUNTER_mesh_drop,
    HIGH_DEBUG_COUNTER_gateway_packets_emitted,
    HIGH_DEBUG_COUNTER_gateway_ble_connects,
    HIGH_DEBUG_COUNTER_gateway_ble_disconnects,
    HIGH_DEBUG_COUNTER_gateway_ble_notify_failures,
    HIGH_DEBUG_COUNTER_gateway_ble_rx_drops,
    HIGH_DEBUG_COUNTER_bootloader_entry_requests,
    HIGH_DEBUG_COUNTER_command_rx,
    HIGH_DEBUG_COUNTER_command_result_tx,
};

#define HIGH_DEBUG_COUNTER_INC(field) high_debug_counter_inc(HIGH_DEBUG_COUNTER_##field)

void high_debug_counter_inc(enum high_debug_counter_field field);
void high_debug_log_event(const char *event, const char *fmt, ...);
void high_debug_dump_counters(const char *event);
void high_debug_boot_banner(void);
int high_debug_request_bootloader(void);
void app_high_debug_set_callbacks(const struct app_high_debug_callbacks *callbacks);
bool app_high_debug_command_poll_enabled(void);
void app_high_debug_start(bool schedule_counter_work);
int high_debug_probe_dwm3000(void);
void high_debug_stage0_rainbow_led_test(void);
int high_debug_stage0_hardware_self_test(void);
int high_debug_stage0_simulated_click(void);
int high_debug_handle_command(const char *command);
bool stage1_anchor_focused_rx_logs_enabled(void);
void stage1_anchor_focused_note_rx_frame(size_t frame_len,
                                         uint8_t quality,
                                         int decode_ret);
void stage1_anchor_focused_note_wake_claim(
    const struct uwb_wake_claim_frame *claim);
void stage1_anchor_focused_log_diagnostics(
    int ret,
    const char *rx_failure,
    bool preamble_detected,
    const struct uwb_anchor_session *session);
void stage1_click_diag(const char *fmt, ...);
void stage1_click_trace_reset(void);
void stage1_click_trace_dump(const char *reason);
void stage1_led_phase(enum stage1_led_phase phase);
void stage1_led_result(enum stage1_led_result result);
void stage1_led_hold_click_result(int ret, uint32_t hold_ms);
void high_debug_clicker_early_led(enum app_clicker_early_led_event event);
#else
#define HIGH_DEBUG_COUNTER_INC(field) do { } while (0)

static inline void high_debug_log_event(const char *event, const char *fmt, ...)
{
    (void)event;
    (void)fmt;
}

static inline void high_debug_dump_counters(const char *event)
{
    (void)event;
}

static inline void high_debug_boot_banner(void)
{
}

static inline int high_debug_request_bootloader(void)
{
    return -1;
}

static inline bool app_high_debug_command_poll_enabled(void)
{
    return false;
}

static inline void app_high_debug_start(bool schedule_counter_work)
{
    (void)schedule_counter_work;
}

static inline int high_debug_probe_dwm3000(void)
{
    return -1;
}

static inline void high_debug_stage0_rainbow_led_test(void)
{
}

static inline int high_debug_stage0_hardware_self_test(void)
{
    return -1;
}

static inline int high_debug_stage0_simulated_click(void)
{
    return -1;
}

static inline int high_debug_handle_command(const char *command)
{
    (void)command;
    return -1;
}

static inline bool stage1_anchor_focused_rx_logs_enabled(void)
{
    return false;
}

static inline void stage1_anchor_focused_note_rx_frame(size_t frame_len,
                                                       uint8_t quality,
                                                       int decode_ret)
{
    (void)frame_len;
    (void)quality;
    (void)decode_ret;
}

static inline void stage1_anchor_focused_note_wake_claim(
    const struct uwb_wake_claim_frame *claim)
{
    (void)claim;
}

static inline void stage1_anchor_focused_log_diagnostics(
    int ret,
    const char *rx_failure,
    bool preamble_detected,
    const struct uwb_anchor_session *session)
{
    (void)ret;
    (void)rx_failure;
    (void)preamble_detected;
    (void)session;
}

static inline void stage1_click_diag(const char *fmt, ...)
{
    (void)fmt;
}

static inline void stage1_click_trace_reset(void)
{
}

static inline void stage1_click_trace_dump(const char *reason)
{
    (void)reason;
}

static inline void stage1_led_phase(enum stage1_led_phase phase)
{
    (void)phase;
}

static inline void stage1_led_result(enum stage1_led_result result)
{
    (void)result;
}

static inline void stage1_led_hold_click_result(int ret, uint32_t hold_ms)
{
    (void)ret;
    (void)hold_ms;
}

static inline void high_debug_clicker_early_led(enum app_clicker_early_led_event event)
{
    (void)event;
}
#endif

int app_high_debug_init(void);

#endif
