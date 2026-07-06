#include "app_anchor.h"

#include "app_board.h"
#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_persistence.h"
#include "app_mesh_report.h"
#include "app_ml.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "route.h"
#include "serial_frame.h"
#include "status.h"
#include "survey.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_anchor, LOG_LEVEL_DBG);

#define ANCHOR_CH5_SCAN_DEBUG_INTERVAL_MS 1000u

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(UWB_RANGE_SCHEDULE_MAX_LEN <= UWB_MESH_MAX_FRAME_LEN,
             "post-wake route RX buffer must still fit normal ranging schedules");
BUILD_ASSERT(ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE >= 8192u,
             "post-wake mesh route RX needs the enlarged anchor scan stack");
BUILD_ASSERT(ANCHOR_UWB_SCAN_BUSY_RETRY_MS > 0u,
             "blocked mesh route-test anchor scans must not spin at zero delay");
#endif

static struct k_work_delayable anchor_uwb_scan_work;
static struct k_work_delayable anchor_heartbeat_work;
static struct k_work_delayable anchor_reboot_work;
static struct k_work_delayable anchor_collection_result_work;
static struct k_work_delayable anchor_command_execute_work;

#if DEVICE_ROLE == ROLE_ANCHOR
K_THREAD_STACK_DEFINE(anchor_uwb_scan_work_q_stack, ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE);
static struct k_work_q anchor_uwb_scan_work_q;
static const struct k_work_queue_config anchor_uwb_scan_work_q_config = {
    .name = "anchor_uwb_scan",
};
#endif
K_THREAD_STACK_DEFINE(anchor_survey_work_q_stack, ANCHOR_SURVEY_WORKQUEUE_STACK_SIZE);

static struct k_work_q anchor_survey_work_q;
static const struct k_work_queue_config anchor_survey_work_q_config = {
    .name = "anchor_survey",
};
static uint16_t anchor_heartbeat_seq;
static uint16_t anchor_survey_seq;
static uint32_t anchor_ch5_scan_debug_next_ms;
static uint32_t anchor_heartbeat_interval_ms = ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS;
static bool anchor_reboot_pending;
static uint32_t anchor_reboot_deadline_ms;
static struct k_spinlock anchor_survey_lock;
static struct survey_pair anchor_survey_pair;
static struct survey_discovery_config anchor_survey_discovery_config;
static uint32_t anchor_survey_discovery_start_ms;
static bool anchor_survey_pair_prepared;
static bool anchor_survey_start_pending;
static bool anchor_survey_start_as_responder;
static bool anchor_survey_running;
static bool anchor_survey_discovery_pending;
static bool anchor_scan_recovery_gap_requested;
static atomic_t anchor_survey_abort_requested;
static struct k_work_delayable anchor_survey_work;
static struct survey_gateway_context gateway_survey_context;
static bool gateway_survey_active;
static struct k_work_delayable gateway_survey_work;
static struct survey_gateway_auto_context gateway_survey_auto;
static uint32_t anchor_collection_node_boot_counter;
static uint16_t anchor_collection_result_seq;
struct anchor_collection_result_pending {
    struct proto_packet command;
    struct command_result_id result_id;
    uint32_t collection_epoch_id;
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool active;
    bool force_rediscovery_after_result;
    bool reboot_after_result;
};
static struct anchor_collection_result_pending anchor_collection_result_pending;
struct anchor_pending_command_options {
    enum command_response_mode response_mode;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint32_t collection_slot_seed;
    uint32_t command_expiry_s;
    uint16_t expected_node_count;
    bool collection_required;
};
struct anchor_pending_command_execution {
    struct proto_packet command;
    struct anchor_pending_command_options options;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint32_t received_at_ms;
    bool active;
};
static struct anchor_pending_command_execution anchor_pending_command_execution;
static struct gateway_command_rx_duplicate_cache anchor_command_seq_cache;
#if defined(CONFIG_IMEC_ML_ANCHOR)
static uint32_t anchor_run_clicker_pair_survey(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t schedule_rx_ms);
#endif
static void anchor_set_uwb_busy(bool busy);
static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us);
static int anchor_start_uwb_scan(void);
static void anchor_survey_work_handler(struct k_work *work);
static void anchor_uwb_scan_work_handler(struct k_work *work);
static void anchor_survey_schedule(k_timeout_t delay);
static void anchor_uwb_scan_schedule_ms(uint32_t delay_ms);
static bool anchor_survey_pair_queueable(const struct survey_pair *pair);
static int anchor_run_survey_discovery(const struct survey_discovery_config *config,
                                       uint32_t start_ms);
static void anchor_handle_survey_discovery_start(const struct proto_packet *packet,
                                                  const uint8_t *payload,
                                                  size_t payload_len);
static int anchor_start_survey_pair_from_command(const struct proto_packet *packet,
                                                  const uint8_t *payload,
                                                  size_t payload_len,
                                                  enum command_status *status,
                                                  uint8_t *reason);
static void anchor_abort_survey_pair(void);
static void anchor_reboot_work_handler(struct k_work *work);
static void anchor_collection_result_work_handler(struct k_work *work);
static void anchor_command_execute_work_handler(struct k_work *work);
static void anchor_schedule_reboot_after_command_result(void);
static void anchor_force_rediscovery_from_command(void);
static void gateway_survey_work_handler(struct k_work *work);
static void gateway_survey_auto_note_command_result(const struct proto_packet *command,
                                                    enum command_id command_id,
                                                    enum command_status status,
                                                    uint8_t reason);
static void gateway_survey_auto_note_command_timeout(const struct proto_packet *command,
                                                     enum command_id command_id);
static void anchor_heartbeat_work_handler(struct k_work *work);

static uint32_t anchor_collection_node_boot_id(void)
{
    if (anchor_collection_node_boot_counter == 0u) {
        anchor_collection_node_boot_counter = sys_rand32_get();
        if (anchor_collection_node_boot_counter == 0u) {
            anchor_collection_node_boot_counter = k_uptime_get_32();
        }
        if (anchor_collection_node_boot_counter == 0u) {
            anchor_collection_node_boot_counter = 1u;
        }
    }
    return anchor_collection_node_boot_counter;
}

static uint16_t anchor_next_collection_result_seq(void)
{
    anchor_collection_result_seq++;
    if (anchor_collection_result_seq == 0u) {
        anchor_collection_result_seq = 1u;
    }
    return anchor_collection_result_seq;
}

static int anchor_send_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason,
                                      const struct command_result_id *collection_result_id,
                                      uint32_t collection_epoch_id)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint32_t session_id;
    uint16_t seq;
    bool diagnostic;
    int ret;

    if (command == NULL) {
        return -EINVAL;
    }

    ret = mesh_append_command_result(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     command_id,
                                     status,
                                     reason);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (status == COMMAND_OK && command_id == CMD_GET_STATUS) {
        ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }
    if (collection_result_id != NULL) {
        ret = gateway_command_append_collection_result_identity(outbound.payload,
                                                               sizeof(outbound.payload),
                                                               &payload_len,
                                                               collection_result_id,
                                                               collection_epoch_id);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        session_id = collection_result_id->command_seq;
        seq = collection_result_id->result_seq;
    } else {
        session_id = command->session_id;
        seq = command->seq;
    }

    diagnostic = (command->flags & FLAG_DIAGNOSTIC) != 0u;
    ret = mesh_init_command_result(&outbound.packet,
                                   DEVICE_ID,
                                   GATEWAY_ID,
                                   session_id,
                                   seq,
                                   (uint8_t)payload_len,
                                   diagnostic);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.payload_len = (uint8_t)payload_len;

    ret = mesh_start_tracked_tx(&outbound, "command-result");
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
    }
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=uwb_mesh command=0x%04x status=%s reason=%u ret=%d",
                         (unsigned int)command_id,
                         command_status_name(status),
                         reason,
                         ret);
    return ret;
}

static void anchor_collection_result_clear(void)
{
    memset(&anchor_collection_result_pending, 0, sizeof(anchor_collection_result_pending));
}

static void anchor_collection_result_persist(uint32_t delay_ms)
{
    struct app_mesh_collection_result_snapshot snapshot = {
        .version = APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION,
        .local_id = DEVICE_ID,
        .gateway_id = GATEWAY_ID,
        .valid = anchor_collection_result_pending.active,
    };

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return;
    }

    snapshot.command = anchor_collection_result_pending.command;
    snapshot.result_id = anchor_collection_result_pending.result_id;
    snapshot.collection_epoch_id = anchor_collection_result_pending.collection_epoch_id;
    snapshot.delay_ms = delay_ms == 0u ? 1u : delay_ms;
    snapshot.command_id = anchor_collection_result_pending.command_id;
    snapshot.status = anchor_collection_result_pending.status;
    snapshot.reason = anchor_collection_result_pending.reason;
    snapshot.force_rediscovery_after_result =
        anchor_collection_result_pending.force_rediscovery_after_result;
    snapshot.reboot_after_result =
        anchor_collection_result_pending.reboot_after_result;

    (void)app_mesh_persistence_save_collection_result(&snapshot);
}

static int anchor_collection_result_restore(void)
{
    struct app_mesh_collection_result_snapshot snapshot;
    uint32_t delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || anchor_collection_result_pending.active) {
        return -EBUSY;
    }

    ret = app_mesh_persistence_restore_collection_result(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid) {
        return 0;
    }
    if (snapshot.local_id != DEVICE_ID ||
        snapshot.gateway_id != GATEWAY_ID ||
        snapshot.result_id.node_id != DEVICE_ID ||
        snapshot.result_id.gateway_id != GATEWAY_ID ||
        (snapshot.command.dst_id != DEVICE_ID &&
         snapshot.command.dst_id != MESH_BROADCAST_ID)) {
        app_mesh_persistence_clear_collection_result();
        return -EINVAL;
    }

    anchor_collection_result_pending.command = snapshot.command;
    anchor_collection_result_pending.result_id = snapshot.result_id;
    anchor_collection_result_pending.collection_epoch_id = snapshot.collection_epoch_id;
    anchor_collection_result_pending.command_id = snapshot.command_id;
    anchor_collection_result_pending.status = snapshot.status;
    anchor_collection_result_pending.reason = snapshot.reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        snapshot.force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result =
        snapshot.reboot_after_result;
    anchor_collection_result_pending.active = true;

    if (anchor_collection_node_boot_counter == 0u) {
        anchor_collection_node_boot_counter = snapshot.result_id.node_boot_counter;
    }
    if (anchor_collection_result_seq < snapshot.result_id.result_seq) {
        anchor_collection_result_seq = snapshot.result_id.result_seq;
    }

    delay_ms = snapshot.delay_ms == 0u ? 1u : snapshot.delay_ms;
    (void)k_work_reschedule(&anchor_collection_result_work, K_MSEC(delay_ms));
    LOG_INF("anchor collection command result restored: cmd=0x%04x delay_ms=%u",
            (unsigned int)anchor_collection_result_pending.command_id,
            delay_ms);
    return 0;
}

static void anchor_collection_result_work_handler(struct k_work *work)
{
    struct anchor_collection_result_pending pending;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return;
    }

    pending = anchor_collection_result_pending;
    anchor_collection_result_clear();

    ret = anchor_send_command_result(&pending.command,
                                     pending.command_id,
                                     pending.status,
                                     pending.reason,
                                     &pending.result_id,
                                     pending.collection_epoch_id);
    if (ret < 0) {
        LOG_WRN("anchor collection command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)pending.command_id,
                pending.status,
                ret);
        pending.active = true;
        anchor_collection_result_pending = pending;
        (void)k_work_reschedule(&anchor_collection_result_work,
                                K_MSEC(COLLECTION_RETRY_ROUND_0_MS));
        anchor_collection_result_persist(COLLECTION_RETRY_ROUND_0_MS);
        return;
    }

    app_mesh_persistence_clear_collection_result();
    LOG_INF("anchor collection command result sent: cmd=0x%04x status=%u reason=%u",
            (unsigned int)pending.command_id,
            pending.status,
            pending.reason);
    if (pending.force_rediscovery_after_result && pending.status == COMMAND_OK) {
        anchor_force_rediscovery_from_command();
    }
    if (pending.reboot_after_result && pending.status == COMMAND_OK) {
        anchor_schedule_reboot_after_command_result();
    }
}

static int anchor_schedule_collection_command_result(
    const struct proto_packet *command,
    const struct gateway_command_options *options,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    bool force_rediscovery_after_result,
    bool reboot_after_result)
{
    uint32_t now_ms;
    uint32_t due_ms;
    uint32_t delay_ms;

    if (command == NULL || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (anchor_collection_result_pending.active) {
        LOG_WRN("anchor collection command result already pending: cmd=0x%04x new=0x%04x",
                (unsigned int)anchor_collection_result_pending.command_id,
                (unsigned int)command_id);
        return -EBUSY;
    }

    now_ms = k_uptime_get_32();
    due_ms = gateway_command_collection_initial_due_ms(now_ms,
                                                       DEVICE_ID,
                                                       options->command_seq,
                                                       options->collection_slot_seed,
                                                       options->expected_node_count);
    delay_ms = due_ms - now_ms;
    if (delay_ms == 0u) {
        delay_ms = 1u;
    }

    anchor_collection_result_pending.command = *command;
    anchor_collection_result_pending.collection_epoch_id = options->collection_epoch_id;
    anchor_collection_result_pending.result_id.gateway_id = GATEWAY_ID;
    anchor_collection_result_pending.result_id.gateway_epoch =
        (uint16_t)mesh_runtime.upstream.current_epoch;
    anchor_collection_result_pending.result_id.command_seq = options->command_seq;
    anchor_collection_result_pending.result_id.node_id = DEVICE_ID;
    anchor_collection_result_pending.result_id.node_boot_counter =
        anchor_collection_node_boot_id();
    anchor_collection_result_pending.result_id.result_seq =
        anchor_next_collection_result_seq();
    anchor_collection_result_pending.command_id = command_id;
    anchor_collection_result_pending.status = status;
    anchor_collection_result_pending.reason = reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result = reboot_after_result;
    anchor_collection_result_pending.active = true;
    (void)k_work_reschedule(&anchor_collection_result_work, K_MSEC(delay_ms));
    anchor_collection_result_persist(delay_ms);

    LOG_INF("anchor collection command result scheduled: cmd=0x%04x due_ms=%u delay_ms=%u",
            (unsigned int)command_id,
            due_ms,
            delay_ms);
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=collection-scheduled command=0x%04x command_seq=%u result_seq=%u collection=%u due_ms=%u delay_ms=%u",
                         (unsigned int)command_id,
                         anchor_collection_result_pending.result_id.command_seq,
                         anchor_collection_result_pending.result_id.result_seq,
                         anchor_collection_result_pending.collection_epoch_id,
                         due_ms,
                         delay_ms);
    return 0;
}

static void anchor_reboot_work_handler(struct k_work *work)
{
    uint32_t now_ms;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_reboot_pending) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (mesh_relay_tx_active(&mesh_runtime) &&
        !uptime_deadline_reached(now_ms, anchor_reboot_deadline_ms)) {
        (void)k_work_reschedule(&anchor_reboot_work,
                                K_MSEC(ANCHOR_REBOOT_RESULT_POLL_MS));
        return;
    }

    LOG_INF("anchor rebooting after command-result drain");
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}

static void anchor_schedule_reboot_after_command_result(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    anchor_reboot_pending = true;
    anchor_reboot_deadline_ms = k_uptime_get_32() + ANCHOR_REBOOT_RESULT_MAX_WAIT_MS;
    (void)k_work_reschedule(&anchor_reboot_work, K_MSEC(ANCHOR_REBOOT_RESULT_DRAIN_MS));
}

static uint16_t anchor_next_heartbeat_seq(void)
{
    anchor_heartbeat_seq++;
    if (anchor_heartbeat_seq == 0u) {
        anchor_heartbeat_seq = 1u;
    }
    return anchor_heartbeat_seq;
}

static uint16_t anchor_next_survey_seq(void)
{
    anchor_survey_seq++;
    if (anchor_survey_seq == 0u) {
        anchor_survey_seq = 1u;
    }
    return anchor_survey_seq;
}

static bool anchor_survey_discovery_is_pending(void)
{
    k_spinlock_key_t key;
    bool pending;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }

    key = k_spin_lock(&anchor_survey_lock);
    pending = anchor_survey_discovery_pending;
    k_spin_unlock(&anchor_survey_lock, key);
    return pending;
}

static int anchor_send_heartbeat(void)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = report_init_anchor_heartbeat_packet(&outbound.packet,
                                              DEVICE_ID,
                                              GATEWAY_ID,
                                              nonzero_uptime_session_id(),
                                              anchor_next_heartbeat_seq(),
                                              (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.payload_len = (uint8_t)payload_len;

    return mesh_start_tracked_tx(&outbound, "anchor-heartbeat");
}

static void anchor_heartbeat_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR && anchor_heartbeat_enabled) {
        (void)k_work_reschedule(&anchor_heartbeat_work, K_MSEC(delay_ms));
    }
}

static void anchor_heartbeat_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_heartbeat_enabled) {
        return;
    }
    if (anchor_uwb_window_active() ||
        anchor_survey_discovery_is_pending() ||
        mesh_relay_tx_active(&mesh_runtime)) {
        anchor_heartbeat_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    ret = anchor_send_heartbeat();
    if (ret < 0) {
        LOG_WRN("anchor heartbeat TX failed: ret=%d interval_ms=%u", ret,
                anchor_heartbeat_interval_ms);
    } else {
        LOG_INF("anchor heartbeat queued: interval_ms=%u seq=%u",
                anchor_heartbeat_interval_ms,
                anchor_heartbeat_seq);
    }

    anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
}

static int anchor_start_heartbeat_from_command(const uint8_t *payload,
                                                size_t payload_len,
                                                uint8_t *reason)
{
    uint32_t interval_ms = 0u;
    int ret;

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS,
                                              &interval_ms);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }
    if (interval_ms < ANCHOR_HEARTBEAT_MIN_INTERVAL_MS ||
        interval_ms > ANCHOR_HEARTBEAT_MAX_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    anchor_heartbeat_interval_ms = interval_ms;
    anchor_heartbeat_enabled = true;
    anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
    return 0;
}

