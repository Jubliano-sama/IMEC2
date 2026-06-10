#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "debug_log.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "serial_frame.h"
#include "status.h"
#include "survey.h"
#include "uwb.h"
#include "uwb_ble_courtesy.h"
#include "uwb_session.h"

#if defined(CONFIG_BT)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/net_buf.h>
#if defined(CONFIG_BT_LL_SOFTDEVICE_HEADERS_INCLUDE)
#include <bluetooth/hci_vs_sdc.h>
#endif
#endif

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#if defined(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/usb_device.h>
#endif

#include <errno.h>
#include <stdarg.h>
#include <string.h>

LOG_MODULE_REGISTER(uwb_app, LOG_LEVEL_DBG);

#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif

#ifndef DEVICE_ID
#define DEVICE_ID 0x1111222233334444ull
#endif

#ifndef GATEWAY_ID
#define GATEWAY_ID 0x9999888877776666ull
#endif

#ifndef NETWORK_ID
#define NETWORK_ID 0x494D4543u
#endif

#ifndef IMEC_BUILD_PRESET_NAME
#define IMEC_BUILD_PRESET_NAME ""
#endif

#ifndef IMEC_GIT_VERSION
#define IMEC_GIT_VERSION "unknown"
#endif

#ifndef IMEC_BUILD_TIMESTAMP
#define IMEC_BUILD_TIMESTAMP "unknown"
#endif

#ifndef IMEC_BOARD_TARGET
#define IMEC_BOARD_TARGET "unknown"
#endif

#if defined(CONFIG_IMEC_HIGH_DEBUG)
#define IMEC_STAGE CONFIG_IMEC_BENCH_STAGE
#else
#define IMEC_STAGE (-1)
#endif

#define CLICK_BUTTON_NODE DT_ALIAS(click_button)
#define STATUS0_RED_NODE DT_ALIAS(status0_red)
#define STATUS0_GREEN_NODE DT_ALIAS(status0_green)
#define STATUS0_BLUE_NODE DT_ALIAS(status0_blue)
#define STATUS1_RED_NODE DT_ALIAS(status1_red)
#define STATUS1_GREEN_NODE DT_ALIAS(status1_green)
#define STATUS1_BLUE_NODE DT_ALIAS(status1_blue)
#define USB_CONSOLE_NODE DT_CHOSEN(zephyr_console)

#if DT_NODE_HAS_STATUS(CLICK_BUTTON_NODE, okay)
static const struct gpio_dt_spec click_button = GPIO_DT_SPEC_GET(CLICK_BUTTON_NODE, gpios);
static struct gpio_callback click_button_cb;
static struct k_work click_button_work;
static struct k_work_delayable self_test_arm_timeout_work;
#define HAS_CLICK_BUTTON 1
#else
#define HAS_CLICK_BUTTON 0
#endif

#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
static const struct gpio_dt_spec status0_red = GPIO_DT_SPEC_GET(STATUS0_RED_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
static const struct gpio_dt_spec status0_green = GPIO_DT_SPEC_GET(STATUS0_GREEN_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
static const struct gpio_dt_spec status0_blue = GPIO_DT_SPEC_GET(STATUS0_BLUE_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
static const struct gpio_dt_spec status1_red = GPIO_DT_SPEC_GET(STATUS1_RED_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
static const struct gpio_dt_spec status1_green = GPIO_DT_SPEC_GET(STATUS1_GREEN_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
static const struct gpio_dt_spec status1_blue = GPIO_DT_SPEC_GET(STATUS1_BLUE_NODE, gpios);
#endif

#if DT_NODE_HAS_STATUS(USB_CONSOLE_NODE, okay)
static const struct device *serial_console = DEVICE_DT_GET(USB_CONSOLE_NODE);
#define HAS_SERIAL_CONSOLE 1
#else
#define HAS_SERIAL_CONSOLE 0
#endif

static struct button_fsm button_fsm;
static uint32_t next_event_seq;
static bool uwb_rf_active;
static struct k_spinlock uwb_rf_lock;
static struct k_work_delayable anchor_uwb_scan_work;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_uwb_rx_work;
static struct k_work_delayable mesh_tx_timeout_work;
static struct k_work_delayable report_tx_work;
static struct k_work_delayable anchor_heartbeat_work;
static struct k_work_delayable anchor_reboot_work;
static struct k_work_delayable gateway_serial_rx_work;
#if defined(CONFIG_IMEC_HIGH_DEBUG)
static struct k_work_delayable high_debug_serial_work;
static struct k_work_delayable high_debug_counter_work;
#endif
static struct k_work_delayable gateway_command_result_timeout_work;
static struct k_work_delayable gateway_time_sync_work;
static struct k_spinlock anchor_uwb_lock;
static struct k_spinlock anchor_time_sync_lock;
static bool anchor_uwb_busy;
static bool mesh_uwb_rx_active;
static bool anchor_heartbeat_enabled;
static struct mesh_relay mesh_runtime;
static struct mesh_event_diagnostics mesh_event_stats;
static struct uwb_anchor_session anchor_uwb_session;
static uint16_t mesh_event_control_seq;

#if defined(CONFIG_IMEC_HIGH_DEBUG)
struct high_debug_counters {
    uint32_t boot_count;
    uint32_t dwm_dev_id_successes;
    uint32_t dwm_dev_id_failures;
    uint32_t wake_claim_tx;
    uint32_t wake_claim_rx;
    uint32_t wake_claim_accepted;
    uint32_t wake_claim_rejected;
    uint32_t discovery_tx;
    uint32_t discovery_rx;
    uint32_t discovery_reply_tx;
    uint32_t discovery_reply_rx;
    uint32_t schedules_tx;
    uint32_t schedules_rx;
    uint32_t schedules_accepted;
    uint32_t schedules_rejected;
    uint32_t ds_twr_attempts;
    uint32_t ds_twr_successes;
    uint32_t ds_twr_failures;
    uint32_t ds_twr_timing_rejects;
    uint32_t mesh_tx;
    uint32_t mesh_rx;
    uint32_t mesh_ack;
    uint32_t mesh_retry;
    uint32_t mesh_drop;
    uint32_t gateway_packets_emitted;
    uint32_t usb_connects;
    uint32_t usb_disconnects;
    uint32_t usb_resets;
    uint32_t bootloader_entry_requests;
    uint32_t command_rx;
    uint32_t command_result_tx;
};

static struct high_debug_counters high_debug_counters;
static char high_debug_command_buf[80];
static size_t high_debug_command_len;
#define HIGH_DEBUG_COUNTER_INC(field) do { high_debug_counters.field++; } while (0)
#else
#define HIGH_DEBUG_COUNTER_INC(field) do { } while (0)
#endif

#define MAX_SCHEDULED_ANCHORS UWB_RANGE_SCHEDULE_MAX_ANCHORS
#define MAX_SUCCESSFUL_ANCHORS 16u
#define WAKE_ADV_MS 430u
#define ANCHOR_UWB_WAIT_MS 500u
#define ANCHOR_UWB_SCAN_INTERVAL_MS 400u
#define ANCHOR_UWB_STARTUP_US 2500u
#define ANCHOR_UWB_PLL_US 170u
#define ANCHOR_UWB_SCAN_RX_US 1000u
#define ANCHOR_UWB_SCAN_RX_MS 1u
#define ANCHOR_UWB_IDLE_BUDGET_US_PER_S 10000u
#define ANCHOR_UWB_IDLE_SCAN_AWAKE_US \
    (ANCHOR_UWB_STARTUP_US + ANCHOR_UWB_PLL_US + ANCHOR_UWB_SCAN_RX_US)
#define ANCHOR_UWB_IDLE_SCAN_PERIOD_US \
    ((ANCHOR_UWB_SCAN_INTERVAL_MS * 1000u) + ANCHOR_UWB_IDLE_SCAN_AWAKE_US)
#define ANCHOR_UWB_SCAN_COMMAND_ABSOLUTE_MAX_INTERVAL_MS 60000u
#define ANCHOR_UWB_SCAN_WAKE_OVERLAP_MAX_INTERVAL_MS \
    ((((uint64_t)WAKE_ADV_MS * 1000ull) - ANCHOR_UWB_IDLE_SCAN_AWAKE_US - 1ull) / \
     1000ull)
#define ANCHOR_UWB_SCAN_MAX_INTERVAL_MS \
    (ANCHOR_UWB_SCAN_WAKE_OVERLAP_MAX_INTERVAL_MS < \
     ANCHOR_UWB_SCAN_COMMAND_ABSOLUTE_MAX_INTERVAL_MS ? \
     ANCHOR_UWB_SCAN_WAKE_OVERLAP_MAX_INTERVAL_MS : \
     ANCHOR_UWB_SCAN_COMMAND_ABSOLUTE_MAX_INTERVAL_MS)
#define ANCHOR_CLAIM_COLLECTION_MS 15u
#define ANCHOR_FALSE_WAKE_COOLDOWN_MS 100u
#define UWB_DISCOVERY_SLOT_US 1000u
#define UWB_DISCOVERY_RX_GUARD_MS 8u
#define UWB_DISCOVERY_LISTEN_MS (UWB_DISCOVERY_RX_GUARD_MS * 2u)
#define UWB_DISCOVERY_REPLY_TX_TIMEOUT_MS 20u
#define UWB_CONTROL_TX_TIMEOUT_MS 20u
#define UWB_RANGE_SCHEDULE_RX_MS 80u
#define UWB_SCHEDULE_GUARD_MS 10u
#define UWB_WAKE_CHANNEL UWB_CHANNEL_WAKE_CONTACT
#define UWB_RANGING_CHANNEL UWB_CHANNEL_WAKE_CONTACT
#define UWB_RANGE_FIRST_POLL_DELAY_MS 5u
#define UWB_SAMPLES_PER_ANCHOR 2u
#define UWB_DISCOVERY_WINDOW_MS \
    (((UWB_DISCOVERY_SLOT_COUNT * UWB_DISCOVERY_SLOT_US) + 999u) / 1000u)
#define UWB_CLICKER_MAX_SAMPLES_PER_ANCHOR UWB_RANGING_REQUESTS_MAX_PER_ANCHOR
#define UWB_SCHEDULED_RANGE_SPAN_MS \
    (UWB_RANGE_FIRST_POLL_DELAY_MS + UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS + \
     UWB_SCHEDULE_GUARD_MS)
#define UWB_POST_WAKE_CLAIMED_DURATION_MS \
    (UWB_DISCOVERY_WINDOW_MS + UWB_RANGE_SCHEDULE_RX_MS + \
     UWB_SCHEDULED_RANGE_SPAN_MS)
#define UWB_CLICKER_CLAIMED_DURATION_MS \
    (WAKE_ADV_MS + UWB_POST_WAKE_CLAIMED_DURATION_MS)
#ifndef UWB_ANCHOR_SLOT
#define UWB_ANCHOR_SLOT 0xffu
#endif
#define MAX_WAKE_ATTEMPTS 6u
#define UWB_ANCHOR_RANGE_WINDOW_MS UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS
#define UWB_POLITE_SAMPLE_RX_MS 2u
#define UWB_POLITE_SAMPLE_PERIOD_MS 25u
#define UWB_POLITE_REQUIRED_QUIET_SAMPLES 2u
#define UWB_POLITE_RELEVANT_FRAME_WAIT_MS UWB_POST_WAKE_CLAIMED_DURATION_MS
#define MAX_POLITENESS_WAIT_MS 500u
#define UWB_RETRY_BASE_DELAY_MS 150u
#define CLICKER_POLITENESS_UWB_RESTART 1
#define BLE_COURTESY_ADV_INTERVAL_MIN_UNITS 0x0020u
#define BLE_COURTESY_ADV_INTERVAL_MAX_UNITS 0x0021u
#define BLE_COURTESY_SCAN_INTERVAL_UNITS 40u
#define BLE_COURTESY_SCAN_WINDOW_UNITS 32u
#define BLE_COURTESY_MIN_WINDOW_MS 75u
#define BLE_COURTESY_PEER_FINISH_MS \
    (BLE_COURTESY_MIN_WINDOW_MS + UWB_CLICKER_CLAIMED_DURATION_MS)
#define BLE_COURTESY_MAX_DEFERS_PER_ATTEMPT 3u
#define BLE_COURTESY_POLL_SLEEP_MS 5u
#define CLICK_REPORT_BUILD_GUARD_MS 20u
#define CLICK_UWB_TIMEOUT_MS 40u
#define CLICK_REPORT_DEADLINE_MS 15000u
#define UWB_MESH_ANCHOR_RX_INTERVAL_MS 6000u
#define UWB_MESH_ANCHOR_RX_WINDOW_MS 2u
#define UWB_MESH_ANCHOR_RX_AWAKE_US \
    (ANCHOR_UWB_STARTUP_US + ANCHOR_UWB_PLL_US + \
     (UWB_MESH_ANCHOR_RX_WINDOW_MS * 1000u))
#define UWB_MESH_ANCHOR_RX_PERIOD_US \
    ((UWB_MESH_ANCHOR_RX_INTERVAL_MS * 1000u) + UWB_MESH_ANCHOR_RX_AWAKE_US)
#define UWB_MESH_ANCHOR_RX_US_PER_S \
    (((uint64_t)UWB_MESH_ANCHOR_RX_AWAKE_US * 1000000ull + \
      UWB_MESH_ANCHOR_RX_PERIOD_US - 1ull) / UWB_MESH_ANCHOR_RX_PERIOD_US)
#define ANCHOR_UWB_SCAN_BUDGET_US_PER_S \
    (ANCHOR_UWB_IDLE_BUDGET_US_PER_S - UWB_MESH_ANCHOR_RX_US_PER_S)
#define ANCHOR_UWB_SCAN_ONLY_MIN_INTERVAL_MS \
    ((((uint64_t)ANCHOR_UWB_IDLE_SCAN_AWAKE_US * 1000000ull + \
       ANCHOR_UWB_IDLE_BUDGET_US_PER_S - 1ull) / \
      ANCHOR_UWB_IDLE_BUDGET_US_PER_S - \
      ANCHOR_UWB_IDLE_SCAN_AWAKE_US + 999ull) / 1000ull)
#define ANCHOR_UWB_SCAN_COMBINED_MIN_INTERVAL_MS \
    ((((uint64_t)ANCHOR_UWB_IDLE_SCAN_AWAKE_US * 1000000ull + \
       ANCHOR_UWB_SCAN_BUDGET_US_PER_S - 1ull) / \
      ANCHOR_UWB_SCAN_BUDGET_US_PER_S - \
      ANCHOR_UWB_IDLE_SCAN_AWAKE_US + 999ull) / 1000ull)
#define ANCHOR_UWB_SCAN_MIN_INTERVAL_MS \
    (ANCHOR_UWB_SCAN_COMBINED_MIN_INTERVAL_MS > \
     ANCHOR_UWB_SCAN_ONLY_MIN_INTERVAL_MS ? \
     ANCHOR_UWB_SCAN_COMBINED_MIN_INTERVAL_MS : \
     ANCHOR_UWB_SCAN_ONLY_MIN_INTERVAL_MS)
#define ANCHOR_UWB_SCAN_US_PER_S \
    (((uint64_t)ANCHOR_UWB_IDLE_SCAN_AWAKE_US * 1000000ull + \
      ANCHOR_UWB_IDLE_SCAN_PERIOD_US - 1ull) / ANCHOR_UWB_IDLE_SCAN_PERIOD_US)
#define ANCHOR_UWB_PERIODIC_IDLE_US_PER_S \
    (ANCHOR_UWB_SCAN_US_PER_S + UWB_MESH_ANCHOR_RX_US_PER_S)
#define UWB_MESH_GATEWAY_RX_WINDOW_MS 50u
#define UWB_MESH_GATEWAY_RX_IDLE_MS 2u
#define MESH_EVENT_DEFAULT_INTERVAL_MS 80u
#define MESH_EVENT_DEFAULT_WINDOW_MS 12u
#define MESH_EVENT_DEFAULT_FIRST_DELAY_MS 20u
#define MESH_EVENT_DEFAULT_GUARD_MS UWB_SCHEDULE_GUARD_MS
#define MESH_EVENT_DEFAULT_MAX_MISSED 3u
#define MESH_EVENT_DEFAULT_SUPERVISION_MS 1000u
#define GATEWAY_SERIAL_POLL_MS 10u
#define GATEWAY_SERIAL_MAX_BYTES_PER_POLL 64u
#define MESH_RX_QUEUE_DEPTH 8
#define REPORT_TX_QUEUE_DEPTH 16
#define ANCHOR_BATTERY_MV_UNKNOWN 0u
#define ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS 60000u
#define ANCHOR_HEARTBEAT_MIN_INTERVAL_MS 5000u
#define ANCHOR_HEARTBEAT_MAX_INTERVAL_MS 3600000u
#define ANCHOR_REBOOT_RESULT_DRAIN_MS 100u
#define ANCHOR_REBOOT_RESULT_POLL_MS 50u
#define ANCHOR_REBOOT_RESULT_MAX_WAIT_MS (GATEWAY_COMMAND_RESULT_TIMEOUT_MS + 500u)
#define SURVEY_REACH_MAX_ENTRIES 12u
#define SURVEY_PAIR_INITIATOR_TIMEOUT_MS 150u
#define SURVEY_PAIR_RESPONDER_WINDOW_MS 500u
#define SURVEY_PAIR_SAMPLE_GAP_MS 10u
#define ANCHOR_SURVEY_WORKQUEUE_STACK_SIZE 4096u
#define ANCHOR_SURVEY_WORKQUEUE_PRIORITY K_LOWEST_APPLICATION_THREAD_PRIO
#define GATEWAY_SURVEY_AUTO_RETRY_MS 100u
#define GATEWAY_SURVEY_PAIR_SETTLE_MS 50u
#define GATEWAY_COMMAND_MESH_TIMEOUT_MARGIN_MS 1000u
#define TIME_SYNC_MAX_DRIFT_MS 60000u
#define TIME_SYNC_WORST_CASE_DRIFT_PPM 500u
#define GATEWAY_TIME_SYNC_MAX_INTERVAL_MS \
    (((uint64_t)TIME_SYNC_MAX_DRIFT_MS * 1000000ull) / \
     TIME_SYNC_WORST_CASE_DRIFT_PPM)
#define GATEWAY_TIME_SYNC_DEFAULT_INTERVAL_MS 3600000u
#define GATEWAY_TIME_SYNC_INITIAL_DELAY_MS 5000u
#define GATEWAY_TIME_SYNC_RETRY_MS 5000u

BUILD_ASSERT(ANCHOR_UWB_SCAN_RX_MS * 1000u >= ANCHOR_UWB_SCAN_RX_US,
             "anchor scan millisecond timeout must cover configured RX microseconds");
BUILD_ASSERT(UWB_MESH_ANCHOR_RX_US_PER_S < ANCHOR_UWB_IDLE_BUDGET_US_PER_S,
             "periodic anchor UWB mesh RX must leave idle duty budget for wake scans");
BUILD_ASSERT(ANCHOR_UWB_SCAN_INTERVAL_MS >= ANCHOR_UWB_SCAN_MIN_INTERVAL_MS,
             "anchor wake scan interval must leave duty budget for periodic mesh RX");
BUILD_ASSERT(ANCHOR_UWB_PERIODIC_IDLE_US_PER_S <= ANCHOR_UWB_IDLE_BUDGET_US_PER_S,
             "anchor periodic UWB scan plus mesh RX must stay at or below 1 percent");
BUILD_ASSERT(ANCHOR_UWB_SCAN_MIN_INTERVAL_MS <= ANCHOR_UWB_SCAN_MAX_INTERVAL_MS,
             "anchor scan duty command range must satisfy duty and wake overlap limits");
BUILD_ASSERT(((uint64_t)ANCHOR_UWB_SCAN_MAX_INTERVAL_MS * 1000ull) +
             ANCHOR_UWB_IDLE_SCAN_AWAKE_US < ((uint64_t)WAKE_ADV_MS * 1000ull),
             "maximum anchor scan duty command interval must preserve wake overlap");
BUILD_ASSERT(WAKE_ADV_MS * 1000u > ANCHOR_UWB_IDLE_SCAN_PERIOD_US,
             "clicker wake train must cover at least one complete anchor scan period");
BUILD_ASSERT(WAKE_ADV_MS * 1000u > ANCHOR_UWB_IDLE_SCAN_AWAKE_US,
             "clicker wake train must exceed one anchor scan awake window");
BUILD_ASSERT(WAKE_ADV_MS <= UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS,
             "wake claim timing bounds must cover the configured clicker wake train");
BUILD_ASSERT(UWB_CLICKER_CLAIMED_DURATION_MS <=
             UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS,
             "wake claim timing bounds must cover the configured advertised click epoch");
BUILD_ASSERT(UWB_POLITE_RELEVANT_FRAME_WAIT_MS <=
             UWB_WAKE_CLAIM_MAX_CLAIMED_DURATION_MS,
             "decoded UWB politeness wait fallback must stay bounded");
BUILD_ASSERT(UWB_POLITE_REQUIRED_QUIET_SAMPLES == 2u,
             "BLE courtesy edge sampling expects one quiet UWB sample at each window edge");
BUILD_ASSERT((2u * UWB_POLITE_SAMPLE_RX_MS) <= BLE_COURTESY_MIN_WINDOW_MS,
             "BLE courtesy window must fit begin and end UWB politeness samples");
BUILD_ASSERT(UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN + 2u <= 31u,
             "BLE courtesy must fit in one legacy advertising data structure");
BUILD_ASSERT(BLE_COURTESY_PEER_FINISH_MS <= UWB_BLE_COURTESY_MAX_DURATION_MS,
             "BLE courtesy peer finish duration must fit in the advertised field");
BUILD_ASSERT(BLE_COURTESY_SCAN_WINDOW_UNITS <= BLE_COURTESY_SCAN_INTERVAL_UNITS,
             "BLE courtesy scan window must fit inside scan interval");
BUILD_ASSERT(CLICK_UWB_TIMEOUT_MS + UWB_SCHEDULE_GUARD_MS <= UWB_ANCHOR_RANGE_WINDOW_MS,
             "failed clicker DS-TWR response waits must fit inside one scheduled anchor slot");
BUILD_ASSERT(GATEWAY_COMMAND_RESULT_TIMEOUT_MS >=
             (UWB_MESH_ANCHOR_RX_INTERVAL_MS + ROUTE_GATEWAY_ACK_TIMEOUT_MS +
              GATEWAY_COMMAND_MESH_TIMEOUT_MARGIN_MS),
             "gateway command timeout must cover anchor UWB mesh RX cadence and ACK");
BUILD_ASSERT(TIME_SYNC_WORST_CASE_DRIFT_PPM > 0u,
             "time sync drift calculation must have a nonzero ppm bound");
BUILD_ASSERT(GATEWAY_TIME_SYNC_DEFAULT_INTERVAL_MS <= GATEWAY_TIME_SYNC_MAX_INTERVAL_MS,
             "gateway time sync interval must keep drift under the configured limit");
#if defined(CONFIG_IMEC_HIGH_DEBUG)
BUILD_ASSERT(!(IS_ENABLED(CONFIG_IMEC_ROLE_TAG) || IS_ENABLED(CONFIG_IMEC_ROLE_CLICKER)) ||
             DEVICE_ROLE == ROLE_CLICKER,
             "tag/clicker high-debug role config must match DEVICE_ROLE");
BUILD_ASSERT(!IS_ENABLED(CONFIG_IMEC_ROLE_ANCHOR) || DEVICE_ROLE == ROLE_ANCHOR,
             "anchor high-debug role config must match DEVICE_ROLE");
BUILD_ASSERT(!IS_ENABLED(CONFIG_IMEC_ROLE_GATEWAY) || DEVICE_ROLE == ROLE_GATEWAY,
             "gateway high-debug role config must match DEVICE_ROLE");
#endif
#define REPORT_TX_RETRY_DELAY_MS 1000u
#define UWB_MESH_TX_TIMEOUT_MS 20u

struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t payload_len;
    uint64_t previous_hop_id;
    uint8_t link_quality;
};

K_MSGQ_DEFINE(mesh_rx_msgq, sizeof(struct mesh_rx_pending), MESH_RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(report_tx_msgq, sizeof(struct mesh_outbound), REPORT_TX_QUEUE_DEPTH, 4);
K_THREAD_STACK_DEFINE(anchor_survey_work_q_stack, ANCHOR_SURVEY_WORKQUEUE_STACK_SIZE);

static struct k_work_q anchor_survey_work_q;
static const struct k_work_queue_config anchor_survey_work_q_config = {
    .name = "anchor_survey",
};
static uint8_t gateway_serial_rx_frame[SERIAL_FRAME_MAX_LEN];
static size_t gateway_serial_rx_len;
static bool gateway_serial_rx_overflow;
static uint16_t gateway_command_seq;
static uint16_t anchor_heartbeat_seq;
static uint16_t anchor_survey_seq;
static uint32_t anchor_heartbeat_interval_ms = ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS;
static bool anchor_reboot_pending;
static uint32_t anchor_reboot_deadline_ms;
static struct gateway_command_pending gateway_command_pending_state;
static struct mesh_outbound mesh_route_waiting_tx;
static bool mesh_route_waiting_tx_valid;
static uint32_t anchor_uwb_scan_interval_ms = ANCHOR_UWB_SCAN_INTERVAL_MS;
static struct k_spinlock anchor_survey_lock;
static struct survey_pair anchor_survey_pair;
static bool anchor_survey_pair_prepared;
static bool anchor_survey_start_pending;
static bool anchor_survey_start_as_responder;
static bool anchor_survey_running;
static atomic_t anchor_survey_abort_requested;
static struct k_work_delayable anchor_survey_work;
static struct survey_gateway_context gateway_survey_context;
static bool gateway_survey_active;
static struct k_work_delayable gateway_survey_work;
static struct survey_gateway_auto_context gateway_survey_auto;

struct anchor_time_sync_state {
    int64_t gateway_offset_ms;
    int64_t synced_local_ms;
    uint64_t synced_gateway_ms;
    bool valid;
};

static struct anchor_time_sync_state anchor_time_sync;

static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason);
static int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason);
static int mesh_request_route(uint64_t target_id, const char *reason);
static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id);
static void mesh_clear_route_waiting_tx(const struct proto_packet *packet);
static void report_tx_schedule(uint32_t delay_ms);
static int queue_anchor_report(const struct mesh_outbound *outbound);
static bool anchor_uwb_window_active(void);
static void anchor_set_uwb_busy(bool busy);
static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us);
static void mesh_stop_role_scan(void);
static void mesh_restart_role_scan(void);
static void anchor_survey_work_handler(struct k_work *work);
static void anchor_survey_schedule(k_timeout_t delay);
static bool anchor_survey_pair_queueable(const struct survey_pair *pair);
static int anchor_start_survey_pair_from_command(const struct proto_packet *packet,
                                                 const uint8_t *payload,
                                                 size_t payload_len,
                                                 enum command_status *status,
                                                 uint8_t *reason);
static void anchor_abort_survey_pair(void);
static void anchor_reboot_work_handler(struct k_work *work);
static void gateway_survey_work_handler(struct k_work *work);
static void gateway_time_sync_work_handler(struct k_work *work);
static void gateway_survey_auto_note_command_result(const struct proto_packet *command,
                                                    enum command_id command_id,
                                                    enum command_status status,
                                                    uint8_t reason);
static void gateway_survey_auto_note_command_timeout(const struct proto_packet *command,
                                                     enum command_id command_id);
static void anchor_heartbeat_work_handler(struct k_work *work);
static bool mesh_queue_from_frame(const uint8_t *frame,
                                  size_t frame_len,
                                  uint8_t link_quality,
                                  bool *valid_mesh_frame,
                                  uint64_t *previous_hop_id);

static const char *role_name(void)
{
    switch (DEVICE_ROLE) {
    case ROLE_CLICKER:
        return "clicker";
    case ROLE_ANCHOR:
        return "anchor";
    case ROLE_GATEWAY:
        return "gateway";
    default:
        return "unknown";
    }
}

#if defined(CONFIG_IMEC_HIGH_DEBUG)
static const char *debug_role_name(void)
{
#if defined(CONFIG_IMEC_ROLE_TAG)
    if (IS_ENABLED(CONFIG_IMEC_ROLE_TAG)) {
        return "tag";
    }
#endif
    return role_name();
}
#endif

static void high_debug_log_event(const char *event, const char *fmt, ...)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    char prefix[96];
    char message[192];
    va_list args;
    int ret;

    if (event == NULL) {
        event = "UNKNOWN";
    }
    ret = debug_log_format_prefix(prefix,
                                  sizeof(prefix),
                                  k_uptime_get_32(),
                                  debug_role_name(),
                                  DEVICE_ID,
                                  CONFIG_IMEC_BENCH_STAGE,
                                  event);
    if (ret < 0) {
        LOG_WRN("high-debug prefix format failed for event=%s", event);
        return;
    }

    if (fmt == NULL || fmt[0] == '\0') {
        LOG_INF("%s", prefix);
        return;
    }

    va_start(args, fmt);
    ret = vsnprintk(message, sizeof(message), fmt, args);
    va_end(args);
    if (ret < 0) {
        LOG_WRN("%s message_format_failed", prefix);
        return;
    }
    LOG_INF("%s %s", prefix, message);
#else
    ARG_UNUSED(event);
    ARG_UNUSED(fmt);
#endif
}

