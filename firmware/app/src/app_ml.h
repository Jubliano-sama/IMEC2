#ifndef APP_ML_H
#define APP_ML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dwm3000_range_result;
struct uwb_clicker_session;
struct uwb_discovery_reply_frame;
struct uwb_range_schedule_frame;
struct uwb_range_step;

int app_ml_init(void);

#if defined(CONFIG_IMEC_ML_CLICKER)
void ml_clicker_handle_ble_frame(const uint8_t *frame, size_t frame_len);
uint8_t ml_clicker_discovery_slot_count_override(void);
void ml_clicker_cache_note_discovery_reply(
    const struct uwb_discovery_reply_frame *reply);
int ml_clicker_seed_cached_anchors(struct uwb_clicker_session *session,
                                   uint8_t max_anchor_count);
void ml_clicker_note_cached_discovery_used(void);
int ml_clicker_relax_range_schedule(struct uwb_range_schedule_frame *schedule,
                                    bool post_burst_diagnostics);
bool ml_clicker_runtime_active(void);
int ml_clicker_emit_range_sample_if_active(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    const struct uwb_range_step *step,
    const struct dwm3000_range_result *range_result);
bool ml_clicker_continue_after_range_start_failure(void);
bool ml_clicker_should_continue_ranging(void);
void ml_clicker_enter_range_quiet(void);
void ml_clicker_exit_range_quiet(void);
void ml_clicker_run_post_burst_diagnostics(
    const struct uwb_clicker_session *session,
    const struct uwb_range_schedule_frame *schedule,
    int64_t schedule_tx_ms,
    int64_t click_deadline_ms);
#endif

#if defined(CONFIG_IMEC_ML_ANCHOR)
uint16_t ml_anchor_cached_battery_mv(void);
uint8_t ml_anchor_discovery_slot(uint8_t discovery_slot_count);
uint32_t ml_anchor_run_post_burst_diagnostics(
    const struct uwb_range_schedule_frame *schedule,
    int64_t schedule_rx_ms,
    size_t total_samples);
#endif

#endif