static void anchor_stop_heartbeat(void)
{
    anchor_heartbeat_enabled = false;
    (void)k_work_cancel_delayable(&anchor_heartbeat_work);
}

static int command_find_u8_tlv(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = tlv_value[0];
    return PROTO_OK;
}

static int command_find_u32_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int command_find_u16_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int command_find_u64_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u64_le(tlv_value);
    return PROTO_OK;
}

static int anchor_set_led_pattern_from_command(const uint8_t *payload,
                                                size_t payload_len,
                                                uint8_t *reason)
{
    uint8_t pattern_id = 0u;
    int ret;

    ret = command_find_u8_tlv(payload, payload_len, TLV_LED_PATTERN_ID, &pattern_id);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }

    switch ((enum status_pattern)pattern_id) {
    case STATUS_PATTERN_OFF:
        status_leds_set(false, false, false);
        break;
    case STATUS_PATTERN_BLUE_PULSE:
    case STATUS_PATTERN_BLUE_CHASE:
        status_leds_set(false, false, true);
        break;
    case STATUS_PATTERN_GREEN_SOLID:
    case STATUS_PATTERN_GREEN_SLOW_BLINK:
        status_leds_set(false, true, false);
        break;
    case STATUS_PATTERN_AMBER_BLINK_ONCE:
    case STATUS_PATTERN_AMBER_SLOW_BLINK:
        status_leds_set(true, true, false);
        break;
    case STATUS_PATTERN_RED_BLINK_CODE:
        status_leds_set(true, false, false);
        break;
    default:
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    LOG_INF("anchor LED pattern command applied: pattern=%u", pattern_id);
    return 0;
}

static int anchor_set_route_from_command(const uint8_t *payload,
                                          size_t payload_len,
                                          uint8_t *reason)
{
    struct route_candidate candidate = {0};
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    int ret;

    ret = command_find_u64_tlv(payload, payload_len, TLV_NEXT_HOP_ID, &candidate.next_hop_id);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u32_tlv(payload, payload_len, TLV_ROUTE_EPOCH, &candidate.route_epoch);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &hop_count);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_QUALITY, &quality);
    if (ret != PROTO_OK) {
        goto malformed;
    }

    candidate.gateway_id = GATEWAY_ID;
    candidate.hop_count = hop_count;
    candidate.link_quality = quality;
    candidate.last_seen_ms = k_uptime_get_32();
    candidate.valid = true;

    ret = route_upsert_candidate(&mesh_runtime.upstream, &candidate);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return ret == PROTO_ERR_NO_SPACE ? -ENOSPC : -EINVAL;
    }
    mesh_relay_note_route_discovery_ready(&mesh_runtime, GATEWAY_ID);

    LOG_INF("anchor route command applied: next=0x%016llx epoch=%u hop_count=%u quality=%u",
            (unsigned long long)candidate.next_hop_id,
            candidate.route_epoch,
            hop_count,
            quality);
    return 0;

malformed:
    if (reason != NULL) {
        *reason = (uint8_t)(-ret);
    }
    return -EINVAL;
}

static void anchor_clear_route_from_command(void)
{
    mesh_relay_invalidate_routes(&mesh_runtime);
    LOG_INF("anchor route command cleared upstream route state");
}

static void anchor_force_rediscovery_from_command(void)
{
    int ret;

    mesh_relay_invalidate_routes(&mesh_runtime);
    ret = mesh_request_route(GATEWAY_ID, "forced-rediscovery");
    if (ret < 0 && ret != -EAGAIN) {
        LOG_WRN("anchor forced rediscovery request failed: ret=%d", ret);
    } else {
        LOG_INF("anchor forced rediscovery requested");
    }
}

static int anchor_set_scan_duty_from_command(const uint8_t *payload,
                                              size_t payload_len,
                                              uint8_t *reason)
{
    uint32_t interval_ms = 0u;
    int ret;

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              anchor_uwb_scan_interval_ms,
                                              &interval_ms);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }
    if (interval_ms < ANCHOR_UWB_SCAN_MIN_INTERVAL_MS ||
        interval_ms > ANCHOR_UWB_SCAN_MAX_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    anchor_uwb_scan_interval_ms = interval_ms;
    if (!anchor_uwb_window_active()) {
        anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
    }
    LOG_INF("anchor UWB scan duty command applied: interval_ms=%u min_ms=%u max_ms=%u awake_us=%u",
            anchor_uwb_scan_interval_ms,
            (unsigned int)ANCHOR_UWB_SCAN_MIN_INTERVAL_MS,
            (unsigned int)ANCHOR_UWB_SCAN_MAX_INTERVAL_MS,
            ANCHOR_UWB_IDLE_SCAN_AWAKE_US);
    return 0;
}

static int8_t survey_quality_to_rssi(uint8_t quality)
{
    uint8_t bounded_quality = quality > 100u ? 100u : quality;

    return (int8_t)(-100 + (int)bounded_quality / 2);
}

static bool survey_peer_reportable(uint64_t peer_id)
{
    return mesh_id_is_unicast(peer_id) &&
           peer_id != DEVICE_ID &&
           peer_id != GATEWAY_ID;
}

static void survey_add_reach_entry(struct survey_reachability_entry *entries,
                                   size_t entry_cap,
                                   size_t *entry_count,
                                   uint64_t peer_id,
                                   uint8_t quality)
{
    if (entries == NULL || entry_count == NULL ||
        !survey_peer_reportable(peer_id) ||
        *entry_count >= entry_cap) {
        return;
    }

    if (quality > 100u) {
        quality = 100u;
    }
    for (size_t i = 0u; i < *entry_count; i++) {
        if (entries[i].peer_id == peer_id) {
            if (quality > entries[i].quality) {
                entries[i].quality = quality;
                entries[i].rssi_dbm = survey_quality_to_rssi(quality);
            }
            return;
        }
    }

    entries[*entry_count].peer_id = peer_id;
    entries[*entry_count].quality = quality;
    entries[*entry_count].rssi_dbm = survey_quality_to_rssi(quality);
    (*entry_count)++;
}

static int anchor_queue_survey_discovery_report(uint32_t survey_id,
                                                const struct survey_reachability_entry *entries,
                                                size_t entry_count,
                                                uint32_t earliest_tx_ms)
{
    struct mesh_outbound outbound = {0};
    size_t report_payload_len = 0u;
    int ret;

    if (survey_id == 0u || (entries == NULL && entry_count != 0u)) {
        return -EINVAL;
    }

    ret = survey_append_reach_report_tlvs(outbound.payload,
                                          sizeof(outbound.payload),
                                          &report_payload_len,
                                          survey_id,
                                          DEVICE_ID,
                                          entries,
                                          entry_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_init_discovery_report_packet(&outbound.packet,
                                              DEVICE_ID,
                                              GATEWAY_ID,
                                              survey_id,
                                              anchor_next_survey_seq(),
                                              (uint8_t)report_payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)report_payload_len;
    outbound.earliest_tx_ms = earliest_tx_ms;

    return queue_anchor_report(&outbound);
}

static void anchor_preempt_for_survey_discovery(uint32_t survey_id)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    (void)k_work_cancel_delayable(&anchor_uwb_scan_work);
    if (anchor_uwb_session.state != UWB_ANCHOR_IDLE) {
        uwb_anchor_abort_epoch(&anchor_uwb_session);
        LOG_INF("survey discovery preempted pending click epoch: survey=%u", survey_id);
    }
    mesh_stop_role_scan();
}

static void anchor_handle_survey_discovery_start(const struct proto_packet *packet,
                                                  const uint8_t *payload,
                                                  size_t payload_len)
{
    struct survey_discovery_config config = {0};
    struct survey_discovery_timing timing = {0};
    k_spinlock_key_t key;
    uint32_t now_ms;
    uint32_t schedule_delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_SURVEY_DISCOVERY_START ||
        packet->dst_id != MESH_BROADCAST_ID ||
        packet->src_id != GATEWAY_ID) {
        return;
    }

    ret = survey_extract_discovery_start_tlvs(payload, payload_len, &config);
    if (ret != PROTO_OK || packet->session_id != config.survey_id) {
        LOG_WRN("survey discovery start rejected: ret=%d session=%u survey=%u",
                ret,
                packet->session_id,
                config.survey_id);
        return;
    }
    ret = survey_discovery_timing_from_age(&config, packet->message_age_ms, &timing);
    if (ret != PROTO_OK || timing.expired) {
        LOG_WRN("survey discovery start stale: ret=%d survey=%u age_ms=%u duration_ms=%u",
                ret,
                config.survey_id,
                packet->message_age_ms,
                timing.duration_ms);
        return;
    }

    anchor_abort_survey_pair();
    anchor_preempt_for_survey_discovery(config.survey_id);
    now_ms = k_uptime_get_32();
    schedule_delay_ms = timing.pending ? timing.wait_ms : 0u;

    key = k_spin_lock(&anchor_survey_lock);
    anchor_survey_discovery_config = config;
    anchor_survey_discovery_start_ms = timing.pending ?
                                       now_ms + timing.wait_ms :
                                       now_ms - timing.elapsed_ms;
    anchor_survey_discovery_pending = true;
    atomic_set(&anchor_survey_abort_requested, 0);
    k_spin_unlock(&anchor_survey_lock, key);

    anchor_survey_schedule(K_MSEC(schedule_delay_ms));
    LOG_INF("survey discovery scheduled: survey=%u start_delay_ms=%u age_ms=%u wait_ms=%u slot_ms=%u slots=%u",
            config.survey_id,
            config.start_delay_ms,
            packet->message_age_ms,
            schedule_delay_ms,
            config.slot_ms,
            config.slot_count);
}

static void anchor_handle_survey_pair_prepare(const struct proto_packet *packet,
                                               const uint8_t *payload,
                                               size_t payload_len)
{
    struct survey_pair pair = {0};
    enum command_status status = COMMAND_OK;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_PREPARE ||
        packet->dst_id != DEVICE_ID ||
        packet->src_id != GATEWAY_ID) {
        return;
    }

    ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
    if (ret != PROTO_OK || packet->session_id != pair.survey_id) {
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
    } else if (pair.initiator_id != DEVICE_ID && pair.responder_id != DEVICE_ID) {
        status = COMMAND_DENIED;
        reason = 2u;
    } else if (!anchor_survey_pair_queueable(&pair)) {
        status = COMMAND_DENIED;
        reason = 4u;
    } else {
        k_spinlock_key_t key = k_spin_lock(&anchor_survey_lock);

        if (anchor_survey_start_pending || anchor_survey_running) {
            status = COMMAND_BUSY;
            reason = 3u;
        } else {
            anchor_survey_pair = pair;
            anchor_survey_pair_prepared = true;
            atomic_set(&anchor_survey_abort_requested, 0);
        }
        k_spin_unlock(&anchor_survey_lock, key);
    }

    ret = anchor_send_command_result(packet, CMD_SURVEY_PREPARE_PAIR, status, reason, NULL, 0u);
    if (ret < 0) {
        LOG_WRN("survey pair prepare result TX failed: status=%u ret=%d", status, ret);
        return;
    }

    LOG_INF("survey pair prepare handled: survey=%u responder=0x%016llx samples=%u status=%u reason=%u",
            pair.survey_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            status,
            reason);
}

static void anchor_pending_options_to_gateway(
    const struct anchor_pending_command_options *pending,
    struct gateway_command_options *options)
{
    memset(options, 0, sizeof(*options));
    options->scope = CMD_SCOPE_ALL_REGISTERED;
    options->response_mode = pending->response_mode;
    options->command_seq = pending->command_seq;
    options->collection_epoch_id = pending->collection_epoch_id;
    options->collection_slot_seed = pending->collection_slot_seed;
    options->command_expiry_s = pending->command_expiry_s;
    options->expected_node_count = pending->expected_node_count;
    options->collection_required = pending->collection_required;
    options->flood_required = true;
}

static void anchor_execute_command_side_effects(const struct proto_packet *packet,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                enum command_id command_id,
                                                enum command_status *status,
                                                uint8_t *reason,
                                                bool *force_rediscovery_after_result,
                                                bool *reboot_after_result)
{
    enum device_role requested_role = ROLE_ANCHOR;
    int ret;

    if (command_id == CMD_REBOOT) {
        *reboot_after_result = true;
    } else if (command_id == CMD_SET_ROLE) {
        ret = gateway_command_extract_role(payload, payload_len, &requested_role);
        if (ret != PROTO_OK) {
            *status = COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        } else if ((uint8_t)requested_role != (uint8_t)DEVICE_ROLE) {
            *status = COMMAND_DENIED;
            *reason = 1u;
        }
    } else if (command_id == CMD_START_HEARTBEAT) {
        ret = anchor_start_heartbeat_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_STOP_HEARTBEAT) {
        anchor_stop_heartbeat();
    } else if (command_id == CMD_SET_LED_PATTERN) {
        ret = anchor_set_led_pattern_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_SET_ROUTE) {
        ret = anchor_set_route_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = ret == -ENOSPC ? COMMAND_BUSY : COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_CLEAR_ROUTE) {
        anchor_clear_route_from_command();
    } else if (command_id == CMD_FORCE_REDISCOVERY) {
        *force_rediscovery_after_result = true;
    } else if (command_id == CMD_SET_SCAN_DUTY) {
        ret = anchor_set_scan_duty_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        ret = anchor_start_survey_pair_from_command(packet,
                                                    payload,
                                                    payload_len,
                                                    status,
                                                    reason);
        if (ret < 0 && *status == COMMAND_OK) {
            *status = COMMAND_INTERNAL_ERROR;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_ABORT) {
        anchor_abort_survey_pair();
    } else if (command_id != CMD_PING && command_id != CMD_GET_STATUS) {
        *status = COMMAND_UNSUPPORTED_COMMAND;
        *reason = 1u;
    }
}

static void anchor_finish_broadcast_command(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            enum command_id command_id,
                                            const struct gateway_command_options *command_options)
{
    enum command_status status = COMMAND_OK;
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    uint8_t reason = 0u;
    int ret;

    anchor_execute_command_side_effects(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &status,
                                        &reason,
                                        &force_rediscovery_after_result,
                                        &reboot_after_result);

    if (command_options->response_mode == CMD_RESPONSE_NONE) {
        LOG_INF("anchor broadcast command handled without result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            anchor_force_rediscovery_from_command();
        }
        if (reboot_after_result && status == COMMAND_OK) {
            anchor_schedule_reboot_after_command_result();
        }
        return;
    }

    ret = anchor_schedule_collection_command_result(packet,
                                                    command_options,
                                                    command_id,
                                                    status,
                                                    reason,
                                                    force_rediscovery_after_result,
                                                    reboot_after_result);
    if (ret < 0) {
        LOG_WRN("anchor collection command result schedule failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
    } else {
        LOG_INF("anchor broadcast command handled with collection result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
    }
}

static int anchor_schedule_broadcast_command_execution(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_command_options *options,
    uint32_t delay_ms)
{
    if (packet == NULL || payload == NULL || options == NULL ||
        payload_len > sizeof(anchor_pending_command_execution.payload)) {
        return -EINVAL;
    }
    if (anchor_pending_command_execution.active) {
        return -EBUSY;
    }

    anchor_pending_command_execution.command = *packet;
    anchor_pending_command_execution.options.response_mode = options->response_mode;
    anchor_pending_command_execution.options.command_seq = options->command_seq;
    anchor_pending_command_execution.options.collection_epoch_id = options->collection_epoch_id;
    anchor_pending_command_execution.options.collection_slot_seed = options->collection_slot_seed;
    anchor_pending_command_execution.options.command_expiry_s = options->command_expiry_s;
    anchor_pending_command_execution.options.expected_node_count = options->expected_node_count;
    anchor_pending_command_execution.options.collection_required = options->collection_required;
    memcpy(anchor_pending_command_execution.payload, payload, payload_len);
    anchor_pending_command_execution.payload_len = (uint16_t)payload_len;
    anchor_pending_command_execution.received_at_ms = k_uptime_get_32();
    anchor_pending_command_execution.active = true;
    (void)k_work_reschedule(&anchor_command_execute_work,
                            K_MSEC(delay_ms == 0u ? 1u : delay_ms));
    LOG_INF("anchor broadcast command execution scheduled: command_seq=%u delay_ms=%u",
            options->command_seq,
            delay_ms);
    return 0;
}

static void anchor_command_execute_work_handler(struct k_work *work)
{
    struct anchor_pending_command_execution pending;
    struct gateway_command_options options = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    uint32_t now_ms;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_pending_command_execution.active) {
        return;
    }

    pending = anchor_pending_command_execution;
    memset(&anchor_pending_command_execution, 0, sizeof(anchor_pending_command_execution));

    now_ms = k_uptime_get_32();
    packet_age_add_elapsed(&pending.command, now_ms - pending.received_at_ms);
    anchor_pending_options_to_gateway(&pending.options, &options);

    if (gateway_command_receive_expired(&pending.command, &options)) {
        LOG_WRN("anchor dropped expired delayed broadcast command: command_seq=%u age_ms=%u expiry_s=%u",
                options.command_seq,
                pending.command.message_age_ms,
                options.command_expiry_s);
        return;
    }

    ret = gateway_command_extract_id(pending.payload, pending.payload_len, &command_id);
    if (ret != PROTO_OK) {
        LOG_WRN("anchor dropped malformed delayed broadcast command: command_seq=%u ret=%d",
                options.command_seq,
                ret);
        return;
    }

    anchor_finish_broadcast_command(&pending.command,
                                    pending.payload,
                                    pending.payload_len,
                                    command_id,
                                    &options);
}

static void anchor_handle_local_command(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_OK;
    struct gateway_command_options command_options = {0};
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    bool broadcast_command = false;
    uint32_t delay_ms;
    uint32_t now_ms;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND) {
        return;
    }
    broadcast_command = packet->dst_id == MESH_BROADCAST_ID;
    if (packet->dst_id != DEVICE_ID && !broadcast_command) {
        return;
    }

    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK) {
        if (broadcast_command) {
            LOG_WRN("anchor ignored malformed broadcast command: ret=%d", ret);
            return;
        }
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(-ret);
    } else {
        if (broadcast_command) {
            ret = gateway_command_extract_options(payload,
                                                  payload_len,
                                                  &command_options);
            if (ret != PROTO_OK || !command_options.flood_required) {
                LOG_WRN("anchor ignored invalid broadcast command options: ret=%d flood=%u",
                        ret,
                        command_options.flood_required ? 1u : 0u);
                return;
            }
        }
    }

    if (broadcast_command) {
        now_ms = k_uptime_get_32();
        if (gateway_command_receive_expired(packet, &command_options)) {
            LOG_WRN("anchor ignored expired broadcast command: cmd=0x%04x command_seq=%u age_ms=%u expiry_s=%u delay_ms=%u",
                    (unsigned int)command_id,
                    command_options.command_seq,
                    packet->message_age_ms,
                    command_options.command_expiry_s,
                    command_options.execute_delay_ms);
            return;
        }
        if (gateway_command_rx_duplicate_seen(&anchor_command_seq_cache,
                                              command_options.command_seq,
                                              now_ms)) {
            LOG_INF("anchor ignored duplicate broadcast command: cmd=0x%04x command_seq=%u",
                    (unsigned int)command_id,
                    command_options.command_seq);
            return;
        }

        delay_ms = gateway_command_execute_delay_remaining_ms(packet, &command_options);
        if (delay_ms != 0u) {
            ret = anchor_schedule_broadcast_command_execution(packet,
                                                              payload,
                                                              payload_len,
                                                              &command_options,
                                                              delay_ms);
            if (ret < 0) {
                LOG_WRN("anchor delayed broadcast command schedule failed: cmd=0x%04x command_seq=%u ret=%d",
                        (unsigned int)command_id,
                        command_options.command_seq,
                        ret);
                return;
            }
            gateway_command_rx_duplicate_store(&anchor_command_seq_cache,
                                               packet,
                                               &command_options,
                                               now_ms);
            return;
        }

        gateway_command_rx_duplicate_store(&anchor_command_seq_cache,
                                           packet,
                                           &command_options,
                                           now_ms);
        anchor_finish_broadcast_command(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &command_options);
        return;
    }

    anchor_execute_command_side_effects(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &status,
                                        &reason,
                                        &force_rediscovery_after_result,
                                        &reboot_after_result);

    ret = anchor_send_command_result(packet, command_id, status, reason, NULL, 0u);
    if (ret < 0) {
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            anchor_force_rediscovery_from_command();
        }
        return;
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
    if (force_rediscovery_after_result && status == COMMAND_OK) {
        anchor_force_rediscovery_from_command();
    }
    if (reboot_after_result && status == COMMAND_OK) {
        anchor_schedule_reboot_after_command_result();
    }
}