static const char *command_status_name(enum command_status status)
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

static const char *claim_decision_name(enum uwb_anchor_claim_decision decision)
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

static bool high_debug_gateway_binary_cdc_active(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG) && defined(CONFIG_IMEC_GATEWAY_BINARY_CDC)
    return DEVICE_ROLE == ROLE_GATEWAY && IS_ENABLED(CONFIG_IMEC_GATEWAY_BINARY_CDC);
#else
    return false;
#endif
}

static bool gateway_binary_cdc_enabled(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        return IS_ENABLED(CONFIG_IMEC_GATEWAY_BINARY_CDC);
    }
#endif
    return true;
}

#if defined(CONFIG_IMEC_HIGH_DEBUG)
static bool high_debug_cdc_command_enabled(void)
{
    return !high_debug_gateway_binary_cdc_active();
}

static void high_debug_dump_counters(const char *event)
{
    struct dwm3000_driver_stats radio_stats = {0};

    dwm3000_driver_stats_get(&radio_stats);
    high_debug_log_event(event == NULL ? "COUNTERS" : event,
                         "boot=%u dev_id_ok=%u dev_id_fail=%u "
                         "sys_poll_loops=%u sys_poll_max_us=%u sys_poll_timeouts=%u "
                         "rx_start=%u rx_done=%u rx_timeout=%u rx_crc=%u rx_fail=%u "
                         "tx_start=%u tx_done=%u tx_fail=%u "
                         "wake_tx=%u wake_rx=%u wake_accept=%u wake_reject=%u "
                         "discover_tx=%u discover_rx=%u discovery_reply_tx=%u discovery_reply_rx=%u "
                         "schedule_tx=%u schedule_rx=%u schedule_accept=%u schedule_reject=%u "
                         "ds_attempt=%u ds_ok=%u ds_fail=%u ds_timing_reject=%u "
                         "mesh_tx=%u mesh_rx=%u mesh_ack=%u mesh_retry=%u mesh_drop=%u "
                         "gateway_packets=%u usb_connect=%u usb_disconnect=%u usb_reset=%u bootloader_req=%u command_rx=%u command_result_tx=%u",
                         high_debug_counters.boot_count,
                         high_debug_counters.dwm_dev_id_successes,
                         high_debug_counters.dwm_dev_id_failures,
                         radio_stats.sys_status_poll_loops,
                         radio_stats.sys_status_poll_max_duration_us,
                         radio_stats.sys_status_poll_timeouts,
                         radio_stats.rx_starts,
                         radio_stats.rx_dones,
                         radio_stats.rx_timeouts,
                         radio_stats.rx_crc_failures,
                         radio_stats.rx_failures,
                         radio_stats.tx_starts,
                         radio_stats.tx_dones,
                         radio_stats.tx_failures,
                         high_debug_counters.wake_claim_tx,
                         high_debug_counters.wake_claim_rx,
                         high_debug_counters.wake_claim_accepted,
                         high_debug_counters.wake_claim_rejected,
                         high_debug_counters.discovery_tx,
                         high_debug_counters.discovery_rx,
                         high_debug_counters.discovery_reply_tx,
                         high_debug_counters.discovery_reply_rx,
                         high_debug_counters.schedules_tx,
                         high_debug_counters.schedules_rx,
                         high_debug_counters.schedules_accepted,
                         high_debug_counters.schedules_rejected,
                         high_debug_counters.ds_twr_attempts,
                         high_debug_counters.ds_twr_successes,
                         high_debug_counters.ds_twr_failures,
                         high_debug_counters.ds_twr_timing_rejects,
                         high_debug_counters.mesh_tx,
                         high_debug_counters.mesh_rx,
                         high_debug_counters.mesh_ack,
                         high_debug_counters.mesh_retry,
                         high_debug_counters.mesh_drop,
                         high_debug_counters.gateway_packets_emitted,
                         high_debug_counters.usb_connects,
                         high_debug_counters.usb_disconnects,
                         high_debug_counters.usb_resets,
                         high_debug_counters.bootloader_entry_requests,
                         high_debug_counters.command_rx,
                         high_debug_counters.command_result_tx);
}

static void high_debug_boot_banner(void)
{
    high_debug_log_event("BOOT_START",
                         "preset=%s git=%s build_time=%s role=%s stage=%d board=%s",
                         IMEC_BUILD_PRESET_NAME[0] == '\0' ? "manual-highdebug" :
                         IMEC_BUILD_PRESET_NAME,
                         IMEC_GIT_VERSION,
                         IMEC_BUILD_TIMESTAMP,
                         debug_role_name(),
                         CONFIG_IMEC_BENCH_STAGE,
                         IMEC_BOARD_TARGET);
    high_debug_log_event("BOOT_CONFIG",
                         "device_id=0x%016llx gateway_id=0x%016llx network_id=0x%08x "
                         "uwb_channel=%u mesh_payload_channel=%u spi_hz=%u sys_status_polling=1 "
                         "usb_bootloader=%u usb_cdc_logs=%u rtt_logs=%u gateway_binary_cdc=%u",
                         (unsigned long long)DEVICE_ID,
                         (unsigned long long)GATEWAY_ID,
                         NETWORK_ID,
                         UWB_CHANNEL_WAKE_CONTACT,
                         UWB_CHANNEL_MESH_PAYLOAD,
                         (unsigned int)dwm3000_port_current_spi_hz(),
                         IS_ENABLED(CONFIG_IMEC_USB_BOOTLOADER) ? 1u : 0u,
                         IS_ENABLED(CONFIG_IMEC_USB_CDC_LOGS) ? 1u : 0u,
                         IS_ENABLED(CONFIG_IMEC_RTT_LOGS) ? 1u : 0u,
                         high_debug_gateway_binary_cdc_active() ? 1u : 0u);
}

static int high_debug_request_bootloader(void)
{
    HIGH_DEBUG_COUNTER_INC(bootloader_entry_requests);
#if defined(CONFIG_RETENTION_BOOT_MODE)
    int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

    if (ret < 0) {
        high_debug_log_event("BOOTLOADER_READY",
                             "request=failed ret=%d method=retention-boot-mode",
                             ret);
        return ret;
    }
    high_debug_log_event("BOOTLOADER_READY",
                         "request=armed method=retention-boot-mode reboot=now");
    k_msleep(50);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
#else
    high_debug_log_event("BOOTLOADER_READY",
                         "request=manual-only reason=retention_boot_mode_not_enabled");
    return -ENOTSUP;
#endif
}

static void high_debug_counter_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    high_debug_dump_counters("HEARTBEAT_TX");
    (void)k_work_reschedule(&high_debug_counter_work,
                             K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COUNTER_PERIOD_MS));
}
#endif

static const char *range_status_name(enum range_status status)
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

static bool range_status_valid(enum range_status status)
{
    return status >= RANGE_OK && status <= RANGE_TIMING_INVALID;
}

static bool mesh_id_is_unicast(uint64_t node_id)
{
    return node_id != MESH_BROADCAST_ID;
}

static int configure_output(const struct gpio_dt_spec *gpio)
{
    if (!gpio_is_ready_dt(gpio)) {
        return -ENODEV;
    }
    return gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE);
}

static void set_output(const struct gpio_dt_spec *gpio, bool enabled)
{
    if (gpio_is_ready_dt(gpio)) {
        (void)gpio_pin_set_dt(gpio, enabled ? 1 : 0);
    }
}

static void status_leds_set(bool red, bool green, bool blue)
{
#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    set_output(&status0_red, red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    set_output(&status0_green, green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    set_output(&status0_blue, blue);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    set_output(&status1_red, red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    set_output(&status1_green, green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    set_output(&status1_blue, blue);
#endif
}

static void status_apply(const struct status_inputs *inputs)
{
    struct status_indication indication;

    if (status_select(inputs, &indication) != PROTO_OK) {
        return;
    }

    switch (indication.pattern) {
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
    case STATUS_PATTERN_OFF:
    default:
        status_leds_set(false, false, false);
        break;
    }

    LOG_INF("status pattern=%d red_blinks=%u repeat=%u duration_ms=%u",
            indication.pattern, indication.red_blink_count,
            indication.repeat_count, indication.duration_ms);
}

static int status_leds_init(void)
{
    int ret = 0;

#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    ret |= configure_output(&status0_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    ret |= configure_output(&status0_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    ret |= configure_output(&status0_blue);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    ret |= configure_output(&status1_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    ret |= configure_output(&status1_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    ret |= configure_output(&status1_blue);
#endif

    status_leds_set(false, false, false);
    return ret;
}

static int radio_guard_uwb_start(const char *reason)
{
    k_spinlock_key_t key;
    bool already_active;

    key = k_spin_lock(&uwb_rf_lock);
    already_active = uwb_rf_active;
    if (!already_active) {
        uwb_rf_active = true;
    }
    k_spin_unlock(&uwb_rf_lock, key);

    if (already_active) {
        LOG_ERR("blocked nested UWB operation: %s", reason);
        return -EBUSY;
    }

    return 0;
}

static void radio_guard_uwb_stop(void)
{
    k_spinlock_key_t key;

    key = k_spin_lock(&uwb_rf_lock);
    uwb_rf_active = false;
    k_spin_unlock(&uwb_rf_lock, key);
}

static int debug_serial_init(void)
{
#if HAS_SERIAL_CONSOLE
    if (!device_is_ready(serial_console)) {
        return -ENODEV;
    }
#endif
#if defined(CONFIG_USB_DEVICE_STACK)
    int ret = usb_enable(NULL);

    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(usb_connects);
    }
#endif
    return 0;
}

static bool debug_serial_dtr_ready(void)
{
#if HAS_SERIAL_CONSOLE && defined(CONFIG_UART_LINE_CTRL)
    uint32_t dtr = 0u;

    if (uart_line_ctrl_get(serial_console, UART_LINE_CTRL_DTR, &dtr) == 0) {
        return dtr != 0u;
    }
#endif
    return true;
}

static int gateway_emit_serial_packet(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len)
{
#if HAS_SERIAL_CONSOLE
    struct proto_packet frame_packet;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_binary_cdc_enabled()) {
        return -ENOTSUP;
    }
    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        payload_len > UINT8_MAX) {
        return -EINVAL;
    }
    if (!device_is_ready(serial_console)) {
        return -ENODEV;
    }
    if (!debug_serial_dtr_ready()) {
        return -EAGAIN;
    }

    frame_packet = *packet;
    frame_packet.payload_len = (uint8_t)payload_len;
    ret = serial_frame_encode_packet(&frame_packet,
                                     payload,
                                     frame,
                                     sizeof(frame),
                                     &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    uart_poll_out(serial_console, SERIAL_FRAME_DELIMITER);
    for (size_t i = 0u; i < frame_len; i++) {
        uart_poll_out(serial_console, frame[i]);
    }

    LOG_INF("gateway USB COBS frame emitted: msg=0x%02x src=0x%016llx seq=%u payload_len=%u frame_len=%u",
            frame_packet.msg_type,
            (unsigned long long)frame_packet.src_id,
            frame_packet.seq,
            (unsigned int)payload_len,
            (unsigned int)frame_len);
    HIGH_DEBUG_COUNTER_INC(gateway_packets_emitted);
    high_debug_log_event("USB_GATEWAY_PACKET_TX",
                         "msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u frame_len=%u binary_cdc=%u",
                         frame_packet.msg_type,
                         (unsigned long long)frame_packet.src_id,
                         (unsigned long long)frame_packet.dst_id,
                         frame_packet.seq,
                         (unsigned int)payload_len,
                         (unsigned int)frame_len,
                         high_debug_gateway_binary_cdc_active() ? 1u : 0u);
    return 0;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENODEV;
#endif
}

static int mesh_errno_from_proto(int ret)
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

static uint16_t gateway_next_command_seq(void)
{
    gateway_command_seq++;
    if (gateway_command_seq == 0u) {
        gateway_command_seq = 1u;
    }
    return gateway_command_seq;
}

static uint32_t next_click_event_seq(void)
{
    next_event_seq++;
    if (next_event_seq == 0u) {
        next_event_seq = 1u;
    }
    return next_event_seq;
}

static int anchor_apply_gateway_time_sync(uint64_t gateway_time_ms)
{
    k_spinlock_key_t key;
    int64_t local_ms;
    int64_t offset_ms;

    if (DEVICE_ROLE != ROLE_ANCHOR || gateway_time_ms > (uint64_t)INT64_MAX) {
        return -EINVAL;
    }

    local_ms = k_uptime_get();
    offset_ms = (int64_t)gateway_time_ms - local_ms;
    key = k_spin_lock(&anchor_time_sync_lock);
    anchor_time_sync.gateway_offset_ms = offset_ms;
    anchor_time_sync.synced_local_ms = local_ms;
    anchor_time_sync.synced_gateway_ms = gateway_time_ms;
    anchor_time_sync.valid = true;
    k_spin_unlock(&anchor_time_sync_lock, key);

    LOG_INF("anchor gateway time synced: gateway_ms=%llu local_ms=%lld offset_ms=%lld",
            (unsigned long long)gateway_time_ms,
            (long long)local_ms,
            (long long)offset_ms);
    return 0;
}

static bool anchor_gateway_time_snapshot_at(int64_t local_ms,
                                            uint64_t *gateway_time_ms,
                                            uint32_t *time_sync_age_ms)
{
    struct anchor_time_sync_state snapshot;
    k_spinlock_key_t key;
    int64_t gateway_ms;
    int64_t age_ms;

    if (gateway_time_ms == NULL || time_sync_age_ms == NULL || local_ms < 0) {
        return false;
    }

    key = k_spin_lock(&anchor_time_sync_lock);
    snapshot = anchor_time_sync;
    k_spin_unlock(&anchor_time_sync_lock, key);
    if (!snapshot.valid) {
        return false;
    }

    gateway_ms = local_ms + snapshot.gateway_offset_ms;
    if (gateway_ms < 0) {
        return false;
    }

    age_ms = local_ms - snapshot.synced_local_ms;
    if (age_ms < 0) {
        age_ms = 0;
    }

    *gateway_time_ms = (uint64_t)gateway_ms;
    *time_sync_age_ms = age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    return true;
}

static void anchor_sequence_timestamp_at(int64_t local_ms,
                                         uint64_t *timestamp_ms,
                                         uint32_t *time_sync_age_ms)
{
    if (local_ms < 0) {
        local_ms = k_uptime_get();
    }
    if (timestamp_ms == NULL || time_sync_age_ms == NULL) {
        return;
    }

    if (anchor_gateway_time_snapshot_at(local_ms, timestamp_ms, time_sync_age_ms)) {
        return;
    }

    *timestamp_ms = (uint64_t)local_ms;
    *time_sync_age_ms = UINT32_MAX;
}

static bool anchor_gateway_time_synced(uint32_t *age_ms)
{
    uint64_t gateway_time_ms;
    uint32_t sync_age_ms;

    if (!anchor_gateway_time_snapshot_at(k_uptime_get(),
                                         &gateway_time_ms,
                                         &sync_age_ms)) {
        return false;
    }
    if (age_ms != NULL) {
        *age_ms = sync_age_ms;
    }
    return true;
}

static int anchor_append_sequence_time_tlvs(uint8_t *payload,
                                            size_t payload_cap,
                                            size_t *payload_len,
                                            int64_t local_ms)
{
    uint64_t timestamp_ms = 0u;
    uint32_t time_sync_age_ms = 0u;
    int ret;

    anchor_sequence_timestamp_at(local_ms, &timestamp_ms, &time_sync_age_ms);

    ret = tlv_append_u64(payload,
                         payload_cap,
                         payload_len,
                         TLV_TIMESTAMP_MS,
                         timestamp_ms);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          payload_len,
                          TLV_TIME_SYNC_AGE_MS,
                          time_sync_age_ms);
}

static uint32_t anchor_status_bits(void)
{
    uint32_t sync_age_ms = 0u;
    uint32_t status_bits;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0u;
    }

    status_bits = uwb_session_status_bits_from_diagnostics(&anchor_uwb_session.diagnostics);
    if (anchor_gateway_time_synced(&sync_age_ms)) {
        status_bits |= STATUS_BIT_TIME_SYNCED;
        if ((uint64_t)sync_age_ms > GATEWAY_TIME_SYNC_MAX_INTERVAL_MS) {
            status_bits |= STATUS_BIT_TIME_SYNC_STALE;
        }
    } else {
        status_bits |= STATUS_BIT_TIME_SYNC_STALE;
    }
    return status_bits;
}

static int append_anchor_status_tlvs(uint8_t *payload, size_t payload_cap, size_t *payload_len)
{
    struct anchor_heartbeat_fields fields = {
        .device_role = (uint8_t)DEVICE_ROLE,
        .battery_mv = ANCHOR_BATTERY_MV_UNKNOWN,
        .status_bits = anchor_status_bits(),
        .uptime_ms = k_uptime_get_32(),
    };
    int ret;

    anchor_sequence_timestamp_at(k_uptime_get(),
                                 &fields.timestamp_ms,
                                 &fields.time_sync_age_ms);
    ret = report_append_anchor_heartbeat_tlvs(payload, payload_cap, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_append_status_tlvs(&mesh_runtime, payload, payload_cap, payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CHANNEL_SWITCHES,
                         mesh_event_stats.channel_switches);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_PLL_READY_FAILURES,
                         mesh_event_stats.pll_ready_failures);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_LATE_CHANNEL5_RETURNS,
                         mesh_event_stats.late_channel5_returns);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_DEFERRALS,
                         mesh_event_stats.mesh_deferrals);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CH9_EVENT_MISSES,
                         mesh_event_stats.ch9_event_misses);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CHANNEL5_PREEMPTIONS,
                         mesh_event_stats.channel5_preemptions);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          payload_len,
                          TLV_MESH_CH9_REPORT_LATENCY_MS,
                          mesh_event_stats.ch9_report_latency_ms);
}

