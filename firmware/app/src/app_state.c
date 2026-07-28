#include "app_config.h"
#include "app_discovery_assignment_policy.h"
#include "app_gateway_ble.h"
#include "app_mesh_radio_owner.h"
#include "app_state.h"
#include "dwm3000_driver.h"

#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_REGISTER(app_state, LOG_LEVEL_DBG);

uint32_t next_event_seq;
struct k_spinlock anchor_uwb_lock;
bool anchor_uwb_busy;
bool anchor_click_window_busy;
bool mesh_uwb_rx_active;
bool anchor_heartbeat_enabled;
struct mesh_relay mesh_runtime;
struct mesh_event_diagnostics mesh_event_stats;
struct uwb_anchor_session anchor_uwb_session;
uint32_t anchor_uwb_scan_interval_ms = ANCHOR_UWB_SCAN_INTERVAL_MS;
uint16_t mesh_event_control_seq;
static uint64_t mesh_event_boot_nonce_value;
static K_MUTEX_DEFINE(mesh_event_boot_nonce_lock);
static struct k_spinlock anchor_discovery_assignment_lock;
static struct app_discovery_assignment_policy anchor_discovery_assignment_policy;
static uint8_t anchor_discovery_assignment_slot;
static uint8_t anchor_discovery_assignment_slot_count;

const char *role_name(void)
{
    switch (DEVICE_ROLE) {
    case ROLE_CLICKER:
        return "clicker";
    case ROLE_ANCHOR:
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
            return "mesh-transmitter";
        }
        return "anchor";
    case ROLE_GATEWAY:
        return "gateway";
    default:
        return "unknown";
    }
}

const char *command_status_name(enum command_status status)
{
    switch (status) {
    case COMMAND_OK:
        return "ok";
    case COMMAND_UNSUPPORTED_COMMAND:
        return "unsupported";
    case COMMAND_MALFORMED_PAYLOAD:
        return "malformed-payload";
    case COMMAND_BUSY:
        return "busy";
    case COMMAND_DENIED:
        return "denied";
    case COMMAND_TIMEOUT:
        return "timeout";
    case COMMAND_RADIO_ERROR:
        return "radio-error";
    case COMMAND_INVALID_STATE:
        return "invalid-state";
    case COMMAND_INTERNAL_ERROR:
        return "internal-error";
    default:
        return "unknown";
    }
}

const char *claim_decision_name(enum uwb_anchor_claim_decision decision)
{
    switch (decision) {
    case UWB_ANCHOR_CLAIM_ACCEPTED:
        return "accepted";
    case UWB_ANCHOR_CLAIM_REJECTED_STALE:
        return "rejected-stale";
    case UWB_ANCHOR_CLAIM_REJECTED_BUSY:
        return "rejected-busy";
    case UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY:
        return "replaced-by-priority";
    case UWB_ANCHOR_CLAIM_REJECTED_LOST_ARBITRATION:
        return "rejected-lost-arbitration";
    case UWB_ANCHOR_CLAIM_REJECTED_MALFORMED:
        return "rejected-malformed";
    default:
        return "unknown";
    }
}

const char *range_status_name(enum range_status status)
{
    switch (status) {
    case RANGE_OK:
        return "ok";
    case RANGE_RX_TIMEOUT:
        return "rx-timeout";
    case RANGE_RX_ERROR:
        return "rx-error";
    case RANGE_BAD_FRAME:
        return "bad-frame";
    case RANGE_WRONG_TARGET:
        return "wrong-target";
    case RANGE_DELAYED_TX_MISSED:
        return "delayed-tx-missed";
    case RANGE_INTERNAL_ERROR:
        return "internal-error";
    case RANGE_TIMING_INVALID:
        return "timing-invalid";
    default:
        return "unknown";
    }
}

bool range_status_valid(enum range_status status)
{
    return status >= RANGE_OK && status <= RANGE_TIMING_INVALID;
}

int app_state_radio_owner_init(void)
{
    const struct app_mesh_radio_owner_platform_ops ops = {
#if defined(CONFIG_IMEC_GATEWAY_BLE)
        .enter_uwb_quiet = gateway_ble_enter_uwb_quiet,
        .exit_uwb_quiet = gateway_ble_exit_uwb_quiet,
#endif
        .request_receive_abort = dwm3000_driver_request_receive_abort,
        .clear_receive_abort = dwm3000_driver_clear_receive_abort,
    };

    return app_mesh_radio_owner_init(&ops);
}

bool mesh_id_is_unicast(uint64_t node_id)
{
    return node_id != MESH_BROADCAST_ID;
}