static bool gateway_command_uses_survey_mesh(enum command_id command_id)
{
    return command_id == CMD_SURVEY_REACHABILITY ||
           command_id == CMD_SURVEY_PREPARE_PAIR;
}

static int gateway_extract_survey_sample_count(const uint8_t *payload,
                                                size_t payload_len,
                                                uint16_t *sample_count)
{
    uint16_t value = UWB_SAMPLES_PER_ANCHOR;
    int ret;

    if (sample_count == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = command_find_u16_tlv(payload, payload_len, TLV_SAMPLE_COUNT, &value);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (!survey_sample_count_valid(value)) {
        return PROTO_ERR_MALFORMED;
    }

    *sample_count = value;
    return PROTO_OK;
}

static int gateway_route_survey_reachability(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len)
{
    const struct survey_discovery_config discovery_config = {
        .survey_id = 0u,
        .start_delay_ms = SURVEY_DISCOVERY_START_DELAY_MS,
        .slot_ms = SURVEY_DISCOVERY_SLOT_MS,
        .slot_count = SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
    };
    struct survey_discovery_config config = discovery_config;
    struct mesh_outbound outbound = {0};
    uint32_t survey_id = 0u;
    uint32_t duration_ms = 0u;
    uint32_t discovery_duration_ms = 0u;
    uint32_t report_mesh_duration_ms = 0u;
    uint32_t collection_delay_ms = 0u;
    uint16_t sample_count = UWB_SAMPLES_PER_ANCHOR;
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (host_packet->dst_id != MESH_BROADCAST_ID && host_packet->dst_id != DEVICE_ID)) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }
    if (gateway_survey_active) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_BUSY,
                                           3u);
        LOG_WRN("gateway survey reachability rejected while survey active: current=%u requested_dst=0x%016llx",
                gateway_survey_context.survey_id,
                (unsigned long long)host_packet->dst_id);
        return -EBUSY;
    }

    ret = survey_extract_reach_request_tlvs(host_payload,
                                            host_payload_len,
                                            &survey_id,
                                            &duration_ms);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_extract_survey_sample_count(host_payload,
                                              host_payload_len,
                                              &sample_count);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    if (sample_count > SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_DENIED,
                                           4u);
        LOG_WRN("gateway survey reachability rejected: samples=%u runtime_max=%u",
                sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
        return -EINVAL;
    }
    ret = survey_extract_discovery_slot_count_tlv(host_payload,
                                                  host_payload_len,
                                                  SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
                                                  &config.slot_count);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    config.survey_id = survey_id;
    discovery_duration_ms = survey_discovery_duration_ms(&config);
    ret = survey_discovery_report_delay_ms(&config,
                                           config.slot_count - 1u,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_mesh_duration_ms);
    if (ret == PROTO_OK && UINT32_MAX - report_mesh_duration_ms >=
        SURVEY_RESULT_MESH_SLOT_MS) {
        report_mesh_duration_ms += SURVEY_RESULT_MESH_SLOT_MS;
    } else {
        ret = PROTO_ERR_NO_SPACE;
    }
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    if (discovery_duration_ms == 0u ||
        UINT32_MAX - config.start_delay_ms < report_mesh_duration_ms ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms < duration_ms) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           2u);
        return -EINVAL;
    }
    collection_delay_ms = config.start_delay_ms + report_mesh_duration_ms + duration_ms;

    ret = survey_append_discovery_start_tlvs(outbound.payload,
                                             sizeof(outbound.payload),
                                             &payload_len,
                                             &config);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    ret = survey_init_discovery_start_packet(&outbound.packet,
                                             DEVICE_ID,
                                             &config,
                                             seq,
                                             (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = MESH_BROADCAST_ID;

    ret = survey_gateway_begin(&gateway_survey_context, survey_id, sample_count);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    ret = survey_gateway_auto_begin(&gateway_survey_auto);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    gateway_survey_active = true;

    ret = mesh_send_c5_control(&outbound,
                               C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                               MESH_C5_CONTROL_WAKE_IF_NEEDED,
                               "survey-discovery-start");
    if (ret < 0) {
        gateway_survey_active = false;
        (void)survey_gateway_auto_begin(&gateway_survey_auto);
        (void)k_work_cancel_delayable(&gateway_survey_work);
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_RADIO_ERROR,
                                           (uint8_t)(-ret));
        return ret;
    }

    (void)k_work_reschedule(&gateway_survey_work, K_MSEC(collection_delay_ms));
    gateway_emit_host_command_result(host_packet, CMD_SURVEY_REACHABILITY, COMMAND_OK, 0u);
    LOG_INF("gateway survey discovery broadcast: survey=%u start_delay_ms=%u slot_ms=%u slots=%u discovery_ms=%u report_train_end_ms=%u report_grace_ms=%u samples=%u seq=%u",
            survey_id,
            config.start_delay_ms,
            config.slot_ms,
            config.slot_count,
            discovery_duration_ms,
            report_mesh_duration_ms,
            duration_ms,
            sample_count,
            seq);
    return 0;
}

static void gateway_handle_survey_discovery_report(const struct proto_packet *packet,
                                                    const uint8_t *payload,
                                                    size_t payload_len)
{
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    size_t entry_count = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY ||
        packet == NULL ||
        payload == NULL ||
        packet->msg_type != MSG_SURVEY_DISCOVERY_REPORT ||
        packet->dst_id != DEVICE_ID) {
        return;
    }

    ret = survey_extract_reach_report_tlvs(payload,
                                           payload_len,
                                           &survey_id,
                                           &anchor_id,
                                           entries,
                                           ARRAY_SIZE(entries),
                                           &entry_count);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report rejected: ret=%d src=0x%016llx session=%u",
                ret,
                (unsigned long long)packet->src_id,
                packet->session_id);
        return;
    }
    if (packet->session_id != survey_id || packet->src_id != anchor_id) {
        LOG_WRN("gateway survey discovery report identity mismatch: pkt_session=%u survey=%u src=0x%016llx anchor=0x%016llx",
                packet->session_id,
                survey_id,
                (unsigned long long)packet->src_id,
                (unsigned long long)anchor_id);
        return;
    }
    if (!gateway_survey_active ||
        gateway_survey_context.survey_id != survey_id) {
        LOG_WRN("gateway survey discovery report stale: survey=%u active=%u current=%u anchor=0x%016llx",
                survey_id,
                gateway_survey_active ? 1u : 0u,
                gateway_survey_context.survey_id,
                (unsigned long long)anchor_id);
        return;
    }
    if (gateway_survey_auto.running) {
        LOG_WRN("gateway survey discovery report ignored after orchestration start: survey=%u anchor=0x%016llx entries=%u",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count);
        return;
    }

    ret = survey_gateway_note_reach_report(&gateway_survey_context,
                                           survey_id,
                                           anchor_id,
                                           entries,
                                           entry_count);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report not recorded: survey=%u anchor=0x%016llx entries=%u ret=%d",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count,
                ret);
        return;
    }

    ret = survey_gateway_plan_pairs(&gateway_survey_context);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey pair planning failed: survey=%u reports=%u ret=%d",
                survey_id,
                (unsigned int)gateway_survey_context.report_count,
                ret);
        return;
    }

    if (gateway_survey_context.pair_count > 0u) {
        const struct survey_pair *pair = &gateway_survey_context.pairs[0];

        LOG_INF("gateway survey report recorded: survey=%u reports=%u pairs=%u first=0x%016llx->0x%016llx samples=%u",
                survey_id,
                (unsigned int)gateway_survey_context.report_count,
                (unsigned int)gateway_survey_context.pair_count,
                (unsigned long long)pair->initiator_id,
                (unsigned long long)pair->responder_id,
                pair->sample_count);
    } else {
        LOG_INF("gateway survey report recorded: survey=%u reports=%u pairs=0",
                survey_id,
                (unsigned int)gateway_survey_context.report_count);
    }
}

static uint32_t gateway_survey_pair_run_delay_ms(const struct survey_pair *pair)
{
    uint32_t per_sample_ms = SURVEY_PAIR_RESPONDER_WINDOW_MS + SURVEY_PAIR_SAMPLE_GAP_MS;

    if (pair == NULL || pair->sample_count == 0u) {
        return GATEWAY_SURVEY_AUTO_RETRY_MS;
    }
    if (pair->sample_count > (UINT32_MAX - GATEWAY_SURVEY_PAIR_SETTLE_MS) / per_sample_ms) {
        return UINT32_MAX;
    }
    return ((uint32_t)pair->sample_count * per_sample_ms) + GATEWAY_SURVEY_PAIR_SETTLE_MS;
}

static void gateway_survey_auto_finish(void)
{
    LOG_INF("gateway survey orchestration complete: survey=%u planned_pairs=%u",
            gateway_survey_context.survey_id,
            (unsigned int)gateway_survey_context.pair_count);
    gateway_survey_active = false;
    (void)survey_gateway_auto_begin(&gateway_survey_auto);
}

static int gateway_survey_auto_send_outbound(struct mesh_outbound *outbound,
                                             enum command_id command_id,
                                             const char *reason)
{
    int ret;

    ret = gateway_begin_command_result_wait(&outbound->packet, command_id);
    if (ret < 0) {
        return ret;
    }

    ret = mesh_start_tracked_tx(outbound, reason);
    if (ret < 0) {
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            LOG_INF("gateway survey auto command waiting for route: cmd=0x%04x dst=0x%016llx",
                    (unsigned int)command_id,
                    (unsigned long long)outbound->packet.dst_id);
            ret = 0;
        } else {
            gateway_clear_pending_command_result(&outbound->packet);
            return ret;
        }
    }

    ret = survey_gateway_auto_mark_waiting(&gateway_survey_auto);
    if (ret != PROTO_OK) {
        gateway_clear_pending_command_result(&outbound->packet);
        return mesh_errno_from_proto(ret);
    }
    return 0;
}

static int gateway_survey_auto_send_prepare(const struct survey_gateway_auto_action *action)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &action->pair);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    ret = survey_init_pair_prepare_packet(&outbound.packet,
                                          &action->pair,
                                          DEVICE_ID,
                                          seq,
                                          (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.dst_id = action->target_id;
    outbound.payload_len = (uint8_t)payload_len;

    LOG_INF("gateway survey auto prepare: survey=%u dst=0x%016llx seq=%u",
            action->pair.survey_id,
            (unsigned long long)action->target_id,
            seq);
    return gateway_survey_auto_send_outbound(&outbound,
                                             CMD_SURVEY_PREPARE_PAIR,
                                             "survey-auto-prepare");
}

static int gateway_survey_auto_send_start(const struct survey_gateway_auto_action *action)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    ret = mesh_append_command_id(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 CMD_SURVEY_START_PAIR);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &action->pair);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    outbound.packet.msg_type = MSG_COMMAND;
    outbound.packet.src_id = DEVICE_ID;
    outbound.packet.dst_id = action->target_id;
    outbound.packet.session_id = action->pair.survey_id;
    outbound.packet.seq = seq;
    outbound.packet.ttl = MESH_DEFAULT_TTL;
    outbound.packet.payload_len = (uint8_t)payload_len;
    outbound.payload_len = (uint8_t)payload_len;

    LOG_INF("gateway survey auto start: survey=%u dst=0x%016llx seq=%u",
            action->pair.survey_id,
            (unsigned long long)action->target_id,
            seq);
    return gateway_survey_auto_send_outbound(&outbound,
                                             CMD_SURVEY_START_PAIR,
                                             "survey-auto-start");
}

static int gateway_survey_auto_send_action(const struct survey_gateway_auto_action *action)
{
    switch (action->command_id) {
    case CMD_SURVEY_PREPARE_PAIR:
        return gateway_survey_auto_send_prepare(action);
    case CMD_SURVEY_START_PAIR:
        return gateway_survey_auto_send_start(action);
    default:
        return -EINVAL;
    }
}

static void gateway_survey_auto_log_skipped_pair(const char *reason,
                                                 enum command_status status,
                                                 uint8_t detail)
{
    LOG_WRN("gateway survey auto pair skipped: survey=%u initiator=0x%016llx responder=0x%016llx reason=%s status=%u detail=%u",
            gateway_survey_auto.pair.survey_id,
            (unsigned long long)gateway_survey_auto.pair.initiator_id,
            (unsigned long long)gateway_survey_auto.pair.responder_id,
            reason,
            status,
            detail);
    (void)k_work_reschedule(&gateway_survey_work, K_MSEC(GATEWAY_SURVEY_AUTO_RETRY_MS));
}

static void gateway_survey_auto_note_command_result(const struct proto_packet *command,
                                                    enum command_id command_id,
                                                    enum command_status status,
                                                    uint8_t reason)
{
    bool pair_launched = false;
    bool pair_skipped = false;
    int ret;

    if (command == NULL) {
        return;
    }

    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          command->dst_id,
                                          command->session_id,
                                          status,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey auto result transition failed: cmd=0x%04x dst=0x%016llx status=%u ret=%d",
                (unsigned int)command_id,
                (unsigned long long)command->dst_id,
                status,
                ret);
        gateway_survey_auto_finish();
        return;
    }

    if (pair_skipped) {
        gateway_survey_auto_log_skipped_pair("command-result", status, reason);
    } else if (pair_launched) {
        LOG_INF("gateway survey pair launched: survey=%u initiator=0x%016llx responder=0x%016llx samples=%u",
                gateway_survey_auto.pair.survey_id,
                (unsigned long long)gateway_survey_auto.pair.initiator_id,
                (unsigned long long)gateway_survey_auto.pair.responder_id,
                gateway_survey_auto.pair.sample_count);
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(gateway_survey_pair_run_delay_ms(&gateway_survey_auto.pair)));
    } else {
        (void)k_work_reschedule(&gateway_survey_work, K_NO_WAIT);
    }
}

static void gateway_survey_auto_note_command_timeout(const struct proto_packet *command,
                                                     enum command_id command_id)
{
    bool pair_launched = false;
    bool pair_skipped = false;
    int ret;

    if (command == NULL) {
        return;
    }

    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          command->dst_id,
                                          command->session_id,
                                          COMMAND_TIMEOUT,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey auto timeout transition failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)command->dst_id,
                ret);
        gateway_survey_auto_finish();
        return;
    }
    if (pair_skipped) {
        gateway_survey_auto_log_skipped_pair("command-timeout", COMMAND_TIMEOUT, 0u);
    }
}

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id)
{
    if (command == NULL) {
        return;
    }

    mesh_clear_route_waiting_tx(command);
    gateway_survey_auto_note_command_timeout(command, command_id);
    if (command_id == CMD_FORCE_REDISCOVERY) {
        mesh_gateway_route_adv_request(0u, "force-rediscovery-timeout");
    }
}

void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason)
{
    gateway_survey_auto_note_command_result(command, command_id, status, reason);
    if (command_id == CMD_FORCE_REDISCOVERY && status == COMMAND_OK) {
        mesh_gateway_route_adv_request(0u, "force-rediscovery-result");
    }
}