static int anchor_send_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
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

    diagnostic = (command->flags & FLAG_DIAGNOSTIC) != 0u;
    ret = mesh_init_command_result(&outbound.packet,
                                   DEVICE_ID,
                                   GATEWAY_ID,
                                   command->session_id,
                                   command->seq,
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

static bool uptime_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t uptime_ms_until_deadline(uint32_t now_ms, uint32_t deadline_ms)
{
    if (uptime_deadline_reached(now_ms, deadline_ms)) {
        return 1u;
    }

    return deadline_ms - now_ms;
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

static uint16_t mesh_next_event_control_seq(void)
{
    mesh_event_control_seq++;
    if (mesh_event_control_seq == 0u) {
        mesh_event_control_seq = 1u;
    }
    return mesh_event_control_seq;
}

static uint32_t nonzero_uptime_session_id(void)
{
    uint32_t session_id = k_uptime_get_32();

    return session_id == 0u ? 1u : session_id;
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
    if (anchor_uwb_window_active() || mesh_relay_tx_active(&mesh_runtime)) {
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

static int command_extract_result_tlvs(const uint8_t *payload,
                                       size_t payload_len,
                                       enum command_id *command_id,
                                       enum command_status *status,
                                       uint8_t *reason)
{
    uint16_t command_value = 0u;
    uint16_t status_value = 0u;
    uint8_t reason_value = 0u;
    int ret;

    if (command_id == NULL || status == NULL || reason == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = command_find_u16_tlv(payload, payload_len, TLV_COMMAND_ID, &command_value);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = command_find_u16_tlv(payload, payload_len, TLV_COMMAND_STATUS, &status_value);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_REASON, &reason_value);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (status_value > COMMAND_INTERNAL_ERROR) {
        return PROTO_ERR_MALFORMED;
    }

    *command_id = (enum command_id)command_value;
    *status = (enum command_status)status_value;
    *reason = reason_value;
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
    route_table_init(&mesh_runtime.upstream, mesh_runtime.upstream.current_epoch + 1u);
    LOG_INF("anchor route command cleared upstream route state");
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
        (void)k_work_reschedule(&anchor_uwb_scan_work, K_MSEC(anchor_uwb_scan_interval_ms));
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

static size_t anchor_collect_survey_reachability(uint64_t previous_hop_id,
                                                 uint8_t link_quality,
                                                 struct survey_reachability_entry *entries,
                                                 size_t entry_cap)
{
    const struct route_candidate *selected;
    size_t entry_count = 0u;

    survey_add_reach_entry(entries, entry_cap, &entry_count, previous_hop_id, link_quality);

    selected = route_selected(&mesh_runtime.upstream);
    if (selected != NULL) {
        survey_add_reach_entry(entries,
                               entry_cap,
                               &entry_count,
                               selected->next_hop_id,
                               selected->link_quality);
    }

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *entry = &mesh_runtime.downlinks[i];

        if (!entry->valid) {
            continue;
        }
        survey_add_reach_entry(entries,
                               entry_cap,
                               &entry_count,
                               entry->next_hop_id,
                               entry->quality);
    }

    return entry_count;
}

static void anchor_handle_survey_reach_request(const struct proto_packet *packet,
                                               const uint8_t *payload,
                                               size_t payload_len,
                                               uint64_t previous_hop_id,
                                               uint8_t link_quality)
{
    struct survey_reachability_entry entries[SURVEY_REACH_MAX_ENTRIES];
    struct mesh_outbound outbound = {0};
    uint32_t survey_id = 0u;
    uint32_t duration_ms = 0u;
    size_t entry_count;
    size_t report_payload_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_SURVEY_REACH_REQ ||
        packet->dst_id != MESH_BROADCAST_ID ||
        packet->src_id != GATEWAY_ID) {
        return;
    }

    ret = survey_extract_reach_request_tlvs(payload, payload_len, &survey_id, &duration_ms);
    if (ret != PROTO_OK || packet->session_id != survey_id) {
        LOG_WRN("survey reach request rejected: ret=%d session=%u survey=%u",
                ret,
                packet->session_id,
                survey_id);
        return;
    }

    entry_count = anchor_collect_survey_reachability(previous_hop_id,
                                                    link_quality,
                                                    entries,
                                                    ARRAY_SIZE(entries));
    ret = survey_append_reach_report_tlvs(outbound.payload,
                                          sizeof(outbound.payload),
                                          &report_payload_len,
                                          survey_id,
                                          DEVICE_ID,
                                          entries,
                                          entry_count);
    if (ret != PROTO_OK) {
        LOG_WRN("survey reach report payload build failed: %d", ret);
        return;
    }
    ret = survey_init_reach_report_packet(&outbound.packet,
                                          DEVICE_ID,
                                          GATEWAY_ID,
                                          survey_id,
                                          anchor_next_survey_seq(),
                                          (uint8_t)report_payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("survey reach report packet build failed: %d", ret);
        return;
    }
    outbound.payload_len = (uint8_t)report_payload_len;

    ret = queue_anchor_report(&outbound);
    if (ret < 0) {
        LOG_WRN("survey reach report queue failed: ret=%d survey=%u", ret, survey_id);
        return;
    }

    LOG_INF("survey reach request accepted: survey=%u duration_ms=%u peers=%u",
            survey_id,
            duration_ms,
            (unsigned int)entry_count);
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

    ret = anchor_send_command_result(packet, CMD_SURVEY_PREPARE_PAIR, status, reason);
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

static void gateway_emit_serial_command_result(const struct proto_packet *command,
                                               enum command_id command_id,
                                               enum command_status status,
                                               uint8_t reason)
{
    struct proto_packet result = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    if (command == NULL || DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    ret = gateway_command_build_failure_result(command,
                                               DEVICE_ID,
                                               command_id,
                                               status,
                                               reason,
                                               k_uptime_get_32(),
                                               &result,
                                               payload,
                                               sizeof(payload),
                                               &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway command failure result build failed: %d", ret);
        return;
    }

    ret = gateway_emit_serial_packet(&result, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway USB command failure result not emitted: %d", ret);
    }
    HIGH_DEBUG_COUNTER_INC(command_result_tx);
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=gateway_cobs command=0x%04x status=%s reason=%u ret=%d",
                         (unsigned int)command_id,
                         command_status_name(status),
                         reason,
                         ret);
}

static bool gateway_pending_command_matches(const struct proto_packet *command)
{
    if (command == NULL || !gateway_command_pending_state.active) {
        return false;
    }

    return gateway_command_pending_state.command.dst_id == command->dst_id &&
           gateway_command_pending_state.command.session_id == command->session_id &&
           gateway_command_pending_state.command.seq == command->seq;
}

static void gateway_clear_pending_command_result(const struct proto_packet *command)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || !gateway_pending_command_matches(command)) {
        return;
    }

    gateway_command_pending_clear(&gateway_command_pending_state);
    (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
}

static void gateway_command_result_timeout_handler(struct k_work *work)
{
    struct proto_packet command = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    if (!gateway_command_pending_expired(&gateway_command_pending_state,
                                         k_uptime_get_32(),
                                         &command,
                                         &command_id)) {
        return;
    }

    LOG_WRN("gateway command result timeout: cmd=0x%04x dst=0x%016llx session=%u seq=%u",
            (unsigned int)command_id,
            (unsigned long long)command.dst_id,
            command.session_id,
            command.seq);
    mesh_relay_note_delivery_failure(&mesh_runtime, command.dst_id);
    mesh_clear_route_waiting_tx(&command);
    gateway_survey_auto_note_command_timeout(&command, command_id);
    gateway_emit_serial_command_result(&command, command_id, COMMAND_TIMEOUT, 0u);
}

static void gateway_note_command_result(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    struct proto_packet command;
    enum command_id pending_command_id;
    enum command_id result_command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_INTERNAL_ERROR;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY ||
        !gateway_command_pending_matches_result(&gateway_command_pending_state, packet)) {
        return;
    }

    command = gateway_command_pending_state.command;
    pending_command_id = gateway_command_pending_state.command_id;
    gateway_command_pending_clear(&gateway_command_pending_state);
    (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
    ret = command_extract_result_tlvs(payload,
                                      payload_len,
                                      &result_command_id,
                                      &status,
                                      &reason);
    if (ret != PROTO_OK || result_command_id != pending_command_id) {
        LOG_WRN("gateway command result payload mismatch: pending=0x%04x result=0x%04x ret=%d",
                (unsigned int)pending_command_id,
                (unsigned int)result_command_id,
                ret);
        status = COMMAND_INTERNAL_ERROR;
        reason = (uint8_t)(ret == PROTO_OK ? 1u : -ret);
    }

    LOG_INF("gateway command result received: src=0x%016llx session=%u seq=%u",
            (unsigned long long)packet->src_id,
            packet->session_id,
            packet->seq);
    gateway_survey_auto_note_command_result(&command, pending_command_id, status, reason);
}

static int gateway_begin_command_result_wait(const struct proto_packet *command,
                                             enum command_id command_id)
{
    int ret;

    ret = gateway_command_pending_start(&gateway_command_pending_state,
                                        command,
                                        command_id,
                                        k_uptime_get_32(),
                                        GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_MALFORMED ? -EBUSY : mesh_errno_from_proto(ret);
    }

    (void)k_work_reschedule(&gateway_command_result_timeout_work,
                            K_MSEC(GATEWAY_COMMAND_RESULT_TIMEOUT_MS));
    return 0;
}

static void gateway_time_sync_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        (void)k_work_reschedule(&gateway_time_sync_work, K_MSEC(delay_ms));
    }
}

static int gateway_time_sync_broadcast(void)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    ret = mesh_append_command_id(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 CMD_SYNC_TIME);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = tlv_append_u64(outbound.payload,
                         sizeof(outbound.payload),
                         &payload_len,
                         TLV_TIMESTAMP_MS,
                         (uint64_t)k_uptime_get());
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    outbound.packet.msg_type = MSG_COMMAND;
    outbound.packet.src_id = DEVICE_ID;
    outbound.packet.dst_id = MESH_BROADCAST_ID;
    outbound.packet.session_id = nonzero_uptime_session_id();
    outbound.packet.seq = gateway_next_command_seq();
    outbound.packet.ttl = MESH_DEFAULT_TTL;
    outbound.packet.payload_len = (uint8_t)payload_len;
    outbound.payload_len = (uint8_t)payload_len;

    ret = mesh_send_outbound(&outbound, "time-sync-broadcast");
    if (ret < 0) {
        return ret;
    }

    LOG_INF("gateway time sync broadcast sent: session=%u seq=%u",
            outbound.packet.session_id,
            outbound.packet.seq);
    return 0;
}

static void gateway_time_sync_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    if (gateway_survey_active ||
        gateway_survey_auto.running ||
        gateway_command_pending_state.active ||
        mesh_relay_tx_active(&mesh_runtime)) {
        gateway_time_sync_schedule(GATEWAY_TIME_SYNC_RETRY_MS);
        return;
    }

    ret = gateway_time_sync_broadcast();
    if (ret == 0) {
        gateway_time_sync_schedule(GATEWAY_TIME_SYNC_DEFAULT_INTERVAL_MS);
        return;
    }

    LOG_WRN("gateway time sync broadcast failed: ret=%d", ret);
    gateway_time_sync_schedule(GATEWAY_TIME_SYNC_RETRY_MS);
}