bool gateway_ble_transport_enabled(void)
{
#if defined(CONFIG_IMEC_GATEWAY_BLE)
    return IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE) &&
           (DEVICE_ROLE == ROLE_GATEWAY ||
            (DEVICE_ROLE == ROLE_CLICKER && IS_ENABLED(CONFIG_IMEC_ML_CLICKER)) ||
            (DEVICE_ROLE == ROLE_ANCHOR &&
             IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
             !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)));
#else
    return false;
#endif
}

int mesh_errno_from_proto(int ret)
{
    switch (ret) {
    case PROTO_OK:
        return 0;
    case PROTO_ERR_MALFORMED:
        return -EBUSY;
    case PROTO_ERR_NOT_FOUND:
    case PROTO_ERR_STALE:
        return -EHOSTUNREACH;
    case PROTO_ERR_NO_SPACE:
        return -ENOSPC;
    default:
        return -EINVAL;
    }
}

bool anchor_uwb_window_active(void)
{
    k_spinlock_key_t key;
    bool busy;

    key = k_spin_lock(&anchor_uwb_lock);
    busy = anchor_uwb_busy;
    k_spin_unlock(&anchor_uwb_lock, key);
    return busy;
}

bool anchor_click_window_active(void)
{
    k_spinlock_key_t key;
    bool busy;

    key = k_spin_lock(&anchor_uwb_lock);
    busy = anchor_click_window_busy;
    k_spin_unlock(&anchor_uwb_lock, key);
    return busy;
}

void anchor_click_window_set_active(bool active)
{
    k_spinlock_key_t key;

    key = k_spin_lock(&anchor_uwb_lock);
    anchor_click_window_busy = active;
    k_spin_unlock(&anchor_uwb_lock, key);
}

void mesh_outbound_refresh_age(struct mesh_outbound *out, uint32_t now_ms)
{
    int ret;

    if (out == NULL) {
        return;
    }

    if (out->queued_at_ms != 0u) {
        packet_age_add_elapsed(&out->packet, now_ms - out->queued_at_ms);
    }
    out->queued_at_ms = now_ms;
    ret = mesh_outbound_set_flood_packet_age_ms(out, out->packet.message_age_ms);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        LOG_WRN("mesh flood age TLV update failed: msg=0x%02x ret=%d",
                out->packet.msg_type,
                ret);
    }
}

bool mesh_outbound_ready_for_tx(const struct mesh_outbound *out, uint32_t now_ms)
{
    return out == NULL ||
           out->earliest_tx_ms == 0u ||
           (int32_t)(now_ms - out->earliest_tx_ms) >= 0;
}

uint32_t next_click_event_seq(void)
{
    next_event_seq++;
    if (next_event_seq == 0u) {
        next_event_seq = 1u;
    }
    return next_event_seq;
}

uint32_t packet_age_add(uint32_t age_ms, uint32_t elapsed_ms)
{
    if (UINT32_MAX - age_ms < elapsed_ms) {
        return UINT32_MAX;
    }
    return age_ms + elapsed_ms;
}

void packet_age_add_elapsed(struct proto_packet *packet, uint32_t elapsed_ms)
{
    if (packet == NULL) {
        return;
    }
    packet->message_age_ms = packet_age_add(packet->message_age_ms, elapsed_ms);
}

bool uptime_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

uint32_t uptime_ms_until_deadline(uint32_t now_ms, uint32_t deadline_ms)
{
    if (uptime_deadline_reached(now_ms, deadline_ms)) {
        return 1u;
    }

    return deadline_ms - now_ms;
}

uint16_t mesh_next_event_control_seq(void)
{
    mesh_event_control_seq++;
    if (mesh_event_control_seq == 0u) {
        mesh_event_control_seq = 1u;
    }
    return mesh_event_control_seq;
}

uint64_t mesh_event_boot_nonce(void)
{
    uint64_t value;

    k_mutex_lock(&mesh_event_boot_nonce_lock, K_FOREVER);
    if (mesh_event_boot_nonce_value == 0u) {
        mesh_event_boot_nonce_value = ((uint64_t)sys_rand32_get() << 32) |
                                      (uint64_t)sys_rand32_get();
        if (mesh_event_boot_nonce_value == 0u) {
            mesh_event_boot_nonce_value = UINT64_C(1);
        }
    }
    value = mesh_event_boot_nonce_value;
    k_mutex_unlock(&mesh_event_boot_nonce_lock);
    return value;
}

uint32_t nonzero_uptime_session_id(void)
{
    uint32_t session_id = k_uptime_get_32();

    return session_id == 0u ? 1u : session_id;
}