static void gateway_survey_work_handler(struct k_work *work)
{
    struct survey_gateway_auto_action action = {0};
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY || !gateway_survey_active) {
        return;
    }
    if (gateway_survey_auto.waiting) {
        return;
    }
    if (!gateway_survey_auto.running) {
        if (!gateway_survey_context.pairs_planned) {
            ret = survey_gateway_plan_pairs(&gateway_survey_context);
            if (ret != PROTO_OK) {
                LOG_WRN("gateway survey final pair planning failed: survey=%u ret=%d",
                        gateway_survey_context.survey_id,
                        ret);
                gateway_survey_auto_finish();
                return;
            }
        }
        LOG_INF("gateway survey orchestration starting: survey=%u reports=%u pairs=%u",
                gateway_survey_context.survey_id,
                (unsigned int)gateway_survey_context.report_count,
                (unsigned int)gateway_survey_context.pair_count);
    }

    ret = survey_gateway_auto_next_action(&gateway_survey_auto,
                                          &gateway_survey_context,
                                          &action);
    if (ret == PROTO_OK && action.complete) {
        gateway_survey_auto_finish();
        return;
    }
    if (ret == PROTO_ERR_BUSY) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey next action failed: survey=%u ret=%d",
                gateway_survey_context.survey_id,
                ret);
        gateway_survey_auto_finish();
        return;
    }

    ret = gateway_survey_auto_send_action(&action);
    if (ret == -EBUSY) {
        (void)k_work_reschedule(&gateway_survey_work, K_MSEC(GATEWAY_SURVEY_AUTO_RETRY_MS));
        return;
    }
    if (ret < 0) {
        LOG_WRN("gateway survey auto send failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)action.command_id,
                (unsigned long long)action.target_id,
                ret);
        gateway_survey_auto.waiting = false;
        gateway_survey_auto.stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
        gateway_survey_auto_log_skipped_pair("send-failed",
                                             ret == -EHOSTUNREACH ? COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
                                             (uint8_t)(-ret));
    }
}

static int gateway_route_survey_pair_prepare(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len)
{
    struct mesh_outbound outbound = {0};
    struct survey_pair pair = {0};
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }

    ret = survey_extract_pair_tlvs(host_payload, host_payload_len, &pair);
    if (ret != PROTO_OK ||
        (host_packet->dst_id != pair.initiator_id &&
         host_packet->dst_id != pair.responder_id)) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           ret == PROTO_OK ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(ret == PROTO_OK ? 2u : -ret));
        return ret == PROTO_OK ? -EINVAL : mesh_errno_from_proto(ret);
    }
    if (pair.sample_count > SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           COMMAND_DENIED,
                                           4u);
        LOG_WRN("gateway survey pair prepare rejected: survey=%u samples=%u runtime_max=%u",
                pair.survey_id,
                pair.sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
        return -EINVAL;
    }

    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &pair);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    ret = survey_init_pair_prepare_packet(&outbound.packet,
                                          &pair,
                                          DEVICE_ID,
                                          seq,
                                          (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.dst_id = host_packet->dst_id;
    outbound.payload_len = (uint8_t)payload_len;

    ret = gateway_begin_command_result_wait(&outbound.packet, CMD_SURVEY_PREPARE_PAIR);
    if (ret < 0) {
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    ret = mesh_start_tracked_tx(&outbound, "survey-pair-prepare");
    if (ret < 0) {
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_host_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           ret == -EHOSTUNREACH ? COMMAND_TIMEOUT :
                                           ret == -EBUSY ? COMMAND_BUSY :
                                           COMMAND_RADIO_ERROR,
                                           (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway survey pair prepare routed: survey=%u initiator=0x%016llx responder=0x%016llx samples=%u seq=%u",
            pair.survey_id,
            (unsigned long long)pair.initiator_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            seq);
    return 0;
}

static int gateway_route_survey_command(const struct proto_packet *host_packet,
                                        const uint8_t *host_payload,
                                        size_t host_payload_len,
                                        enum command_id command_id)
{
    switch (command_id) {
    case CMD_SURVEY_REACHABILITY:
        return gateway_route_survey_reachability(host_packet, host_payload, host_payload_len);
    case CMD_SURVEY_PREPARE_PAIR:
        return gateway_route_survey_pair_prepare(host_packet, host_payload, host_payload_len);
    default:
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_UNSUPPORTED_COMMAND,
                                         1u);
        return -ENOTSUP;
    }
}

static int GATEWAY_BLE_HOST_COMMAND_UNUSED gateway_route_host_packet(struct proto_packet *packet,
                                                                    uint8_t *payload,
                                                                    size_t payload_len)
{
    struct mesh_outbound outbound = {0};
    struct gateway_command_options command_options = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    enum gateway_command_tracking_mode tracking_mode = GATEWAY_COMMAND_TRACK_NONE;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    if (packet != NULL && packet->msg_type == MSG_COMMAND) {
        ret = gateway_command_extract_id(payload, payload_len, &command_id);
        if (ret == PROTO_OK && gateway_command_uses_survey_mesh(command_id)) {
            return gateway_route_survey_command(packet, payload, payload_len, command_id);
        }
    }

    ret = gateway_command_prepare_outbound(packet,
                                           payload,
                                           payload_len,
                                           DEVICE_ID,
                                           k_uptime_get_32(),
                                           packet != NULL && packet->seq == 0u ?
                                           gateway_next_command_seq() : 0u,
                                           &outbound,
                                           &command_id);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway rejected BLE host command: %d", ret);
        gateway_emit_host_command_result(packet,
                                         CMD_VENDOR_BASE,
                                         ret == PROTO_ERR_ARG ? COMMAND_DENIED :
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_command_extract_options(payload, payload_len, &command_options);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway rejected BLE host command options after prepare: %d", ret);
        gateway_emit_host_command_result(&outbound.packet,
                                         command_id,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    tracking_mode = gateway_command_tracking_mode_from_options(&command_options);
    if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
        ret = gateway_begin_command_result_wait(&outbound.packet, command_id);
        if (ret < 0) {
            LOG_WRN("gateway command result tracker busy: cmd=0x%04x dst=0x%016llx ret=%d",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id,
                    ret);
            gateway_emit_host_command_result(&outbound.packet,
                                             command_id,
                                             ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                             (uint8_t)(-ret));
            return ret;
        }
    } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
        ret = gateway_begin_command_collection(&command_options);
        if (ret < 0) {
            LOG_WRN("gateway command collection tracker busy: cmd=0x%04x command_seq=%u ret=%d",
                    (unsigned int)command_id,
                    command_options.command_seq,
                    ret);
            gateway_emit_host_command_result(&outbound.packet,
                                             command_id,
                                             ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                             (uint8_t)(-ret));
            return ret;
        }
    }

    if (gateway_command_transport_mode_from_outbound(&outbound) ==
        GATEWAY_COMMAND_TRANSPORT_C5_BROADCAST) {
        ret = mesh_send_c5_flood(&outbound,
                                 C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                                 "ble-command-broadcast");
        if (ret == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &outbound, k_uptime_get_32());
        }
    } else {
        ret = mesh_start_tracked_tx(&outbound, "ble-command");
    }
    if (ret < 0) {
        LOG_WRN("gateway BLE command route failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound.packet.dst_id,
                ret);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            LOG_INF("gateway command waiting for reactive route discovery: cmd=0x%04x dst=0x%016llx",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id);
            return 0;
        }
        if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
            gateway_clear_pending_command_result(&outbound.packet);
        } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
            gateway_clear_command_collection(&command_options);
        }
        gateway_emit_host_command_result(&outbound.packet,
                                         command_id,
                                         ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                         (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway BLE command routed: cmd=0x%04x dst=0x%016llx session=%u seq=%u ttl=%u",
            (unsigned int)command_id,
            (unsigned long long)outbound.packet.dst_id,
            outbound.packet.session_id,
            outbound.packet.seq,
            outbound.packet.ttl);
    return 0;
}

#if defined(CONFIG_IMEC_GATEWAY_BLE)
void gateway_handle_ble_frame(const uint8_t *frame, size_t frame_len)
{
#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    int ret;

    ret = gateway_ble_send_packet_frame(frame, frame_len);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(gateway_ble_notify_failures);
        LOG_WRN("gateway BLE connectivity test echo failed: frame_len=%u ret=%d",
                (unsigned int)frame_len,
                ret);
        return;
    }
    LOG_INF("gateway BLE connectivity test echoed packet frame: frame_len=%u",
            (unsigned int)frame_len);
#elif defined(CONFIG_IMEC_ML_CLICKER)
    ml_clicker_handle_ble_frame(frame, frame_len);
#elif defined(CONFIG_IMEC_ML_ANCHOR)
    LOG_WRN("ML anchor BLE packet RX ignored: frame_len=%u",
            (unsigned int)frame_len);
#else
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        return;
    }
    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX",
                         "transport=gateway_ble msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u",
                         packet.msg_type,
                         (unsigned long long)packet.src_id,
                         (unsigned long long)packet.dst_id,
                         packet.seq,
                         (unsigned int)payload_len);

    ret = gateway_route_host_packet(&packet, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                packet.msg_type,
                (unsigned long long)packet.dst_id,
                ret);
    }
#endif
}
#endif

static bool survey_pair_matches_prepared(const struct survey_pair *pair)
{
    if (pair == NULL || !anchor_survey_pair_prepared) {
        return false;
    }

    return anchor_survey_pair.survey_id == pair->survey_id &&
           anchor_survey_pair.initiator_id == pair->initiator_id &&
           anchor_survey_pair.responder_id == pair->responder_id &&
           anchor_survey_pair.sample_count == pair->sample_count;
}

static bool anchor_survey_abort_is_requested(void)
{
    return atomic_get(&anchor_survey_abort_requested) != 0;
}

static bool anchor_survey_pair_queueable(const struct survey_pair *pair)
{
    return pair != NULL && pair->sample_count <= REPORT_TX_QUEUE_DEPTH;
}

static void anchor_survey_schedule(k_timeout_t delay)
{
    (void)k_work_reschedule_for_queue(&anchor_survey_work_q,
                                      &anchor_survey_work,
                                      delay);
}

static void anchor_uwb_scan_schedule_ms(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    int ret = k_work_reschedule_for_queue(&anchor_uwb_scan_work_q,
                                          &anchor_uwb_scan_work,
                                          K_MSEC(delay_ms));

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_RESCHED ret=%d delay=%u\n",
                            ret,
                            delay_ms);
    }
#else
    ARG_UNUSED(delay_ms);
#endif
}

static uint32_t anchor_uwb_scan_blocked_retry_ms(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return ANCHOR_UWB_SCAN_BUSY_RETRY_MS;
    }
    return anchor_uwb_scan_interval_ms;
}

