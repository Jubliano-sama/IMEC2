#ifndef APP_CLICKER_H
#define APP_CLICKER_H

#include "app_config.h"
#include "status.h"

#include <stdbool.h>
#include <stdint.h>

struct k_work;
struct dwm3000_range_result;
struct mesh_outbound;
struct uwb_clicker_session;
struct uwb_discovery_reply_frame;
struct uwb_range_schedule_frame;
struct uwb_range_step;

enum app_clicker_early_led_event {
    APP_CLICKER_EARLY_LED_SYSTEMOFF_BUTTON_WAKE,
    APP_CLICKER_EARLY_LED_SYSTEMOFF_CAPTURE_FAILED,
    APP_CLICKER_EARLY_LED_BUTTON_WAKE_INPUT_UNAVAILABLE,
    APP_CLICKER_EARLY_LED_BUTTON_STILL_HELD_NO_WAKE_ARM,
    APP_CLICKER_EARLY_LED_BUTTON_WAKE_ARM_FAILED,
    APP_CLICKER_EARLY_LED_SYSTEMON_BUTTON_PRESS,
};

typedef void (*app_clicker_early_led_handler_t)(enum app_clicker_early_led_event event);
typedef int (*app_clicker_run_handler_t)(void);
typedef int (*app_clicker_mesh_send_handler_t)(
    const struct mesh_outbound *out,
    const char *reason);
typedef uint8_t (*app_clicker_discovery_slot_count_handler_t)(void);
typedef void (*app_clicker_discovery_reply_handler_t)(
    const struct uwb_discovery_reply_frame *reply);
typedef int (*app_clicker_seed_cached_anchors_handler_t)(
    struct uwb_clicker_session *session,
    uint8_t max_anchor_count);
typedef void (*app_clicker_void_handler_t)(void);
typedef bool (*app_clicker_bool_handler_t)(void);
typedef int (*app_clicker_relax_schedule_handler_t)(
    struct uwb_range_schedule_frame *schedule,
    bool post_burst_diagnostics);
typedef int (*app_clicker_range_sample_handler_t)(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_step *step,
    const struct dwm3000_range_result *range_result);
typedef void (*app_clicker_post_burst_handler_t)(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    int64_t schedule_tx_ms,
    int64_t click_deadline_ms);

struct app_clicker_callbacks {
    app_clicker_early_led_handler_t early_led;
    app_clicker_run_handler_t run_stage0_simulated_click;
    app_clicker_run_handler_t run_stage0_hardware_self_test;
    app_clicker_mesh_send_handler_t send_mesh_outbound;
    app_clicker_discovery_slot_count_handler_t ml_discovery_slot_count_override;
    app_clicker_discovery_reply_handler_t ml_cache_note_discovery_reply;
    app_clicker_seed_cached_anchors_handler_t ml_seed_cached_anchors;
    app_clicker_void_handler_t ml_note_cached_discovery_used;
    app_clicker_relax_schedule_handler_t ml_relax_range_schedule;
    app_clicker_bool_handler_t ml_runtime_active;
    app_clicker_range_sample_handler_t ml_emit_range_sample_if_active;
    app_clicker_bool_handler_t ml_continue_after_range_start_failure;
    app_clicker_bool_handler_t ml_should_continue_ranging;
    app_clicker_void_handler_t ml_enter_range_quiet;
    app_clicker_void_handler_t ml_exit_range_quiet;
    app_clicker_post_burst_handler_t ml_run_post_burst_diagnostics;
};

struct app_clicker_attempt_gate_config {
    uint32_t wake_adv_ms;
    uint32_t max_politeness_wait_ms;
    uint32_t polite_sample_rx_ms;
    uint8_t polite_required_quiet_samples;
    uint16_t polite_relevant_frame_wait_ms;
    uint32_t ble_courtesy_min_window_ms;
    uint32_t ble_courtesy_peer_finish_ms;
    uint8_t ble_courtesy_max_defers_per_attempt;
};

struct app_clicker_wake_train_config {
    uint32_t wake_adv_ms;
    uint32_t post_wake_claimed_duration_ms;
    uint32_t control_tx_timeout_ms;
};

struct app_clicker_range_tx_config {
    uint32_t control_tx_timeout_ms;
    bool prepare_range_mode_after_schedule;
};

struct app_clicker_continuous_wake_claims_config {
    uint32_t wake_adv_ms;
    uint8_t min_anchor_count;
    uint8_t max_anchor_count;
    uint8_t max_attempts;
    uint8_t samples_per_anchor;
    uint8_t wake_channel;
    uint8_t ranging_channel;
    uint8_t flags;
    struct app_clicker_wake_train_config wake_train;
};

int app_clicker_init(const struct app_clicker_callbacks *callbacks);
static inline bool clicker_systemon_retained_idle_enabled(void)
{
    return DEVICE_ROLE == ROLE_CLICKER &&
           IS_ENABLED(CONFIG_IMEC_CLICKER_SYSTEMON_RETAINED_IDLE) &&
           !IS_ENABLED(CONFIG_IMEC_STAGE1_TAG_CONTINUOUS_WAKE_CLAIMS);
}
int app_clicker_start_work_queue(void);
int app_clicker_submit_work(struct k_work *work);
uint8_t app_clicker_debug_min_anchor_count(void);
uint8_t app_clicker_debug_max_anchor_count(void);
uint8_t app_clicker_debug_samples_per_anchor(void);
uint8_t app_clicker_debug_session_flags(void);
void app_clicker_prepare_startup_idle(enum button_action *boot_action);
int app_clicker_button_init(void);
void app_clicker_handle_button_action(enum button_action action);
void app_clicker_enter_idle(void);
int app_clicker_attempt_gate(struct uwb_clicker_session *session,
                             uint32_t event_seq,
                             uint64_t priority_id,
                             int64_t click_deadline_ms,
                             bool use_ble_courtesy,
                             const struct app_clicker_attempt_gate_config *config);
int app_clicker_send_wake_claim_train(struct uwb_clicker_session *session,
                                      uint64_t priority_id,
                                      const struct app_clicker_wake_train_config *config);
int app_clicker_discover_uwb_anchors(struct uwb_clicker_session *session);
int app_clicker_collect_uwb_attempt_with_options(
    struct uwb_clicker_session *session,
    uint64_t priority_id,
    struct uwb_range_schedule_frame *schedule,
    bool allow_cached_discovery,
    bool post_burst_diagnostics);
int app_clicker_range_scheduled_anchors(struct uwb_clicker_session *session,
                                        const struct uwb_range_schedule_frame *schedule,
                                        int64_t click_deadline_ms,
                                        uint8_t *attempted_count);
void app_clicker_run_continuous_wake_claims(
    const struct app_clicker_continuous_wake_claims_config *config);

#endif
