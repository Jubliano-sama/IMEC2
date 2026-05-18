#ifndef UWB_SESSION_H
#define UWB_SESSION_H

#include "uwb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UWB_SESSION_DISCOVERY_CAPACITY UWB_DISCOVERY_SLOT_COUNT
#define UWB_SESSION_DEFAULT_MIN_ANCHORS 4u
#define UWB_SESSION_DEFAULT_MAX_ATTEMPTS 6u
#define UWB_SESSION_MAX_FAILED_RANGING_PER_ANCHOR 2u
#define UWB_SESSION_REPLY_DELAY_TOLERANCE_US 50u
#define UWB_CLICKER_CONTENTION_SLOT_MS 12u
#define UWB_CLICKER_CONTENTION_ATTEMPT1_SLOTS 16u
#define UWB_CLICKER_CONTENTION_ATTEMPT2_SLOTS 32u
#define UWB_CLICKER_CONTENTION_ATTEMPT3_PLUS_SLOTS 64u
#define UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US 400u

enum uwb_clicker_state {
    UWB_CLICKER_IDLE = 0,
    UWB_CLICKER_POLITENESS = 1,
    UWB_CLICKER_WAKE = 2,
    UWB_CLICKER_DISCOVERY = 3,
    UWB_CLICKER_SCHEDULED = 4,
    UWB_CLICKER_RANGING = 5,
    UWB_CLICKER_RETRY_WAIT = 6,
    UWB_CLICKER_SUCCEEDED = 7,
    UWB_CLICKER_FAILED = 8,
};

enum uwb_anchor_state {
    UWB_ANCHOR_IDLE = 0,
    UWB_ANCHOR_CLAIMED = 1,
    UWB_ANCHOR_DISCOVERY_REPLIED = 2,
    UWB_ANCHOR_SCHEDULED = 3,
    UWB_ANCHOR_RANGING = 4,
    UWB_ANCHOR_ABORTED = 5,
};

enum uwb_wake_decode_failure {
    UWB_WAKE_DECODE_PREAMBLE_ONLY = 0,
    UWB_WAKE_DECODE_SFD_TIMEOUT = 1,
    UWB_WAKE_DECODE_FRAME_TIMEOUT = 2,
    UWB_WAKE_DECODE_CRC_FAILURE = 3,
};

struct uwb_session_diagnostics {
    uint32_t scans;
    uint32_t preambles;
    uint32_t sfd_timeouts;
    uint32_t frame_timeouts;
    uint32_t crc_failures;
    uint32_t claims;
    uint32_t collisions;
    uint32_t arbitration_wins;
    uint32_t arbitration_losses;
    uint32_t discovery_replies;
    uint32_t schedules;
    uint32_t ds_twr_successes;
    uint32_t ds_twr_failures;
    uint32_t timing_rejections;
    uint32_t retries;
    uint32_t false_wake_cooldowns;
    uint32_t scan_startup_time_us;
    uint32_t scan_pll_time_us;
    uint32_t scan_rx_time_us;
    uint32_t awake_time_us;
    uint32_t uwb_mesh_packets;
    uint32_t sample_order_count;
    uint32_t politeness_samples;
    uint32_t politeness_activity_hits;
    uint32_t contention_delay_ms;
    uint32_t retry_delay_ms;
    uint32_t wake_claim_tx_count;
};

struct uwb_clicker_config {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint64_t nonce;
    uint8_t min_anchor_count;
    uint8_t max_anchor_count;
    uint8_t max_attempts;
    uint8_t samples_per_anchor;
    uint8_t wake_channel;
    uint8_t ranging_channel;
    uint8_t flags;
};

struct uwb_anchor_candidate {
    uint64_t anchor_id;
    uint8_t anchor_slot;
    uint8_t rx_quality;
    uint8_t sample_count;
    uint8_t failure_count;
    bool ranged_ok;
};

struct uwb_clicker_session {
    struct uwb_clicker_config config;
    enum uwb_clicker_state state;
    struct uwb_anchor_candidate candidates[UWB_SESSION_DISCOVERY_CAPACITY];
    uint64_t successful_anchor_ids[UWB_SESSION_DISCOVERY_CAPACITY];
    struct uwb_range_schedule_frame schedule;
    struct uwb_session_diagnostics diagnostics;
    size_t next_sample_index;
    uint8_t attempt_index;
    uint8_t candidate_count;
    uint8_t successful_unique_count;
    bool range_step_active;
};

struct uwb_range_step {
    uint64_t anchor_id;
    uint8_t anchor_index;
    uint8_t round_index;
    uint8_t seq;
    size_t sample_index;
};

struct uwb_anchor_config {
    uint32_t network_id;
    uint64_t anchor_id;
    uint8_t anchor_slot;
    uint8_t wake_channel;
    uint8_t ranging_channel;
};

struct uwb_range_exchange_identity {
    uint32_t network_id;
    uint64_t clicker_id;
    uint32_t click_event_id;
    uint8_t attempt_index;
    uint64_t nonce;
    uint64_t anchor_id;
    uint8_t ranging_channel;
    uint16_t reply_delay_us;
    uint8_t seq;
    uint8_t flags;
};