static int anchor_queue_survey_sample_result(const struct survey_pair *pair,
                                             uint16_t sample_index,
                                             uint64_t reporter_id,
                                             const struct dwm3000_range_result *range_result)
{
    struct mesh_outbound outbound = {0};
    struct survey_sample sample = {0};
    size_t payload_len = 0u;
    enum range_status status;
    int64_t sample_local_ms;
    int ret;

    if (pair == NULL || reporter_id == 0u || range_result == NULL) {
        return -EINVAL;
    }

    status = range_status_valid(range_result->status) ?
             range_result->status : RANGE_INTERNAL_ERROR;

    sample.pair = *pair;
    sample.sample_index = sample_index;
    sample.distance_mm = range_result->distance_mm;
    sample.quality = range_result->quality;
    sample.range_status = status;

    ret = survey_append_sample_tlvs(outbound.payload,
                                    sizeof(outbound.payload),
                                    &payload_len,
                                    &sample);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    sample_local_ms = range_result->exchange_started ?
                      range_result->exchange_start_ms :
                      k_uptime_get();
    ret = anchor_append_sequence_time_tlvs(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           sample_local_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = append_range_result_timing_tlvs(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          range_result);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_init_result_packet_from_reporter(&outbound.packet,
                                                  &sample,
                                                  reporter_id,
                                                  GATEWAY_ID,
                                                  anchor_next_survey_seq(),
                                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    return queue_anchor_report(&outbound);
}

static int anchor_run_survey_pair_initiator(const struct survey_pair *pair)
{
    int last_ret = 0;

    for (uint16_t sample_index = 0u;
         sample_index < pair->sample_count && !anchor_survey_abort_is_requested();
         sample_index++) {
        struct dwm3000_range_request request = {0};
        struct dwm3000_range_result result = {0};
        int ret = -ETIMEDOUT;

        request.initiator_id = pair->initiator_id;
        request.responder_id = pair->responder_id;
        request.network_id = NETWORK_ID;
        request.session_nonce = survey_sample_nonce(pair, sample_index);
        request.responder_short_addr = uwb_session_short_addr_from_id(pair->responder_id);
        request.session_id = pair->survey_id;
        request.seq = survey_sample_seq(sample_index);
        request.flags = FLAG_DIAGNOSTIC;
        request.timeout_ms = SURVEY_PAIR_INITIATOR_TIMEOUT_MS;
        request.capture_rsl = sample_index == 0u;
        result.status = RANGE_RX_TIMEOUT;

        LOG_INF("survey DS-TWR initiator sample start: survey=%u responder=0x%016llx sample=%u/%u seq=%u",
                pair->survey_id,
                (unsigned long long)pair->responder_id,
                (unsigned int)(sample_index + 1u),
                pair->sample_count,
                request.seq);
        ret = dwm3000_driver_range_initiator(&request, &result);
        if (result.initiator_id == 0u) {
            result.initiator_id = pair->initiator_id;
        }
        if (result.responder_id == 0u) {
            result.responder_id = pair->responder_id;
        }
        result.session_id = pair->survey_id;
        result.seq = request.seq;
        result.flags = FLAG_DIAGNOSTIC;
        if (result.status == RANGE_OK && ret < 0) {
            result.status = RANGE_INTERNAL_ERROR;
        }

        if (ret == 0 && result.status == RANGE_OK) {
            LOG_INF("survey DS-TWR initiator sample complete: survey=%u responder=0x%016llx sample=%u/%u distance_mm=%d quality=%u rsl=%d rsl_present=%u clock=%d clock_present=%u carrier=%d carrier_present=%u",
                    pair->survey_id,
                    (unsigned long long)result.responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality,
                    result.rsl_dbm,
                    result.rsl_sampled ? 1u : 0u,
                    result.clock_offset_raw,
                    result.clock_offset_sampled ? 1u : 0u,
                    result.carrier_integrator,
                    result.carrier_integrator_sampled ? 1u : 0u);
        } else {
            last_ret = ret < 0 ? ret : -EIO;
            LOG_WRN("survey DS-TWR initiator sample failed: survey=%u responder=0x%016llx sample=%u/%u ret=%d status=%s(%u)",
                    pair->survey_id,
                    (unsigned long long)pair->responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    ret,
                    range_status_name(result.status),
                    result.status);
        }

        ret = anchor_queue_survey_sample_result(pair, sample_index, DEVICE_ID, &result);
        if (ret < 0) {
            LOG_WRN("survey sample result queue failed: survey=%u sample=%u ret=%d",
                    pair->survey_id,
                    sample_index,
                    ret);
            last_ret = ret;
        }
        if (sample_index + 1u < pair->sample_count) {
            k_msleep(SURVEY_PAIR_SAMPLE_GAP_MS);
        }
    }

    return last_ret;
}

static int anchor_run_survey_pair_responder(const struct survey_pair *pair)
{
    int last_ret = 0;

    for (uint16_t sample_index = 0u;
         sample_index < pair->sample_count && !anchor_survey_abort_is_requested();
         sample_index++) {
        struct dwm3000_range_request expected = {0};
        struct dwm3000_range_result result = {0};
        int64_t deadline_ms;
        int ret = -ETIMEDOUT;

        expected.initiator_id = pair->initiator_id;
        expected.responder_id = pair->responder_id;
        expected.network_id = NETWORK_ID;
        expected.session_nonce = survey_sample_nonce(pair, sample_index);
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = pair->survey_id;
        expected.seq = survey_sample_seq(sample_index);
        expected.flags = FLAG_DIAGNOSTIC;
        expected.capture_rsl = sample_index == 0u;
        result.status = RANGE_RX_TIMEOUT;

        deadline_ms = k_uptime_get() + SURVEY_PAIR_RESPONDER_WINDOW_MS;
        LOG_INF("survey DS-TWR responder listen: survey=%u initiator=0x%016llx sample=%u/%u seq=%u",
                pair->survey_id,
                (unsigned long long)pair->initiator_id,
                (unsigned int)(sample_index + 1u),
                pair->sample_count,
                expected.seq);
        while (k_uptime_get() < deadline_ms && !anchor_survey_abort_is_requested()) {
            uint32_t remaining_ms = (uint32_t)MAX(1, deadline_ms - k_uptime_get());

            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         remaining_ms,
                                                         &result);
            if (ret == -EAGAIN) {
                continue;
            }
            break;
        }

        if (ret == 0 && result.status == RANGE_OK) {
            LOG_INF("survey DS-TWR responder sample complete: survey=%u initiator=0x%016llx sample=%u/%u distance_mm=%d quality=%u rsl=%d rsl_present=%u clock=%d clock_present=%u carrier=%d carrier_present=%u",
                    pair->survey_id,
                    (unsigned long long)result.initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality,
                    result.rsl_dbm,
                    result.rsl_sampled ? 1u : 0u,
                    result.clock_offset_raw,
                    result.clock_offset_sampled ? 1u : 0u,
                    result.carrier_integrator,
                    result.carrier_integrator_sampled ? 1u : 0u);
        } else {
            last_ret = ret < 0 ? ret : -EIO;
            if (result.status == RANGE_OK || !range_status_valid(result.status)) {
                result.status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : RANGE_INTERNAL_ERROR;
            }
            LOG_WRN("survey DS-TWR responder sample failed: survey=%u initiator=0x%016llx sample=%u/%u ret=%d status=%s(%u)",
                    pair->survey_id,
                    (unsigned long long)pair->initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    ret,
                    range_status_name(result.status),
                    result.status);
        }

        ret = anchor_queue_survey_sample_result(pair, sample_index, DEVICE_ID, &result);
        if (ret < 0) {
            LOG_WRN("survey responder sample result queue failed: survey=%u sample=%u ret=%d",
                    pair->survey_id,
                    sample_index,
                    ret);
            last_ret = ret;
        }
    }

    return last_ret;
}

static void anchor_survey_work_handler(struct k_work *work)
{
    struct survey_pair pair;
    struct survey_discovery_config discovery_config = {0};
    uint32_t discovery_start_ms = 0u;
    bool as_responder;
    bool run_discovery = false;
    int64_t uwb_window_start_ms;
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    key = k_spin_lock(&anchor_survey_lock);
    if (anchor_survey_discovery_pending) {
        discovery_config = anchor_survey_discovery_config;
        discovery_start_ms = anchor_survey_discovery_start_ms;
        anchor_survey_discovery_pending = false;
        anchor_survey_running = true;
        run_discovery = true;
    }
    k_spin_unlock(&anchor_survey_lock, key);

    if (run_discovery) {
        mesh_stop_role_scan();
        ret = radio_guard_uwb_start("survey discovery");
        if (ret < 0) {
            key = k_spin_lock(&anchor_survey_lock);
            anchor_survey_discovery_pending = true;
            anchor_survey_running = false;
            k_spin_unlock(&anchor_survey_lock, key);
            anchor_survey_schedule(K_MSEC(REPORT_TX_RETRY_DELAY_MS));
            return;
        }

        anchor_set_uwb_busy(true);
        uwb_window_start_ms = k_uptime_get();
        ret = anchor_run_survey_discovery(&discovery_config, discovery_start_ms);
        (void)dwm3000_driver_standby();
        anchor_note_uwb_awake_since(uwb_window_start_ms, 0u);
        anchor_set_uwb_busy(false);
        radio_guard_uwb_stop();
        mesh_restart_role_scan();
        (void)anchor_start_uwb_scan();
        report_tx_schedule(0u);
        key = k_spin_lock(&anchor_survey_lock);
        anchor_survey_running = false;
        k_spin_unlock(&anchor_survey_lock, key);
        LOG_INF("survey discovery run finished: survey=%u ret=%d",
                discovery_config.survey_id,
                ret);
        return;
    }

    key = k_spin_lock(&anchor_survey_lock);
    if (!anchor_survey_start_pending) {
        k_spin_unlock(&anchor_survey_lock, key);
        return;
    }
    k_spin_unlock(&anchor_survey_lock, key);

    if (anchor_uwb_window_active() ||
        anchor_survey_discovery_is_pending() ||
        mesh_relay_tx_active(&mesh_runtime)) {
        anchor_survey_schedule(K_MSEC(REPORT_TX_RETRY_DELAY_MS));
        return;
    }

    key = k_spin_lock(&anchor_survey_lock);
    if (!anchor_survey_start_pending) {
        k_spin_unlock(&anchor_survey_lock, key);
        return;
    }
    pair = anchor_survey_pair;
    as_responder = anchor_survey_start_as_responder;
    anchor_survey_start_pending = false;
    anchor_survey_running = true;
    k_spin_unlock(&anchor_survey_lock, key);

    if (report_tx_queue_used() + pair.sample_count > REPORT_TX_QUEUE_DEPTH) {
        key = k_spin_lock(&anchor_survey_lock);
        anchor_survey_start_pending = true;
        anchor_survey_running = false;
        k_spin_unlock(&anchor_survey_lock, key);
        report_tx_schedule(0u);
        anchor_survey_schedule(K_MSEC(REPORT_TX_RETRY_DELAY_MS));
        return;
    }

    mesh_stop_role_scan();
    ret = radio_guard_uwb_start(as_responder ? "survey responder DS-TWR" :
                                               "survey initiator DS-TWR");
    if (ret < 0) {
        bool reschedule;

        mesh_restart_role_scan();
        key = k_spin_lock(&anchor_survey_lock);
        anchor_survey_running = false;
        if (!anchor_survey_abort_is_requested()) {
            anchor_survey_start_pending = true;
        }
        reschedule = anchor_survey_start_pending;
        k_spin_unlock(&anchor_survey_lock, key);
        if (reschedule) {
            anchor_survey_schedule(K_MSEC(REPORT_TX_RETRY_DELAY_MS));
        }
        return;
    }

    anchor_set_uwb_busy(true);
    uwb_window_start_ms = k_uptime_get();
    if (as_responder) {
        ret = anchor_run_survey_pair_responder(&pair);
    } else {
        ret = anchor_run_survey_pair_initiator(&pair);
    }
    (void)dwm3000_driver_standby();
    anchor_note_uwb_awake_since(uwb_window_start_ms, 0u);
    anchor_set_uwb_busy(false);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    report_tx_schedule(0u);
    key = k_spin_lock(&anchor_survey_lock);
    anchor_survey_running = false;
    k_spin_unlock(&anchor_survey_lock, key);

    LOG_INF("survey pair run finished: survey=%u role=%s ret=%d aborted=%u",
            pair.survey_id,
            as_responder ? "responder" : "initiator",
            ret,
            anchor_survey_abort_is_requested() ? 1u : 0u);
}

static int anchor_start_survey_pair_from_command(const struct proto_packet *packet,
                                                  const uint8_t *payload,
                                                  size_t payload_len,
                                                  enum command_status *status,
                                                  uint8_t *reason)
{
    struct survey_pair pair = {0};
    bool as_responder;
    k_spinlock_key_t key;
    int ret;

    if (status == NULL || reason == NULL) {
        return -EINVAL;
    }

    ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
    if (ret != PROTO_OK || packet == NULL) {
        *status = COMMAND_MALFORMED_PAYLOAD;
        *reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
        return -EINVAL;
    }
    if (pair.initiator_id != DEVICE_ID && pair.responder_id != DEVICE_ID) {
        *status = COMMAND_DENIED;
        *reason = 2u;
        return -EINVAL;
    }
    if (!anchor_survey_pair_queueable(&pair)) {
        *status = COMMAND_DENIED;
        *reason = 4u;
        return -EINVAL;
    }
    if (anchor_uwb_window_active()) {
        *status = COMMAND_BUSY;
        *reason = 3u;
        return -EBUSY;
    }
    key = k_spin_lock(&anchor_survey_lock);

    if (anchor_survey_start_pending || anchor_survey_running) {
        k_spin_unlock(&anchor_survey_lock, key);
        *status = COMMAND_BUSY;
        *reason = 3u;
        return -EBUSY;
    }
    if (anchor_survey_pair_prepared && !survey_pair_matches_prepared(&pair)) {
        k_spin_unlock(&anchor_survey_lock, key);
        *status = COMMAND_INVALID_STATE;
        *reason = 4u;
        return -EINVAL;
    }

    as_responder = pair.responder_id == DEVICE_ID;
    anchor_survey_pair = pair;
    anchor_survey_pair_prepared = true;
    anchor_survey_start_as_responder = as_responder;
    anchor_survey_start_pending = true;
    atomic_set(&anchor_survey_abort_requested, 0);
    k_spin_unlock(&anchor_survey_lock, key);
    anchor_survey_schedule(K_NO_WAIT);
    *status = COMMAND_OK;
    *reason = 0u;
    LOG_INF("survey pair start accepted: survey=%u initiator=0x%016llx responder=0x%016llx samples=%u local_role=%s",
            pair.survey_id,
            (unsigned long long)pair.initiator_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            as_responder ? "responder" : "initiator");
    return 0;
}

static void anchor_abort_survey_pair(void)
{
    k_spinlock_key_t key;

    atomic_set(&anchor_survey_abort_requested, 1);
    key = k_spin_lock(&anchor_survey_lock);
    anchor_survey_start_pending = false;
    anchor_survey_pair_prepared = false;
    anchor_survey_discovery_pending = false;
    k_spin_unlock(&anchor_survey_lock, key);
    (void)k_work_cancel_delayable(&anchor_survey_work);
    LOG_INF("survey pair state aborted locally");
}

static int anchor_run_survey_discovery(const struct survey_discovery_config *config,
                                       uint32_t start_ms)
{
    struct survey_reachability_entry entries[SURVEY_REACH_MAX_ENTRIES] = {0};
    size_t entry_count = 0u;
    uint8_t local_slot;
    uint32_t report_delay_ms = 0u;
    uint32_t now_ms;
    uint8_t first_slot;
    int ret;

    if (survey_discovery_config_validate(config) != PROTO_OK) {
        return -EINVAL;
    }

    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        return ret;
    }

    local_slot = local_survey_discovery_slot(config->slot_count);
    now_ms = k_uptime_get_32();
    if (!uptime_deadline_reached(now_ms, start_ms)) {
        (void)sleep_with_uwb_standby_until_ms(start_ms);
        now_ms = k_uptime_get_32();
        ret = dwm3000_driver_configure_wake_mode();
        if (ret < 0) {
            return ret;
        }
    }

    first_slot = (uint8_t)MIN((uint32_t)config->slot_count,
                              (now_ms - start_ms) / config->slot_ms);
    LOG_INF("survey discovery timing: survey=%u start_ms=%u now_ms=%u elapsed_ms=%u slot_ms=%u slots=%u first_slot=%u local_slot=%u",
            config->survey_id,
            start_ms,
            now_ms,
            now_ms - start_ms,
            config->slot_ms,
            config->slot_count,
            first_slot,
            local_slot);
    for (uint8_t slot = first_slot;
         slot < config->slot_count && !anchor_survey_abort_is_requested();
         slot++) {
        int64_t slot_start_ms = (int64_t)start_ms + ((int64_t)slot * config->slot_ms);
        int64_t slot_end_ms = slot_start_ms + config->slot_ms;

        if (slot == local_slot) {
            struct uwb_survey_discovery_probe_frame probe = {
                .network_id = NETWORK_ID,
                .survey_id = config->survey_id,
                .anchor_id = DEVICE_ID,
                .anchor_slot = local_slot,
                .slot_count = config->slot_count,
                .flags = FLAG_DIAGNOSTIC,
            };
            uint8_t frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
            size_t frame_len = 0u;

            sleep_until_ms(slot_start_ms + SURVEY_DISCOVERY_RX_GUARD_MS);
            ret = uwb_encode_survey_discovery_probe(&probe,
                                                    frame,
                                                    sizeof(frame),
                                                    &frame_len);
            if (ret != PROTO_OK) {
                LOG_WRN("survey discovery probe encode failed: survey=%u ret=%d",
                        config->survey_id,
                        ret);
                continue;
            }
            ret = dwm3000_driver_send_frame(frame,
                                            frame_len,
                                            SURVEY_DISCOVERY_TX_TIMEOUT_MS);
            if (ret < 0) {
                LOG_WRN("survey discovery probe TX failed: survey=%u slot=%u ret=%d",
                        config->survey_id,
                        slot,
                        ret);
            } else {
                high_debug_log_event("SURVEY_DISCOVERY_PROBE_TX",
                                     "survey=%u slot=%u slot_ms=%u",
                                     config->survey_id,
                                     slot,
                                     config->slot_ms);
            }
            continue;
        }

        sleep_until_ms(slot_start_ms);
        while (k_uptime_get() < slot_end_ms && !anchor_survey_abort_is_requested()) {
            struct uwb_survey_discovery_probe_frame probe = {0};
            uint8_t frame[UWB_SURVEY_DISCOVERY_PROBE_LEN];
            size_t frame_len = 0u;
            uint8_t quality = 0u;
            int8_t rsl_dbm = 0;
            uint32_t remaining_ms = (uint32_t)MAX(1, slot_end_ms - k_uptime_get());

            ret = dwm3000_driver_receive_frame(remaining_ms,
                                               frame,
                                               sizeof(frame),
                                               &frame_len,
                                               &quality,
                                               &rsl_dbm);
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                continue;
            }
            ret = uwb_decode_survey_discovery_probe(frame, frame_len, &probe);
            if (ret != PROTO_OK ||
                probe.network_id != NETWORK_ID ||
                probe.survey_id != config->survey_id ||
                probe.anchor_id == DEVICE_ID ||
                probe.anchor_slot != slot ||
                probe.slot_count != config->slot_count) {
                continue;
            }
            if (quality > 100u) {
                quality = 100u;
            }
            survey_add_reach_entry(entries,
                                   ARRAY_SIZE(entries),
                                   &entry_count,
                                   probe.anchor_id,
                                   quality);
            for (size_t i = 0u; i < entry_count; i++) {
                if (entries[i].peer_id == probe.anchor_id && quality >= entries[i].quality) {
                    entries[i].rssi_dbm = rsl_dbm;
                    break;
                }
            }
            high_debug_log_event("SURVEY_DISCOVERY_PROBE_RX",
                                 "survey=%u peer=0x%016llx slot=%u quality=%u rsl=%d peers=%u",
                                 config->survey_id,
                                 (unsigned long long)probe.anchor_id,
                                 slot,
                                 quality,
                                 rsl_dbm,
                                 (unsigned int)entry_count);
        }
    }

    ret = survey_discovery_report_delay_ms(config,
                                           local_slot,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_delay_ms);
    if (ret != PROTO_OK) {
        LOG_WRN("survey discovery report slot calculation failed: survey=%u slot=%u ret=%d",
                config->survey_id,
                local_slot,
                ret);
        return mesh_errno_from_proto(ret);
    }

    ret = anchor_queue_survey_discovery_report(config->survey_id,
                                               entries,
                                               entry_count,
                                               u32_saturating_add(start_ms,
                                                                  report_delay_ms));
    if (ret < 0) {
        LOG_WRN("survey discovery report queue failed: survey=%u peers=%u ret=%d",
                config->survey_id,
                (unsigned int)entry_count,
                ret);
        return ret;
    }

    LOG_INF("survey discovery complete: survey=%u local_slot=%u peers=%u report_delay_ms=%u",
            config->survey_id,
            local_slot,
            (unsigned int)entry_count,
            report_delay_ms);
    return 0;
}



static int anchor_start_uwb_scan(void);
static void anchor_uwb_scan_work_handler(struct k_work *work);

static enum uwb_wake_decode_failure wake_failure_from_rx(enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
        return UWB_WAKE_DECODE_SFD_TIMEOUT;
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_NONE:
        return UWB_WAKE_DECODE_FRAME_TIMEOUT;
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return UWB_WAKE_DECODE_CRC_FAILURE;
    default:
        return UWB_WAKE_DECODE_FRAME_TIMEOUT;
    }
}

static enum uwb_wake_decode_failure wake_failure_from_proto_ret(int ret)
{
    return ret == PROTO_ERR_BAD_CRC ? UWB_WAKE_DECODE_CRC_FAILURE :
                                      UWB_WAKE_DECODE_FRAME_TIMEOUT;
}

static const char *wake_decode_failure_name(enum uwb_wake_decode_failure failure)
{
    switch (failure) {
    case UWB_WAKE_DECODE_PREAMBLE_ONLY:
        return "preamble_only";
    case UWB_WAKE_DECODE_SFD_TIMEOUT:
        return "sfd_timeout";
    case UWB_WAKE_DECODE_FRAME_TIMEOUT:
        return "frame_timeout";
    case UWB_WAKE_DECODE_CRC_FAILURE:
        return "crc_failure";
    default:
        return "unknown";
    }
}

static const char *rx_failure_name(enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_NONE:
        return "none";
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
        return "no_preamble_timeout";
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
        return "sfd_timeout";
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
        return "frame_timeout";
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
        return "crc_or_phy";
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return "bad_frame";
    default:
        return "unknown";
    }
}

static bool rx_failure_detected_preamble(enum dwm3000_rx_failure failure)
{
    switch (failure) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return true;
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    case DWM3000_RX_FAILURE_NONE:
    default:
        return false;
    }
}

static void anchor_range_window_record(struct anchor_range_window_report *report,
                                       const struct dwm3000_range_result *result)
{
    int8_t sampled_rsl = 0;
    uint8_t sampled_cir[UWB_CIR_SAMPLE_LEN] = {0};

    if (report == NULL || result == NULL) {
        return;
    }

    if (report->rsl_sampled) {
        sampled_rsl = report->result.rsl_dbm;
    }
    if (report->cir_sampled) {
        memcpy(sampled_cir, report->result.cir_sample, UWB_CIR_SAMPLE_LEN);
    }
    if (!report->have_exchange_start_ms && result->exchange_started) {
        report->first_exchange_start_ms = result->exchange_start_ms;
        report->have_exchange_start_ms = true;
    }

    if (!report->have_result || result->status == RANGE_OK) {
        report->result = *result;
        report->have_result = true;
    }
    if (report->have_exchange_start_ms) {
        report->result.exchange_start_ms = report->first_exchange_start_ms;
        report->result.exchange_started = true;
    }
    if (result->rsl_sampled && !report->rsl_sampled) {
        sampled_rsl = result->rsl_dbm;
        report->rsl_sampled = true;
    }
    if (report->rsl_sampled) {
        report->result.rsl_dbm = sampled_rsl;
        report->result.rsl_sampled = true;
    }
    if (result->cir_sampled && !report->cir_sampled) {
        memcpy(sampled_cir, result->cir_sample, UWB_CIR_SAMPLE_LEN);
        report->cir_sampled = true;
    }
    if (report->cir_sampled) {
        memcpy(report->result.cir_sample, sampled_cir, UWB_CIR_SAMPLE_LEN);
        report->result.cir_sampled = true;
    }

    if (result->status != RANGE_OK ||
        report->sample_count >= RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return;
    }

    report->distance_samples_mm[report->sample_count] = result->distance_mm;
    report->sample_sequence_start_ms[report->sample_count] =
        result->exchange_started ? result->exchange_start_ms : k_uptime_get();
    report->range_round_indices[report->sample_count] = result->round_index;
    report->distance_sum_mm += result->distance_mm;
    report->quality_sum += result->quality;
    report->sample_count++;
}

static void anchor_range_window_finalize(struct anchor_range_window_report *report)
{
    if (report == NULL || !report->have_result || report->sample_count == 0u) {
        return;
    }

    report->result.distance_mm = average_i32_nearest(report->distance_sum_mm,
                                                     report->sample_count);
    report->result.quality = (uint8_t)((report->quality_sum +
                                        (report->sample_count / 2u)) /
                                       report->sample_count);
    report->result.status = RANGE_OK;
}

static void anchor_set_uwb_busy(bool busy)
{
    k_spinlock_key_t key = k_spin_lock(&anchor_uwb_lock);

    anchor_uwb_busy = busy;
    k_spin_unlock(&anchor_uwb_lock, key);
}

static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us)
{
    int64_t elapsed_ms;
    uint64_t elapsed_us;

    if (DEVICE_ROLE != ROLE_ANCHOR || start_ms < 0) {
        return;
    }

    elapsed_ms = k_uptime_get() - start_ms;
    if (elapsed_ms <= 0) {
        return;
    }

    elapsed_us = (uint64_t)elapsed_ms * 1000u;
    if (elapsed_us <= already_counted_us) {
        return;
    }
    elapsed_us -= already_counted_us;
    uwb_anchor_note_awake_time(&anchor_uwb_session,
                               (uint32_t)MIN(elapsed_us, (uint64_t)UINT32_MAX));
}