static void anchor_handle_local_command(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_OK;
    enum device_role requested_role = ROLE_ANCHOR;
    uint64_t timestamp_ms = 0u;
    bool reboot_after_result = false;
    bool broadcast_command = false;
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
    } else if (broadcast_command && command_id != CMD_SYNC_TIME) {
        return;
    } else if (command_id == CMD_REBOOT) {
        reboot_after_result = true;
    } else if (command_id == CMD_SET_ROLE) {
        ret = gateway_command_extract_role(payload, payload_len, &requested_role);
        if (ret != PROTO_OK) {
            status = COMMAND_MALFORMED_PAYLOAD;
            reason = (uint8_t)(-ret);
        } else if ((uint8_t)requested_role != (uint8_t)DEVICE_ROLE) {
            status = COMMAND_DENIED;
            reason = 1u;
        }
    } else if (command_id == CMD_START_HEARTBEAT) {
        ret = anchor_start_heartbeat_from_command(payload, payload_len, &reason);
        if (ret < 0) {
            status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_STOP_HEARTBEAT) {
        anchor_stop_heartbeat();
    } else if (command_id == CMD_SET_LED_PATTERN) {
        ret = anchor_set_led_pattern_from_command(payload, payload_len, &reason);
        if (ret < 0) {
            status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_SET_ROUTE) {
        ret = anchor_set_route_from_command(payload, payload_len, &reason);
        if (ret < 0) {
            status = ret == -ENOSPC ? COMMAND_BUSY : COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_CLEAR_ROUTE) {
        anchor_clear_route_from_command();
    } else if (command_id == CMD_SET_SCAN_DUTY) {
        ret = anchor_set_scan_duty_from_command(payload, payload_len, &reason);
        if (ret < 0) {
            status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_SYNC_TIME) {
        ret = gateway_command_extract_timestamp_ms(payload,
                                                   payload_len,
                                                   &timestamp_ms);
        if (ret != PROTO_OK) {
            status = COMMAND_MALFORMED_PAYLOAD;
            reason = (uint8_t)(-ret);
        } else if (anchor_apply_gateway_time_sync(timestamp_ms) < 0) {
            status = COMMAND_MALFORMED_PAYLOAD;
            reason = 1u;
        }
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        ret = anchor_start_survey_pair_from_command(packet,
                                                    payload,
                                                    payload_len,
                                                    &status,
                                                    &reason);
        if (ret < 0 && status == COMMAND_OK) {
            status = COMMAND_INTERNAL_ERROR;
            reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_ABORT) {
        anchor_abort_survey_pair();
    } else if (command_id != CMD_PING && command_id != CMD_GET_STATUS) {
        status = COMMAND_UNSUPPORTED_COMMAND;
        reason = 1u;
    }

    if (broadcast_command) {
        LOG_INF("anchor broadcast command handled: cmd=0x%04x status=%u reason=%u",
                (unsigned int)command_id,
                status,
                reason);
        return;
    }

    ret = anchor_send_command_result(packet, command_id, status, reason);
    if (ret < 0) {
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        return;
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
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
    struct mesh_outbound outbound = {0};
    uint32_t survey_id = 0u;
    uint32_t duration_ms = 0u;
    uint16_t sample_count = UWB_SAMPLES_PER_ANCHOR;
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (host_packet->dst_id != MESH_BROADCAST_ID && host_packet->dst_id != DEVICE_ID)) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }

    ret = survey_extract_reach_request_tlvs(host_payload,
                                            host_payload_len,
                                            &survey_id,
                                            &duration_ms);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_extract_survey_sample_count(host_payload,
                                              host_payload_len,
                                              &sample_count);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    ret = survey_append_reach_request_tlvs(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           survey_id,
                                           duration_ms);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    ret = survey_init_reach_request_packet(&outbound.packet,
                                           DEVICE_ID,
                                           survey_id,
                                           seq,
                                           (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = MESH_BROADCAST_ID;

    ret = survey_gateway_begin(&gateway_survey_context, survey_id, sample_count);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    ret = survey_gateway_auto_begin(&gateway_survey_auto);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    gateway_survey_active = true;

    ret = mesh_send_outbound(&outbound, "survey-reach-request");
    if (ret < 0) {
        gateway_survey_active = false;
        (void)survey_gateway_auto_begin(&gateway_survey_auto);
        (void)k_work_cancel_delayable(&gateway_survey_work);
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_REACHABILITY,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_RADIO_ERROR,
                                           (uint8_t)(-ret));
        return ret;
    }

    (void)k_work_reschedule(&gateway_survey_work, K_MSEC(duration_ms));
    gateway_emit_serial_command_result(host_packet, CMD_SURVEY_REACHABILITY, COMMAND_OK, 0u);
    LOG_INF("gateway survey reachability broadcast: survey=%u duration_ms=%u samples=%u seq=%u",
            survey_id,
            duration_ms,
            sample_count,
            seq);
    return 0;
}

static void gateway_handle_survey_reach_report(const struct proto_packet *packet,
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
        packet->msg_type != MSG_SURVEY_REACH_REPORT ||
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
        LOG_WRN("gateway survey reach report rejected: ret=%d src=0x%016llx session=%u",
                ret,
                (unsigned long long)packet->src_id,
                packet->session_id);
        return;
    }
    if (packet->session_id != survey_id || packet->src_id != anchor_id) {
        LOG_WRN("gateway survey reach report identity mismatch: pkt_session=%u survey=%u src=0x%016llx anchor=0x%016llx",
                packet->session_id,
                survey_id,
                (unsigned long long)packet->src_id,
                (unsigned long long)anchor_id);
        return;
    }
    if (!gateway_survey_active ||
        gateway_survey_context.survey_id != survey_id) {
        LOG_WRN("gateway survey reach report stale: survey=%u active=%u current=%u anchor=0x%016llx",
                survey_id,
                gateway_survey_active ? 1u : 0u,
                gateway_survey_context.survey_id,
                (unsigned long long)anchor_id);
        return;
    }
    if (gateway_survey_auto.running) {
        LOG_WRN("gateway survey reach report ignored after orchestration start: survey=%u anchor=0x%016llx entries=%u",
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
        LOG_WRN("gateway survey reach report not recorded: survey=%u anchor=0x%016llx entries=%u ret=%d",
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
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }

    ret = survey_extract_pair_tlvs(host_payload, host_payload_len, &pair);
    if (ret != PROTO_OK ||
        (host_packet->dst_id != pair.initiator_id &&
         host_packet->dst_id != pair.responder_id)) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           ret == PROTO_OK ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(ret == PROTO_OK ? 2u : -ret));
        return ret == PROTO_OK ? -EINVAL : mesh_errno_from_proto(ret);
    }

    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &pair);
    if (ret != PROTO_OK) {
        gateway_emit_serial_command_result(host_packet,
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
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.dst_id = host_packet->dst_id;
    outbound.payload_len = (uint8_t)payload_len;

    ret = gateway_begin_command_result_wait(&outbound.packet, CMD_SURVEY_PREPARE_PAIR);
    if (ret < 0) {
        gateway_emit_serial_command_result(host_packet,
                                           CMD_SURVEY_PREPARE_PAIR,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    ret = mesh_start_tracked_tx(&outbound, "survey-pair-prepare");
    if (ret < 0) {
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_serial_command_result(host_packet,
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
        gateway_emit_serial_command_result(host_packet,
                                           command_id,
                                           COMMAND_UNSUPPORTED_COMMAND,
                                           1u);
        return -ENOTSUP;
    }
}

static int gateway_route_serial_packet(struct proto_packet *packet,
                                       uint8_t *payload,
                                       size_t payload_len)
{
    struct mesh_outbound outbound = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
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
        LOG_WRN("gateway rejected USB command: %d", ret);
        gateway_emit_serial_command_result(packet,
                                           CMD_VENDOR_BASE,
                                           ret == PROTO_ERR_ARG ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_begin_command_result_wait(&outbound.packet, command_id);
    if (ret < 0) {
        LOG_WRN("gateway command result tracker busy: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound.packet.dst_id,
                ret);
        gateway_emit_serial_command_result(&outbound.packet,
                                           command_id,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    ret = mesh_start_tracked_tx(&outbound, "usb-command");
    if (ret < 0) {
        LOG_WRN("gateway USB command route failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound.packet.dst_id,
                ret);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            LOG_INF("gateway command waiting for reactive route discovery: cmd=0x%04x dst=0x%016llx",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id);
            return 0;
        }
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_serial_command_result(&outbound.packet,
                                           command_id,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway USB command routed: cmd=0x%04x dst=0x%016llx session=%u seq=%u ttl=%u",
            (unsigned int)command_id,
            (unsigned long long)outbound.packet.dst_id,
            outbound.packet.session_id,
            outbound.packet.seq,
            outbound.packet.ttl);
    return 0;
}

static void gateway_handle_serial_frame(const uint8_t *frame, size_t frame_len)
{
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
        LOG_WRN("gateway USB COBS frame decode failed: %d", ret);
        return;
    }
    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX",
                         "transport=gateway_cobs msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u",
                         packet.msg_type,
                         (unsigned long long)packet.src_id,
                         (unsigned long long)packet.dst_id,
                         packet.seq,
                         (unsigned int)payload_len);

    ret = gateway_route_serial_packet(&packet, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway USB packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                packet.msg_type,
                (unsigned long long)packet.dst_id,
                ret);
    }
}

static void gateway_serial_rx_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
    if (!gateway_binary_cdc_enabled()) {
        return;
    }

#if HAS_SERIAL_CONSOLE
    if (device_is_ready(serial_console) && debug_serial_dtr_ready()) {
        unsigned char byte;
        uint16_t read_count = 0u;

        while (read_count < GATEWAY_SERIAL_MAX_BYTES_PER_POLL &&
               uart_poll_in(serial_console, &byte) == 0) {
            read_count++;

            if (gateway_serial_rx_overflow) {
                if (byte == SERIAL_FRAME_DELIMITER) {
                    gateway_serial_rx_overflow = false;
                    gateway_serial_rx_len = 0u;
                    LOG_WRN("gateway USB RX frame dropped after overflow");
                }
                continue;
            }

            if (gateway_serial_rx_len >= sizeof(gateway_serial_rx_frame)) {
                gateway_serial_rx_overflow = true;
                gateway_serial_rx_len = 0u;
                continue;
            }

            gateway_serial_rx_frame[gateway_serial_rx_len] = byte;
            gateway_serial_rx_len++;
            if (byte == SERIAL_FRAME_DELIMITER) {
                if (gateway_serial_rx_len > 1u) {
                    gateway_handle_serial_frame(gateway_serial_rx_frame, gateway_serial_rx_len);
                }
                gateway_serial_rx_len = 0u;
            }
        }
    }
#endif

    (void)k_work_reschedule(&gateway_serial_rx_work, K_MSEC(GATEWAY_SERIAL_POLL_MS));
}

static uint16_t local_uwb_short_addr(void)
{
    uint16_t short_addr = (uint16_t)(DEVICE_ID & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

static uint8_t local_uwb_anchor_slot(void)
{
    if (UWB_ANCHOR_SLOT < UWB_DISCOVERY_SLOT_COUNT) {
        return (uint8_t)UWB_ANCHOR_SLOT;
    }

    return (uint8_t)(DEVICE_ID % UWB_DISCOVERY_SLOT_COUNT);
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value == 0u ? 1u : value;
}

static uint64_t clicker_priority_id(uint32_t event_seq, uint8_t attempt_index)
{
    return mix64(DEVICE_ID ^
                 ((uint64_t)event_seq << 17) ^
                 ((uint64_t)attempt_index << 49));
}

static uint64_t clicker_nonce(uint32_t event_seq)
{
    return mix64(DEVICE_ID ^
                 ((uint64_t)event_seq << 32) ^
                 k_cycle_get_32());
}

static uint8_t survey_sample_seq(uint16_t sample_index)
{
    uint8_t seq = (uint8_t)((sample_index + 1u) & 0xffu);

    return seq == 0u ? 1u : seq;
}

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
        int ret;

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
            LOG_INF("survey DS-TWR initiator sample complete: survey=%u responder=0x%016llx sample=%u/%u distance_mm=%d quality=%u",
                    pair->survey_id,
                    (unsigned long long)result.responder_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality);
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
            LOG_INF("survey DS-TWR responder sample complete: survey=%u initiator=0x%016llx sample=%u/%u distance_mm=%d quality=%u",
                    pair->survey_id,
                    (unsigned long long)result.initiator_id,
                    (unsigned int)(sample_index + 1u),
                    pair->sample_count,
                    result.distance_mm,
                    result.quality);
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
    bool as_responder;
    int64_t uwb_window_start_ms;
    k_spinlock_key_t key;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    key = k_spin_lock(&anchor_survey_lock);
    if (!anchor_survey_start_pending) {
        k_spin_unlock(&anchor_survey_lock, key);
        return;
    }
    k_spin_unlock(&anchor_survey_lock, key);

    if (anchor_uwb_window_active() || mesh_relay_tx_active(&mesh_runtime)) {
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

    if ((uint32_t)k_msgq_num_used_get(&report_tx_msgq) + pair.sample_count >
        REPORT_TX_QUEUE_DEPTH) {
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
    k_spin_unlock(&anchor_survey_lock, key);
    (void)k_work_cancel_delayable(&anchor_survey_work);
    LOG_INF("survey pair state aborted locally");
}

static void sleep_until_ms(int64_t target_ms)
{
    while (true) {
        int64_t now_ms = k_uptime_get();

        if (now_ms >= target_ms) {
            return;
        }
        k_msleep((uint32_t)MIN(10, target_ms - now_ms));
    }
}

static uint32_t u32_saturating_add(uint32_t lhs, uint32_t rhs)
{
    uint64_t sum = (uint64_t)lhs + rhs;

    return (uint32_t)MIN(sum, (uint64_t)UINT32_MAX);
}

static uint32_t sleep_with_uwb_standby_until_ms(int64_t target_ms)
{
    int64_t now_ms = k_uptime_get();

    if (now_ms < target_ms) {
        int ret = dwm3000_driver_standby();

        if (ret < 0) {
            LOG_WRN("DWM3000 retained-sleep entry before scheduled wait failed: %d", ret);
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

static void sleep_precise_us(uint32_t delay_us)
{
    if (delay_us >= 1000u) {
        k_msleep(delay_us / 1000u);
        delay_us %= 1000u;
    }
    if (delay_us > 0u) {
        k_busy_wait(delay_us);
    }
}

static void sleep_until_us(int64_t target_us)
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

struct mesh_frame_parse_context {
    bool found;
    uint64_t previous_hop_id;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

static int anchor_start_uwb_scan(void);
static void anchor_uwb_scan_work_handler(struct k_work *work);
static int mesh_start_uwb_rx(const char *reason);
static bool anchor_uwb_window_active(void);
static void mesh_preempt_for_click_event(void);
static void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
static int build_range_report_samples(uint64_t clicker_id,
                                      uint32_t event_seq,
                                      uint32_t burst_id,
                                      const struct dwm3000_range_result *range_result,
                                      const int32_t *distance_samples_mm,
                                      const uint8_t *range_round_indices,
                                      const int64_t *sample_sequence_start_ms,
                                      uint16_t sample_count);

static uint16_t delay_ms_to_u16(int64_t delay_ms)
{
    if (delay_ms <= 0) {
        return 0u;
    }
    if (delay_ms > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)delay_ms;
}

static uint32_t uwb_schedule_burst_id(uint32_t event_seq, uint8_t attempt_index)
{
    return ((event_seq & 0x00ffffffu) << 8) | attempt_index;
}

static int64_t scheduled_range_sample_target_us(int64_t schedule_start_ms,
                                                const struct uwb_range_schedule_frame *schedule,
                                                size_t sample_index)
{
    return ((schedule_start_ms + schedule->first_poll_delay_ms) * 1000) +
           ((int64_t)sample_index * schedule->exchange_stride_us);
}

static int64_t ceil_us_to_ms(int64_t value_us)
{
    if (value_us <= 0) {
        return 0;
    }
    return (value_us + 999) / 1000;
}

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

struct anchor_range_window_report {
    struct dwm3000_range_result result;
    int32_t distance_samples_mm[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t sample_sequence_start_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    uint8_t range_round_indices[RANGE_REPORT_MAX_DISTANCE_SAMPLES];
    int64_t distance_sum_mm;
    int64_t first_exchange_start_ms;
    uint32_t quality_sum;
    uint16_t sample_count;
    bool have_result;
    bool have_exchange_start_ms;
    bool rsl_sampled;
    bool cir_sampled;
};

static int32_t average_i32_nearest(int64_t sum, uint16_t count)
{
    int64_t half;

    if (count == 0u) {
        return 0;
    }

    half = count / 2;
    return (int32_t)(sum >= 0 ? (sum + half) / count : (sum - half) / count);
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

static void build_uwb_schedule_report_if_relevant(const struct uwb_anchor_session *session,
                                                  uint8_t flags,
                                                  const struct anchor_range_window_report *report)
{
    int ret;

    if ((flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) == 0u) {
        return;
    }
    if (session == NULL || report == NULL || !report->have_result) {
        return;
    }

    ret = build_range_report_samples(session->epoch.clicker_id,
                                     session->epoch.click_event_id,
                                     uwb_schedule_burst_id(session->epoch.click_event_id,
                                                           session->epoch.attempt_index),
                                     &report->result,
                                     report->distance_samples_mm,
                                     report->range_round_indices,
                                     report->sample_sequence_start_ms,
                                     report->sample_count);
    if (ret < 0) {
        LOG_WRN("failed to build UWB scheduled anchor range report: %d", ret);
    }
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

    if (schedule == NULL) {
        return 0u;
    }

    total_samples = uwb_range_schedule_total_samples(schedule);
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
        retained_sleep_us = u32_saturating_add(
            retained_sleep_us,
            sleep_with_uwb_standby_until_ms(listen_start_ms));

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

        LOG_INF("anchor scheduled UWB sample listen: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                (unsigned int)(sample_index + 1u),
                (unsigned int)total_samples,
                round_index,
                seq);
        high_debug_log_event("DS_TWR_POLL_RX",
                             "listen=1 clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             uwb_schedule_burst_id(schedule->click_event_id,
                                                   schedule->attempt_index),
                             (unsigned int)(sample_index + 1u),
                             (unsigned int)total_samples,
                             round_index,
                             seq);
        while (k_uptime_get() < listen_deadline_ms) {
            int64_t remaining_ms = listen_deadline_ms - k_uptime_get();

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

        if (!range_result.exchange_started) {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_RX_TIMEOUT;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
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

    anchor_range_window_finalize(&window_report);
    build_uwb_schedule_report_if_relevant(&anchor_uwb_session,
                                          schedule->flags,
                                          &window_report);
    return retained_sleep_us;
}

static bool anchor_handle_uwb_claim(const struct uwb_wake_claim_frame *first_claim,
                                    uint8_t first_quality,
                                    int64_t first_rx_ms,
                                    uint32_t *retained_sleep_us)
{
    struct uwb_wake_claim_frame selected_claim;
    int64_t selected_rx_ms;
    int64_t collect_deadline_ms;
    uint8_t selected_quality = first_quality;
    uint8_t frame[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t frame_len = 0u;
    int ret;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    enum uwb_anchor_claim_decision selected_decision = UWB_ANCHOR_CLAIM_ACCEPTED;

    if (first_claim == NULL) {
        return false;
    }
    if (retained_sleep_us != NULL) {
        *retained_sleep_us = 0u;
    }

    selected_claim = *first_claim;
    selected_rx_ms = first_rx_ms;
    HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
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
    mesh_preempt_for_click_event();

    collect_deadline_ms = k_uptime_get() + ANCHOR_CLAIM_COLLECTION_MS;
    while (k_uptime_get() < collect_deadline_ms) {
        struct uwb_wake_claim_frame claim;
        uint8_t quality = 0u;
        int64_t remaining_ms = collect_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame((uint32_t)MAX(1, remaining_ms),
                                           frame,
                                           sizeof(frame),
                                           &frame_len,
                                           &quality,
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
            high_debug_log_event("WAKE_CLAIM_ACCEPT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 (unsigned long long)claim.priority_id,
                                 claim_decision_name(decision));
        } else {
            HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
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

    {
        int64_t discovery_start_ms =
            (int64_t)selected_rx_ms + selected_claim.discovery_starts_in_ms;
        int64_t discover_listen_start_ms =
            discovery_start_ms - UWB_DISCOVERY_RX_GUARD_MS;

        if (retained_sleep_us != NULL) {
            *retained_sleep_us = u32_saturating_add(
                *retained_sleep_us,
                sleep_with_uwb_standby_until_ms(discover_listen_start_ms));
        } else {
            (void)sleep_with_uwb_standby_until_ms(discover_listen_start_ms);
        }
    }

    ret = dwm3000_driver_receive_frame(UWB_DISCOVERY_LISTEN_MS,
                                       frame,
                                       sizeof(frame),
                                       &frame_len,
                                       &selected_quality,
                                       NULL);
    if (ret == 0) {
        struct uwb_discover_frame discover;
        struct uwb_discovery_reply_frame reply;

        ret = uwb_decode_discover(frame, frame_len, &discover);
        if (ret != PROTO_OK) {
            enum uwb_wake_decode_failure failure = wake_failure_from_proto_ret(ret);

            uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                failure);
            LOG_WRN("anchor UWB DISCOVER decode failed: ret=%d reason=%s frame_len=%u",
                    ret,
                    wake_decode_failure_name(failure),
                    (unsigned int)frame_len);
            return true;
        }
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
                                               0u,
                                               &reply);
        if (ret != PROTO_OK) {
            LOG_WRN("anchor UWB DISCOVERY_REPLY build rejected: %d", ret);
            return true;
        }

        sleep_precise_us((uint32_t)local_uwb_anchor_slot() * UWB_DISCOVERY_SLOT_US);
        ret = uwb_encode_discovery_reply(&reply, frame, sizeof(frame), &frame_len);
        if (ret != PROTO_OK) {
            LOG_WRN("anchor UWB DISCOVERY_REPLY encode failed: %d", ret);
            return true;
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        UWB_DISCOVERY_REPLY_TX_TIMEOUT_MS);
        if (ret < 0) {
            LOG_WRN("anchor UWB DISCOVERY_REPLY TX failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(discovery_reply_tx);
        high_debug_log_event("DISCOVERY_REPLY_TX",
                             "clicker=0x%016llx event_seq=%u slot=%u quality=%u",
                             (unsigned long long)reply.selected_clicker_id,
                             reply.click_event_id,
                             reply.anchor_slot,
                             reply.rx_quality);
        LOG_INF("anchor UWB DISCOVERY_REPLY sent: clicker=0x%016llx slot=%u quality=%u",
                (unsigned long long)reply.selected_clicker_id,
                reply.anchor_slot,
                reply.rx_quality);
    } else {
        LOG_WRN("anchor UWB DISCOVER not received: ret=%d", ret);
        return true;
    }

    ret = dwm3000_driver_receive_frame(UWB_RANGE_SCHEDULE_RX_MS,
                                       frame,
                                       sizeof(frame),
                                       &frame_len,
                                       NULL,
                                       NULL);
    if (ret == 0) {
        struct uwb_range_schedule_frame schedule;
        int64_t schedule_rx_ms = k_uptime_get();

        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_RELEASE) {
            struct uwb_range_release_frame release;

            ret = uwb_decode_range_release(frame, frame_len, &release);
            if (ret != PROTO_OK) {
                LOG_WRN("anchor UWB RANGE_RELEASE decode failed: %d", ret);
                return true;
            }

            ret = uwb_anchor_accept_range_release(&anchor_uwb_session, &release);
            if (ret != PROTO_OK) {
                LOG_WRN("anchor rejected UWB RANGE_RELEASE: %d", ret);
                return true;
            }

            LOG_INF("anchor UWB RANGE_RELEASE accepted: clicker=0x%016llx event_seq=%u discovered=%u min=%u reason=%u",
                    (unsigned long long)release.clicker_id,
                    release.click_event_id,
                    release.discovered_anchor_count,
                    release.min_anchor_count,
                    release.reason);
            return true;
        }

        ret = uwb_decode_range_schedule(frame, frame_len, &schedule);
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
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
            LOG_INF("anchor not selected in UWB RANGE_SCHEDULE: clicker=0x%016llx event_seq=%u",
                    (unsigned long long)schedule.clicker_id,
                    schedule.click_event_id);
            return true;
        }
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
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
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (anchor_uwb_window_active() || mesh_relay_tx_active(&mesh_runtime)) {
        (void)k_work_reschedule(&anchor_uwb_scan_work,
                                K_MSEC(anchor_uwb_scan_interval_ms));
        return;
    }

    ret = radio_guard_uwb_start("anchor low-duty UWB wake scan");
    if (ret < 0) {
        (void)k_work_reschedule(&anchor_uwb_scan_work,
                                K_MSEC(anchor_uwb_scan_interval_ms));
        return;
    }
    anchor_set_uwb_busy(true);
    uwb_window_start_ms = k_uptime_get();
    high_debug_log_event("UWB_RX_START",
                         "mode=anchor_wake_scan interval_ms=%u rx_window_ms=%u",
                         anchor_uwb_scan_interval_ms,
                         ANCHOR_UWB_SCAN_RX_MS);

    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_receive_frame_detailed(ANCHOR_UWB_SCAN_RX_MS,
                                                    frame,
                                                    sizeof(frame),
                                                    &frame_len,
                                                    &quality,
                                                    NULL,
                                                    &rx_failure);
    }
    preamble_detected = ret == 0 || rx_failure_detected_preamble(rx_failure);
    uwb_anchor_note_idle_scan(&anchor_uwb_session,
                              ANCHOR_UWB_STARTUP_US,
                              ANCHOR_UWB_PLL_US,
                              ANCHOR_UWB_SCAN_RX_US,
                              preamble_detected);

    if (ret == 0) {
        struct uwb_wake_claim_frame claim;
        int decode_ret;

        high_debug_log_event("UWB_RX_DONE",
                             "mode=anchor_wake_scan frame_len=%u quality=%u rx_failure=%s",
                             (unsigned int)frame_len,
                             quality,
                             rx_failure_name(rx_failure));
        decode_ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (decode_ret == PROTO_OK) {
            if (anchor_handle_uwb_claim(&claim,
                                        quality,
                                        k_uptime_get(),
                                        &retained_sleep_us)) {
                uwb_anchor_abort_epoch(&anchor_uwb_session);
            }
        } else {
            bool valid_mesh_frame = false;

            if (mesh_queue_from_frame(frame, frame_len, quality, &valid_mesh_frame, NULL) ||
                valid_mesh_frame) {
                goto scan_complete;
            }
            {
                enum uwb_wake_decode_failure failure = wake_failure_from_proto_ret(decode_ret);

                uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                    failure);
                uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
                next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
                LOG_WRN("anchor false UWB wake cooldown: wake_decode_ret=%d reason=%s frame_len=%u quality=%u cooldown_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
            }
        }
    } else if (ret != -ETIMEDOUT) {
        enum uwb_wake_decode_failure failure = wake_failure_from_rx(rx_failure);

        uwb_anchor_note_wake_decode_failure(&anchor_uwb_session, failure);
        uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
        next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
        LOG_WRN("anchor UWB wake scan failure cooldown: ret=%d rx_failure=%s reason=%s cooldown_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                ret,
                rx_failure_name(rx_failure),
                wake_decode_failure_name(failure),
                next_scan_delay_ms,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures);
    }
    if (ret == -ETIMEDOUT) {
        high_debug_log_event("UWB_RX_TIMEOUT",
                             "mode=anchor_wake_scan rx_failure=%s preamble=%u",
                             rx_failure_name(rx_failure),
                             preamble_detected ? 1u : 0u);
    }

scan_complete:
    (void)dwm3000_driver_standby();
    anchor_note_uwb_awake_since(
        uwb_window_start_ms,
        u32_saturating_add(ANCHOR_UWB_IDLE_SCAN_AWAKE_US, retained_sleep_us));
    radio_guard_uwb_stop();
    anchor_set_uwb_busy(false);
    report_tx_schedule(0u);
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
    (void)k_work_reschedule(&anchor_uwb_scan_work, K_MSEC(next_scan_delay_ms));
}

static int anchor_start_uwb_scan(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    (void)k_work_reschedule(&anchor_uwb_scan_work, K_NO_WAIT);
    LOG_INF("anchor low-duty UWB wake scan scheduled: interval_ms=%u rx_window_ms=%u status-poll slot=%u",
            anchor_uwb_scan_interval_ms,
            ANCHOR_UWB_SCAN_RX_MS,
            local_uwb_anchor_slot());
    return 0;
}

static void mesh_preempt_for_click_event(void)
{
    struct mesh_outbound pending_report = {0};
    bool requeue_report = false;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        if (mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
            mesh_runtime.pending.packet.src_id == DEVICE_ID) {
            pending_report.packet = mesh_runtime.pending.packet;
            pending_report.payload_len = mesh_runtime.pending.payload_len;
            if (pending_report.payload_len > 0u) {
                memcpy(pending_report.payload,
                       mesh_runtime.pending.payload,
                       pending_report.payload_len);
            }
            requeue_report = true;
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        LOG_INF("anchor click discovery preempted active mesh TX");
    }

    k_msgq_purge(&mesh_rx_msgq);
    if (requeue_report) {
        if (k_msgq_put(&report_tx_msgq, &pending_report, K_NO_WAIT) != 0) {
            LOG_WRN("preempted click report could not be requeued");
        }
    }
}

static void mesh_schedule_tx_timeout(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_relay_tx_active(&mesh_runtime)) {
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        return;
    }

    deadline = mesh_runtime.pending.gateway_ack_deadline_ms;
    delay_ms = uptime_ms_until_deadline(now, deadline);
    (void)k_work_reschedule(&mesh_tx_timeout_work, K_MSEC(delay_ms));
}

static bool mesh_role_uses_uwb_rx(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY;
}

static uint32_t mesh_uwb_rx_window_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_WINDOW_MS :
           UWB_MESH_ANCHOR_RX_WINDOW_MS;
}

static uint32_t mesh_uwb_rx_idle_delay_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_IDLE_MS :
           UWB_MESH_ANCHOR_RX_INTERVAL_MS;
}

static bool mesh_select_channel9_rx_event(uint32_t now_ms,
                                          struct mesh_event_plan *selected_plan,
                                          uint64_t *selected_peer,
                                          uint8_t *selected_index)
{
    struct mesh_channel5_requirements requirements;
    bool selected = false;

    if (selected_plan == NULL || selected_peer == NULL || selected_index == NULL) {
        return false;
    }

    (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
    mesh_fill_channel5_requirements(&requirements);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_plan plan = {0};
        int ret;

        if (!entry->valid) {
            continue;
        }
        ret = mesh_event_plan_channel9(&entry->timing, &requirements, now_ms, &plan);
        if (ret != PROTO_OK) {
            continue;
        }
        mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        if (plan.action != MESH_EVENT_PLAN_START &&
            plan.action != MESH_EVENT_PLAN_CLIP) {
            continue;
        }
        if (!selected || uptime_deadline_reached(selected_plan->start_ms, plan.start_ms)) {
            *selected_plan = plan;
            *selected_peer = entry->next_hop_id;
            *selected_index = i;
            selected = true;
        }
    }
    return selected;
}

static uint32_t mesh_next_channel9_rx_delay_ms(uint32_t now_ms)
{
    uint32_t delay_ms = mesh_uwb_rx_idle_delay_ms();

    (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        uint32_t candidate_delay_ms;

        if (!entry->valid || !mesh_event_timing_usable(&entry->timing, now_ms)) {
            continue;
        }
        candidate_delay_ms = uptime_ms_until_deadline(now_ms, entry->timing.next_event_time_ms);
        if (candidate_delay_ms < delay_ms) {
            delay_ms = candidate_delay_ms;
        }
    }
    return delay_ms;
}

static void mesh_schedule_uwb_rx(uint32_t delay_ms)
{
    if (!mesh_role_uses_uwb_rx()) {
        return;
    }

    mesh_uwb_rx_active = true;
    (void)k_work_reschedule(&mesh_uwb_rx_work, K_MSEC(delay_ms));
}

static void mesh_stop_role_scan(void)
{
    if (mesh_uwb_rx_active) {
        (void)k_work_cancel_delayable(&mesh_uwb_rx_work);
        mesh_uwb_rx_active = false;
    }
}

static void mesh_restart_role_scan(void)
{
    int ret;

    if (DEVICE_ROLE == ROLE_ANCHOR && !anchor_uwb_busy) {
        ret = mesh_start_uwb_rx("anchor mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart anchor UWB mesh RX: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = mesh_start_uwb_rx("gateway mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart gateway UWB mesh RX: %d", ret);
        }
    }
}

static int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    int64_t uwb_window_start_ms = -1;
    int ret;

    ret = uwb_mesh_frame_encode(NETWORK_ID,
                                DEVICE_ID,
                                out->next_hop_id,
                                &out->packet,
                                out->payload,
                                frame,
                                sizeof(frame),
                                &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        return -EINVAL;
    }

    mesh_stop_role_scan();
    ret = radio_guard_uwb_start("mesh UWB TX");
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = out->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
          dwm3000_driver_configure_mesh_payload_mode() :
          dwm3000_driver_configure_default();
    if (out->radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    }
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_MESH_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_standby();
    anchor_note_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                out->packet.msg_type,
                (unsigned long long)out->next_hop_id,
                (unsigned int)frame_len,
                ret);
        return ret;
    }

    HIGH_DEBUG_COUNTER_INC(mesh_tx);
    if (out->packet.msg_type == MSG_GATEWAY_ACK) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_TX",
                             "dst=0x%016llx next=0x%016llx seq=%u channel=%u",
                             (unsigned long long)out->packet.dst_id,
                             (unsigned long long)out->next_hop_id,
                             out->packet.seq,
                             out->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                             UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT);
    }
    high_debug_log_event("MESH_TX",
                         "reason=%s msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u channel=%u frame_len=%u",
                         reason,
                         out->packet.msg_type,
                         (unsigned long long)out->packet.src_id,
                         (unsigned long long)out->packet.dst_id,
                         (unsigned long long)out->next_hop_id,
                         out->packet.seq,
                         out->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                         UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
                         (unsigned int)frame_len);
    LOG_INF("mesh UWB TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u channel=%u frame_len=%u",
            reason,
            out->packet.msg_type,
            (unsigned long long)out->packet.src_id,
            (unsigned long long)out->packet.dst_id,
            (unsigned long long)out->next_hop_id,
            out->packet.seq,
            out->packet.ttl,
            out->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
            UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
            (unsigned int)frame_len);
    return 0;
}

static int mesh_request_route(uint64_t target_id, const char *reason)
{
    struct mesh_outbound route_req;
    int ret;

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    ret = mesh_relay_build_route_request(&mesh_runtime,
                                         target_id,
                                         &route_req,
                                         k_uptime_get_32());
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("mesh route discovery request: target=0x%016llx reason=%s",
            (unsigned long long)target_id,
            reason);
    return mesh_send_outbound(&route_req, "route-request");
}

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_GATEWAY_ACK:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements)
{
    uint32_t now_ms;

    if (requirements == NULL) {
        return;
    }

    memset(requirements, 0, sizeof(*requirements));
    requirements->retune_guard_ms = UWB_SCHEDULE_GUARD_MS;
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        now_ms = k_uptime_get_32();
        requirements->next_required_scan_start_ms = now_ms + anchor_uwb_scan_interval_ms;
        requirements->click_epoch_active = anchor_uwb_window_active();
        requirements->discovery_active = requirements->click_epoch_active;
        requirements->ranging_active = requirements->click_epoch_active;
    }
}

static int mesh_prepare_event_timing(struct mesh_event_timing *timing, uint32_t now_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = MESH_EVENT_DEFAULT_INTERVAL_MS,
        .event_window_ms = MESH_EVENT_DEFAULT_WINDOW_MS,
        .first_event_time_ms = now_ms + MESH_EVENT_DEFAULT_FIRST_DELAY_MS,
        .guard_ms = MESH_EVENT_DEFAULT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = MESH_EVENT_DEFAULT_MAX_MISSED,
        .supervision_timeout_ms = MESH_EVENT_DEFAULT_SUPERVISION_MS,
    };

    return mesh_event_timing_negotiate(timing, &params, true);
}

static int mesh_send_event_control(uint64_t peer_id,
                                   uint8_t msg_type,
                                   const struct mesh_event_timing *accepted_timing,
                                   bool install_local,
                                   const char *reason)
{
    struct mesh_event_timing timing = {0};
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return -EINVAL;
    }

    if (accepted_timing != NULL) {
        timing = *accepted_timing;
    } else {
        ret = mesh_prepare_event_timing(&timing, now_ms);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }

    ret = mesh_append_event_timing_tlvs_at(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           &timing,
                                           now_ms);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = mesh_init_event_control(&outbound.packet,
                                  msg_type,
                                  DEVICE_ID,
                                  peer_id,
                                  nonzero_uptime_session_id(),
                                  mesh_next_event_control_seq(),
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = peer_id;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    LOG_INF("mesh channel-9 event control TX: msg=0x%02x peer=0x%016llx interval_ms=%u window_ms=%u next_ms=%u reason=%s",
            msg_type,
            (unsigned long long)peer_id,
            timing.event_interval_ms,
            timing.event_window_ms,
            timing.next_event_time_ms,
            reason == NULL ? "event-control" : reason);
    ret = mesh_send_outbound(&outbound, reason == NULL ? "mesh-event-control" : reason);
    if (ret < 0) {
        return ret;
    }
    if (install_local) {
        ret = mesh_relay_set_channel9_timing(&mesh_runtime, peer_id, &timing);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                      timing.next_event_time_ms));
    }
    return 0;
}

static void mesh_propose_event_after_channel5_contact(uint64_t peer_id, const char *reason)
{
    int ret;

    ret = mesh_send_event_control(peer_id,
                                  MSG_MESH_EVENT_PROPOSE,
                                  NULL,
                                  true,
                                  reason);
    if (ret < 0) {
        LOG_WRN("mesh channel-9 event proposal failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-propose" : reason);
    }
}

static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id)
{
    struct mesh_event_timing timing = {0};
    int ret;

    if (packet == NULL) {
        return false;
    }

    if (packet->msg_type != MSG_MESH_EVENT_PROPOSE &&
        packet->msg_type != MSG_MESH_EVENT_ACCEPT &&
        packet->msg_type != MSG_MESH_EVENT_UPDATE &&
        packet->msg_type != MSG_MESH_EVENT_END) {
        return false;
    }
    if (!mesh_id_is_unicast(previous_hop_id)) {
        LOG_WRN("mesh event control ignored without unicast previous hop");
        return true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        mesh_relay_clear_channel9_timing(&mesh_runtime, previous_hop_id);
        LOG_INF("mesh channel-9 event timing cleared for next=0x%016llx",
                (unsigned long long)previous_hop_id);
        return true;
    }

    ret = mesh_event_timing_from_tlvs_at(&timing,
                                         payload,
                                         payload_len,
                                         k_uptime_get_32(),
                                         true);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing parse failed: msg=0x%02x ret=%d",
                packet->msg_type,
                ret);
        return true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        ret = mesh_send_event_control(previous_hop_id,
                                      MSG_MESH_EVENT_ACCEPT,
                                      &timing,
                                      false,
                                      "mesh-event-accept");
        if (ret < 0) {
            LOG_WRN("mesh channel-9 event ACCEPT failed: next=0x%016llx ret=%d",
                    (unsigned long long)previous_hop_id,
                    ret);
            return true;
        }
    }

    ret = mesh_relay_set_channel9_timing(&mesh_runtime, previous_hop_id, &timing);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing install failed: next=0x%016llx ret=%d",
                (unsigned long long)previous_hop_id,
                ret);
        return true;
    }

    LOG_INF("mesh channel-9 event timing installed: next=0x%016llx interval_ms=%u window_ms=%u next_ms=%u",
            (unsigned long long)previous_hop_id,
            timing.event_interval_ms,
            timing.event_window_ms,
            timing.next_event_time_ms);
    mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(), timing.next_event_time_ms));
    return true;
}