struct uwb_anchor_session {
    struct uwb_anchor_config config;
    struct uwb_anchor_epoch epoch;
    struct uwb_range_schedule_entry schedule_entry;
    struct uwb_session_diagnostics diagnostics;
    enum uwb_anchor_state state;
    uint32_t uwb_wait_deadline_ms;
    uint16_t reply_delay_us;
    uint8_t expected_ranging_channel;
    bool scheduled;
};

int uwb_clicker_session_start(struct uwb_clicker_session *session,
                              const struct uwb_clicker_config *config);
int uwb_clicker_build_wake_claim(struct uwb_clicker_session *session,
                                 uint64_t priority_id,
                                 uint16_t wake_train_ends_in_ms,
                                 uint16_t discovery_starts_in_ms,
                                 uint16_t claimed_duration_ms,
                                 struct uwb_wake_claim_frame *claim);
int uwb_clicker_build_discover(struct uwb_clicker_session *session,
                               struct uwb_discover_frame *discover);
int uwb_clicker_note_discovery_reply(struct uwb_clicker_session *session,
                                     const struct uwb_discovery_reply_frame *reply);
int uwb_clicker_build_range_schedule(struct uwb_clicker_session *session,
                                     uint16_t reply_delay_us,
                                     uint16_t first_poll_delay_ms,
                                     uint16_t poll_spacing_ms,
                                     struct uwb_range_schedule_frame *schedule);
int uwb_clicker_build_range_release(struct uwb_clicker_session *session,
                                    uint8_t reason,
                                    struct uwb_range_release_frame *release);
int uwb_clicker_next_range_step(struct uwb_clicker_session *session,
                                struct uwb_range_step *step);
int uwb_clicker_record_range_result(struct uwb_clicker_session *session,
                                    const struct uwb_range_step *step,
                                    enum range_status status);
int uwb_clicker_abort_attempt(struct uwb_clicker_session *session);
int uwb_clicker_prepare_retry(struct uwb_clicker_session *session);
uint8_t uwb_clicker_contention_window_slots(uint8_t attempt_index);
uint32_t uwb_clicker_contention_delay_ms(uint8_t attempt_index,
                                         uint32_t random_value);
uint32_t uwb_clicker_wake_claim_jitter_us(uint32_t random_value);
void uwb_clicker_note_politeness_sample(struct uwb_clicker_session *session,
                                        bool activity_detected);
void uwb_clicker_note_contention_delay(struct uwb_clicker_session *session,
                                       uint32_t delay_ms);
void uwb_clicker_note_retry_delay(struct uwb_clicker_session *session,
                                  uint32_t delay_ms);
void uwb_clicker_note_wake_claim_tx(struct uwb_clicker_session *session,
                                    uint32_t tx_count);

int uwb_anchor_session_init(struct uwb_anchor_session *session,
                            const struct uwb_anchor_config *config);
void uwb_anchor_note_idle_scan(struct uwb_anchor_session *session,
                               uint16_t startup_us,
                               uint16_t pll_us,
                               uint16_t rx_us,
                               bool preamble_detected);
void uwb_anchor_note_awake_time(struct uwb_anchor_session *session,
                                uint32_t awake_us);
void uwb_anchor_note_wake_decode_failure(struct uwb_anchor_session *session,
                                         enum uwb_wake_decode_failure failure);
void uwb_anchor_note_false_wake_cooldown(struct uwb_anchor_session *session);
void uwb_anchor_note_timing_rejection(struct uwb_anchor_session *session);
int uwb_anchor_note_range_result(struct uwb_anchor_session *session,
                                 enum range_status status);
void uwb_anchor_note_mesh_packet(struct uwb_anchor_session *session);
void uwb_anchor_note_sample_order(struct uwb_anchor_session *session);
int uwb_anchor_accept_wake_claim(struct uwb_anchor_session *session,
                                 const struct uwb_wake_claim_frame *claim,
                                 uint32_t now_ms,
                                 enum uwb_anchor_claim_decision *decision);
int uwb_anchor_build_discovery_reply(struct uwb_anchor_session *session,
                                     const struct uwb_discover_frame *discover,
                                     uint8_t rx_quality,
                                     uint16_t battery_mv,
                                     struct uwb_discovery_reply_frame *reply);
int uwb_anchor_accept_range_schedule(struct uwb_anchor_session *session,
                                     const struct uwb_range_schedule_frame *schedule,
                                     uint32_t now_ms,
                                     uint16_t guard_ms);
int uwb_anchor_accept_range_release(struct uwb_anchor_session *session,
                                    const struct uwb_range_release_frame *release);
bool uwb_anchor_accepts_range_exchange(const struct uwb_anchor_session *session,
                                       const struct uwb_range_exchange_identity *identity);
int uwb_anchor_range_round_index(const struct uwb_anchor_session *session,
                                 const struct uwb_range_exchange_identity *identity,
                                 uint8_t *round_index);
void uwb_anchor_abort_epoch(struct uwb_anchor_session *session);

int uwb_session_validate_reply_timing(uint16_t poll_to_resp_us,
                                      uint16_t resp_to_final_us,
                                      uint16_t expected_reply_delay_us,
                                      uint16_t tolerance_us);
uint16_t uwb_session_short_addr_from_id(uint64_t device_id);
uint32_t uwb_session_status_bits_from_diagnostics(const struct uwb_session_diagnostics *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