static void log_uwb_range_schedule_entries(const char *role,
                                           const struct uwb_range_schedule_frame *schedule)
{
    if (role == NULL || schedule == NULL) {
        return;
    }

    for (uint8_t i = 0u; i < schedule->selected_count; i++) {
        const struct uwb_range_schedule_entry *entry = &schedule->entries[i];

        LOG_INF("%s UWB RANGE_SCHEDULE entry: order=%u/%u anchor=0x%016llx seq_base=%u samples=%u first_poll_ms=%u stride_us=%u burst_ms=%u",
                role,
                (unsigned int)(i + 1u),
                schedule->selected_count,
                (unsigned long long)entry->anchor_id,
                entry->seq,
                entry->sample_count,
                schedule->first_poll_delay_ms,
                schedule->exchange_stride_us,
                schedule->burst_window_ms);
    }
}

static uint32_t anchor_run_scheduled_uwb_ranges(const struct uwb_range_schedule_frame *schedule,
                                                int64_t schedule_rx_ms)
{
    struct anchor_range_window_report window_report = {0};
    uint32_t retained_sleep_us = 0u;
    size_t total_samples;
    int64_t no_poll_deadline_ms;
    bool poll_received = false;
    bool poll_guard_expired = false;

    if (schedule == NULL) {
        return 0u;
    }

    total_samples = uwb_range_schedule_total_samples(schedule);
    no_poll_deadline_ms = k_uptime_get() + ANCHOR_SCHEDULE_NO_POLL_GUARD_MS;
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_enter_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    LOG_INF("anchor scheduled UWB ranging start: clicker=0x%016llx event_seq=%u selected=%u total_samples=%u",
            (unsigned long long)schedule->clicker_id,
            schedule->click_event_id,
            schedule->selected_count,
            (unsigned int)total_samples);
    log_uwb_range_schedule_entries("anchor", schedule);

    for (size_t sample_index = 0u; sample_index < total_samples; sample_index++) {
        struct dwm3000_range_request expected;
        struct dwm3000_range_result range_result;
        struct uwb_range_exchange_identity identity;
        uint64_t anchor_id = 0u;
        uint8_t seq = 0u;
        uint8_t round_index = 0u;
        int64_t listen_start_ms;
        int64_t listen_deadline_ms;
        int ret = -ETIMEDOUT;

        ret = uwb_range_schedule_sample_at(schedule, sample_index, &anchor_id, &seq);
        if (ret != PROTO_OK || anchor_id != DEVICE_ID) {
            high_debug_log_event("DS_TWR_POLL_RX",
                                 "sample=%u/%u expected_anchor=0x%016llx local_anchor=0x%016llx result=wrong-target",
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 (unsigned long long)anchor_id,
                                 (unsigned long long)DEVICE_ID);
            continue;
        }

        if (!poll_received && k_uptime_get() >= no_poll_deadline_ms) {
            poll_guard_expired = true;
            break;
        }

        memset(&identity, 0, sizeof(identity));
        identity.network_id = schedule->network_id;
        identity.clicker_id = schedule->clicker_id;
        identity.click_event_id = schedule->click_event_id;
        identity.attempt_index = schedule->attempt_index;
        identity.nonce = schedule->nonce;
        identity.anchor_id = anchor_id;
        identity.ranging_channel = schedule->ranging_channel;
        identity.reply_delay_us = schedule->reply_delay_us;
        identity.seq = seq;
        identity.flags = schedule->flags;
        ret = uwb_anchor_range_round_index(&anchor_uwb_session,
                                           &identity,
                                           &round_index);
        if (ret != PROTO_OK) {
            uwb_anchor_note_timing_rejection(&anchor_uwb_session);
            continue;
        }
        uwb_anchor_note_sample_order(&anchor_uwb_session);

        listen_start_ms =
            scheduled_range_sample_target_us(schedule_rx_ms, schedule, sample_index) / 1000;
        listen_start_ms -= UWB_SCHEDULE_GUARD_MS;
        if (!poll_received && listen_start_ms > no_poll_deadline_ms) {
            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(no_poll_deadline_ms));
            poll_guard_expired = true;
            break;
        }
        retained_sleep_us = u32_saturating_add(
            retained_sleep_us,
            sleep_with_uwb_idle_until_ms(listen_start_ms));

        listen_deadline_ms = ceil_us_to_ms(
            scheduled_range_sample_target_us(schedule_rx_ms, schedule, sample_index + 1u));
        listen_deadline_ms += UWB_SCHEDULE_GUARD_MS;

        memset(&expected, 0, sizeof(expected));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        expected.initiator_id = schedule->clicker_id;
        expected.responder_id = DEVICE_ID;
        expected.network_id = schedule->network_id;
        expected.session_nonce = schedule->nonce;
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = schedule->click_event_id;
        expected.seq = seq;
        expected.round_index = round_index;
        expected.flags = schedule->flags;
        expected.capture_rsl = true;
        expected.skip_responder_report = false;
        expected.expect_clicker_diag = false;
        expected.send_anchor_diag = false;

        LOG_INF("anchor scheduled UWB sample listen: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                (unsigned int)(sample_index + 1u),
                (unsigned int)total_samples,
                round_index,
                seq);
        stage1_led_phase(STAGE1_LED_PHASE_RANGE);
        stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
        high_debug_log_event("DS_TWR_POLL_RX",
                             "listen=1 clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u schedule_rx_ms=%lld target_us=%lld listen_start_ms=%lld now_ms=%lld deadline_ms=%lld first_poll_ms=%u stride_us=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             uwb_schedule_burst_id(schedule->click_event_id,
                                                   schedule->attempt_index),
                             (unsigned int)(sample_index + 1u),
                             (unsigned int)total_samples,
                             round_index,
                             seq,
                             (long long)schedule_rx_ms,
                             (long long)scheduled_range_sample_target_us(schedule_rx_ms,
                                                                         schedule,
                                                                         sample_index),
                             (long long)listen_start_ms,
                             (long long)k_uptime_get(),
                             (long long)listen_deadline_ms,
                             schedule->first_poll_delay_ms,
                             schedule->exchange_stride_us);
        while (k_uptime_get() < listen_deadline_ms) {
            int64_t bounded_deadline_ms = listen_deadline_ms;
            int64_t remaining_ms;

            if (!poll_received && bounded_deadline_ms > no_poll_deadline_ms) {
                bounded_deadline_ms = no_poll_deadline_ms;
            }
            remaining_ms = bounded_deadline_ms - k_uptime_get();
            if (remaining_ms <= 0) {
                break;
            }

            expected.timeout_ms = (uint32_t)MAX(1, remaining_ms);
            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         expected.timeout_ms,
                                                         &range_result);
            if (ret == -EAGAIN) {
                if (range_result.status == RANGE_WRONG_TARGET) {
                    high_debug_log_event("DS_TWR_POLL_RX",
                                         "result=wrong-target expected_clicker=0x%016llx observed_clicker=0x%016llx expected_anchor=0x%016llx observed_anchor=0x%016llx event_seq=%u observed_event_seq=%u expected_round=%u observed_round=%u expected_seq=%u observed_seq=%u",
                                         (unsigned long long)schedule->clicker_id,
                                         (unsigned long long)range_result.initiator_id,
                                         (unsigned long long)DEVICE_ID,
                                         (unsigned long long)range_result.responder_id,
                                         schedule->click_event_id,
                                         range_result.session_id,
                                         round_index,
                                         range_result.round_index,
                                         seq,
                                         range_result.seq);
                    LOG_DBG("anchor ignored nonmatching UWB POLL: expected_clicker=0x%016llx observed_clicker=0x%016llx expected_anchor=0x%016llx observed_anchor=0x%016llx event_seq=%u observed_event_seq=%u expected_round=%u observed_round=%u expected_seq=%u observed_seq=%u",
                            (unsigned long long)schedule->clicker_id,
                            (unsigned long long)range_result.initiator_id,
                            (unsigned long long)DEVICE_ID,
                            (unsigned long long)range_result.responder_id,
                            schedule->click_event_id,
                            range_result.session_id,
                            round_index,
                            range_result.round_index,
                            seq,
                            range_result.seq);
                } else if (range_result.status == RANGE_BAD_FRAME ||
                           range_result.status == RANGE_RX_ERROR) {
                    LOG_DBG("anchor ignored pre-POLL UWB frame inside scheduled slot: status=%s(%u) expected_clicker=0x%016llx event_seq=%u seq=%u",
                            range_status_name(range_result.status),
                            range_result.status,
                            (unsigned long long)schedule->clicker_id,
                            schedule->click_event_id,
                            seq);
                }
                continue;
            }
            break;
        }
        if (range_result.exchange_started) {
            poll_received = true;
        }
        if (!poll_received && k_uptime_get() >= no_poll_deadline_ms) {
            poll_guard_expired = true;
            break;
        }

        if (!range_result.exchange_started) {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_RX_TIMEOUT;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
            LOG_WRN("anchor scheduled UWB sample did not start: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret);
            high_debug_log_event("RANGE_FAIL",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s exchange_started=0",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 seq,
                                 ret,
                                 range_status_name(range_result.status));
        } else if (ret == 0 && range_result.status == RANGE_OK) {
            HIGH_DEBUG_COUNTER_INC(ds_twr_successes);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, RANGE_OK);
            anchor_range_window_record(&window_report, &range_result);
            stage1_led_result(STAGE1_LED_RESULT_OK);
            LOG_INF("anchor scheduled UWB sample complete: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u count=%u",
                    (unsigned long long)range_result.initiator_id,
                    range_result.session_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    range_result.seq,
                    range_result.distance_mm,
                    range_result.quality,
                    window_report.sample_count);
            high_debug_log_event("RANGE_OK",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u anchor=0x%016llx distance_mm=%d quality=%u",
                                 (unsigned long long)range_result.initiator_id,
                                 range_result.session_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 range_result.seq,
                                 (unsigned long long)range_result.responder_id,
                                 range_result.distance_mm,
                                 range_result.quality);
        } else {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            if (range_result.status == RANGE_TIMING_INVALID) {
                HIGH_DEBUG_COUNTER_INC(ds_twr_timing_rejects);
            }
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            if (range_result.initiator_id != 0u && range_result.responder_id != 0u) {
                anchor_range_window_record(&window_report, &range_result);
            }
            stage1_led_result(range_result.status == RANGE_RX_TIMEOUT ?
                              STAGE1_LED_RESULT_TIMEOUT :
                              STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor scheduled UWB sample failed: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret,
                    range_status_name(range_result.status),
                    range_result.status);
            high_debug_log_event(range_result.status == RANGE_TIMING_INVALID ?
                                 "RANGE_TIMING_REJECT" : "RANGE_FAIL",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s status=%u",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 seq,
                                 ret,
                                 range_status_name(range_result.status),
                                 range_result.status);
        }
    }

    if (poll_guard_expired) {
        LOG_WRN("anchor scheduled UWB poll guard expired: clicker=0x%016llx event_seq=%u waited_ms=%u total_samples=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                ANCHOR_SCHEDULE_NO_POLL_GUARD_MS,
                (unsigned int)total_samples);
        high_debug_log_event("RANGE_FAIL",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=no-poll guard_ms=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             ANCHOR_SCHEDULE_NO_POLL_GUARD_MS);
    } else if (poll_received &&
               schedule->diagnostics_required != UWB_RANGE_SCHEDULE_DIAGNOSTICS_OMITTED) {
#if defined(CONFIG_IMEC_ML_ANCHOR)
        anchor_scan_recovery_gap_requested = true;
        retained_sleep_us = u32_saturating_add(
            retained_sleep_us,
            ml_anchor_run_post_burst_diagnostics(schedule, schedule_rx_ms, total_samples));
#endif
    }

    anchor_range_window_finalize(&window_report);
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    build_uwb_schedule_report_if_relevant(&anchor_uwb_session,
                                          schedule->flags,
                                          &window_report);
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_exit_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    return retained_sleep_us;
}

#if defined(CONFIG_IMEC_ML_ANCHOR)
static bool anchor_pair_schedule_matches_epoch(
    const struct uwb_anchor_pair_schedule_frame *schedule)
{
    return schedule != NULL &&
           anchor_uwb_session.epoch.active &&
           schedule->network_id == anchor_uwb_session.epoch.network_id &&
           schedule->clicker_id == anchor_uwb_session.epoch.clicker_id &&
           schedule->survey_id == anchor_uwb_session.epoch.click_event_id &&
           schedule->attempt_index == anchor_uwb_session.epoch.attempt_index &&
           schedule->nonce == anchor_uwb_session.epoch.nonce &&
           schedule->flags == anchor_uwb_session.epoch.flags;
}

static int anchor_send_pair_survey_result(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    uint8_t pair_index,
    uint64_t initiator_id,
    uint64_t responder_id,
    const struct dwm3000_range_result *range_result)
{
    struct uwb_anchor_pair_result_frame result = {0};
    uint8_t frame[UWB_ANCHOR_PAIR_RESULT_LEN];
    size_t frame_len = 0u;
    int ret;

    if (schedule == NULL || range_result == NULL) {
        return -EINVAL;
    }

    result.network_id = schedule->network_id;
    result.clicker_id = schedule->clicker_id;
    result.survey_id = schedule->survey_id;
    result.nonce = schedule->nonce;
    result.initiator_id = initiator_id;
    result.responder_id = responder_id;
    result.pair_index = pair_index;
    result.pair_count = schedule->pair_count;
    result.seq = (uint8_t)(pair_index + 1u);
    result.status = range_status_valid(range_result->status) ?
                    range_result->status : RANGE_INTERNAL_ERROR;
    result.quality = range_result->quality;
    result.distance_mm = range_result->status == RANGE_OK ?
                         range_result->distance_mm : 0;
    result.rsl_dbm = range_result->rsl_sampled ? range_result->rsl_dbm : 0;
    result.flags = schedule->flags;

    ret = uwb_encode_anchor_pair_result(&result, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    if (ret < 0) {
        LOG_WRN("anchor pair-survey result TX failed: survey=%u pair=%u ret=%d",
                schedule->survey_id,
                pair_index,
                ret);
        return ret;
    }
    LOG_INF("anchor pair-survey result TX: survey=%u pair=%u/%u initiator=0x%016llx responder=0x%016llx status=%s(%u) distance_mm=%d",
            schedule->survey_id,
            (unsigned int)(pair_index + 1u),
            schedule->pair_count,
            (unsigned long long)initiator_id,
            (unsigned long long)responder_id,
            range_status_name(result.status),
            result.status,
            result.distance_mm);
    return 0;
}

static uint32_t anchor_run_clicker_pair_survey(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t schedule_rx_ms)
{
    uint32_t retained_sleep_us = 0u;
    uint16_t local_start_delay_ms = 0u;
    bool local_listed = false;

    if (!anchor_pair_schedule_matches_epoch(schedule)) {
        LOG_WRN("anchor rejected pair-survey schedule identity");
        return 0u;
    }

    for (uint8_t i = 0u; i < schedule->anchor_count; i++) {
        if (schedule->anchor_ids[i] == DEVICE_ID) {
            local_start_delay_ms = schedule->anchor_start_delay_ms[i];
            local_listed = true;
            break;
        }
    }
    if (!local_listed) {
        LOG_INF("anchor pair-survey schedule not for local anchor: survey=%u",
                schedule->survey_id);
        return 0u;
    }

    LOG_INF("anchor pair-survey start: survey=%u anchors=%u pairs=%u schedule_start_ms=%u stride_ms=%u window_ms=%u rx_guard_ms=%u",
            schedule->survey_id,
            schedule->anchor_count,
            schedule->pair_count,
            local_start_delay_ms,
            schedule->pair_stride_ms,
            schedule->pair_window_ms,
            UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS);

    for (uint8_t pair_index = 0u; pair_index < schedule->pair_count; pair_index++) {
        struct dwm3000_range_request request = {0};
        struct dwm3000_range_result range_result = {0};
        uint64_t initiator_id = 0u;
        uint64_t responder_id = 0u;
        int64_t target_ms;
        int ret;

        ret = uwb_anchor_pair_at(schedule, pair_index, &initiator_id, &responder_id);
        if (ret != PROTO_OK) {
            continue;
        }
        if (DEVICE_ID != initiator_id && DEVICE_ID != responder_id) {
            continue;
        }

        target_ms = schedule_rx_ms + local_start_delay_ms +
                    ((int64_t)pair_index * schedule->pair_stride_ms);
        request.initiator_id = initiator_id;
        request.responder_id = responder_id;
        request.network_id = schedule->network_id;
        request.session_nonce = schedule->nonce;
        request.responder_short_addr = uwb_session_short_addr_from_id(responder_id);
        request.session_id = schedule->survey_id;
        request.seq = (uint8_t)(pair_index + 1u);
        request.round_index = 0u;
        request.flags = schedule->flags;
        request.timeout_ms = schedule->pair_window_ms;
        request.reply_delay_uus = 0u;
        request.capture_rsl = false;
        request.skip_responder_report = false;

        if (DEVICE_ID == initiator_id) {
            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(target_ms));
            memset(&range_result, 0, sizeof(range_result));
            range_result.status = RANGE_RX_TIMEOUT;
            ret = dwm3000_driver_range_initiator(&request, &range_result);
            (void)dwm3000_driver_idle();
            if (ret < 0 && !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            (void)anchor_send_pair_survey_result(schedule,
                                                 pair_index,
                                                 initiator_id,
                                                 responder_id,
                                                 &range_result);
        } else {
            int64_t listen_start_ms = target_ms -
                                      UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS;
            int64_t listen_deadline_ms = target_ms + schedule->pair_window_ms;

            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(listen_start_ms));
            while (k_uptime_get() < listen_deadline_ms) {
                int64_t remaining_ms = listen_deadline_ms - k_uptime_get();

                memset(&range_result, 0, sizeof(range_result));
                range_result.status = RANGE_RX_TIMEOUT;
                request.timeout_ms = (uint32_t)MAX(1, remaining_ms);
                ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                             &request,
                                                             request.timeout_ms,
                                                             &range_result);
                if (ret == -EAGAIN) {
                    continue;
                }
                break;
            }
            (void)dwm3000_driver_idle();
        }
    }

    LOG_INF("anchor pair-survey done: survey=%u", schedule->survey_id);
    return retained_sleep_us;
}
#endif