static bool mesh_tx_can_wait_for_route(const struct mesh_outbound *out)
{
    if (out == NULL ||
        out->packet.dst_id == MESH_BROADCAST_ID ||
        out->packet.dst_id == DEVICE_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_GATEWAY_ACK:
    case MSG_MESH_DATA:
    case MSG_SELF_TEST_REPORT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static void mesh_store_route_waiting_tx(const struct mesh_outbound *out)
{
    if (!mesh_tx_can_wait_for_route(out)) {
        return;
    }

    mesh_route_waiting_tx = *out;
    mesh_route_waiting_tx_valid = true;
}

static void mesh_clear_route_waiting_tx(const struct proto_packet *packet)
{
    if (!mesh_route_waiting_tx_valid || packet == NULL) {
        return;
    }
    if (mesh_route_waiting_tx.packet.dst_id == packet->dst_id &&
        mesh_route_waiting_tx.packet.session_id == packet->session_id &&
        mesh_route_waiting_tx.packet.seq == packet->seq) {
        mesh_route_waiting_tx_valid = false;
    }
}

static void mesh_try_route_waiting_tx(void)
{
    struct mesh_outbound pending;
    int ret;

    if (!mesh_route_waiting_tx_valid || mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    pending = mesh_route_waiting_tx;
    ret = mesh_start_tracked_tx(&pending, "route-discovered-packet");
    if (ret == 0) {
        mesh_route_waiting_tx_valid = false;
    } else if (ret == -EHOSTUNREACH) {
        (void)mesh_request_route(pending.packet.dst_id, "route-waiting-packet");
    }
}

static int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
{
    struct mesh_outbound tx;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint32_t now_ms;
    uint32_t channel9_event_start_ms = 0u;
    uint64_t channel9_next_hop_id = 0u;
    bool channel9_success_pending = false;
    bool channel9_report_latency_pending = false;
    int ret;

    if (mesh_packet_prefers_channel9(&out->packet)) {
        now_ms = k_uptime_get_32();
        mesh_fill_channel5_requirements(&requirements);
        ret = mesh_relay_start_channel9_tx(&mesh_runtime,
                                           &out->packet,
                                           out->payload,
                                           out->payload_len,
                                           &requirements,
                                           now_ms,
                                           &plan,
                                           &tx);
        if (ret != PROTO_ERR_NOT_FOUND) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        if (ret == PROTO_OK) {
            channel9_success_pending = true;
            channel9_report_latency_pending = out->packet.msg_type == MSG_CLICK_REPORT;
            channel9_event_start_ms = plan.start_ms;
            channel9_next_hop_id = tx.next_hop_id;
            goto send_prepared;
        }
        if (ret == PROTO_ERR_BUSY) {
            return -EBUSY;
        }
        if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
            LOG_WRN("mesh channel-9 timing unavailable for %s; refreshing channel-5 contact: ret=%d",
                    reason,
                    ret);
            mesh_store_route_waiting_tx(out);
            (void)mesh_request_route(out->packet.dst_id, reason);
            return -EHOSTUNREACH;
        }
        LOG_WRN("mesh channel-9 TX rejected for %s: ret=%d", reason, ret);
        return mesh_errno_from_proto(ret);
    }

    ret = mesh_relay_start_tx(&mesh_runtime,
                              &out->packet,
                              out->payload,
                              out->payload_len,
                              k_uptime_get_32(),
                              &tx);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh could not start tracked TX for %s: %d", reason, ret);
        if (ret == PROTO_ERR_NOT_FOUND) {
            mesh_store_route_waiting_tx(out);
            (void)mesh_request_route(out->packet.dst_id, reason);
        }
        return mesh_errno_from_proto(ret);
    }
send_prepared:
    ret = mesh_send_outbound(&tx, reason);
    if (ret < 0) {
        mesh_relay_cancel_tx(&mesh_runtime);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            mesh_relay_note_delivery_failure(&mesh_runtime, out->packet.dst_id);
            mesh_store_route_waiting_tx(out);
            (void)mesh_request_route(out->packet.dst_id, reason);
        }
        return ret;
    }
    mesh_relay_note_tx_sent(&mesh_runtime, &tx, k_uptime_get_32());
    if (channel9_success_pending) {
        mesh_relay_note_channel9_success(&mesh_runtime,
                                         channel9_next_hop_id,
                                         channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms > now_ms ?
                                           channel9_event_start_ms - now_ms : 0u);
        }
    }
    mesh_schedule_tx_timeout();
    return 0;
}

static bool anchor_uwb_window_active(void)
{
    k_spinlock_key_t key;
    bool busy;

    key = k_spin_lock(&anchor_uwb_lock);
    busy = anchor_uwb_busy;
    k_spin_unlock(&anchor_uwb_lock, key);
    return busy;
}

static void report_tx_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        (void)k_work_reschedule(&report_tx_work, K_MSEC(delay_ms));
    }
}

static void report_tx_work_handler(struct k_work *work)
{
    struct mesh_outbound outbound;
    struct mesh_outbound dropped;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (anchor_uwb_window_active() || mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    ret = k_msgq_peek(&report_tx_msgq, &outbound);
    if (ret != 0) {
        return;
    }

    ret = mesh_start_tracked_tx(&outbound, "queued-click-report");
    if (ret == 0) {
        (void)k_msgq_get(&report_tx_msgq, &dropped, K_NO_WAIT);
        return;
    }

    if (ret == -EHOSTUNREACH || ret == -EBUSY) {
        LOG_WRN("queued gateway-bound report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    (void)k_msgq_get(&report_tx_msgq, &dropped, K_NO_WAIT);
    LOG_WRN("queued gateway-bound report dropped after permanent TX error: ret=%d", ret);
}

static int queue_anchor_report(const struct mesh_outbound *outbound)
{
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }

    ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
    if (ret != 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("anchor report queue full; gateway-bound report dropped");
        return -ENOSPC;
    }

    high_debug_log_event("ANCHOR_REPORT_QUEUE",
                         "msg=0x%02x dst=0x%016llx seq=%u queue_depth=%u",
                         outbound->packet.msg_type,
                         (unsigned long long)outbound->packet.dst_id,
                         outbound->packet.seq,
                         k_msgq_num_used_get(&report_tx_msgq));
    LOG_INF("anchor queued gateway-bound report: msg=0x%02x queue_depth=%u",
            outbound->packet.msg_type,
            k_msgq_num_used_get(&report_tx_msgq));
    if (!anchor_uwb_window_active()) {
        report_tx_schedule(0u);
    }
    return 0;
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result)
{
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        struct mesh_outbound gateway_ack = result->gateway_ack;
        struct mesh_channel5_requirements requirements;
        struct mesh_event_plan plan = {0};
        uint32_t now_ms;
        int ret;

        mesh_fill_channel5_requirements(&requirements);
        now_ms = k_uptime_get_32();
        (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
        ret = mesh_relay_require_channel9_event(&mesh_runtime,
                                                gateway_ack.next_hop_id,
                                                &requirements,
                                                now_ms,
                                                &plan);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        if (ret != PROTO_OK) {
            LOG_WRN("gateway ACK falling back to channel-5 contact: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack.next_hop_id,
                    ret);
            gateway_ack.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
            if (mesh_send_outbound(&gateway_ack, "gateway-ack-channel5") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, &gateway_ack, k_uptime_get_32());
            } else {
                mesh_store_route_waiting_tx(&gateway_ack);
                (void)mesh_request_route(gateway_ack.packet.dst_id,
                                         "gateway-ack-channel9-refresh");
            }
        } else {
            gateway_ack.radio_channel = MESH_EVENT_CHANNEL;
            if (mesh_send_outbound(&gateway_ack, "gateway-ack") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, &gateway_ack, k_uptime_get_32());
                mesh_relay_note_channel9_success(&mesh_runtime,
                                                 gateway_ack.next_hop_id,
                                                 plan.start_ms);
            }
        }
    }
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        if (result->forward.packet.dst_id == MESH_BROADCAST_ID) {
            (void)mesh_send_outbound(&result->forward, "broadcast-forward");
        } else {
            (void)mesh_start_tracked_tx(&result->forward, "forward");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_send_outbound(&result->route_request, "route-request-forward");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) {
        if (mesh_send_outbound(&result->route_reply, "route-reply") == 0 &&
            mesh_id_is_unicast(result->route_reply.next_hop_id)) {
            mesh_propose_event_after_channel5_contact(result->route_reply.next_hop_id,
                                                      "route-reply-event-propose");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        struct mesh_outbound retransmit = result->retransmit;
        struct mesh_event_plan plan = {0};
        bool channel9_replanned = false;
        int ret = PROTO_OK;

        if (mesh_packet_prefers_channel9(&retransmit.packet)) {
            struct mesh_channel5_requirements requirements;
            uint32_t now_ms = k_uptime_get_32();

            mesh_fill_channel5_requirements(&requirements);
            (void)mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);
            ret = mesh_relay_require_channel9_event(&mesh_runtime,
                                                    retransmit.next_hop_id,
                                                    &requirements,
                                                    now_ms,
                                                    &plan);
            if (ret != PROTO_ERR_STALE) {
                mesh_event_note_plan_action(&mesh_event_stats, plan.action);
            }
            if (ret == PROTO_OK) {
                retransmit.radio_channel = MESH_EVENT_CHANNEL;
                channel9_replanned = true;
            } else {
                LOG_WRN("mesh retransmit deferred until channel-9 timing is refreshed: msg=0x%02x dst=0x%016llx ret=%d",
                        retransmit.packet.msg_type,
                        (unsigned long long)retransmit.packet.dst_id,
                        ret);
                mesh_store_route_waiting_tx(&retransmit);
                (void)mesh_request_route(retransmit.packet.dst_id,
                                         "retransmit-channel9-refresh");
            }
        }
        if (ret == PROTO_OK && mesh_send_outbound(&retransmit, "retransmit") == 0) {
            HIGH_DEBUG_COUNTER_INC(mesh_retry);
            mesh_relay_note_tx_sent(&mesh_runtime, &retransmit, k_uptime_get_32());
            if (channel9_replanned) {
                mesh_relay_note_channel9_success(&mesh_runtime,
                                                 retransmit.next_hop_id,
                                                 plan.start_ms);
            }
        }
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        LOG_WRN("mesh route discovery needed after delivery failure");
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY) {
        const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);

        LOG_INF("mesh reactive route ready");
        if (selected != NULL) {
            mesh_propose_event_after_channel5_contact(selected->next_hop_id,
                                                      "route-ready-event-propose");
        }
        mesh_try_route_waiting_tx();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        LOG_INF("mesh pending TX gateway acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !mesh_relay_tx_active(&mesh_runtime)) {
        report_tx_schedule(0u);
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        mesh_try_route_waiting_tx();
    }
}

static void mesh_rx_work_handler(struct k_work *work)
{
    struct mesh_rx_pending pending;
    struct mesh_relay_result result;
    int ret;

    ARG_UNUSED(work);

    while (k_msgq_get(&mesh_rx_msgq, &pending, K_NO_WAIT) == 0) {
        ret = mesh_relay_handle_rx(&mesh_runtime,
                                   &pending.packet,
                                   pending.payload,
                                   pending.payload_len,
                                   pending.previous_hop_id,
                                   pending.link_quality,
                                   k_uptime_get_32(),
                                   &result);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh RX rejected: %d", ret);
            continue;
        }

        LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx actions=0x%08x status=%d",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id,
                (unsigned long long)pending.previous_hop_id,
                result.actions,
                result.status);
        if (result.status == PROTO_ERR_NOT_FOUND &&
            pending.packet.dst_id != DEVICE_ID &&
            pending.packet.dst_id != MESH_BROADCAST_ID) {
            (void)mesh_request_route(pending.packet.dst_id, "rx-forward-miss");
        }
        mesh_handle_result_actions(&result);
        if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_handle_event_control(&pending.packet,
                                      pending.payload,
                                      pending.payload_len,
                                      pending.previous_hop_id)) {
            continue;
        }
        if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            DEVICE_ROLE == ROLE_GATEWAY) {
            if (pending.packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending.packet,
                                            pending.payload,
                                            pending.payload_len);
            }
            gateway_handle_survey_reach_report(&pending.packet,
                                               pending.payload,
                                               pending.payload_len);
            ret = gateway_emit_serial_packet(&pending.packet,
                                             pending.payload,
                                             pending.payload_len);
            if (ret < 0) {
                LOG_WRN("gateway USB COBS frame not emitted: %d", ret);
            }
        } else if ((result.actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
                   DEVICE_ROLE == ROLE_ANCHOR) {
            anchor_handle_local_command(&pending.packet, pending.payload, pending.payload_len);
            anchor_handle_survey_reach_request(&pending.packet,
                                               pending.payload,
                                               pending.payload_len,
                                               pending.previous_hop_id,
                                               pending.link_quality);
            anchor_handle_survey_pair_prepare(&pending.packet,
                                              pending.payload,
                                              pending.payload_len);
        }
    }
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result result;
    struct mesh_outbound pending_waiting = {0};
    bool pending_route_waiting = false;
    struct mesh_outbound pending_report = {0};
    bool pending_anchor_report = false;

    ARG_UNUSED(work);

    if (DEVICE_ROLE == ROLE_ANCHOR &&
        mesh_relay_tx_active(&mesh_runtime) &&
        mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
        mesh_runtime.pending.packet.src_id == DEVICE_ID) {
        pending_report.packet = mesh_runtime.pending.packet;
        pending_report.payload_len = mesh_runtime.pending.payload_len;
        if (pending_report.payload_len > 0u) {
            memcpy(pending_report.payload,
                   mesh_runtime.pending.payload,
                   pending_report.payload_len);
        }
        pending_anchor_report = true;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        pending_waiting.packet = mesh_runtime.pending.packet;
        pending_waiting.payload_len = mesh_runtime.pending.payload_len;
        pending_waiting.radio_channel = mesh_runtime.pending.radio_channel;
        pending_waiting.next_hop_id = mesh_runtime.pending.next_hop_id;
        if (pending_waiting.payload_len > 0u) {
            memcpy(pending_waiting.payload,
                   mesh_runtime.pending.payload,
                   pending_waiting.payload_len);
        }
        pending_route_waiting = mesh_tx_can_wait_for_route(&pending_waiting);
    }

    if (mesh_relay_tick(&mesh_runtime, k_uptime_get_32(), &result) != PROTO_OK) {
        return;
    }
    mesh_handle_result_actions(&result);

    if (pending_route_waiting &&
        (result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        mesh_store_route_waiting_tx(&pending_waiting);
        (void)mesh_request_route(pending_waiting.packet.dst_id, "pending-tx-timeout");
    }

    if (pending_anchor_report &&
        (result.actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        LOG_WRN("requeueing click report after mesh route loss");
        (void)queue_anchor_report(&pending_report);
    }
}

static bool mesh_queue_from_frame(const uint8_t *frame,
                                  size_t frame_len,
                                  uint8_t link_quality,
                                  bool *valid_mesh_frame,
                                  uint64_t *previous_hop_id)
{
    struct mesh_frame_parse_context context = {0};
    struct mesh_rx_pending pending = {0};
    int ret;

    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = false;
    }
    if (frame == NULL || frame_len == 0u) {
        return false;
    }
    if (uwb_mesh_frame_decode(frame,
                              frame_len,
                              NETWORK_ID,
                              DEVICE_ID,
                              &context.previous_hop_id,
                              &context.packet,
                              context.payload,
                              sizeof(context.payload),
                              &context.payload_len) != PROTO_OK ||
        context.payload_len > UINT8_MAX) {
        return false;
    }
    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = true;
    }
    if (previous_hop_id != NULL) {
        *previous_hop_id = context.previous_hop_id;
    }

    pending.packet = context.packet;
    if (context.payload_len > 0u) {
        memcpy(pending.payload, context.payload, context.payload_len);
    }
    pending.payload_len = (uint8_t)context.payload_len;
    pending.previous_hop_id = context.previous_hop_id;
    pending.link_quality = link_quality;

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB RX queue full; dropped msg=0x%02x src=0x%016llx dst=0x%016llx",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id);
        return false;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        uwb_anchor_note_mesh_packet(&anchor_uwb_session);
    }
    HIGH_DEBUG_COUNTER_INC(mesh_rx);
    if (pending.packet.msg_type == MSG_GATEWAY_ACK) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_RX",
                             "src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                             (unsigned long long)pending.packet.src_id,
                             (unsigned long long)pending.packet.dst_id,
                             (unsigned long long)pending.previous_hop_id,
                             pending.packet.seq,
                             pending.link_quality);
    }
    high_debug_log_event("MESH_RX",
                         "msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u queue_depth=%u",
                         pending.packet.msg_type,
                         (unsigned long long)pending.packet.src_id,
                         (unsigned long long)pending.packet.dst_id,
                         (unsigned long long)pending.previous_hop_id,
                         pending.packet.seq,
                         pending.link_quality,
                         k_msgq_num_used_get(&mesh_rx_msgq));
    LOG_INF("mesh UWB RX queued: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u queue_depth=%u role=%s",
            pending.packet.msg_type,
            (unsigned long long)pending.packet.src_id,
            (unsigned long long)pending.packet.dst_id,
            (unsigned long long)pending.previous_hop_id,
            pending.link_quality,
            k_msgq_num_used_get(&mesh_rx_msgq),
            role_name());

    (void)k_work_submit(&mesh_rx_work);
    return true;
}

static void mesh_uwb_rx_work_handler(struct k_work *work)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint64_t channel9_peer_id = 0u;
    uint64_t rx_previous_hop_id = 0u;
    struct mesh_event_plan channel9_plan = {0};
    uint32_t window_ms;
    uint32_t observed_packet_ms = 0u;
    uint8_t channel9_timing_index = 0u;
    int64_t uwb_window_start_ms = -1;
    bool channel9_event;
    int ret;

    ARG_UNUSED(work);

    if (!mesh_role_uses_uwb_rx()) {
        mesh_uwb_rx_active = false;
        return;
    }
    if ((DEVICE_ROLE == ROLE_ANCHOR && anchor_uwb_window_active()) ||
        mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_uwb_rx(mesh_uwb_rx_idle_delay_ms());
        return;
    }

    ret = radio_guard_uwb_start("mesh UWB RX");
    if (ret < 0) {
        mesh_schedule_uwb_rx(mesh_uwb_rx_idle_delay_ms());
        return;
    }

    channel9_event = mesh_select_channel9_rx_event(k_uptime_get_32(),
                                                   &channel9_plan,
                                                   &channel9_peer_id,
                                                   &channel9_timing_index);
    window_ms = channel9_event ? channel9_plan.window_ms : mesh_uwb_rx_window_ms();
    uwb_window_start_ms = k_uptime_get();
    ret = channel9_event ?
          dwm3000_driver_configure_mesh_payload_mode() :
          dwm3000_driver_configure_default();
    if (channel9_event) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    }
    if (ret == 0) {
        ret = dwm3000_driver_receive_frame(window_ms,
                                           frame,
                                           sizeof(frame),
                                           &frame_len,
                                           &quality,
                                           NULL);
        if (ret == 0) {
            observed_packet_ms = k_uptime_get_32();
        }
    }
    (void)dwm3000_driver_standby();
    anchor_note_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();

    if (ret == 0) {
        bool valid_mesh_frame = false;

        if (mesh_queue_from_frame(frame,
                                  frame_len,
                                  quality,
                                  &valid_mesh_frame,
                                  &rx_previous_hop_id)) {
            if (channel9_event && rx_previous_hop_id == channel9_peer_id) {
                mesh_relay_note_channel9_rx(&mesh_runtime,
                                            rx_previous_hop_id,
                                            channel9_plan.start_ms,
                                            observed_packet_ms);
            }
            LOG_DBG("mesh UWB RX frame accepted: len=%u", (unsigned int)frame_len);
        } else if (!valid_mesh_frame) {
            LOG_DBG("mesh UWB RX ignored non-mesh frame: len=%u quality=%u",
                    (unsigned int)frame_len,
                    quality);
        }
    } else if (ret != -ETIMEDOUT) {
        LOG_WRN("mesh UWB RX failed: ret=%d role=%s", ret, role_name());
    } else if (channel9_event && channel9_timing_index < MESH_RELAY_EVENT_TIMINGS &&
               mesh_runtime.event_timings[channel9_timing_index].valid) {
        mesh_relay_note_channel9_missed(&mesh_runtime,
                                        channel9_peer_id,
                                        &mesh_event_stats);
    }

    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
}

static int mesh_start_uwb_rx(const char *reason)
{
    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }

    mesh_schedule_uwb_rx(0u);
    LOG_INF("mesh UWB RX scheduled: role=%s window_ms=%u idle_ms=%u reason=%s",
            role_name(),
            mesh_uwb_rx_window_ms(),
            mesh_uwb_rx_idle_delay_ms(),
            reason == NULL ? "start" : reason);
    return 0;
}

static int build_range_report_samples(uint64_t clicker_id,
                                      uint32_t event_seq,
                                      uint32_t burst_id,
                                      const struct dwm3000_range_result *range_result,
                                      const int32_t *distance_samples_mm,
                                      const uint8_t *range_round_indices,
                                      const int64_t *sample_sequence_start_ms,
                                      uint16_t sample_count)
{
    struct range_report_fields fields;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t encoded[PACKET_MAX_LEN];
    uint16_t sample_index = 0u;
    uint16_t packet_index = 0u;
    bool fragmented;
    int ret;

    if (range_result == NULL ||
        clicker_id == 0u ||
        event_seq == 0u ||
        range_result->responder_id == 0u ||
        (sample_count > 0u &&
         (distance_samples_mm == NULL ||
          range_round_indices == NULL ||
          sample_sequence_start_ms == NULL)) ||
        sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return -EINVAL;
    }
    fragmented = sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;

    do {
        struct range_report_diagnostics diagnostics;
        size_t payload_len = 0u;
        size_t encoded_len = 0u;
        uint16_t chunk_count = 0u;
        uint16_t chunk_cap = 0u;
        uint16_t packet_seq;
        uint64_t sequence_start_timestamps_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET] = {0};
        int64_t range_local_ms;

        if (sample_count > 0u) {
            chunk_cap = fragmented ?
                        RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT :
                        RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;
            chunk_count = MIN(chunk_cap, sample_count - sample_index);
        }

build_payload:
        payload_len = 0u;
        encoded_len = 0u;
        memset(&fields, 0, sizeof(fields));
        if (chunk_count > 0u) {
            uint32_t first_sync_age_ms = 0u;

            range_local_ms = sample_sequence_start_ms[sample_index];
            if (range_local_ms < 0) {
                range_local_ms = k_uptime_get();
            }
            for (uint16_t i = 0u; i < chunk_count; i++) {
                uint32_t sample_sync_age_ms = 0u;
                int64_t sample_local_ms = sample_sequence_start_ms[sample_index + i];

                if (sample_local_ms < 0) {
                    sample_local_ms = k_uptime_get();
                }
                anchor_sequence_timestamp_at(sample_local_ms,
                                             &sequence_start_timestamps_ms[i],
                                             &sample_sync_age_ms);
                if (i == 0u) {
                    first_sync_age_ms = sample_sync_age_ms;
                }
            }
            fields.timestamp_ms = sequence_start_timestamps_ms[0];
            fields.time_sync_age_ms = first_sync_age_ms;
        } else {
            range_local_ms = range_result->exchange_started ?
                             range_result->exchange_start_ms :
                             k_uptime_get();
            anchor_sequence_timestamp_at(range_local_ms,
                                         &fields.timestamp_ms,
                                         &fields.time_sync_age_ms);
        }

        fields.clicker_id = clicker_id;
        fields.anchor_id = range_result->responder_id;
        fields.event_seq = event_seq;
        fields.distance_mm = range_result->distance_mm;
        fields.quality = range_result->quality;
        fields.rsl_dbm = range_result->rsl_dbm;
        fields.cir_sample = range_result->cir_sampled ? range_result->cir_sample : NULL;
        fields.range_status = range_result->status;
        fields.distance_samples_mm = chunk_count > 0u ? &distance_samples_mm[sample_index] : NULL;
        fields.range_round_indices = chunk_count > 0u ?
                                     &range_round_indices[sample_index] :
                                     NULL;
        fields.sequence_start_timestamps_ms = chunk_count > 0u ?
                                              sequence_start_timestamps_ms :
                                              NULL;
        fields.sample_index = sample_index;
        fields.sample_count = sample_count;
        fields.distance_sample_count = chunk_count;
        fields.omit_rsl = packet_index != 0u;
        fields.omit_cir = packet_index != 0u;
        fragmented = sample_count > chunk_count || packet_index != 0u;
        if (packet_index == 0u) {
            uint32_t anchor_diag_len = range_result->cir_sampled ?
                                       UWB_CIR_SAMPLE_LEN : 0u;
            uint32_t clicker_diag_len = range_result->clicker_diag_received ?
                                        range_result->clicker_diag_len : 0u;
            uint32_t clicker_diag_copy_len = clicker_diag_len > 15u ?
                                             clicker_diag_len - 15u : 0u;
            uint32_t clicker_diag_raw_len =
                (range_result->clicker_diag_received && clicker_diag_len >= 15u) ?
                range_result->clicker_diag[14] : clicker_diag_copy_len;

            memset(&diagnostics, 0, sizeof(diagnostics));
            diagnostics.status_flags = range_result->cir_sampled ?
                                       RANGE_DIAG_ANCHOR_PRESENT :
                                       RANGE_DIAG_ANCHOR_MISSING;
            diagnostics.status_flags |= range_result->clicker_diag_received ?
                                        RANGE_DIAG_CLICKER_PRESENT :
                                        RANGE_DIAG_CLICKER_MISSING;
            if (range_result->clicker_diag_truncated) {
                diagnostics.status_flags |= RANGE_DIAG_TRUNCATED;
            }
            if (range_result->clicker_diag_dropped) {
                diagnostics.status_flags |= RANGE_DIAG_CAPTURE_FAILED;
            }
            diagnostics.burst_id = burst_id;
            diagnostics.exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US;
            diagnostics.burst_duration_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS;
            diagnostics.uwb_awake_time_us = anchor_uwb_session.diagnostics.awake_time_us;
            diagnostics.diag_bytes_captured = anchor_diag_len + clicker_diag_raw_len;
            diagnostics.diag_bytes_transmitted = anchor_diag_len + clicker_diag_copy_len;
            diagnostics.diag_bytes_truncated = clicker_diag_raw_len > clicker_diag_copy_len ?
                                               clicker_diag_raw_len - clicker_diag_copy_len :
                                               0u;
            diagnostics.diag_frames_dropped = range_result->clicker_diag_dropped ?
                                              1u : 0u;
            diagnostics.report_fragment_count = fragmented ?
                                                (uint16_t)(1u +
                                                           ((sample_count - chunk_count +
                                                             RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT -
                                                             1u) /
                                                            RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT)) :
                                                1u;
            diagnostics.phy_config_id = UWB_CHANNEL_WAKE_CONTACT;
            diagnostics.clicker_diag = range_result->clicker_diag_received ?
                                       range_result->clicker_diag : NULL;
            diagnostics.clicker_diag_len = range_result->clicker_diag_received ?
                                           range_result->clicker_diag_len : 0u;
            diagnostics.anchor_diag = range_result->cir_sampled ?
                                      range_result->cir_sample : NULL;
            diagnostics.anchor_diag_len = range_result->cir_sampled ?
                                          UWB_CIR_SAMPLE_LEN : 0u;
            fields.diagnostics = &diagnostics;
        }

        ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
        if (ret == PROTO_ERR_NO_SPACE && chunk_count > 1u) {
            chunk_count--;
            goto build_payload;
        }
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        packet_seq = (uint16_t)((range_result->seq == 0u ?
                                 (uint16_t)event_seq :
                                 range_result->seq) + packet_index);
        ret = report_init_range_packet(&packet,
                                       range_result->responder_id,
                                       GATEWAY_ID,
                                       event_seq,
                                       packet_seq,
                                       range_result->flags,
                                       (uint8_t)payload_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        ret = proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        LOG_INF("range report ready: clicker=0x%016llx event_seq=%u anchor=0x%016llx distance_mm=%d samples=%u chunk_index=%u chunk_samples=%u first_round=%u quality=%u diagnostic=%u rsl_included=%u packet_len=%u",
                (unsigned long long)clicker_id,
                event_seq,
                (unsigned long long)range_result->responder_id,
                range_result->distance_mm,
                sample_count,
                sample_index,
                chunk_count,
                chunk_count > 0u ? range_round_indices[sample_index] : 0u,
                range_result->quality,
                (range_result->flags & FLAG_DIAGNOSTIC) != 0u ? 1u : 0u,
                packet_index == 0u ? 1u : 0u,
                (unsigned int)encoded_len);

        if (DEVICE_ROLE == ROLE_ANCHOR) {
            struct mesh_outbound outbound = {
                .packet = packet,
                .payload_len = (uint8_t)payload_len,
            };

            memcpy(outbound.payload, payload, payload_len);
            ret = queue_anchor_report(&outbound);
            if (ret < 0) {
                LOG_WRN("click report could not be queued for mesh TX: %d", ret);
                return ret;
            }
        }

        sample_index += chunk_count;
        packet_index++;
    } while (sample_index < sample_count);

    return 0;
}

