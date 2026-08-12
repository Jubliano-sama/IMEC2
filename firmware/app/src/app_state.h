#ifndef APP_STATE_H
#define APP_STATE_H

#include "app_discovery_assignment_policy.h"
#include "app_radio_guard.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Temporary mechanical-refactor shared state.
 * New code should not add globals here unless two or more modules truly
 * require ownership. Prefer moving state into the owning module after the
 * first behavior-preserving split.
 */

extern struct k_spinlock anchor_uwb_lock;
extern bool anchor_uwb_busy;
extern bool anchor_click_window_busy;
extern bool mesh_uwb_rx_active;
extern bool anchor_heartbeat_enabled;
extern struct mesh_relay mesh_runtime;
extern struct mesh_event_diagnostics mesh_event_stats;
extern struct uwb_anchor_session anchor_uwb_session;
extern uint32_t anchor_uwb_scan_interval_ms;

const char *role_name(void);
const char *command_status_name(enum command_status status);
const char *claim_decision_name(enum uwb_anchor_claim_decision decision);
const char *range_status_name(enum range_status status);
bool range_status_valid(enum range_status status);
bool mesh_id_is_unicast(uint64_t node_id);
bool gateway_ble_transport_enabled(void);
int mesh_errno_from_proto(int ret);
bool anchor_uwb_window_active(void);
bool anchor_click_window_active(void);
void anchor_click_window_set_active(bool active);
void mesh_outbound_refresh_age(struct mesh_outbound *out, uint32_t now_ms);
bool mesh_outbound_ready_for_tx(const struct mesh_outbound *out, uint32_t now_ms);
void packet_age_add_elapsed(struct proto_packet *packet, uint32_t elapsed_ms);
bool uptime_deadline_reached(uint32_t now_ms, uint32_t deadline_ms);
uint32_t uptime_ms_until_deadline(uint32_t now_ms, uint32_t deadline_ms);
uint16_t mesh_next_event_control_seq(void);
/* Durable production boot incarnation; random only in non-durable builds. */
uint64_t mesh_event_boot_nonce(void);
uint32_t nonzero_uptime_session_id(void);
uint16_t local_uwb_short_addr(void);
uint32_t discovery_window_ms_for_slots(uint8_t slot_count);
int local_anchor_discovery_slot(uint8_t slot_count, uint8_t *anchor_slot);
int local_anchor_restore_discovery_assignment(uint32_t epoch,
                                              uint32_t table_seq,
                                              const struct discovery_assignment_table_commitment *table_commitment,
                                              uint8_t anchor_slot,
                                              uint8_t slot_count,
                                              bool provisioned,
                                              bool ordered_epoch_valid,
                                              const uint32_t *retired_epochs,
                                              uint8_t retired_epoch_count,
                                              uint32_t pending_epoch,
                                              uint32_t pending_table_seq,
                                              const struct discovery_assignment_table_commitment *pending_table_commitment,
                                              bool pending_valid);
int local_anchor_commit_discovery_assignment(uint32_t epoch,
                                             uint32_t table_seq,
                                             const struct discovery_assignment_table_commitment *table_commitment,
                                             uint8_t anchor_slot,
                                             uint8_t slot_count);
void local_anchor_reset_discovery_assignment(void);
int local_anchor_mark_discovery_assignment_unprovisioned(
    uint32_t epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment);
bool local_anchor_discovery_assignment_project_pending_commit(
    uint32_t next_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint32_t *retired_epochs,
    uint8_t *retired_epoch_count);
bool local_anchor_discovery_assignment_export_retired_epochs(
    uint32_t *retired_epochs,
    uint8_t *retired_epoch_count);
enum app_discovery_assignment_claim_decision
local_anchor_discovery_assignment_note_claim(uint32_t epoch);
enum app_discovery_assignment_table_decision
local_anchor_discovery_assignment_note_table(uint32_t epoch,
                                             uint32_t table_seq,
                                             const struct discovery_assignment_table_commitment *table_commitment);
enum app_discovery_assignment_table_decision
local_anchor_discovery_assignment_preview_table(uint32_t epoch,
                                                uint32_t table_seq,
                                                const struct discovery_assignment_table_commitment *table_commitment);
enum app_discovery_assignment_provisioning_state
local_anchor_discovery_assignment_provisioning_state(void);
bool local_anchor_discovery_assignment_get(uint32_t *epoch,
                                           uint8_t *anchor_slot,
                                           uint8_t *slot_count);
uint8_t local_survey_discovery_slot(uint8_t slot_count);
uint64_t clicker_priority_id(uint32_t event_seq, uint8_t attempt_index);
uint64_t clicker_nonce(uint32_t event_seq);
uint8_t survey_sample_seq(uint16_t sample_index);
uint32_t u32_saturating_add(uint32_t lhs, uint32_t rhs);
uint16_t delay_ms_to_u16(int64_t delay_ms);
uint32_t uwb_schedule_burst_id(uint32_t event_seq, uint8_t attempt_index);
int64_t scheduled_range_sample_target_us(int64_t schedule_start_ms,
                                         const struct uwb_range_schedule_frame *schedule,
                                         size_t sample_index);
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_ML_ANCHOR)
int64_t scheduled_post_burst_diag_target_us(
    int64_t schedule_start_ms,
    const struct uwb_range_schedule_frame *schedule,
    size_t total_samples,
    uint8_t entry_index);
uint8_t scheduled_post_burst_diag_seq(const struct uwb_range_schedule_entry *entry);
#endif
void anchor_sequence_timestamp_at(int64_t local_ms,
                                  uint64_t *timestamp_ms);
int64_t ceil_us_to_ms(int64_t value_us);
int32_t average_i32_nearest(int64_t sum, uint16_t count);
void sleep_until_ms(int64_t target_ms);
uint32_t sleep_with_uwb_standby_until_ms(int64_t target_ms);
uint32_t sleep_with_uwb_idle_until_ms(int64_t target_ms);
void sleep_precise_us(uint32_t delay_us);
void sleep_until_us(int64_t target_us);
#endif