static bool anchor_handle_uwb_claim(const struct uwb_wake_claim_frame *first_claim,
                                    uint8_t first_quality,
                                    int64_t first_rx_ms,
                                    uint32_t *retained_sleep_us,
                                    bool *deferred_mesh_rx_queued)
{
    struct uwb_wake_claim_frame selected_claim;
    int64_t selected_rx_ms;
    int64_t collect_deadline_ms;
    uint8_t selected_quality = first_quality;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    int ret;
    int last_discovery_ret = -ETIMEDOUT;
    int last_discovery_decode_ret = PROTO_OK;
    int64_t discovery_start_ms;
    int64_t discovery_deadline_ms;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    enum uwb_anchor_claim_decision selected_decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    bool discover_received = false;
    bool schedule_frame_ready = false;
    int64_t schedule_rx_ms = -1;
    struct uwb_discover_frame discover = {0};

    if (first_claim == NULL) {
        return false;
    }
    if (retained_sleep_us != NULL) {
        *retained_sleep_us = 0u;
    }

    selected_claim = *first_claim;
    selected_rx_ms = first_rx_ms;
    HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    high_debug_log_event("WAKE_CLAIM_RX",
                         "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx nonce=0x%016llx quality=%u",
                         (unsigned long long)first_claim->clicker_id,
                         first_claim->click_event_id,
                         first_claim->attempt_index,
                         (unsigned long long)first_claim->priority_id,
                         (unsigned long long)first_claim->nonce,
                         first_quality);
    ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                       first_claim,
                                       (uint32_t)first_rx_ms,
                                       &decision);
    if (ret != PROTO_OK) {
        HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        high_debug_log_event("WAKE_CLAIM_REJECT",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=%s ret=%d",
                             (unsigned long long)first_claim->clicker_id,
                             first_claim->click_event_id,
                             first_claim->attempt_index,
                             claim_decision_name(decision),
                             ret);
        LOG_DBG("anchor rejected UWB WAKE_CLAIM: clicker=0x%016llx ret=%d decision=%u",
                (unsigned long long)first_claim->clicker_id,
                ret,
                decision);
        return false;
    }
    selected_decision = decision;
    HIGH_DEBUG_COUNTER_INC(wake_claim_accepted);
    stage1_led_result(STAGE1_LED_RESULT_OK);
    high_debug_log_event("WAKE_CLAIM_ACCEPT",
                         "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                         (unsigned long long)first_claim->clicker_id,
                         first_claim->click_event_id,
                         first_claim->attempt_index,
                         (unsigned long long)first_claim->priority_id,
                         claim_decision_name(decision));
    LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
            (unsigned long long)first_claim->clicker_id,
            first_claim->click_event_id,
            first_claim->attempt_index,
            (unsigned long long)first_claim->priority_id,
            ret,
            decision);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_WAKE_ACCEPT clicker=0x%llx evt=%u attempt=%u flags=0x%02x disc_in=%u q=%u\n",
                            (unsigned long long)first_claim->clicker_id,
                            first_claim->click_event_id,
                            first_claim->attempt_index,
                            first_claim->flags,
                            first_claim->discovery_starts_in_ms,
                            first_quality);
    }
    mesh_preempt_for_click_event();

    collect_deadline_ms = k_uptime_get() + ANCHOR_CLAIM_COLLECTION_MS;
    while (k_uptime_get() < collect_deadline_ms) {
        struct uwb_wake_claim_frame claim;
        uint8_t quality = 0u;
        int64_t remaining_ms = collect_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            continue;
        }
        ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (ret != PROTO_OK) {
            enum uwb_wake_decode_failure failure = wake_failure_from_proto_ret(ret);

            uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                failure);
            LOG_DBG("anchor ignored competing UWB WAKE_CLAIM decode failure: ret=%d reason=%s frame_len=%u",
                    ret,
                    wake_decode_failure_name(failure),
                    (unsigned int)frame_len);
            continue;
        }

        ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                           &claim,
                                           k_uptime_get_32(),
                                           &decision);
        HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
        LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
                (unsigned long long)claim.clicker_id,
                claim.click_event_id,
                claim.attempt_index,
                (unsigned long long)claim.priority_id,
                ret,
                decision);
        if (ret == PROTO_OK &&
            (decision == UWB_ANCHOR_CLAIM_ACCEPTED ||
             decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY)) {
            HIGH_DEBUG_COUNTER_INC(wake_claim_accepted);
            selected_claim = claim;
            selected_rx_ms = k_uptime_get();
            selected_quality = quality;
            selected_decision = decision;
            stage1_led_result(STAGE1_LED_RESULT_OK);
            high_debug_log_event("WAKE_CLAIM_ACCEPT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 (unsigned long long)claim.priority_id,
                                 claim_decision_name(decision));
        } else {
            HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("WAKE_CLAIM_REJECT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u reason=%s ret=%d",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 claim_decision_name(decision),
                                 ret);
        }
    }

    LOG_INF("anchor selected UWB claim: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx decision=%u",
            (unsigned long long)anchor_uwb_session.epoch.clicker_id,
            anchor_uwb_session.epoch.click_event_id,
            anchor_uwb_session.epoch.attempt_index,
            (unsigned long long)anchor_uwb_session.epoch.priority_id,
            selected_decision);
    anchor_click_window_set_active(
        app_mesh_c5_wake_claim_preempts_mesh(selected_claim.flags));
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_WINDOW active=%u flags=0x%02x\n",
                            anchor_click_window_active() ? 1u : 0u,
                            selected_claim.flags);
    }

    discovery_start_ms =
        (int64_t)selected_rx_ms + selected_claim.discovery_starts_in_ms;
    discovery_deadline_ms = discovery_start_ms +
                            (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
                             MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS :
                             UWB_DISCOVERY_RX_LATE_GUARD_MS);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_DISC_WAIT now=%u start=%lld deadline=%lld guard=%u listen=%u route_win=%u\n",
                            k_uptime_get_32(),
                            (long long)discovery_start_ms,
                            (long long)discovery_deadline_ms,
                            UWB_DISCOVERY_RX_GUARD_MS,
                            UWB_DISCOVERY_LISTEN_MS,
                            MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS);
    }
    high_debug_log_event("DISCOVER_WAIT",
                         "clicker=0x%016llx event_seq=%u attempt=%u selected_rx_ms=%lld discovery_starts_in_ms=%u wait_start_ms=%lld deadline_ms=%lld listen_ms=%u",
                         (unsigned long long)selected_claim.clicker_id,
                         selected_claim.click_event_id,
                         selected_claim.attempt_index,
                         (long long)selected_rx_ms,
                         selected_claim.discovery_starts_in_ms,
                         (long long)(discovery_start_ms - UWB_DISCOVERY_RX_GUARD_MS),
                         (long long)discovery_deadline_ms,
                         UWB_DISCOVERY_LISTEN_MS);

    {
        int64_t discover_listen_start_ms =
            discovery_start_ms - UWB_DISCOVERY_RX_GUARD_MS;

        if (retained_sleep_us != NULL) {
            *retained_sleep_us = u32_saturating_add(
                *retained_sleep_us,
                sleep_with_uwb_idle_until_ms(discover_listen_start_ms));
        } else {
            (void)sleep_with_uwb_idle_until_ms(discover_listen_start_ms);
        }
    }

    stage1_led_phase(STAGE1_LED_PHASE_DISCOVERY);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        ret = dwm3000_driver_configure_wake_mesh_control_mode();
        status_debug_printf("DBG_ANCHOR_DISC_EXT_CONFIG ret=%d\n", ret);
        if (ret < 0) {
            last_discovery_ret = ret;
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("DISCOVER_RX",
                                 "clicker=0x%016llx event_seq=%u attempt=%u result=config-fail ret=%d",
                                 (unsigned long long)selected_claim.clicker_id,
                                 selected_claim.click_event_id,
                                 selected_claim.attempt_index,
                                 ret);
            goto discovery_miss;
        }
    }

    while (k_uptime_get() < discovery_deadline_ms) {
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        uint8_t quality = 0u;
        int64_t remaining_ms = discovery_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
        if (ret == -ETIMEDOUT) {
            last_discovery_ret = ret;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_RX_TIMEOUT now=%u rem=%lld fail=%u\n",
                                    k_uptime_get_32(),
                                    (long long)(discovery_deadline_ms - k_uptime_get()),
                                    (unsigned int)rx_failure);
            }
            break;
        }
        if (ret < 0) {
            last_discovery_ret = ret;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_RX_FAIL ret=%d now=%u rem=%lld fail=%u\n",
                                    ret,
                                    k_uptime_get_32(),
                                    (long long)(discovery_deadline_ms - k_uptime_get()),
                                    (unsigned int)rx_failure);
            }
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_FRAME len=%u type=0x%02x q=%u now=%u\n",
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                                quality,
                                k_uptime_get_32());
        }

        if ((selected_claim.flags & FLAG_RANGE_ONLY) != 0u &&
            frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_SCHEDULE) {
            schedule_frame_ready = true;
            schedule_rx_ms = k_uptime_get();
            break;
        }

        last_discovery_decode_ret = uwb_decode_discover(frame, frame_len, &discover);
        if (last_discovery_decode_ret == PROTO_OK) {
            selected_quality = quality;
            discover_received = true;
            break;
        }

        if (frame_len == UWB_WAKE_CLAIM_LEN) {
            struct uwb_wake_claim_frame late_claim;

            ret = uwb_decode_wake_claim(frame, frame_len, &late_claim);
            if (ret == PROTO_OK) {
                LOG_DBG("anchor ignored late UWB WAKE_CLAIM while waiting for DISCOVER: clicker=0x%016llx event_seq=%u attempt=%u",
                        (unsigned long long)late_claim.clicker_id,
                        late_claim.click_event_id,
                        late_claim.attempt_index);
                continue;
            }
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued || valid_mesh_frame) {
                if (queued && deferred_mesh_rx_queued != NULL) {
                    *deferred_mesh_rx_queued = true;
                }
                high_debug_log_event("MESH_CH5_AFTER_WAKE_RX",
                                     "clicker=0x%016llx event_seq=%u frame_len=%u quality=%u valid=%u",
                                     (unsigned long long)selected_claim.clicker_id,
                                     selected_claim.click_event_id,
                                     (unsigned int)frame_len,
                                     quality,
                                     valid_mesh_frame ? 1u : 0u);
                LOG_INF("anchor accepted channel-5 mesh frame after wake claim: clicker=0x%016llx event_seq=%u len=%u quality=%u valid=%u",
                        (unsigned long long)selected_claim.clicker_id,
                        selected_claim.click_event_id,
                        (unsigned int)frame_len,
                        quality,
                        valid_mesh_frame ? 1u : 0u);
                return true;
            }
            status_debug_printf("DBG_ANCHOR_MESH_AFTER_WAKE_REJECT len=%u type=0x%02x q=%u discover_ret=%d valid=%u queued=%u\n",
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                                quality,
                                last_discovery_decode_ret,
                                valid_mesh_frame ? 1u : 0u,
                                queued ? 1u : 0u);
        }

        LOG_DBG("anchor ignored non-DISCOVER frame during discovery wait: decode_ret=%d frame_len=%u type=0x%02x quality=%u",
                last_discovery_decode_ret,
                (unsigned int)frame_len,
                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                quality);
    }

discovery_miss:
    if (discover_received) {
        struct uwb_discovery_reply_frame reply;
#if defined(CONFIG_IMEC_ML_ANCHOR)
        uint16_t discovery_battery_mv = ml_anchor_cached_battery_mv();
#else
        uint16_t discovery_battery_mv = 0u;
#endif

        HIGH_DEBUG_COUNTER_INC(discovery_rx);
        high_debug_log_event("DISCOVER_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u network_id=0x%08x channel=%u quality=%u",
                             (unsigned long long)discover.clicker_id,
                             discover.click_event_id,
                             discover.attempt_index,
                             discover.network_id,
                             UWB_WAKE_CHANNEL,
                             selected_quality);

        ret = uwb_anchor_build_discovery_reply(&anchor_uwb_session,
                                               &discover,
                                               selected_quality,
                                               discovery_battery_mv,
                                               &reply);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY build rejected: %d", ret);
            return true;
        }
#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (discover.discovery_slot_count > 0u) {
            reply.anchor_slot = ml_anchor_discovery_slot(discover.discovery_slot_count);
        }
#else
        ret = local_anchor_discovery_slot(discover.discovery_slot_count,
                                          &reply.anchor_slot);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY slot rejected: slot_count=%u ret=%d",
                    discover.discovery_slot_count,
                    ret);
            return true;
        }
#endif

        sleep_precise_us((uint32_t)reply.anchor_slot * UWB_DISCOVERY_SLOT_US);
        ret = uwb_encode_discovery_reply(&reply, frame, sizeof(frame), &frame_len);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY encode failed: %d", ret);
            return true;
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        UWB_DISCOVERY_REPLY_TX_TIMEOUT_MS);
        if (ret < 0) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY TX failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(discovery_reply_tx);
        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("DISCOVERY_REPLY_TX",
                             "clicker=0x%016llx event_seq=%u slot=%u slot_source=%s quality=%u",
                             (unsigned long long)reply.selected_clicker_id,
                             reply.click_event_id,
                             reply.anchor_slot,
                             ANCHOR_DISCOVERY_SLOT_SOURCE,
                             reply.rx_quality);
        LOG_INF("anchor UWB DISCOVERY_REPLY sent: clicker=0x%016llx slot=%u source=%s quality=%u",
                (unsigned long long)reply.selected_clicker_id,
                reply.anchor_slot,
                ANCHOR_DISCOVERY_SLOT_SOURCE,
                reply.rx_quality);
    } else if (schedule_frame_ready) {
        stage1_led_result(STAGE1_LED_RESULT_OK);
        LOG_INF("anchor fast range-only path: using direct RANGE_SCHEDULE without DISCOVER reply");
        high_debug_log_event("DISCOVER_SKIP",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=range_only_schedule",
                             (unsigned long long)selected_claim.clicker_id,
                             selected_claim.click_event_id,
                             selected_claim.attempt_index);
    } else {
        stage1_led_result(last_discovery_ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_MISS ret=%d decode=%d now=%u\n",
                                last_discovery_ret,
                                last_discovery_decode_ret,
                                k_uptime_get_32());
        }
        LOG_WRN("anchor UWB DISCOVER not received: ret=%d decode_ret=%d listen_ms=%u",
                last_discovery_ret,
                last_discovery_decode_ret,
                UWB_DISCOVERY_LISTEN_MS);
        return true;
    }

    stage1_led_phase(STAGE1_LED_PHASE_SCHEDULE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    if (!schedule_frame_ready) {
        uint32_t schedule_rx_window_ms =
            UWB_RANGE_SCHEDULE_RX_MS +
            discovery_window_ms_for_slots(discover.discovery_slot_count) +
            UWB_DISCOVERY_RX_GUARD_MS;
        int64_t schedule_deadline_ms = k_uptime_get() + schedule_rx_window_ms;
        int last_schedule_ret = -ETIMEDOUT;

        while (k_uptime_get() < schedule_deadline_ms) {
            uint8_t frame_type = 0u;
            int64_t remaining_ms = schedule_deadline_ms - k_uptime_get();

            ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                          frame,
                                                          sizeof(frame),
                                                          &frame_len,
                                                          NULL,
                                                          NULL,
                                                          NULL);
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                last_schedule_ret = ret;
                continue;
            }
            if (frame_len < UWB_SYNC_HEADER_LEN) {
                last_schedule_ret = -EBADMSG;
                continue;
            }

            frame_type = frame[2];
            if (frame_type != MSG_UWB_RANGE_SCHEDULE &&
#if defined(CONFIG_IMEC_ML_ANCHOR)
                frame_type != MSG_UWB_ANCHOR_PAIR_SCHEDULE &&
#endif
                frame_type != MSG_UWB_RANGE_RELEASE) {
                last_schedule_ret = -EBADMSG;
                LOG_DBG("anchor ignored non-schedule frame while waiting for RANGE_SCHEDULE: type=0x%02x len=%u",
                        frame_type,
                        (unsigned int)frame_len);
                continue;
            }

            schedule_frame_ready = true;
            schedule_rx_ms = k_uptime_get();
            break;
        }
        if (!schedule_frame_ready) {
            ret = last_schedule_ret;
        }
    }
    if (ret == 0) {
        struct uwb_range_schedule_frame schedule;

        if (schedule_rx_ms < 0) {
            schedule_rx_ms = k_uptime_get();
        }

        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_RELEASE) {
            struct uwb_range_release_frame release;

            ret = uwb_decode_range_release(frame, frame_len, &release);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor UWB RANGE_RELEASE decode failed: %d", ret);
                return true;
            }

            ret = uwb_anchor_accept_range_release(&anchor_uwb_session, &release);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor rejected UWB RANGE_RELEASE: %d", ret);
                return true;
            }

            stage1_led_result(STAGE1_LED_RESULT_OK);
            LOG_INF("anchor UWB RANGE_RELEASE accepted: clicker=0x%016llx event_seq=%u discovered=%u min=%u reason=%u",
                    (unsigned long long)release.clicker_id,
                    release.click_event_id,
                    release.discovered_anchor_count,
                    release.min_anchor_count,
                    release.reason);
            return true;
        }

#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_ANCHOR_PAIR_SCHEDULE) {
            struct uwb_anchor_pair_schedule_frame pair_schedule;

            ret = uwb_decode_anchor_pair_schedule(frame, frame_len, &pair_schedule);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor pair-survey schedule decode failed: %d", ret);
                return true;
            }
            if (!anchor_pair_schedule_matches_epoch(&pair_schedule)) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor rejected pair-survey schedule identity: survey=%u",
                        pair_schedule.survey_id);
                return true;
            }
            stage1_led_result(STAGE1_LED_RESULT_OK);
            if (retained_sleep_us != NULL) {
                *retained_sleep_us = u32_saturating_add(
                    *retained_sleep_us,
                    anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms));
            } else {
                (void)anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms);
            }
            return true;
        }