static uint32_t clicker_bound_delay_ms(uint32_t requested_ms,
                                       int64_t deadline_ms,
                                       uint32_t required_after_delay_ms)
{
    int64_t now_ms = k_uptime_get();
    int64_t latest_start_ms = deadline_ms - required_after_delay_ms - 1;
    int64_t available_ms = latest_start_ms - now_ms;

    if (available_ms <= 0) {
        return 0u;
    }
    if ((uint64_t)requested_ms > (uint64_t)available_ms) {
        return (uint32_t)available_ms;
    }
    return requested_ms;
}

static uint32_t clicker_sleep_bounded(uint32_t requested_ms,
                                      int64_t deadline_ms,
                                      uint32_t required_after_delay_ms)
{
    uint32_t bounded_ms = clicker_bound_delay_ms(requested_ms,
                                                deadline_ms,
                                                required_after_delay_ms);

    if (bounded_ms > 0u) {
        k_msleep(bounded_ms);
    }
    return bounded_ms;
}

#if defined(CONFIG_BT) && DEVICE_ROLE == ROLE_CLICKER
static bool ble_courtesy_init_attempted;
static bool ble_courtesy_available;
static bool ble_courtesy_adv_active;
static bool ble_courtesy_scan_active;
static struct k_spinlock ble_courtesy_lock;
static uint32_t ble_courtesy_higher_wait_ms;
static uint8_t ble_courtesy_adv_data[UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN];
static struct uwb_ble_courtesy_frame ble_courtesy_local;

static int clicker_ble_courtesy_set_scan_channel(void)
{
#if defined(CONFIG_BT_LL_SOFTDEVICE_HEADERS_INCLUDE)
    const sdc_hci_cmd_vs_scan_channel_map_set_t params = {
        .channel_map = {0xffu, 0xffu, 0xffu, 0xffu, 0x3fu},
    };

    return hci_vs_sdc_scan_channel_map_set(&params);
#else
    return -ENOTSUP;
#endif
}

static void clicker_ble_courtesy_note_higher_peer(uint32_t wait_ms)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);

    if (wait_ms > ble_courtesy_higher_wait_ms) {
        ble_courtesy_higher_wait_ms = wait_ms;
    }
    k_spin_unlock(&ble_courtesy_lock, key);
}

static void clicker_ble_courtesy_clear_higher_peer(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);

    ble_courtesy_higher_wait_ms = 0u;
    k_spin_unlock(&ble_courtesy_lock, key);
}

static bool clicker_ble_courtesy_parse_ad(struct bt_data *data, void *user_data)
{
    struct uwb_ble_courtesy_frame peer;
    int cmp;

    ARG_UNUSED(user_data);

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;
    }
    if (uwb_ble_courtesy_decode(data->data, data->data_len, &peer) != PROTO_OK) {
        return true;
    }
    if (peer.network_id != NETWORK_ID || peer.clicker_id == ble_courtesy_local.clicker_id) {
        return false;
    }

    cmp = uwb_claim_precedence_compare(peer.attempt_index,
                                       peer.priority_id,
                                       peer.clicker_id,
                                       peer.click_event_id,
                                       ble_courtesy_local.attempt_index,
                                       ble_courtesy_local.priority_id,
                                       ble_courtesy_local.clicker_id,
                                       ble_courtesy_local.click_event_id);
    if (cmp > 0) {
        uint32_t wait_ms = uwb_ble_courtesy_duration_ms(peer.defer_duration_units);

        clicker_ble_courtesy_note_higher_peer(wait_ms);
        LOG_INF("BLE courtesy saw higher-precedence clicker: peer=%llx event=%u attempt=%u priority=%llx wait_ms=%u",
                (unsigned long long)peer.clicker_id,
                peer.click_event_id,
                peer.attempt_index,
                (unsigned long long)peer.priority_id,
                wait_ms);
    }
    return false;
}

static void clicker_ble_courtesy_scan_cb(const bt_addr_le_t *addr,
                                         int8_t rssi,
                                         uint8_t adv_type,
                                         struct net_buf_simple *buf)
{
    ARG_UNUSED(addr);
    ARG_UNUSED(rssi);
    ARG_UNUSED(adv_type);

    if (!ble_courtesy_scan_active) {
        return;
    }
    bt_data_parse(buf, clicker_ble_courtesy_parse_ad, NULL);
}

static int clicker_ble_courtesy_init_once(void)
{
    int ret;

    if (ble_courtesy_available) {
        return 0;
    }
    if (ble_courtesy_init_attempted) {
        return -ENOTSUP;
    }

    ble_courtesy_init_attempted = true;
    ret = bt_enable(NULL);
    if (ret != 0 && ret != -EALREADY) {
        LOG_WRN("BLE courtesy disabled: bt_enable failed: %d", ret);
        return ret;
    }

    ret = clicker_ble_courtesy_set_scan_channel();
    if (ret != 0) {
        LOG_WRN("BLE courtesy disabled: scan channel 37 map failed: %d", ret);
        return ret;
    }

    ble_courtesy_available = true;
    LOG_INF("BLE courtesy enabled on advertising/scanning channel 37");
    return 0;
}

static int clicker_ble_courtesy_start(uint32_t event_seq,
                                      uint8_t attempt_index,
                                      uint64_t priority_id)
{
    const struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BLE_COURTESY_SCAN_INTERVAL_UNITS,
        .window = BLE_COURTESY_SCAN_WINDOW_UNITS,
        .timeout = 0u,
        .interval_coded = 0u,
        .window_coded = 0u,
    };
    const struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 0u,
        .secondary_max_skip = 0u,
        .options = BT_LE_ADV_OPT_USE_IDENTITY |
                   BT_LE_ADV_OPT_DISABLE_CHAN_38 |
                   BT_LE_ADV_OPT_DISABLE_CHAN_39,
        .interval_min = BLE_COURTESY_ADV_INTERVAL_MIN_UNITS,
        .interval_max = BLE_COURTESY_ADV_INTERVAL_MAX_UNITS,
        .peer = NULL,
    };
    const struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA,
                ble_courtesy_adv_data,
                sizeof(ble_courtesy_adv_data)),
    };
    size_t written = 0u;
    int ret;

    ret = clicker_ble_courtesy_init_once();
    if (ret < 0) {
        return ret;
    }

    ble_courtesy_local.network_id = NETWORK_ID;
    ble_courtesy_local.clicker_id = DEVICE_ID;
    ble_courtesy_local.click_event_id = event_seq;
    ble_courtesy_local.attempt_index = attempt_index;
    ble_courtesy_local.priority_id = priority_id;
    ble_courtesy_local.defer_duration_units =
        uwb_ble_courtesy_duration_units_from_ms(BLE_COURTESY_PEER_FINISH_MS);
    ret = uwb_ble_courtesy_encode(&ble_courtesy_local,
                                  ble_courtesy_adv_data,
                                  sizeof(ble_courtesy_adv_data),
                                  &written);
    if (ret != PROTO_OK || written != sizeof(ble_courtesy_adv_data)) {
        return -EINVAL;
    }

    clicker_ble_courtesy_clear_higher_peer();
    ble_courtesy_scan_active = true;
    ret = bt_le_scan_start(&scan_param, clicker_ble_courtesy_scan_cb);
    if (ret != 0) {
        LOG_WRN("BLE courtesy scan start failed: %d", ret);
        ble_courtesy_scan_active = false;
        return ret;
    }

    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0u);
    if (ret != 0) {
        LOG_WRN("BLE courtesy advertising start failed: %d", ret);
        (void)bt_le_scan_stop();
        ble_courtesy_scan_active = false;
        return ret;
    }
    ble_courtesy_adv_active = true;
    return 0;
}

static uint32_t clicker_ble_courtesy_higher_wait_ms(void)
{
    k_spinlock_key_t key = k_spin_lock(&ble_courtesy_lock);
    uint32_t wait_ms = ble_courtesy_higher_wait_ms;

    k_spin_unlock(&ble_courtesy_lock, key);
    return wait_ms;
}

static void clicker_ble_courtesy_stop(void)
{
    int ret;

    if (ble_courtesy_adv_active) {
        ret = bt_le_adv_stop();
        if (ret != 0 && ret != -EALREADY) {
            LOG_WRN("BLE courtesy advertising stop failed: %d", ret);
        }
        ble_courtesy_adv_active = false;
    }
    if (ble_courtesy_scan_active) {
        ret = bt_le_scan_stop();
        if (ret != 0 && ret != -EALREADY) {
            LOG_WRN("BLE courtesy scan stop failed: %d", ret);
        }
        ble_courtesy_scan_active = false;
    }
}
#else
static int clicker_ble_courtesy_start(uint32_t event_seq,
                                      uint8_t attempt_index,
                                      uint64_t priority_id)
{
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(priority_id);
    return -ENOTSUP;
}

static uint32_t clicker_ble_courtesy_higher_wait_ms(void)
{
    return 0u;
}

static void clicker_ble_courtesy_stop(void)
{
}
#endif

static uint32_t clicker_apply_contention_delay(struct uwb_clicker_session *session,
                                               int64_t click_deadline_ms)
{
    uint32_t requested_ms;
    uint32_t delay_ms;

    if (session == NULL) {
        return 0u;
    }

    requested_ms = uwb_clicker_contention_delay_ms(session->attempt_index,
                                                  sys_rand32_get());
    delay_ms = clicker_sleep_bounded(requested_ms,
                                     click_deadline_ms,
                                     WAKE_ADV_MS);
    uwb_clicker_note_contention_delay(session, delay_ms);
    LOG_INF("clicker contention delay: attempt=%u requested_ms=%u applied_ms=%u",
            session->attempt_index,
            requested_ms,
            delay_ms);
    return delay_ms;
}

static uint32_t clicker_apply_retry_delay(struct uwb_clicker_session *session,
                                          int64_t click_deadline_ms)
{
    uint32_t delay_ms;

    if (session == NULL) {
        return 0u;
    }

    delay_ms = clicker_sleep_bounded(UWB_RETRY_BASE_DELAY_MS,
                                     click_deadline_ms,
                                     WAKE_ADV_MS);
    uwb_clicker_note_retry_delay(session, delay_ms);
    LOG_INF("clicker retry base delay: attempt=%u requested_ms=%u applied_ms=%u",
            session->attempt_index,
            UWB_RETRY_BASE_DELAY_MS,
            delay_ms);
    return delay_ms;
}

static uint32_t clicker_sleep_until_ble_or_timeout(uint32_t sleep_ms, int64_t deadline_ms)
{
    uint32_t remaining_ms = sleep_ms;

    while (remaining_ms > 0u && k_uptime_get() < deadline_ms) {
        uint32_t step_ms = MIN(remaining_ms, BLE_COURTESY_POLL_SLEEP_MS);
        int64_t deadline_remaining_ms = deadline_ms - k_uptime_get();
        uint32_t wait_ms = clicker_ble_courtesy_higher_wait_ms();

        if (wait_ms > 0u) {
            return wait_ms;
        }
        if (deadline_remaining_ms <= 0) {
            break;
        }
        step_ms = MIN(step_ms, (uint32_t)deadline_remaining_ms);
        if (step_ms == 0u) {
            break;
        }
        k_msleep(step_ms);
        remaining_ms -= step_ms;
    }
    return clicker_ble_courtesy_higher_wait_ms();
}

static int clicker_sample_uwb_gate(struct uwb_clicker_session *session,
                                   uint32_t listen_ms,
                                   uint32_t *uwb_restart_wait_ms,
                                   uint32_t *sample_count,
                                   uint32_t *activity_count,
                                   uint8_t *quiet_samples)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    uint16_t relevant_wait_ms = 0u;
    uint8_t frame_type = 0u;
    bool relevant_activity_detected = false;
    int ret;

    ret = dwm3000_driver_receive_frame(listen_ms,
                                       frame,
                                       sizeof(frame),
                                       &frame_len,
                                       NULL,
                                       NULL);
    if (ret == 0) {
        int decode_ret = uwb_clicker_decode_politeness_wait(
            session,
            frame,
            frame_len,
            UWB_POLITE_RELEVANT_FRAME_WAIT_MS,
            &relevant_wait_ms,
            &frame_type);

        relevant_activity_detected = decode_ret == PROTO_OK && relevant_wait_ms > 0u;
        if (relevant_activity_detected) {
            if (uwb_restart_wait_ms != NULL) {
                *uwb_restart_wait_ms = relevant_wait_ms;
            }
            ret = CLICKER_POLITENESS_UWB_RESTART;
            LOG_INF("clicker relevant UWB gate packet: type=0x%02x wait_ms=%u frame_len=%u",
                    frame_type,
                    relevant_wait_ms,
                    (unsigned int)frame_len);
        } else if (decode_ret != PROTO_OK) {
            LOG_DBG("clicker ignored undecodable UWB gate packet: type=0x%02x ret=%d frame_len=%u",
                    frame_type,
                    decode_ret,
                    (unsigned int)frame_len);
        } else {
            LOG_DBG("clicker ignored irrelevant UWB gate packet: type=0x%02x frame_len=%u",
                    frame_type,
                    (unsigned int)frame_len);
        }
    } else if (ret != -ETIMEDOUT) {
        LOG_DBG("clicker UWB gate receive sample failed: ret=%d", ret);
        ret = 0;
    } else {
        ret = 0;
    }
    (void)dwm3000_driver_standby();

    if (sample_count != NULL) {
        (*sample_count)++;
    }
    uwb_clicker_note_politeness_sample(session, relevant_activity_detected);

    if (relevant_activity_detected) {
        if (quiet_samples != NULL) {
            *quiet_samples = 0u;
        }
        if (activity_count != NULL) {
            (*activity_count)++;
        }
    } else if (quiet_samples != NULL) {
        (*quiet_samples)++;
    }

    return ret;
}

static int clicker_politeness_phase(struct uwb_clicker_session *session,
                                    uint32_t event_seq,
                                    uint64_t priority_id,
                                    bool use_ble_courtesy,
                                    uint32_t *uwb_restart_wait_ms,
                                    uint32_t *ble_defer_wait_ms)
{
    int64_t deadline_ms = k_uptime_get() + MAX_POLITENESS_WAIT_MS;
    uint8_t quiet_samples = 0u;
    uint32_t sample_count = 0u;
    uint32_t activity_count = 0u;
    bool ble_started = false;
    int64_t ble_courtesy_until_ms = 0;
    int ret;

    if (session == NULL) {
        return -EINVAL;
    }
    if (uwb_restart_wait_ms != NULL) {
        *uwb_restart_wait_ms = 0u;
    }
    if (ble_defer_wait_ms != NULL) {
        *ble_defer_wait_ms = 0u;
    }

    if (use_ble_courtesy) {
        ret = clicker_ble_courtesy_start(event_seq,
                                         session->attempt_index,
                                         priority_id);
        if (ret == 0) {
            ble_started = true;
            ble_courtesy_until_ms = k_uptime_get() + BLE_COURTESY_MIN_WINDOW_MS;
        } else {
            LOG_WRN("BLE courtesy unavailable for attempt=%u: %d",
                    session->attempt_index,
                    ret);
        }
    }

    ret = radio_guard_uwb_start("clicker politeness sniff");
    if (ret < 0) {
        if (ble_started) {
            clicker_ble_courtesy_stop();
        }
        return ret;
    }
    ret = dwm3000_driver_configure_range_mode();
    if (ret < 0) {
        radio_guard_uwb_stop();
        if (ble_started) {
            clicker_ble_courtesy_stop();
        }
        return ret;
    }

    if (ble_started) {
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        uint32_t peer_wait_ms = 0u;

        if (remaining_ms > 0) {
            ret = clicker_sample_uwb_gate(session,
                                         MIN(UWB_POLITE_SAMPLE_RX_MS,
                                             (uint32_t)remaining_ms),
                                         uwb_restart_wait_ms,
                                         &sample_count,
                                         &activity_count,
                                         &quiet_samples);
        }
        if (ret == 0) {
            peer_wait_ms = clicker_ble_courtesy_higher_wait_ms();
        }
        if (peer_wait_ms > 0u) {
            if (ble_defer_wait_ms != NULL) {
                *ble_defer_wait_ms = peer_wait_ms;
            }
            ret = -EAGAIN;
        }
        if (ret == 0 && k_uptime_get() < ble_courtesy_until_ms) {
            int64_t courtesy_remaining_ms = ble_courtesy_until_ms - k_uptime_get();
            int64_t deadline_remaining_ms = deadline_ms - k_uptime_get();

            if (courtesy_remaining_ms > 0 && deadline_remaining_ms > 0 &&
                (peer_wait_ms = clicker_sleep_until_ble_or_timeout(
                    (uint32_t)MIN(courtesy_remaining_ms, deadline_remaining_ms),
                    deadline_ms)) > 0u) {
                if (ble_defer_wait_ms != NULL) {
                    *ble_defer_wait_ms = peer_wait_ms;
                }
                ret = -EAGAIN;
            }
        }
        if (ret == 0 && k_uptime_get() < deadline_ms) {
            remaining_ms = deadline_ms - k_uptime_get();
            ret = clicker_sample_uwb_gate(session,
                                         MIN(UWB_POLITE_SAMPLE_RX_MS,
                                             (uint32_t)remaining_ms),
                                         uwb_restart_wait_ms,
                                         &sample_count,
                                         &activity_count,
                                         &quiet_samples);
        }
    } else {
        while (quiet_samples < UWB_POLITE_REQUIRED_QUIET_SAMPLES &&
               k_uptime_get() < deadline_ms) {
            int64_t sample_start_ms = k_uptime_get();
            int64_t remaining_ms = deadline_ms - k_uptime_get();
            uint32_t listen_ms;

            if (remaining_ms <= 0) {
                break;
            }
            listen_ms = MIN(UWB_POLITE_SAMPLE_RX_MS, (uint32_t)remaining_ms);
            if (listen_ms == 0u) {
                break;
            }

            ret = clicker_sample_uwb_gate(session,
                                         listen_ms,
                                         uwb_restart_wait_ms,
                                         &sample_count,
                                         &activity_count,
                                         &quiet_samples);
            if (ret == CLICKER_POLITENESS_UWB_RESTART) {
                break;
            }

            if (quiet_samples < UWB_POLITE_REQUIRED_QUIET_SAMPLES &&
                k_uptime_get() < deadline_ms) {
                int64_t elapsed_ms = k_uptime_get() - sample_start_ms;
                int64_t sleep_ms = (int64_t)UWB_POLITE_SAMPLE_PERIOD_MS - elapsed_ms;
                int64_t remaining_after_sample_ms = deadline_ms - k_uptime_get();

                if (sleep_ms > 0 && remaining_after_sample_ms > 0) {
                    k_msleep((uint32_t)MIN(sleep_ms, remaining_after_sample_ms));
                }
            }
        }
    }

    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ble_started) {
        clicker_ble_courtesy_stop();
    }
    if (ret == -EAGAIN) {
        LOG_INF("clicker BLE courtesy deferred attempt=%u event_seq=%u priority=%llx peer_wait_ms=%u",
                session->attempt_index,
                event_seq,
                (unsigned long long)priority_id,
                ble_defer_wait_ms != NULL ? *ble_defer_wait_ms : 0u);
        return ret;
    }
    if (ret == CLICKER_POLITENESS_UWB_RESTART) {
        LOG_INF("clicker UWB gate will restart after relevant packet wait: attempt=%u wait_ms=%u samples=%u activity=%u",
                session->attempt_index,
                uwb_restart_wait_ms != NULL ? *uwb_restart_wait_ms : 0u,
                sample_count,
                activity_count);
        return ret;
    }
    LOG_INF("clicker sampled politeness complete: quiet_samples=%u/%u samples=%u activity=%u max_wait_ms=%u",
            quiet_samples,
            UWB_POLITE_REQUIRED_QUIET_SAMPLES,
            sample_count,
            activity_count,
            MAX_POLITENESS_WAIT_MS);
    return 0;
}

static int clicker_attempt_gate(struct uwb_clicker_session *session,
                                uint32_t event_seq,
                                uint64_t priority_id,
                                int64_t click_deadline_ms,
                                bool use_ble_courtesy)
{
    uint8_t defer_count = 0u;
    bool ble_courtesy_allowed = use_ble_courtesy;
    int ret;

    while (true) {
        uint32_t uwb_restart_wait_ms = 0u;
        uint32_t ble_defer_wait_ms = 0u;

        if (k_uptime_get() + WAKE_ADV_MS >= click_deadline_ms) {
            return -ETIMEDOUT;
        }
        ret = clicker_politeness_phase(session,
                                       event_seq,
                                       priority_id,
                                       ble_courtesy_allowed,
                                       &uwb_restart_wait_ms,
                                       &ble_defer_wait_ms);
        if (ret == CLICKER_POLITENESS_UWB_RESTART) {
            uint32_t slept_ms = clicker_sleep_bounded(uwb_restart_wait_ms,
                                                      click_deadline_ms,
                                                      WAKE_ADV_MS);

            LOG_INF("clicker UWB gate restart: event_seq=%u attempt=%u requested_wait_ms=%u slept_ms=%u",
                    event_seq,
                    session->attempt_index,
                    uwb_restart_wait_ms,
                    slept_ms);
            continue;
        }
        if (ret != -EAGAIN) {
            break;
        }
        if (defer_count >= BLE_COURTESY_MAX_DEFERS_PER_ATTEMPT) {
            LOG_WRN("BLE courtesy defer cap reached: event_seq=%u attempt=%u",
                    event_seq,
                    session->attempt_index);
            ble_courtesy_allowed = false;
            continue;
        }
        defer_count++;
        if (ble_defer_wait_ms == 0u) {
            ble_defer_wait_ms = BLE_COURTESY_PEER_FINISH_MS;
        }
        LOG_INF("BLE courtesy peer defer: event_seq=%u attempt=%u defer=%u/%u requested_wait_ms=%u",
                event_seq,
                session->attempt_index,
                defer_count,
                BLE_COURTESY_MAX_DEFERS_PER_ATTEMPT,
                ble_defer_wait_ms);
        (void)clicker_sleep_bounded(ble_defer_wait_ms,
                                    click_deadline_ms,
                                    WAKE_ADV_MS);
        if (k_uptime_get() + WAKE_ADV_MS >= click_deadline_ms) {
            return -ETIMEDOUT;
        }
    }
    if (ret < 0) {
        return ret;
    }

    return 0;
}