uint16_t local_uwb_short_addr(void)
{
    uint16_t short_addr = (uint16_t)(DEVICE_ID & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

uint32_t discovery_window_ms_for_slots(uint8_t slot_count)
{
    uint32_t slot_window_us;

    if (slot_count == 0u) {
        return 0u;
    }
    slot_window_us = (uint32_t)slot_count * UWB_DISCOVERY_SLOT_US;
    return (slot_window_us + 999u) / 1000u;
}

int local_anchor_discovery_slot(uint8_t slot_count, uint8_t *anchor_slot)
{
    if (anchor_slot == NULL || slot_count == 0u) {
        return PROTO_ERR_ARG;
    }
#if IMEC_HIGH_DEBUG_ANCHOR_SLOT_ENABLED
    if ((uint32_t)IMEC_HIGH_DEBUG_ANCHOR_SLOT >= slot_count) {
        return PROTO_ERR_MALFORMED;
    }
    *anchor_slot = (uint8_t)IMEC_HIGH_DEBUG_ANCHOR_SLOT;
    return PROTO_OK;
#elif defined(CONFIG_IMEC_ML_ANCHOR)
    return uwb_discovery_slot_for_anchor(DEVICE_ID, slot_count, anchor_slot);
#else
    uint32_t epoch = 0u;
    uint8_t assigned_slot = 0u;
    uint8_t assigned_slot_count = 0u;

    if (!local_anchor_discovery_assignment_get(&epoch,
                                               &assigned_slot,
                                               &assigned_slot_count) ||
        epoch == 0u || assigned_slot_count != slot_count ||
        assigned_slot >= assigned_slot_count) {
        /* Keep local click ownership alive while enumeration is repaired. */
        return uwb_discovery_slot_for_anchor(DEVICE_ID, slot_count, anchor_slot);
    }
    *anchor_slot = assigned_slot;
    return PROTO_OK;
#endif
}

static bool local_anchor_discovery_assignment_values_valid(uint32_t epoch,
                                                           uint8_t anchor_slot,
                                                           uint8_t slot_count)
{
    return epoch != 0u && slot_count != 0u &&
           slot_count <= UWB_DISCOVERY_SLOT_COUNT && anchor_slot < slot_count;
}

int local_anchor_restore_discovery_assignment(uint32_t epoch,
                                              uint32_t table_seq,
                                              uint32_t table_fingerprint,
                                              uint8_t anchor_slot,
                                              uint8_t slot_count,
                                              bool provisioned)
{
    k_spinlock_key_t key;

    if (epoch == 0u || table_seq == 0u || table_fingerprint == 0u ||
        slot_count == 0u || slot_count > UWB_DISCOVERY_SLOT_COUNT ||
        (provisioned && anchor_slot >= slot_count)) {
        return PROTO_ERR_MALFORMED;
    }
    key = k_spin_lock(&anchor_discovery_assignment_lock);
    app_discovery_assignment_policy_init(&anchor_discovery_assignment_policy,
                                         true,
                                         provisioned,
                                         epoch,
                                         table_seq,
                                         table_fingerprint);
    anchor_discovery_assignment_slot = provisioned ? anchor_slot : 0u;
    anchor_discovery_assignment_slot_count = provisioned ? slot_count : 0u;
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return PROTO_OK;
}

int local_anchor_commit_discovery_assignment(uint32_t epoch,
                                             uint32_t table_seq,
                                             uint32_t table_fingerprint,
                                             uint8_t anchor_slot,
                                             uint8_t slot_count)
{
    k_spinlock_key_t key;
    bool committed;

    if (!local_anchor_discovery_assignment_values_valid(epoch,
                                                        anchor_slot,
                                                        slot_count)) {
        return PROTO_ERR_MALFORMED;
    }
    key = k_spin_lock(&anchor_discovery_assignment_lock);
    committed = app_discovery_assignment_policy_commit(
        &anchor_discovery_assignment_policy,
        epoch,
        table_seq,
        table_fingerprint);
    if (committed) {
        anchor_discovery_assignment_slot = anchor_slot;
        anchor_discovery_assignment_slot_count = slot_count;
    }
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return committed ? PROTO_OK : PROTO_ERR_STALE;
}

void local_anchor_reset_discovery_assignment(void)
{
    k_spinlock_key_t key = k_spin_lock(&anchor_discovery_assignment_lock);

    app_discovery_assignment_policy_init(&anchor_discovery_assignment_policy,
                                         false,
                                         false,
                                         0u,
                                         0u,
                                         0u);
    anchor_discovery_assignment_slot = 0u;
    anchor_discovery_assignment_slot_count = 0u;
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
}

int local_anchor_mark_discovery_assignment_unprovisioned(
    uint32_t epoch,
    uint32_t table_seq,
    uint32_t table_fingerprint)
{
    k_spinlock_key_t key = k_spin_lock(&anchor_discovery_assignment_lock);
    bool accepted;

    accepted = app_discovery_assignment_policy_note_unassigned(
        &anchor_discovery_assignment_policy,
        epoch,
        table_seq,
        table_fingerprint);
    if (accepted) {
        anchor_discovery_assignment_slot = 0u;
        anchor_discovery_assignment_slot_count = 0u;
    }
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return accepted ? PROTO_OK : PROTO_ERR_STALE;
}

enum app_discovery_assignment_claim_decision
local_anchor_discovery_assignment_note_claim(uint32_t epoch)
{
    enum app_discovery_assignment_claim_decision decision;
    k_spinlock_key_t key = k_spin_lock(&anchor_discovery_assignment_lock);

    decision = app_discovery_assignment_policy_note_claim(
        &anchor_discovery_assignment_policy,
        epoch);
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return decision;
}

enum app_discovery_assignment_table_decision
local_anchor_discovery_assignment_note_table(uint32_t epoch,
                                             uint32_t table_seq,
                                             uint32_t table_fingerprint)
{
    enum app_discovery_assignment_table_decision decision;
    k_spinlock_key_t key = k_spin_lock(&anchor_discovery_assignment_lock);

    decision = app_discovery_assignment_policy_note_table(
        &anchor_discovery_assignment_policy,
        epoch,
        table_seq,
        table_fingerprint);
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return decision;
}

enum app_discovery_assignment_provisioning_state
local_anchor_discovery_assignment_provisioning_state(void)
{
    enum app_discovery_assignment_provisioning_state state;
    k_spinlock_key_t key = k_spin_lock(&anchor_discovery_assignment_lock);

    state = app_discovery_assignment_policy_provisioning_state(
        &anchor_discovery_assignment_policy);
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return state;
}

bool local_anchor_discovery_assignment_get(uint32_t *epoch,
                                           uint8_t *anchor_slot,
                                           uint8_t *slot_count)
{
    k_spinlock_key_t key;
    bool valid;

    if (epoch == NULL || anchor_slot == NULL || slot_count == NULL) {
        return false;
    }
    key = k_spin_lock(&anchor_discovery_assignment_lock);
    *epoch = anchor_discovery_assignment_policy.committed_epoch;
    *anchor_slot = anchor_discovery_assignment_slot;
    *slot_count = anchor_discovery_assignment_slot_count;
    valid = app_discovery_assignment_policy_normal_click_reply_allowed(
        &anchor_discovery_assignment_policy);
    k_spin_unlock(&anchor_discovery_assignment_lock, key);
    return valid;
}

uint8_t local_survey_discovery_slot(uint8_t slot_count)
{
    uint32_t epoch = 0u;
    uint8_t anchor_slot = 0u;
    uint8_t assigned_slot_count = 0u;

    if (slot_count != 0u &&
        local_anchor_discovery_assignment_get(&epoch,
                                              &anchor_slot,
                                              &assigned_slot_count) &&
        epoch != 0u && assigned_slot_count != 0u) {
        return (uint8_t)(anchor_slot % slot_count);
    }

    if (uwb_discovery_slot_for_anchor(DEVICE_ID, slot_count, &anchor_slot) != PROTO_OK) {
        return 0u;
    }
    return anchor_slot;
}

uint64_t mix64(uint64_t value)
{
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value == 0u ? 1u : value;
}

uint64_t clicker_priority_id(uint32_t event_seq, uint8_t attempt_index)
{
    return mix64(DEVICE_ID ^
                 ((uint64_t)event_seq << 17) ^
                 ((uint64_t)attempt_index << 49));
}

uint64_t clicker_nonce(uint32_t event_seq)
{
    return mix64(DEVICE_ID ^
                 ((uint64_t)event_seq << 32) ^
                 k_cycle_get_32());
}

uint8_t survey_sample_seq(uint16_t sample_index)
{
    uint8_t seq = (uint8_t)((sample_index + 1u) & 0xffu);

    return seq == 0u ? 1u : seq;
}

uint32_t u32_saturating_add(uint32_t lhs, uint32_t rhs)
{
    uint64_t sum = (uint64_t)lhs + rhs;

    return (uint32_t)MIN(sum, (uint64_t)UINT32_MAX);
}

uint16_t delay_ms_to_u16(int64_t delay_ms)
{
    if (delay_ms <= 0) {
        return 0u;
    }
    if (delay_ms > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)delay_ms;
}

uint32_t uwb_schedule_burst_id(uint32_t event_seq, uint8_t attempt_index)
{
    return ((event_seq & 0x00ffffffu) << 8) | attempt_index;
}

int64_t scheduled_range_sample_target_us(int64_t schedule_start_ms,
                                         const struct uwb_range_schedule_frame *schedule,
                                         size_t sample_index)
{
    return ((schedule_start_ms + schedule->first_poll_delay_ms) * 1000) +
           ((int64_t)sample_index * schedule->exchange_stride_us);
}

#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_ML_ANCHOR)
int64_t scheduled_post_burst_diag_target_us(
    int64_t schedule_start_ms,
    const struct uwb_range_schedule_frame *schedule,
    size_t total_samples,
    uint8_t entry_index)
{
    return scheduled_range_sample_target_us(schedule_start_ms, schedule, total_samples) +
           ((int64_t)UWB_POST_BURST_DIAG_GUARD_MS * 1000) +
           ((int64_t)entry_index * UWB_POST_BURST_DIAG_SLOT_MS * 1000);
}

uint8_t scheduled_post_burst_diag_seq(const struct uwb_range_schedule_entry *entry)
{
    if (entry == NULL) {
        return 0u;
    }
    return (uint8_t)(entry->seq + entry->sample_count);
}
#endif

void anchor_sequence_timestamp_at(int64_t local_ms,
                                         uint64_t *timestamp_ms)
{
    if (local_ms < 0) {
        local_ms = k_uptime_get();
    }
    if (timestamp_ms == NULL) {
        return;
    }

    *timestamp_ms = (uint64_t)local_ms;
}

int64_t ceil_us_to_ms(int64_t value_us)
{
    if (value_us <= 0) {
        return 0;
    }
    return (value_us + 999) / 1000;
}

int32_t average_i32_nearest(int64_t sum, uint16_t count)
{
    int64_t half;

    if (count == 0u) {
        return 0;
    }

    half = count / 2;
    return (int32_t)(sum >= 0 ? (sum + half) / count : (sum - half) / count);
}

void sleep_until_ms(int64_t target_ms)
{
    while (true) {
        int64_t now_ms = k_uptime_get();

        if (now_ms >= target_ms) {
            return;
        }
        k_msleep((uint32_t)MIN(10, target_ms - now_ms));
    }
}

uint32_t sleep_with_uwb_standby_until_ms(int64_t target_ms)
{
    int64_t now_ms = k_uptime_get();

    if (now_ms < target_ms) {
        int ret = dwm3000_driver_standby();

        if (ret < 0) {
            LOG_WRN("DWM3000 sleep entry before scheduled wait failed: %d", ret);
        } else {
            int64_t sleep_start_ms = k_uptime_get();

            sleep_until_ms(target_ms);
            return (uint32_t)MIN((uint64_t)MAX(0, k_uptime_get() - sleep_start_ms) *
                                 1000u,
                                 (uint64_t)UINT32_MAX);
        }
    }

    sleep_until_ms(target_ms);
    return 0u;
}

uint32_t sleep_with_uwb_idle_until_ms(int64_t target_ms)
{
    int64_t now_ms = k_uptime_get();

    if (now_ms < target_ms) {
        int ret = dwm3000_driver_idle();

        if (ret < 0) {
            LOG_WRN("DWM3000 idle entry before scheduled wait failed: %d", ret);
        } else {
            int64_t sleep_start_ms = k_uptime_get();

            sleep_until_ms(target_ms);
            return (uint32_t)MIN((uint64_t)MAX(0, k_uptime_get() - sleep_start_ms) *
                                 1000u,
                                 (uint64_t)UINT32_MAX);
        }
    }

    sleep_until_ms(target_ms);
    return 0u;
}

void sleep_precise_us(uint32_t delay_us)
{
    if (delay_us >= 1000u) {
        k_msleep(delay_us / 1000u);
        delay_us %= 1000u;
    }
    if (delay_us > 0u) {
        k_busy_wait(delay_us);
    }
}

void sleep_until_us(int64_t target_us)
{
    int64_t target_ms;
    uint32_t sub_ms_us;

    if (target_us <= 0) {
        return;
    }

    target_ms = target_us / 1000;
    sub_ms_us = (uint32_t)(target_us % 1000);
    sleep_until_ms(target_ms);
    if (sub_ms_us > 0u && k_uptime_get() <= target_ms) {
        sleep_precise_us(sub_ms_us);
    }
}

int app_state_init(void)
{
    return 0;
}