#endif

        ret = uwb_decode_range_schedule(frame, frame_len, &schedule);
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB RANGE_SCHEDULE decode failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(schedules_rx);
        high_debug_log_event("RANGE_SCHEDULE_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u selected=%u reply_delay_uus=%u nonce=0x%016llx",
                             (unsigned long long)schedule.clicker_id,
                             schedule.click_event_id,
                             schedule.attempt_index,
                             schedule.selected_count,
                             schedule.reply_delay_us,
                             (unsigned long long)schedule.nonce);

        ret = uwb_anchor_accept_range_schedule(&anchor_uwb_session,
                                               &schedule,
                                               (uint32_t)schedule_rx_ms,
                                               UWB_SCHEDULE_GUARD_MS);
        if (ret == PROTO_ERR_NOT_FOUND) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_INF("anchor not selected in UWB RANGE_SCHEDULE: clicker=0x%016llx event_seq=%u",
                    (unsigned long long)schedule.clicker_id,
                    schedule.click_event_id);
            return true;
        }
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("RANGE_SCHEDULE_REJECT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u reason=proto_%d",
                                 (unsigned long long)schedule.clicker_id,
                                 schedule.click_event_id,
                                 schedule.attempt_index,
                                 ret);
            LOG_WRN("anchor rejected UWB RANGE_SCHEDULE: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(schedules_accepted);
        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("RANGE_SCHEDULE_ACCEPT",
                             "clicker=0x%016llx event_seq=%u attempt=%u selected=%u burst_id=%u",
                             (unsigned long long)schedule.clicker_id,
                             schedule.click_event_id,
                             schedule.attempt_index,
                             schedule.selected_count,
                             uwb_schedule_burst_id(schedule.click_event_id,
                                                   schedule.attempt_index));

        if (retained_sleep_us != NULL) {
            *retained_sleep_us = u32_saturating_add(
                *retained_sleep_us,
                anchor_run_scheduled_uwb_ranges(&schedule, schedule_rx_ms));
        } else {
            (void)anchor_run_scheduled_uwb_ranges(&schedule, schedule_rx_ms);
        }
    } else {
        stage1_led_result(ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
        LOG_WRN("anchor UWB RANGE_SCHEDULE not received: ret=%d", ret);
    }
    return true;
}

static void anchor_uwb_scan_work_handler(struct k_work *work)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    uint32_t retained_sleep_us = 0u;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int64_t uwb_window_start_ms = -1;
    bool preamble_detected = false;
    bool focused_logs = stage1_anchor_focused_rx_logs_enabled();
    bool log_scan_window = false;
    uint32_t scan_debug_now_ms = 0u;
    int64_t focused_spin_deadline_ms = -1;
    bool handled_claim = false;
    bool deferred_mesh_rx_queued = false;
    bool route_waiting_active = false;
    bool relay_tx_active = false;
    bool mesh_rx_active = false;
    bool ch9_rx_conflict = false;
    bool uwb_radio_busy = false;
    uint32_t ch9_retry_ms = 0u;
    uint32_t mesh_rx_queue_depth = 0u;
    bool mesh_rx_response_busy = false;
    uint32_t blocked_retry_ms = anchor_uwb_scan_blocked_retry_ms();
    int ret;

    ARG_UNUSED(work);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_HANDLER role=%u\n", DEVICE_ROLE);
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_rx_queue_depth = mesh_rx_pending_count();
        mesh_rx_response_busy = mesh_rx_response_active();
        mesh_rx_active = mesh_rx_response_busy;
    }
    ch9_rx_conflict = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                      mesh_anchor_low_duty_scan_should_defer(&ch9_retry_ms);
    uwb_radio_busy = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                     radio_guard_uwb_busy();
    if (anchor_uwb_window_active() ||
        anchor_survey_discovery_is_pending() ||
        relay_tx_active ||
        route_waiting_active ||
        mesh_rx_active ||
        ch9_rx_conflict ||
        uwb_radio_busy) {
        uint32_t retry_ms = mesh_rx_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            uwb_radio_busy ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            ch9_rx_conflict ? ch9_retry_ms : blocked_retry_ms;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_BLOCKED win=%u survey=%u tx=%u wait=%u rx=%u rxq=%u rxr=%u ch9=%u rf=%u retry=%u interval=%u\n",
                                anchor_uwb_window_active() ? 1u : 0u,
                                anchor_survey_discovery_is_pending() ? 1u : 0u,
                                relay_tx_active ? 1u : 0u,
                                route_waiting_active ? 1u : 0u,
                                mesh_rx_active ? 1u : 0u,
                                mesh_rx_queue_depth,
                                mesh_rx_response_busy ? 1u : 0u,
                                ch9_rx_conflict ? 1u : 0u,
                                uwb_radio_busy ? 1u : 0u,
                                retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(retry_ms);
        return;
    }

    ret = radio_guard_uwb_start("anchor low-duty UWB wake scan");
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret == -EBUSY) {
            blocked_retry_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_GUARD_FAIL ret=%d retry=%u interval=%u\n",
                                ret,
                                blocked_retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(blocked_retry_ms);
        return;
    }
    anchor_set_uwb_busy(true);
    anchor_click_window_set_active(false);
    uwb_window_start_ms = k_uptime_get();
    if (focused_logs) {
        focused_spin_deadline_ms = uwb_window_start_ms + ANCHOR_STAGE1_FOCUSED_RX_SPIN_MS;
    }

focused_scan_attempt:
    frame_len = 0u;
    quality = 0u;
    retained_sleep_us = 0u;
    rx_failure = DWM3000_RX_FAILURE_NONE;
    preamble_detected = false;
    next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    stage1_led_phase(STAGE1_LED_PHASE_SCAN);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    if (!focused_logs) {
        high_debug_log_event("UWB_RX_START",
                             "mode=anchor_wake_scan interval_ms=%u rx_window_ms=%u",
                             anchor_uwb_scan_interval_ms,
                             ANCHOR_UWB_SCAN_RX_MS);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        scan_debug_now_ms = k_uptime_get_32();
        log_scan_window = anchor_ch5_scan_debug_next_ms == 0u ||
                          uptime_deadline_reached(scan_debug_now_ms,
                                                  anchor_ch5_scan_debug_next_ms);
        if (log_scan_window) {
            anchor_ch5_scan_debug_next_ms =
                scan_debug_now_ms + ANCHOR_CH5_SCAN_DEBUG_INTERVAL_MS;
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_ARM now=%u win=%u interval=%u wait=%u tx=%u\n",
                                scan_debug_now_ms,
                                ANCHOR_UWB_SCAN_RX_MS,
                                anchor_uwb_scan_interval_ms,
                                mesh_route_waiting_tx_active() ? 1u : 0u,
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u);
        }
    }

    ret = dwm3000_driver_configure_wake_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret != 0) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_CONFIG_FAIL ret=%d\n", ret);
    }
    if (ret == 0) {
        ret = dwm3000_driver_receive_frame_continuous(ANCHOR_UWB_SCAN_RX_MS,
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
    }
    preamble_detected = ret == 0 || rx_failure_detected_preamble(rx_failure);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (log_scan_window || ret == 0 || ret != -ETIMEDOUT ||
         preamble_detected)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_DONE ret=%d len=%u fail=%u pre=%u q=%u\n",
                            ret,
                            (unsigned int)frame_len,
                            (unsigned int)rx_failure,
                            preamble_detected ? 1u : 0u,
                            quality);
    }
    uwb_anchor_note_idle_scan(&anchor_uwb_session,
                              ANCHOR_UWB_STARTUP_US,
                              ANCHOR_UWB_PLL_US,
                              ANCHOR_UWB_SCAN_RX_US,
                              preamble_detected);

    if (ret == 0) {
        struct uwb_wake_claim_frame claim;
        int decode_ret;

        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("UWB_RX_DONE",
                             "mode=anchor_wake_scan frame_len=%u quality=%u rx_failure=%s",
                             (unsigned int)frame_len,
                             quality,
                             rx_failure_name(rx_failure));
        decode_ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_FRAME len=%u q=%u wake_ret=%d\n",
                                (unsigned int)frame_len,
                                quality,
                                decode_ret);
        }
        stage1_anchor_focused_note_rx_frame(frame_len, quality, decode_ret);
        if (decode_ret == PROTO_OK) {
            stage1_anchor_focused_note_wake_claim(&claim);
            if (anchor_handle_uwb_claim(&claim,
                                        quality,
                                        k_uptime_get(),
                                        &retained_sleep_us,
                                        &deferred_mesh_rx_queued)) {
                handled_claim = true;
                uwb_anchor_abort_epoch(&anchor_uwb_session);
            }
        } else {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued || valid_mesh_frame) {
                if (queued) {
                    deferred_mesh_rx_queued = true;
                }
                goto scan_complete;
            }
            {
                enum uwb_wake_decode_failure failure = wake_failure_from_proto_ret(decode_ret);

                uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                    failure);
#if IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
                next_scan_delay_ms = 0u;
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor high-duty UWB wake ignored bad frame: wake_decode_ret=%d reason=%s frame_len=%u quality=%u retry_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
#else
                uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
                next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor false UWB wake cooldown: wake_decode_ret=%d reason=%s frame_len=%u quality=%u cooldown_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
#endif
            }
        }
    } else if (ret != -ETIMEDOUT) {
        enum uwb_wake_decode_failure failure = wake_failure_from_rx(rx_failure);

        uwb_anchor_note_wake_decode_failure(&anchor_uwb_session, failure);
#if IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
        next_scan_delay_ms = 0u;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        if (!focused_logs) {
            LOG_WRN("anchor high-duty UWB wake scan retry after activity: ret=%d rx_failure=%s reason=%s retry_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                    ret,
                    rx_failure_name(rx_failure),
                    wake_decode_failure_name(failure),
                    next_scan_delay_ms,
                    anchor_uwb_session.diagnostics.preambles,
                    anchor_uwb_session.diagnostics.sfd_timeouts,
                    anchor_uwb_session.diagnostics.frame_timeouts,
                    anchor_uwb_session.diagnostics.crc_failures);
        }
#else
        uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
        next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        LOG_WRN("anchor UWB wake scan failure cooldown: ret=%d rx_failure=%s reason=%s cooldown_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                ret,
                rx_failure_name(rx_failure),
                wake_decode_failure_name(failure),
                next_scan_delay_ms,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures);
#endif
    }
    if (ret == -ETIMEDOUT) {
        stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
        if (!focused_logs) {
            high_debug_log_event("UWB_RX_TIMEOUT",
                                 "mode=anchor_wake_scan rx_failure=%s preamble=%u",
                                 rx_failure_name(rx_failure),
                                 preamble_detected ? 1u : 0u);
        }
    }

    if (focused_logs &&
        ret != 0 &&
        focused_spin_deadline_ms > 0 &&
        k_uptime_get() < focused_spin_deadline_ms &&
        !anchor_uwb_window_active() &&
        !anchor_survey_discovery_is_pending() &&
        !mesh_relay_tx_active(&mesh_runtime)) {
        goto focused_scan_attempt;
    }

scan_complete:
    (void)dwm3000_driver_standby();
    anchor_note_uwb_awake_since(
        uwb_window_start_ms,
        u32_saturating_add(ANCHOR_UWB_IDLE_SCAN_AWAKE_US, retained_sleep_us));
    radio_guard_uwb_stop();
    anchor_set_uwb_busy(false);
    anchor_click_window_set_active(false);
    if (deferred_mesh_rx_queued &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS;
    }
    if (deferred_mesh_rx_queued) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_MESH_RX_SUBMIT_DEFERRED delay=%u\n",
                                next_scan_delay_ms);
        }
        if (!mesh_process_queued_rx_now("anchor-post-ch5")) {
            mesh_submit_queued_rx();
        }
    }
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    report_tx_schedule(0u);
#endif
    if (handled_claim &&
        anchor_scan_recovery_gap_requested &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS;
    }
    anchor_scan_recovery_gap_requested = false;
    if (focused_logs) {
        stage1_anchor_focused_log_diagnostics(ret,
                                              rx_failure_name(rx_failure),
                                              preamble_detected,
                                              &anchor_uwb_session);
    } else {
        LOG_DBG("anchor UWB diagnostics: last_ret=%d last_rx_failure=%s last_preamble=%u scans=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u claims=%u collisions=%u wins=%u losses=%u replies=%u schedules=%u ds_ok=%u ds_fail=%u timing_rejections=%u mesh_packets=%u sample_order=%u scan_startup_us=%u scan_pll_us=%u scan_rx_us=%u awake_us=%u false_cooldowns=%u",
                ret,
                rx_failure_name(rx_failure),
                preamble_detected ? 1u : 0u,
                anchor_uwb_session.diagnostics.scans,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures,
                anchor_uwb_session.diagnostics.claims,
                anchor_uwb_session.diagnostics.collisions,
                anchor_uwb_session.diagnostics.arbitration_wins,
                anchor_uwb_session.diagnostics.arbitration_losses,
                anchor_uwb_session.diagnostics.discovery_replies,
                anchor_uwb_session.diagnostics.schedules,
                anchor_uwb_session.diagnostics.ds_twr_successes,
                anchor_uwb_session.diagnostics.ds_twr_failures,
                anchor_uwb_session.diagnostics.timing_rejections,
                anchor_uwb_session.diagnostics.uwb_mesh_packets,
                anchor_uwb_session.diagnostics.sample_order_count,
                anchor_uwb_session.diagnostics.scan_startup_time_us,
                anchor_uwb_session.diagnostics.scan_pll_time_us,
                anchor_uwb_session.diagnostics.scan_rx_time_us,
                anchor_uwb_session.diagnostics.awake_time_us,
                anchor_uwb_session.diagnostics.false_wake_cooldowns);
        high_debug_log_event("UWB_SLEEP",
                             "mode=anchor_wake_scan next_delay_ms=%u retained_sleep_us=%u",
                             next_scan_delay_ms,
                             retained_sleep_us);
    }
    anchor_uwb_scan_schedule_ms(next_scan_delay_ms);
}

static int anchor_start_uwb_scan(void)
{
    uint32_t startup_delay_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ? 2000u : 0u;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        LOG_INF("mesh-test source transmitter skipping anchor low-duty UWB scan");
        status_debug_note("DBG_ANCHOR_SCAN_SKIPPED_TX\n");
        return 0;
    }

    anchor_uwb_scan_schedule_ms(startup_delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_SCHEDULED startup=%u interval=%u win=%u\n",
                            startup_delay_ms,
                            anchor_uwb_scan_interval_ms,
                            ANCHOR_UWB_SCAN_RX_MS);
    }
    LOG_INF("anchor low-duty UWB wake scan scheduled: startup_delay_ms=%u interval_ms=%u rx_window_ms=%u hash_slots=%u",
            startup_delay_ms,
            anchor_uwb_scan_interval_ms,
            ANCHOR_UWB_SCAN_RX_MS,
            UWB_DISCOVERY_SLOT_COUNT);
    return 0;
}


static const struct app_mesh_report_callbacks anchor_mesh_report_callbacks = {
    .anchor_survey_discovery_is_pending = anchor_survey_discovery_is_pending,
    .anchor_note_uwb_awake_since = anchor_note_uwb_awake_since,
    .anchor_handle_local_command = anchor_handle_local_command,
    .anchor_handle_survey_discovery_start = anchor_handle_survey_discovery_start,
    .anchor_handle_survey_pair_prepare = anchor_handle_survey_pair_prepare,
    .gateway_handle_survey_discovery_report = gateway_handle_survey_discovery_report,
};

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void)
{
    return &anchor_mesh_report_callbacks;
}

int app_anchor_init(void)
{
    k_work_init_delayable(&gateway_survey_work, gateway_survey_work_handler);
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    return 0;
}

int app_anchor_start_anchor_role(void)
{
    const struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = DEVICE_ID,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
    };
    int ret;

    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_ANCHOR,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
    ret = app_mesh_persistence_restore_outbox(&mesh_runtime, k_uptime_get_32());
    if (ret < 0) {
        LOG_WRN("anchor mesh outbox restore unavailable: %d", ret);
    }
    mesh_report_resume_restored_outbox("anchor-startup");
    ret = app_mesh_persistence_restore_child_custody(&mesh_runtime,
                                                    k_uptime_get_32());
    if (ret < 0) {
        LOG_WRN("anchor mesh child custody restore unavailable: %d", ret);
    }
    ret = uwb_anchor_session_init(&anchor_uwb_session, &anchor_config);
    if (ret < 0) {
        LOG_ERR("anchor UWB session init failed: %d", ret);
        return ret;
    }
    k_work_init_delayable(&anchor_uwb_scan_work, anchor_uwb_scan_work_handler);
    k_work_init_delayable(&anchor_heartbeat_work, anchor_heartbeat_work_handler);
    k_work_init_delayable(&anchor_reboot_work, anchor_reboot_work_handler);
    k_work_init_delayable(&anchor_survey_work, anchor_survey_work_handler);
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    if (mesh_relay_tx_active(&mesh_runtime)) {
        app_mesh_persistence_clear_collection_result();
    } else {
        ret = anchor_collection_result_restore();
        if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP) {
            LOG_WRN("anchor collection result restore unavailable: %d", ret);
        }
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (gateway_ble_transport_enabled()) {
        status_debug_note("DBG_ANCHOR_BLE_INIT_BEGIN\n");
        ret = gateway_ble_init();
        status_debug_note("DBG_ANCHOR_BLE_INIT_DONE\n");
        if (ret < 0) {
            LOG_ERR("mesh-test anchor BLE debug log link unavailable: %d", ret);
        } else {
            LOG_INF("mesh-test anchor BLE debug log link ready before UWB scan");
        }
    } else {
        status_debug_note("DBG_ANCHOR_BLE_DISABLED\n");
    }
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
    (void)app_ml_init();
#endif
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_queue_start(&anchor_uwb_scan_work_q,
                       anchor_uwb_scan_work_q_stack,
                       K_THREAD_STACK_SIZEOF(anchor_uwb_scan_work_q_stack),
                       ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
                       &anchor_uwb_scan_work_q_config);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_ANCHOR_CH5_SCAN_WQ_STARTED\n");
    }
#endif
    k_work_queue_start(&anchor_survey_work_q,
                       anchor_survey_work_q_stack,
                       K_THREAD_STACK_SIZEOF(anchor_survey_work_q_stack),
                       ANCHOR_SURVEY_WORKQUEUE_PRIORITY,
                       &anchor_survey_work_q_config);
    ret = anchor_start_uwb_scan();
    if (ret < 0) {
        LOG_ERR("anchor UWB scan unavailable: %d", ret);
    }
    return 0;
}

int app_anchor_start_gateway_role(void)
{
    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_GATEWAY,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
    return 0;
}