static uint16_t clicker_claimed_duration_ms(uint16_t wake_train_ends_in_ms)
{
    uint32_t claimed_ms = (uint32_t)wake_train_ends_in_ms +
                          UWB_POST_WAKE_CLAIMED_DURATION_MS;

    return claimed_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)claimed_ms;
}

static int clicker_send_wake_claim_train(struct uwb_clicker_session *session,
                                         uint64_t priority_id)
{
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    int64_t close_ms;
    uint16_t sent_count = 0u;
    int ret;

    ret = radio_guard_uwb_start("clicker UWB WAKE_CLAIM train");
    if (ret < 0) {
        return ret;
    }

    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        goto out;
    }

    close_ms = k_uptime_get() + WAKE_ADV_MS;
    while (k_uptime_get() < close_ms) {
        struct uwb_wake_claim_frame claim;
        int64_t remaining_ms = close_ms - k_uptime_get();
        uint16_t remaining_u16 = delay_ms_to_u16(remaining_ms);

        ret = uwb_clicker_build_wake_claim(session,
                                           priority_id,
                                           remaining_u16,
                                           remaining_u16,
                                           clicker_claimed_duration_ms(remaining_u16),
                                           &claim);
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            break;
        }
        ret = uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len);
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            break;
        }

        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
        if (ret < 0) {
            break;
        }
        sent_count++;
        HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
        high_debug_log_event("WAKE_CLAIM_TX",
                             "event_seq=%u attempt=%u remaining_ms=%u sent=%u",
                             session->config.click_event_id,
                             session->attempt_index,
                             remaining_u16,
                             sent_count);
        uwb_clicker_note_wake_claim_tx(session, 1u);

        if (k_uptime_get() < close_ms) {
            uint32_t jitter_us = uwb_clicker_wake_claim_jitter_us(sys_rand32_get());
            int64_t remaining_after_tx_ms = close_ms - k_uptime_get();

            if (jitter_us > 0u && remaining_after_tx_ms > 0) {
                k_busy_wait(jitter_us);
            }
        }
    }

out:
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ret < 0) {
        LOG_WRN("clicker UWB WAKE_CLAIM train failed: sent=%u ret=%d",
                sent_count,
                ret);
        return ret;
    }

    LOG_INF("clicker UWB WAKE_CLAIM train complete: sent=%u duration_ms=%u",
            sent_count,
            WAKE_ADV_MS);
    return sent_count == 0u ? -ETIMEDOUT : 0;
}

static int clicker_discover_uwb_anchors(struct uwb_clicker_session *session)
{
    struct uwb_discover_frame discover;
    uint8_t frame[UWB_DISCOVERY_REPLY_LEN];
    size_t frame_len = 0u;
    int64_t deadline_ms;
    uint32_t reply_window_ms = UWB_DISCOVERY_WINDOW_MS + UWB_DISCOVERY_RX_GUARD_MS;
    uint16_t rx_frames = 0u;
    uint16_t decoded_replies = 0u;
    uint16_t malformed_frames = 0u;
    uint16_t rejected_replies = 0u;
    int ret;
    int last_ret = -ETIMEDOUT;

    ret = uwb_clicker_build_discover(session, &discover);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_encode_discover(&discover, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB DISCOVER");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        goto out;
    }

    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    if (ret < 0) {
        goto out;
    }
    HIGH_DEBUG_COUNTER_INC(discovery_tx);
    high_debug_log_event("DISCOVER_TX",
                         "event_seq=%u attempt=%u window_ms=%u",
                         session->config.click_event_id,
                         session->attempt_index,
                         reply_window_ms);

    deadline_ms = k_uptime_get() + reply_window_ms;
    while (k_uptime_get() < deadline_ms) {
        struct uwb_discovery_reply_frame reply;
        uint8_t quality = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame((uint32_t)MAX(1, remaining_ms),
                                           frame,
                                           sizeof(frame),
                                           &frame_len,
                                           &quality,
                                           NULL);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            last_ret = ret;
            continue;
        }
        rx_frames++;

        ret = uwb_decode_discovery_reply(frame, frame_len, &reply);
        if (ret != PROTO_OK) {
            malformed_frames++;
            LOG_DBG("clicker ignored malformed UWB discovery frame: ret=%d frame_len=%u quality=%u",
                    ret,
                    (unsigned int)frame_len,
                    quality);
            last_ret = -EBADMSG;
            continue;
        }
        decoded_replies++;
        ret = uwb_clicker_note_discovery_reply(session, &reply);
        if (ret == PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(discovery_reply_rx);
            high_debug_log_event("DISCOVERY_REPLY_RX",
                                 "anchor=0x%016llx slot=%u quality=%u candidates=%u",
                                 (unsigned long long)reply.anchor_id,
                                 reply.anchor_slot,
                                 reply.rx_quality,
                                 session->candidate_count);
            LOG_INF("clicker UWB discovery reply: anchor=0x%016llx slot=%u quality=%u status=%u candidates=%u",
                    (unsigned long long)reply.anchor_id,
                    reply.anchor_slot,
                    reply.rx_quality,
                    reply.status,
                    session->candidate_count);
        } else {
            rejected_replies++;
            high_debug_log_event("DISCOVERY_REPLY_RX",
                                 "anchor=0x%016llx rejected_reason=proto_%d selected_clicker=0x%016llx event_seq=%u attempt=%u",
                                 (unsigned long long)reply.anchor_id,
                                 ret,
                                 (unsigned long long)reply.selected_clicker_id,
                                 reply.click_event_id,
                                 reply.attempt_index);
            LOG_DBG("clicker rejected UWB discovery reply: ret=%d anchor=0x%016llx selected_clicker=0x%016llx event_seq=%u attempt=%u status=%u quality=%u",
                    ret,
                    (unsigned long long)reply.anchor_id,
                    (unsigned long long)reply.selected_clicker_id,
                    reply.click_event_id,
                    reply.attempt_index,
                    reply.status,
                    reply.rx_quality);
        }
    }

    ret = session->candidate_count > 0u ? (int)session->candidate_count : last_ret;
    LOG_INF("clicker UWB discovery complete: rx_frames=%u decoded_replies=%u candidates=%u malformed_frames=%u rejected_replies=%u window_ms=%u ret=%d",
            rx_frames,
            decoded_replies,
            session->candidate_count,
            malformed_frames,
            rejected_replies,
            reply_window_ms,
            ret);

out:
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    return ret;
}

static int clicker_send_range_schedule(const struct uwb_range_schedule_frame *schedule)
{
    uint8_t frame[UWB_RANGE_SCHEDULE_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    ret = uwb_encode_range_schedule(schedule, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB RANGE_SCHEDULE");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();

    if (ret < 0) {
        LOG_WRN("clicker UWB RANGE_SCHEDULE TX failed: ret=%d", ret);
        return ret;
    }
    HIGH_DEBUG_COUNTER_INC(schedules_tx);
    high_debug_log_event("RANGE_SCHEDULE_TX",
                         "clicker=0x%016llx event_seq=%u attempt=%u selected=%u samples_per_anchor=%u reply_delay_uus=%u BENCH_ONLY=%u",
                         (unsigned long long)schedule->clicker_id,
                         schedule->click_event_id,
                         schedule->attempt_index,
                         schedule->selected_count,
                         schedule->samples_per_anchor,
                         schedule->reply_delay_us,
                         (IMEC_STAGE == 1 &&
                          IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) ? 1u : 0u);
    LOG_INF("clicker UWB RANGE_SCHEDULE TX complete: selected=%u samples_per_anchor=%u",
            schedule->selected_count,
            schedule->samples_per_anchor);
    log_uwb_range_schedule_entries("clicker", schedule);
    return 0;
}

static int clicker_send_range_release(struct uwb_clicker_session *session,
                                      uint8_t reason)
{
    struct uwb_range_release_frame release;
    uint8_t frame[UWB_RANGE_RELEASE_LEN];
    size_t frame_len = 0u;
    int ret;

    ret = uwb_clicker_build_range_release(session, reason, &release);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_encode_range_release(&release, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("clicker UWB RANGE_RELEASE");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();

    if (ret < 0) {
        LOG_WRN("clicker UWB RANGE_RELEASE TX failed: ret=%d", ret);
        return ret;
    }
    LOG_INF("clicker UWB RANGE_RELEASE TX complete: candidates=%u min=%u reason=%u",
            release.discovered_anchor_count,
            release.min_anchor_count,
            release.reason);
    return 0;
}

static int clicker_collect_uwb_attempt(struct uwb_clicker_session *session,
                                       uint64_t priority_id,
                                       struct uwb_range_schedule_frame *schedule)
{
    int ret;

    ret = clicker_send_wake_claim_train(session, priority_id);
    if (ret < 0) {
        return ret;
    }

    ret = clicker_discover_uwb_anchors(session);
    if (ret < 0) {
        return ret;
    }

    if ((session->config.flags & FLAG_COUNT_AS_CLICK) != 0u &&
        session->candidate_count > 0u &&
        session->candidate_count < session->config.min_anchor_count) {
        int release_ret = clicker_send_range_release(
            session,
            UWB_RANGE_RELEASE_REASON_INSUFFICIENT_ANCHORS);

        if (release_ret < 0) {
            LOG_WRN("clicker could not release anchors after insufficient discovery: ret=%d candidates=%u min=%u",
                    release_ret,
                    session->candidate_count,
                    session->config.min_anchor_count);
        }
        (void)uwb_clicker_abort_attempt(session);
        return -ETIMEDOUT;
    }

    ret = uwb_clicker_build_range_schedule(session,
                                           UWB_RANGE_REPLY_DELAY_UUS,
                                           UWB_RANGE_FIRST_POLL_DELAY_MS,
                                           UWB_ANCHOR_RANGE_WINDOW_MS,
                                           schedule);
    if (ret != PROTO_OK) {
        return ret == PROTO_ERR_NOT_FOUND ? -ETIMEDOUT : -EINVAL;
    }

    ret = clicker_send_range_schedule(schedule);
    if (ret < 0) {
        (void)uwb_clicker_abort_attempt(session);
        LOG_WRN("clicker aborting UWB attempt before DS-TWR: reason=range_schedule_tx ret=%d attempt=%u retries=%u ds_fail=%u",
                ret,
                session->attempt_index,
                session->diagnostics.retries,
                session->diagnostics.ds_twr_failures);
    }
    return ret;
}

static int range_uwb_scheduled_anchors(struct uwb_clicker_session *session,
                                       const struct uwb_range_schedule_frame *schedule,
                                       int64_t click_deadline_ms,
                                       uint8_t *attempted_count)
{
    int64_t schedule_tx_ms = k_uptime_get();
    size_t total_samples = uwb_range_schedule_total_samples(schedule);
    int last_ret = -ETIMEDOUT;

    while (session->state == UWB_CLICKER_RANGING) {
        struct uwb_range_step step;
        struct dwm3000_range_request range_request;
        struct dwm3000_range_result range_result;
        int64_t target_us;
        int64_t remaining_ms;
        uint32_t slot_timeout_ms = CLICK_UWB_TIMEOUT_MS;
        int ret;

        ret = uwb_clicker_next_range_step(session, &step);
        if (ret == PROTO_ERR_NOT_FOUND) {
            break;
        }
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        target_us = scheduled_range_sample_target_us(schedule_tx_ms, schedule, step.sample_index);
        sleep_until_us(target_us);

        remaining_ms = click_deadline_ms - k_uptime_get();
        if (remaining_ms <= CLICK_REPORT_BUILD_GUARD_MS) {
            (void)uwb_clicker_record_range_result(session, &step, RANGE_RX_TIMEOUT);
            (void)uwb_clicker_abort_attempt(session);
            LOG_WRN("scheduled click DS-TWR not started: reason=click_budget anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u remaining_ms=%lld guard_ms=%u attempt=%u ds_fail=%u",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    (long long)remaining_ms,
                    CLICK_REPORT_BUILD_GUARD_MS,
                    session->attempt_index,
                    session->diagnostics.ds_twr_failures);
            last_ret = -ETIMEDOUT;
            break;
        }

        memset(&range_request, 0, sizeof(range_request));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        range_request.initiator_id = DEVICE_ID;
        range_request.responder_id = step.anchor_id;
        range_request.network_id = session->config.network_id;
        range_request.session_nonce = session->config.nonce;
        range_request.responder_short_addr = uwb_session_short_addr_from_id(step.anchor_id);
        range_request.session_id = session->config.click_event_id;
        range_request.seq = step.seq;
        range_request.round_index = step.round_index;
        range_request.flags = session->config.flags;
        slot_timeout_ms = MIN(slot_timeout_ms,
                              (uint32_t)ceil_us_to_ms(schedule->exchange_stride_us) +
                              UWB_SCHEDULE_GUARD_MS);
        range_request.timeout_ms = MIN(slot_timeout_ms,
                                       (uint32_t)(remaining_ms - CLICK_REPORT_BUILD_GUARD_MS));

        ret = radio_guard_uwb_start("clicker scheduled UWB range");
        if (ret < 0) {
            (void)uwb_clicker_record_range_result(session, &step, RANGE_INTERNAL_ERROR);
            (void)uwb_clicker_abort_attempt(session);
            LOG_WRN("scheduled click DS-TWR not started: reason=radio_guard anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u ret=%d attempt=%u ds_fail=%u",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    ret,
                    session->attempt_index,
                    session->diagnostics.ds_twr_failures);
            return ret;
        }
        LOG_INF("scheduled click DS-TWR start: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u timeout_ms=%u",
                (unsigned long long)step.anchor_id,
                step.anchor_index,
                (unsigned int)(step.sample_index + 1u),
                (unsigned int)total_samples,
                step.round_index,
                step.seq,
                range_request.timeout_ms);
        HIGH_DEBUG_COUNTER_INC(ds_twr_attempts);
        high_debug_log_event("DS_TWR_POLL_TX",
                             "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u timeout_ms=%u",
                             (unsigned long long)step.anchor_id,
                             session->config.click_event_id,
                             session->attempt_index,
                             uwb_schedule_burst_id(session->config.click_event_id,
                                                   session->attempt_index),
                             (unsigned int)(step.sample_index + 1u),
                             (unsigned int)total_samples,
                             step.round_index,
                             step.seq,
                             range_request.timeout_ms);
        ret = dwm3000_driver_range_initiator(&range_request, &range_result);
        (void)dwm3000_driver_standby();
        radio_guard_uwb_stop();

        if (!range_result.exchange_started) {
            enum range_status status = range_result.status;

            if (status == RANGE_OK || !range_status_valid(status)) {
                status = RANGE_RX_TIMEOUT;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            (void)uwb_clicker_record_range_result(session, &step, status);
            (void)uwb_clicker_abort_attempt(session);
            LOG_WRN("scheduled click DS-TWR did not start: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    ret,
                    range_status_name(range_result.status),
                    range_result.status);
            high_debug_log_event("RANGE_FAIL",
                                 "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s exchange_started=0",
                                 (unsigned long long)step.anchor_id,
                                 session->config.click_event_id,
                                 session->attempt_index,
                                 uwb_schedule_burst_id(session->config.click_event_id,
                                                       session->attempt_index),
                                 (unsigned int)(step.sample_index + 1u),
                                 (unsigned int)total_samples,
                                 step.round_index,
                                 step.seq,
                                 ret,
                                 range_status_name(status));
            last_ret = ret < 0 ? ret : -EIO;
            break;
        }
        if (attempted_count != NULL) {
            (*attempted_count)++;
        }

        if (ret == 0 && range_result.status == RANGE_OK) {
            int record_ret;

            HIGH_DEBUG_COUNTER_INC(ds_twr_successes);
            record_ret = uwb_clicker_record_range_result(session, &step, RANGE_OK);
            if (record_ret != PROTO_OK) {
                LOG_ERR("scheduled click DS-TWR state update failed: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)step.anchor_id,
                        step.seq,
                        record_ret);
                return -EINVAL;
            }
            LOG_INF("scheduled click DS-TWR complete: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u",
                    (unsigned long long)range_result.responder_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    range_result.seq,
                    range_result.distance_mm,
                    range_result.quality);
            high_debug_log_event("RANGE_OK",
                                 "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u rsl_dbm=%d rsl_present=%u",
                                 (unsigned long long)range_result.responder_id,
                                 range_result.session_id,
                                 session->attempt_index,
                                 uwb_schedule_burst_id(session->config.click_event_id,
                                                       session->attempt_index),
                                 (unsigned int)(step.sample_index + 1u),
                                 (unsigned int)total_samples,
                                 step.round_index,
                                 range_result.seq,
                                 range_result.distance_mm,
                                 range_result.quality,
                                 range_result.rsl_dbm,
                                 range_result.rsl_sampled ? 1u : 0u);
            last_ret = 0;
        } else {
            enum range_status status = range_result.status;
            int record_ret;

            if (status == RANGE_OK || !range_status_valid(status)) {
                status = RANGE_INTERNAL_ERROR;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            if (status == RANGE_TIMING_INVALID) {
                HIGH_DEBUG_COUNTER_INC(ds_twr_timing_rejects);
            }
            record_ret = uwb_clicker_record_range_result(session, &step, status);
            if (record_ret != PROTO_OK) {
                LOG_ERR("scheduled click DS-TWR failure state update failed: anchor=0x%016llx seq=%u ret=%d status=%s(%u)",
                        (unsigned long long)step.anchor_id,
                        step.seq,
                        record_ret,
                        range_status_name(status),
                        status);
                return -EINVAL;
            }
            LOG_WRN("scheduled click DS-TWR failed: anchor=0x%016llx anchor_index=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)step.anchor_id,
                    step.anchor_index,
                    (unsigned int)(step.sample_index + 1u),
                    (unsigned int)total_samples,
                    step.round_index,
                    step.seq,
                    ret,
                    range_status_name(status),
                    status);
            high_debug_log_event(status == RANGE_TIMING_INVALID ?
                                 "RANGE_TIMING_REJECT" : "RANGE_FAIL",
                                 "anchor=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s status=%u",
                                 (unsigned long long)step.anchor_id,
                                 session->config.click_event_id,
                                 session->attempt_index,
                                 uwb_schedule_burst_id(session->config.click_event_id,
                                                       session->attempt_index),
                                 (unsigned int)(step.sample_index + 1u),
                                 (unsigned int)total_samples,
                                 step.round_index,
                                 step.seq,
                                 ret,
                                 range_status_name(status),
                                 status);
            last_ret = ret < 0 ? ret : -EIO;
        }

        if (session->state == UWB_CLICKER_SUCCEEDED) {
            return 0;
        }
    }

    return session->state == UWB_CLICKER_SUCCEEDED ? 0 : last_ret;
}

static uint8_t clicker_debug_min_anchor_count(void);
static uint8_t clicker_debug_max_anchor_count(void);
static uint8_t clicker_debug_samples_per_anchor(void);

static int run_normal_click(void)
{
    uint32_t event_seq = next_click_event_seq();
    uint8_t attempted_count = 0u;
    uint16_t total_candidate_count = 0u;
    int64_t click_deadline_ms;
    struct uwb_clicker_session session;
    struct uwb_clicker_config config = {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = clicker_debug_min_anchor_count(),
        .max_anchor_count = clicker_debug_max_anchor_count(),
        .max_attempts = MAX_WAKE_ATTEMPTS,
        .samples_per_anchor = clicker_debug_samples_per_anchor(),
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = FLAG_COUNT_AS_CLICK,
    };
    int last_ret = -ETIMEDOUT;
    int ret;

    BUILD_ASSERT(UWB_NORMAL_CLICK_MIN_ANCHORS <= MAX_SUCCESSFUL_ANCHORS,
                 "successful anchor result storage must cover the success threshold");

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    LOG_INF("normal click started on UWB wake path: event_seq=%u wake_ms=%u max_attempts=%u min_unique_anchors=%u samples_per_anchor=%u",
            event_seq,
            WAKE_ADV_MS,
            MAX_WAKE_ATTEMPTS,
            config.min_anchor_count,
            config.samples_per_anchor);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        high_debug_log_event("BENCH_ONLY",
                             "stage=1 one_anchor_schedule=enabled production_min_unique_anchors=%u",
                             UWB_NORMAL_CLICK_MIN_ANCHORS);
    }
#endif

    click_deadline_ms = k_uptime_get() + CLICK_REPORT_DEADLINE_MS;

    while (session.attempt_index <= session.config.max_attempts) {
        struct uwb_range_schedule_frame schedule;
        uint64_t priority_id = clicker_priority_id(event_seq, session.attempt_index);

        ret = clicker_attempt_gate(&session,
                                   event_seq,
                                   priority_id,
                                   click_deadline_ms,
                                   true);
        if (ret < 0) {
            LOG_WRN("normal click attempt gate failed: event_seq=%u attempt=%u ret=%d",
                    event_seq,
                    session.attempt_index,
                    ret);
            return ret;
        }

        if (k_uptime_get() + WAKE_ADV_MS >= click_deadline_ms) {
            break;
        }

        ret = clicker_collect_uwb_attempt(&session, priority_id, &schedule);
        if (ret < 0) {
            last_ret = ret;
            LOG_WRN("normal click UWB attempt found no scheduled anchors: event_seq=%u attempt=%u ret=%d",
                    event_seq,
                    session.attempt_index,
                    ret);
        } else {
            total_candidate_count += schedule.selected_count;
            LOG_INF("normal click UWB attempt scheduled anchors: event_seq=%u attempt=%u selected=%u unique_success=%u/%u",
                    event_seq,
                    session.attempt_index,
                    schedule.selected_count,
                    session.successful_unique_count,
                    session.config.min_anchor_count);

            ret = range_uwb_scheduled_anchors(&session,
                                              &schedule,
                                              click_deadline_ms,
                                              &attempted_count);
            if (ret == 0 && session.state == UWB_CLICKER_SUCCEEDED) {
                LOG_INF("normal click completed: event_seq=%u candidates_scheduled=%u attempted_ranges=%u successful_unique_ranges=%u retries=%u sample_order=%u ds_ok=%u ds_fail=%u timing_reject=%u polite_samples=%u polite_activity=%u contention_ms=%u retry_ms=%u wake_claim_tx=%u",
                        event_seq,
                        total_candidate_count,
                        attempted_count,
                        session.successful_unique_count,
                        session.diagnostics.retries,
                        session.diagnostics.sample_order_count,
                        session.diagnostics.ds_twr_successes,
                        session.diagnostics.ds_twr_failures,
                        session.diagnostics.timing_rejections,
                        session.diagnostics.politeness_samples,
                        session.diagnostics.politeness_activity_hits,
                        session.diagnostics.contention_delay_ms,
                        session.diagnostics.retry_delay_ms,
                        session.diagnostics.wake_claim_tx_count);
                return 0;
            }
            if (ret < 0) {
                last_ret = ret;
            }
        }

        if (session.state == UWB_CLICKER_SUCCEEDED) {
            return 0;
        }
        if (session.attempt_index < session.config.max_attempts) {
            ret = uwb_clicker_prepare_retry(&session);
            if (ret != PROTO_OK) {
                return -EINVAL;
            }
            (void)clicker_apply_retry_delay(&session, click_deadline_ms);
            (void)clicker_apply_contention_delay(&session, click_deadline_ms);
            LOG_INF("normal click retry scheduled: event_seq=%u next_attempt=%u retries=%u unique_success=%u/%u ds_ok=%u ds_fail=%u timing_reject=%u",
                    event_seq,
                    session.attempt_index,
                    session.diagnostics.retries,
                    session.successful_unique_count,
                    session.config.min_anchor_count,
                    session.diagnostics.ds_twr_successes,
                    session.diagnostics.ds_twr_failures,
                    session.diagnostics.timing_rejections);
        } else {
            break;
        }
    }

    LOG_WRN("normal click failed: event_seq=%u candidates_scheduled=%u attempted_ranges=%u successful_unique_ranges=%u required=%u retries=%u sample_order=%u ds_ok=%u ds_fail=%u timing_reject=%u polite_samples=%u polite_activity=%u contention_ms=%u retry_ms=%u wake_claim_tx=%u",
            event_seq,
            total_candidate_count,
            attempted_count,
            session.successful_unique_count,
            session.config.min_anchor_count,
            session.diagnostics.retries,
            session.diagnostics.sample_order_count,
            session.diagnostics.ds_twr_successes,
            session.diagnostics.ds_twr_failures,
            session.diagnostics.timing_rejections,
            session.diagnostics.politeness_samples,
            session.diagnostics.politeness_activity_hits,
            session.diagnostics.contention_delay_ms,
            session.diagnostics.retry_delay_ms,
            session.diagnostics.wake_claim_tx_count);
    return last_ret < 0 ? last_ret : -EIO;
}

static int run_uwb_diagnostic_click(uint32_t event_seq)
{
    uint8_t attempted_count = 0u;
    int64_t click_deadline_ms = k_uptime_get() + CLICK_REPORT_DEADLINE_MS;
    struct uwb_clicker_session session;
    struct uwb_range_schedule_frame schedule;
    struct uwb_clicker_config config = {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = FLAG_DIAGNOSTIC,
    };
    int ret;

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = clicker_attempt_gate(&session,
                               event_seq,
                               clicker_priority_id(event_seq, 1u),
                               click_deadline_ms,
                               false);
    if (ret < 0) {
        return ret;
    }

    ret = clicker_collect_uwb_attempt(&session,
                                      clicker_priority_id(event_seq, 1u),
                                      &schedule);
    if (ret < 0) {
        return ret;
    }

    ret = range_uwb_scheduled_anchors(&session,
                                      &schedule,
                                      click_deadline_ms,
                                      &attempted_count);
    if (ret == 0 && session.state == UWB_CLICKER_SUCCEEDED) {
        LOG_INF("self-test UWB diagnostic click passed: event_seq=%u attempted=%u",
                event_seq,
                attempted_count);
        return 0;
    }
    return ret < 0 ? ret : -EIO;
}

static uint8_t clicker_debug_min_anchor_count(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return 1u;
    }
#endif
    return UWB_NORMAL_CLICK_MIN_ANCHORS;
}

static uint8_t clicker_debug_max_anchor_count(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return 1u;
    }
#endif
    return MAX_SCHEDULED_ANCHORS;
}

static uint8_t clicker_debug_samples_per_anchor(void)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (CONFIG_IMEC_BENCH_STAGE == 1 &&
        IS_ENABLED(CONFIG_IMEC_STAGE1_ALLOW_SINGLE_ANCHOR_RANGE)) {
        return 1u;
    }
#endif
    return UWB_CLICKER_MAX_SAMPLES_PER_ANCHOR;
}

static int clicker_emit_self_test_report(uint32_t event_seq, enum self_test_failure failure)
{
    struct mesh_outbound outbound = {0};
    struct self_test_report_fields fields = {
        .clicker_id = DEVICE_ID,
        .event_seq = event_seq,
        .failure_code = (uint8_t)failure,
        .battery_mv = 0u,
    };
    size_t payload_len = 0u;
    uint16_t packet_seq = (uint16_t)event_seq;
    int ret;

    if (packet_seq == 0u) {
        packet_seq = 1u;
    }

    ret = report_append_self_test_tlvs(outbound.payload,
                                       sizeof(outbound.payload),
                                       &payload_len,
                                       &fields);
    if (ret != PROTO_OK) {
        LOG_WRN("self-test report TLV build failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return -EINVAL;
    }

    ret = report_init_self_test_packet(&outbound.packet,
                                       DEVICE_ID,
                                       GATEWAY_ID,
                                       event_seq,
                                       packet_seq,
                                       (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("self-test report packet build failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return -EINVAL;
    }

    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = GATEWAY_ID;

    ret = mesh_send_outbound(&outbound, "self-test-report");
    if (ret < 0) {
        LOG_WRN("self-test report UWB mesh TX failed: event_seq=%u failure=%u ret=%d",
                event_seq,
                (unsigned int)failure,
                ret);
        return ret;
    }

    LOG_INF("self-test report sent over UWB mesh: event_seq=%u failure=%u",
            event_seq,
            (unsigned int)failure);
    return 0;
}

static enum self_test_failure run_self_test(uint32_t event_seq)
{
    uint32_t dev_id;
    int ret;

    LOG_INF("self-test started on UWB wake path");

    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 port init failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 wake failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 reset failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 decadriver DEV_ID probe failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        LOG_ERR("self-test DWM3000 fast SPI config failed: %d", ret);
        return SELF_TEST_FAILURE_DWM3000;
    }

    LOG_INF("self-test DWM3000 decadriver DEV_ID=0x%08x; fast SPI config checked at %u Hz",
            dev_id,
            (unsigned int)dwm3000_port_current_spi_hz());
    (void)dwm3000_driver_standby();

    ret = run_uwb_diagnostic_click(event_seq);
    if (ret == 0) {
        return SELF_TEST_FAILURE_NONE;
    }

    LOG_WRN("self-test UWB diagnostic click failed: ret=%d", ret);
    if (ret == -ETIMEDOUT) {
        return SELF_TEST_FAILURE_NO_ANCHOR;
    }
    return SELF_TEST_FAILURE_UWB;
}

#if defined(CONFIG_IMEC_HIGH_DEBUG)
static int high_debug_probe_dwm3000(void)
{
    uint32_t dev_id = 0u;
    int ret;

    high_debug_log_event("DWM_RESET_ASSERT", "action=probe_start");
    ret = dwm3000_port_init();
    if (ret < 0) {
        high_debug_log_event("DWM_DEV_ID_FAIL", "phase=port_init ret=%d", ret);
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        return ret;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        high_debug_log_event("DWM_DEV_ID_FAIL", "phase=wakeup ret=%d", ret);
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        return ret;
    }
    high_debug_log_event("UWB_WAKE", "source=probe");

    ret = dwm3000_port_hw_reset();
    high_debug_log_event("DWM_RESET_RELEASE", "ret=%d", ret);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        return ret;
    }

    high_debug_log_event("DWM_DEV_ID_READ", "spi_hz=%u",
                         (unsigned int)dwm3000_port_current_spi_hz());
    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(dwm_dev_id_failures);
        high_debug_log_event("DWM_DEV_ID_FAIL", "ret=%d dev_id=0x%08x", ret, dev_id);
        return ret;
    }

    HIGH_DEBUG_COUNTER_INC(dwm_dev_id_successes);
    high_debug_log_event("DWM_DEV_ID_OK", "dev_id=0x%08x", dev_id);
    ret = dwm3000_port_set_fast_spi();
    high_debug_log_event("DWM_SPI_SPEED_SET", "ret=%d spi_hz=%u",
                         ret,
                         (unsigned int)dwm3000_port_current_spi_hz());
    return ret;
}

static int high_debug_stage0_hardware_self_test(void)
{
    int ret;

    ret = high_debug_probe_dwm3000();
    if (ret < 0) {
        return ret;
    }

    ret = dwm3000_driver_standby();
    high_debug_log_event("UWB_SLEEP", "phase=stage0_self_test ret=%d", ret);
    if (ret < 0) {
        return ret;
    }

    k_msleep(10);
    ret = dwm3000_driver_configure_default();
    high_debug_log_event("UWB_WAKE", "phase=stage0_self_test ret=%d spi_hz=%u",
                         ret,
                         (unsigned int)dwm3000_port_current_spi_hz());
    (void)dwm3000_driver_standby();
    high_debug_log_event("UWB_SLEEP", "phase=stage0_self_test_complete");
    return ret;
}

static int high_debug_send_wake_claim_once(void)
{
    struct uwb_clicker_session session;
    struct uwb_wake_claim_frame claim;
    uint32_t event_seq = next_click_event_seq();
    struct uwb_clicker_config config = {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = FLAG_DIAGNOSTIC,
    };
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    size_t frame_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_CLICKER) {
        return -EINVAL;
    }

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_clicker_build_wake_claim(&session,
                                       clicker_priority_id(config.click_event_id, 1u),
                                       0u,
                                       0u,
                                       UWB_POST_WAKE_CLAIMED_DURATION_MS,
                                       &claim);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    ret = uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = radio_guard_uwb_start("high-debug WAKE_CLAIM once");
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        high_debug_log_event("WAKE_CLAIM_TX",
                             "mode=single event_seq=%u attempt=1 nonce=0x%016llx",
                             config.click_event_id,
                             (unsigned long long)config.nonce);
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
    }
    return ret;
}

static int high_debug_send_wake_train_command(void)
{
    struct uwb_clicker_session session;
    uint32_t event_seq = next_click_event_seq();
    struct uwb_clicker_config config = {
        .network_id = NETWORK_ID,
        .clicker_id = DEVICE_ID,
        .click_event_id = event_seq,
        .nonce = clicker_nonce(event_seq),
        .min_anchor_count = 1u,
        .max_anchor_count = 1u,
        .max_attempts = 1u,
        .samples_per_anchor = 1u,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
        .flags = FLAG_DIAGNOSTIC,
    };
    int ret;

    if (DEVICE_ROLE != ROLE_CLICKER) {
        return -EINVAL;
    }
    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    high_debug_log_event("WAKE_CLAIM_TX",
                         "mode=train event_seq=%u attempt=1 nonce=0x%016llx",
                         config.click_event_id,
                         (unsigned long long)config.nonce);
    return clicker_send_wake_claim_train(&session,
                                         clicker_priority_id(config.click_event_id, 1u));
}

static int high_debug_handle_command(const char *command)
{
    int ret = -EINVAL;

    if (command == NULL || command[0] == '\0') {
        return 0;
    }

    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX", "command=%s", command);

    if (strcmp(command, "status") == 0) {
        high_debug_boot_banner();
        high_debug_dump_counters("COUNTERS");
        ret = 0;
    } else if (strcmp(command, "dump_counters") == 0) {
        high_debug_dump_counters("COUNTERS");
        ret = 0;
    } else if (strcmp(command, "uwb_probe") == 0) {
        ret = high_debug_probe_dwm3000();
        (void)dwm3000_driver_standby();
    } else if (strcmp(command, "uwb_sleep") == 0) {
        ret = dwm3000_driver_standby();
        high_debug_log_event("UWB_SLEEP", "command=uwb_sleep ret=%d", ret);
    } else if (strcmp(command, "uwb_wake") == 0) {
        ret = dwm3000_driver_configure_default();
        high_debug_log_event("UWB_WAKE", "command=uwb_wake ret=%d", ret);
    } else if (strcmp(command, "send_wake_claim_once") == 0) {
        ret = high_debug_send_wake_claim_once();
    } else if (strcmp(command, "send_wake_train") == 0) {
        ret = high_debug_send_wake_train_command();
    } else if (strcmp(command, "reboot") == 0) {
        high_debug_log_event("COMMAND_RESULT_TX", "command=reboot status=ok reboot=now");
        k_msleep(50);
        sys_reboot(SYS_REBOOT_COLD);
        ret = 0;
    } else if (strcmp(command, "bootloader") == 0) {
        ret = high_debug_request_bootloader();
    } else {
        high_debug_log_event("COMMAND_RESULT_TX",
                             "command=%s status=unsupported reason=unknown_command",
                             command);
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
        return -EINVAL;
    }

    high_debug_log_event("COMMAND_RESULT_TX",
                         "command=%s status=%s ret=%d",
                         command,
                         ret == 0 ? "ok" : "failed",
                         ret);
    HIGH_DEBUG_COUNTER_INC(command_result_tx);
    return ret;
}

static void high_debug_serial_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!high_debug_cdc_command_enabled()) {
        return;
    }
#if HAS_SERIAL_CONSOLE
    if (device_is_ready(serial_console) && debug_serial_dtr_ready()) {
        unsigned char byte;

        while (uart_poll_in(serial_console, &byte) == 0) {
            if (byte == '\r' || byte == '\n') {
                if (high_debug_command_len > 0u) {
                    high_debug_command_buf[high_debug_command_len] = '\0';
                    (void)high_debug_handle_command(high_debug_command_buf);
                    high_debug_command_len = 0u;
                }
                continue;
            }
            if (byte == '\b' || byte == 0x7fu) {
                if (high_debug_command_len > 0u) {
                    high_debug_command_len--;
                }
                continue;
            }
            if (high_debug_command_len + 1u < sizeof(high_debug_command_buf)) {
                high_debug_command_buf[high_debug_command_len++] = (char)byte;
            } else {
                high_debug_command_len = 0u;
                high_debug_log_event("COMMAND_RESULT_TX",
                                     "status=failed reason=line_too_long");
            }
        }
    }
#endif
    (void)k_work_reschedule(&high_debug_serial_work,
                            K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COMMAND_POLL_MS));
}

static int high_debug_stage0_simulated_click(void)
{
    uint32_t event_seq = next_click_event_seq();
    int ret = 0;

    high_debug_log_event("COMMAND_RX",
                         "source=button action=simulated_click event_seq=%u",
                         event_seq);
    status_leds_set(false, true, true);
    k_msleep(80);
    status_leds_set(false, false, false);
    high_debug_log_event("RANGE_OK",
                         "BENCH_ONLY simulated=1 event_seq=%u anchor_required=0",
                         event_seq);
    if (IS_ENABLED(CONFIG_IMEC_STAGE0_SEND_WAKE_CLAIM_ON_CLICK)) {
        ret = high_debug_send_wake_train_command();
    }
    return ret;
}
#endif

static void handle_button_action(enum button_action action)
{
    struct status_inputs status = {0};
    enum self_test_failure failure;
    uint32_t self_test_event_seq;
    int ret;

    switch (action) {
    case BUTTON_ACTION_NORMAL_CLICK:
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        if (DEVICE_ROLE == ROLE_CLICKER && CONFIG_IMEC_BENCH_STAGE == 0) {
            ret = high_debug_stage0_simulated_click();
            status.click_accepted = ret == 0;
            status.click_failure = ret == 0 ? 0 : CLICK_FAILURE_INSUFFICIENT_RANGES;
            status_apply(&status);
            break;
        }
#endif
        ret = run_normal_click();
        status.click_accepted = ret == 0;
        if (ret != 0) {
            status.click_failure = (ret == -ETIMEDOUT) ?
                                    CLICK_FAILURE_NO_ANCHOR :
                                    CLICK_FAILURE_INSUFFICIENT_RANGES;
        }
        status_apply(&status);
        if (ret == 0) {
            LOG_INF("normal click MVP completed");
        } else {
            LOG_WRN("normal click MVP failed: %d", ret);
        }
        break;
    case BUTTON_ACTION_SELF_TEST_ARMED:
        status.self_test_armed = true;
        status_apply(&status);
#if HAS_CLICK_BUTTON
        (void)k_work_reschedule(&self_test_arm_timeout_work, K_MSEC(SELF_TEST_ARM_WINDOW_MS + 1u));
#endif
        LOG_INF("self-test armed");
        break;
    case BUTTON_ACTION_SELF_TEST_START:
#if HAS_CLICK_BUTTON
        (void)k_work_cancel_delayable(&self_test_arm_timeout_work);
#endif
        status.self_test_running = true;
        status_apply(&status);
        self_test_event_seq = next_click_event_seq();
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        if (DEVICE_ROLE == ROLE_CLICKER && CONFIG_IMEC_BENCH_STAGE == 0) {
            ret = high_debug_stage0_hardware_self_test();
            failure = ret == 0 ? SELF_TEST_FAILURE_NONE : SELF_TEST_FAILURE_DWM3000;
        } else
#endif
        failure = run_self_test(self_test_event_seq);
        status.self_test_running = false;
        status.failure = failure;
        status.self_test_passed = failure == SELF_TEST_FAILURE_NONE;
        status_apply(&status);
#if defined(CONFIG_IMEC_HIGH_DEBUG)
        if (!(DEVICE_ROLE == ROLE_CLICKER && CONFIG_IMEC_BENCH_STAGE == 0))
#endif
        {
            (void)clicker_emit_self_test_report(self_test_event_seq, failure);
        }
        break;
    case BUTTON_ACTION_SELF_TEST_CANCELLED:
        status_apply(&status);
        LOG_INF("self-test arm cancelled");
        break;
    case BUTTON_ACTION_NONE:
    default:
        break;
    }
}

#if HAS_CLICK_BUTTON
static void click_button_work_handler(struct k_work *work)
{
    enum button_action action;
    enum button_signal signal;
    int value;

    ARG_UNUSED(work);

    value = gpio_pin_get_dt(&click_button);
    if (value < 0) {
        LOG_ERR("failed to read click button: %d", value);
        return;
    }

    signal = value != 0 ? BUTTON_SIGNAL_PRESS : BUTTON_SIGNAL_RELEASE;
    if (button_fsm_handle(&button_fsm, signal, k_uptime_get_32(), &action) != PROTO_OK) {
        LOG_ERR("button FSM rejected signal");
        return;
    }
    handle_button_action(action);
}

static void click_button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    (void)k_work_submit(&click_button_work);
}

static void self_test_arm_timeout_handler(struct k_work *work)
{
    enum button_action action;

    ARG_UNUSED(work);

    if (button_fsm_handle(&button_fsm, BUTTON_SIGNAL_TICK,
                               k_uptime_get_32(), &action) == PROTO_OK) {
        handle_button_action(action);
    }
}

static int click_button_init(void)
{
    int ret;

    if (!gpio_is_ready_dt(&click_button)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&click_button, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&click_button, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        return ret;
    }

    k_work_init(&click_button_work, click_button_work_handler);
    k_work_init_delayable(&self_test_arm_timeout_work, self_test_arm_timeout_handler);
    gpio_init_callback(&click_button_cb, click_button_isr, BIT(click_button.pin));
    return gpio_add_callback(click_button.port, &click_button_cb);
}
#else
static int click_button_init(void)
{
    return -ENODEV;
}
#endif

int main(void)
{
    int ret;

    ret = debug_serial_init();
    if (ret < 0) {
        printk("USB serial debug init failed: %d\n", ret);
    } else {
        printk("USB serial debug active; firmware booting\n");
    }

#if defined(CONFIG_IMEC_HIGH_DEBUG)
    HIGH_DEBUG_COUNTER_INC(boot_count);
    high_debug_boot_banner();
    high_debug_log_event("USB_READY",
                         "cdc_logs=%u rtt_logs=%u gateway_binary_cdc=%u command_parser=%u",
                         IS_ENABLED(CONFIG_IMEC_USB_CDC_LOGS) ? 1u : 0u,
                         IS_ENABLED(CONFIG_IMEC_RTT_LOGS) ? 1u : 0u,
                         high_debug_gateway_binary_cdc_active() ? 1u : 0u,
                         high_debug_cdc_command_enabled() ? 1u : 0u);
    high_debug_log_event("BOOTLOADER_READY",
                         "configured=%u entry_command=%u recovery=jlink",
                         IS_ENABLED(CONFIG_IMEC_USB_BOOTLOADER) ? 1u : 0u,
                         IS_ENABLED(CONFIG_RETENTION_BOOT_MODE) ? 1u : 0u);
    k_work_init_delayable(&high_debug_counter_work, high_debug_counter_work_handler);
    k_work_init_delayable(&high_debug_serial_work, high_debug_serial_work_handler);
    (void)k_work_schedule(&high_debug_counter_work,
                          K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COUNTER_PERIOD_MS));
    if (high_debug_cdc_command_enabled()) {
        (void)k_work_schedule(&high_debug_serial_work,
                              K_MSEC(CONFIG_IMEC_HIGH_DEBUG_COMMAND_POLL_MS));
    }
#endif

    button_fsm_init(&button_fsm);
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    k_work_init_delayable(&mesh_uwb_rx_work, mesh_uwb_rx_work_handler);
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
    k_work_init_delayable(&gateway_command_result_timeout_work,
                          gateway_command_result_timeout_handler);
    k_work_init_delayable(&gateway_survey_work, gateway_survey_work_handler);
    k_work_init_delayable(&gateway_time_sync_work, gateway_time_sync_work_handler);
    LOG_INF("UWB firmware starting as %s", role_name());
    LOG_INF("runtime config: device_id=0x%016llx gateway_id=0x%016llx max_scheduled=%u wake_ms=%u max_attempts=%u min_unique_anchors=%u anchor_scan_interval_ms=%u anchor_scan_window_ms=%u anchor_mesh_rx_interval_ms=%u anchor_idle_uwb_us_per_s=%u anchor_uwb_wait_ms=%u gateway_time_sync_interval_ms=%u gateway_time_sync_worst_case_ppm=%u",
            (unsigned long long)DEVICE_ID,
            (unsigned long long)GATEWAY_ID,
            MAX_SCHEDULED_ANCHORS,
            WAKE_ADV_MS,
            MAX_WAKE_ATTEMPTS,
            UWB_NORMAL_CLICK_MIN_ANCHORS,
            anchor_uwb_scan_interval_ms,
            ANCHOR_UWB_SCAN_RX_MS,
            UWB_MESH_ANCHOR_RX_INTERVAL_MS,
            (unsigned int)ANCHOR_UWB_PERIODIC_IDLE_US_PER_S,
            ANCHOR_UWB_WAIT_MS,
            GATEWAY_TIME_SYNC_DEFAULT_INTERVAL_MS,
            TIME_SYNC_WORST_CASE_DRIFT_PPM);

    ret = status_leds_init();
    if (ret < 0) {
        LOG_WRN("status LED setup incomplete: %d", ret);
    }

    ret = dwm3000_port_init();
    if (ret < 0) {
        LOG_WRN("DWM3000 reset/wake setup failed: %d", ret);
    } else {
        LOG_INF("DWM3000 wake pin parked inactive; SYS_STATUS polling ready; radio init waits for UWB wake windows");
    }
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    if (ret == 0) {
        ret = high_debug_probe_dwm3000();
        if (ret < 0) {
            LOG_WRN("high-debug DWM3000 boot probe failed: %d", ret);
        }
        (void)dwm3000_driver_standby();
    }
#endif

    if (DEVICE_ROLE == ROLE_CLICKER) {
        ret = click_button_init();
        if (ret < 0) {
            LOG_WRN("click button unavailable: %d", ret);
        }
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        const struct uwb_anchor_config anchor_config = {
            .network_id = NETWORK_ID,
            .anchor_id = DEVICE_ID,
            .anchor_slot = local_uwb_anchor_slot(),
            .wake_channel = UWB_WAKE_CHANNEL,
            .ranging_channel = UWB_RANGING_CHANNEL,
        };

        mesh_relay_init(&mesh_runtime,
                        MESH_RELAY_ROLE_ANCHOR,
                        DEVICE_ID,
                        GATEWAY_ID,
                        1u);
        ret = uwb_anchor_session_init(&anchor_uwb_session, &anchor_config);
        if (ret < 0) {
            LOG_ERR("anchor UWB session init failed: %d", ret);
            return 0;
        }
        k_work_init_delayable(&anchor_uwb_scan_work, anchor_uwb_scan_work_handler);
        k_work_init_delayable(&anchor_heartbeat_work, anchor_heartbeat_work_handler);
        k_work_init_delayable(&anchor_reboot_work, anchor_reboot_work_handler);
        k_work_init_delayable(&anchor_survey_work, anchor_survey_work_handler);
        k_work_queue_start(&anchor_survey_work_q,
                           anchor_survey_work_q_stack,
                           K_THREAD_STACK_SIZEOF(anchor_survey_work_q_stack),
                           ANCHOR_SURVEY_WORKQUEUE_PRIORITY,
                           &anchor_survey_work_q_config);
        ret = anchor_start_uwb_scan();
        if (ret < 0) {
            LOG_ERR("anchor UWB scan unavailable: %d", ret);
        }
        ret = mesh_start_uwb_rx("anchor startup");
        if (ret < 0) {
            LOG_ERR("anchor UWB mesh RX unavailable: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        mesh_relay_init(&mesh_runtime,
                        MESH_RELAY_ROLE_GATEWAY,
                        DEVICE_ID,
                        GATEWAY_ID,
                        1u);
        k_work_init_delayable(&gateway_serial_rx_work, gateway_serial_rx_work_handler);
        ret = mesh_start_uwb_rx("gateway startup");
        if (ret < 0) {
            LOG_ERR("gateway UWB mesh RX unavailable: %d", ret);
        }
        if (gateway_binary_cdc_enabled()) {
            (void)k_work_schedule(&gateway_serial_rx_work, K_MSEC(GATEWAY_SERIAL_POLL_MS));
        }
        (void)k_work_schedule(&gateway_time_sync_work,
                              K_MSEC(GATEWAY_TIME_SYNC_INITIAL_DELAY_MS));
        LOG_INF("gateway reactive mesh root active; USB COBS packet input/output %s",
                gateway_binary_cdc_enabled() ? "active" : "disabled");
    }

    return 0;
}
