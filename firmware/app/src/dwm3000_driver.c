#include "dwm3000_driver.h"

#include "debug_log.h"
#include "dwm3000_port.h"
#include "uwb.h"
#include "uwb_session.h"

#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_IMEC_HIGH_DEBUG)
#define DWM3000_DRIVER_LOG_LEVEL LOG_LEVEL_DBG
#else
#define DWM3000_DRIVER_LOG_LEVEL LOG_LEVEL_INF
#endif

LOG_MODULE_REGISTER(dwm3000_driver, DWM3000_DRIVER_LOG_LEVEL);

#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif

static bool focused_anchor_rx_logs_enabled(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_FOCUSED_RX_LOGS);
}

#ifndef GATEWAY_ID
#define GATEWAY_ID 0x9999888877776666ull
#endif

#ifndef DEVICE_ID
#if DEVICE_ROLE == ROLE_ANCHOR
#define DEVICE_ID 0x2222222222222222ull
#elif DEVICE_ROLE == ROLE_GATEWAY
#define DEVICE_ID GATEWAY_ID
#else
#define DEVICE_ID 0x1111111111111111ull
#endif
#endif

#ifndef DWM3000_TX_ANT_DLY
#define DWM3000_TX_ANT_DLY 16385u
#endif
#ifndef DWM3000_RX_ANT_DLY
#define DWM3000_RX_ANT_DLY 16385u
#endif
#define DWM3000_UUS_TO_DWT_TIME 63898ULL
#define DWM3000_TIME_UNITS (1.0 / (499.2e6 * 128.0))
#define DWM3000_SPEED_OF_LIGHT_MPS 299702547.0
/* Q8.8 constants for 10*log10(ipatovPower*2^17/ipatovAccumCount^2)-121.74 dBm. */
#define DWM3000_LOG2_TO_DB_Q8 771
#define DWM3000_IPATOV_POWER_SCALE_DB_Q8 13101
#define DWM3000_IPATOV_RSL_OFFSET_DB_Q8 31165
#define DWM3000_ACCUM_CIR_SAMPLE_READ_LEN (UWB_CIR_SAMPLE_LEN + 1u)

#ifndef DWM3000_POLL_TX_TO_RESP_RX_DLY_UUS
#define DWM3000_POLL_TX_TO_RESP_RX_DLY_UUS 690u
#endif
#define POLL_TX_TO_RESP_RX_DLY_UUS DWM3000_POLL_TX_TO_RESP_RX_DLY_UUS
/*
 * DS-TWR ranging accuracy is more sensitive to unequal reply delays than to
 * making both replies absolutely shortest. Keep the responder response delay
 * and initiator final delay identical first, then reduce this common value only
 * after proving the slower path still has headroom.
 *
 * Current timing-critical frame sizes before the radio FCS:
 *   poll=41 B, response=49 B, final=53 B, report=49 B.
 * The initiator receives an 8 B longer response before scheduling the final
 * than the responder receives before scheduling the response. At 850 kbps that
 * path delta is ceil(8 B * 8 bits * 1e6 / 850e3) = 76 us. The shared
 * UWB_RANGE_REPLY_DELAY_UUS DWM/DW3000 delayed-TX delay intentionally lets the
 * shorter poll path wait, keeping both reply times equal. The shared protocol
 * header keeps separate provisional short-range and long-range delay presets;
 * both must be recalibrated on the final firmware path. Optional RX diagnostics
 * must stay out of the RX-to-delayed-TX critical path; delayed TX misses mean
 * the selected common value must be raised.
 */
#define UWB_PHY_DATA_RATE_BPS 850000u
#define DS_TWR_RX_PATH_DELTA_BYTES (UWB_RESP_LEN - UWB_POLL_LEN)
#define DS_TWR_RX_PATH_DELTA_US (((DS_TWR_RX_PATH_DELTA_BYTES * 8u * 1000000u) + \
                                  UWB_PHY_DATA_RATE_BPS - 1u) / UWB_PHY_DATA_RATE_BPS)
#ifndef DWM3000_DS_TWR_REPLY_DLY_UUS
#define DWM3000_DS_TWR_REPLY_DLY_UUS UWB_RANGE_REPLY_DELAY_UUS
#define DWM3000_DS_TWR_REPLY_DLY_UUS_USES_PROTOCOL_DEFAULT 1
#else
#define DWM3000_DS_TWR_REPLY_DLY_UUS_USES_PROTOCOL_DEFAULT 0
#endif
#define DS_TWR_REPLY_DLY_UUS DWM3000_DS_TWR_REPLY_DLY_UUS
#define RESP_RX_TO_FINAL_TX_DLY_UUS DS_TWR_REPLY_DLY_UUS
#ifndef DWM3000_RESP_RX_TIMEOUT_UUS
#define DWM3000_RESP_RX_TIMEOUT_UUS 2000u
#endif
#define RESP_RX_TIMEOUT_UUS DWM3000_RESP_RX_TIMEOUT_UUS
#ifndef DWM3000_RESP_RX_TIMEOUT_MS
#define DWM3000_RESP_RX_TIMEOUT_MS 8u
#endif
#define RESP_RX_TIMEOUT_MS DWM3000_RESP_RX_TIMEOUT_MS
#define POLL_RX_TO_RESP_TX_DLY_UUS DS_TWR_REPLY_DLY_UUS
#ifndef DWM3000_RESP_TX_TO_FINAL_RX_DLY_UUS
#define DWM3000_RESP_TX_TO_FINAL_RX_DLY_UUS 670u
#endif
#define RESP_TX_TO_FINAL_RX_DLY_UUS DWM3000_RESP_TX_TO_FINAL_RX_DLY_UUS
#ifndef DWM3000_FINAL_RX_TIMEOUT_UUS
#define DWM3000_FINAL_RX_TIMEOUT_UUS 2000u
#endif
#define FINAL_RX_TIMEOUT_UUS DWM3000_FINAL_RX_TIMEOUT_UUS
#ifndef DWM3000_FINAL_RX_TIMEOUT_MS
#define DWM3000_FINAL_RX_TIMEOUT_MS 8u
#endif
#define FINAL_RX_TIMEOUT_MS DWM3000_FINAL_RX_TIMEOUT_MS
#ifndef DWM3000_REPORT_RX_TIMEOUT_UUS
#define DWM3000_REPORT_RX_TIMEOUT_UUS 2500u
#endif
#define REPORT_RX_TIMEOUT_UUS DWM3000_REPORT_RX_TIMEOUT_UUS
#ifndef DWM3000_REPORT_RX_TIMEOUT_MS
#define DWM3000_REPORT_RX_TIMEOUT_MS 12u
#endif
#define REPORT_RX_TIMEOUT_MS DWM3000_REPORT_RX_TIMEOUT_MS
#ifndef DWM3000_REPLY_TIMING_TOLERANCE_US
#define DWM3000_REPLY_TIMING_TOLERANCE_US UWB_SESSION_REPLY_DELAY_TOLERANCE_US
#endif
#ifndef DWM3000_REPLY_DELAY_FROM_ROUND_INDEX
#define DWM3000_REPLY_DELAY_FROM_ROUND_INDEX 0
#endif
#ifndef DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS
#define DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS DS_TWR_REPLY_DLY_UUS
#endif
#ifndef DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS
#define DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS DS_TWR_REPLY_DLY_UUS
#endif
#ifndef DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS
#define DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS 1u
#endif
#define CLICKER_DIAG_TX_TIMEOUT_MS 4u
#define CLICKER_DIAG_RX_TIMEOUT_MS 4u
#define CLICKER_DIAG_COMPACT_BYTES_LEN 12u
/*
 * A zero preamble-detect timeout disables the preamble hunt timeout. That is
 * acceptable on optimized DS-TWR delayed-RX legs because the receiver is armed
 * at a scheduled offset and still bounded by the frame wait timeout.
 */
#define IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC 5u
#define DELAYED_RX_PREAMBLE_TIMEOUT_PAC 0u
#define DEFAULT_RESPONDER_WINDOW_MS UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS
#define DWM3000_SLEEP_MODE DWT_CONFIG
#define DWM3000_SLEEP_WAKE_FLAGS \
    (DWT_PRES_SLEEP | DWT_WAKE_CSN | DWT_WAKE_WUP | DWT_SLP_EN)
#define DWM3000_WAKE_IDLE_RC_TIMEOUT_US 3000u
#define DWM3000_FIRST_PATH_NTM_LOW 12u
#define DWM3000_STATUS_POLL_INTERVAL_US 50u
#ifndef DWM3000_PHY_CHANNEL
#define DWM3000_PHY_CHANNEL 5
#endif
#ifndef DWM3000_PHY_PREAMBLE_LENGTH
#define DWM3000_PHY_PREAMBLE_LENGTH DWT_PLEN_4096
#endif
#ifndef DWM3000_PHY_RX_PAC
#define DWM3000_PHY_RX_PAC DWT_PAC32
#endif
#ifndef DWM3000_PHY_TX_CODE
#define DWM3000_PHY_TX_CODE 9
#endif
#ifndef DWM3000_PHY_RX_CODE
#define DWM3000_PHY_RX_CODE 9
#endif
#ifndef DWM3000_PHY_SFD_TYPE
#define DWM3000_PHY_SFD_TYPE DWT_SFD_DW_8
#endif
#ifndef DWM3000_PHY_DATA_RATE
#define DWM3000_PHY_DATA_RATE DWT_BR_850K
#endif
#ifndef DWM3000_PHY_PHR_MODE
#define DWM3000_PHY_PHR_MODE DWT_PHRMODE_STD
#endif
#ifndef DWM3000_PHY_PHR_RATE
#define DWM3000_PHY_PHR_RATE DWT_PHRRATE_STD
#endif
#ifndef DWM3000_PHY_SFD_TIMEOUT
#define DWM3000_PHY_SFD_TIMEOUT 4073
#endif
#ifndef DWM3000_WAKE_PHY_SFD_TIMEOUT
#define DWM3000_WAKE_PHY_SFD_TIMEOUT DWM3000_PHY_SFD_TIMEOUT
#endif
#ifndef DWM3000_PHY_STS_MODE
#define DWM3000_PHY_STS_MODE DWT_STS_MODE_OFF
#endif
#ifndef DWM3000_PHY_STS_LENGTH
#define DWM3000_PHY_STS_LENGTH DWT_STS_LEN_64
#endif
#ifndef DWM3000_PHY_PDOA_MODE
#define DWM3000_PHY_PDOA_MODE DWT_PDOA_M0
#endif
#ifndef DWM3000_TX_PG_DELAY
#define DWM3000_TX_PG_DELAY 0x34
#endif
#ifndef DWM3000_TX_POWER
#define DWM3000_TX_POWER 0xffffffffu
#endif
#ifndef DWM3000_TX_PG_COUNT
#define DWM3000_TX_PG_COUNT 0x0
#endif
#ifndef DWM3000_CIA_DIAG_MODE
#define DWM3000_CIA_DIAG_MODE DW_CIA_DIAG_LOG_ALL
#endif
#ifndef DWM3000_CLICKER_DIAG_ENABLED
#define DWM3000_CLICKER_DIAG_ENABLED 1
#endif

BUILD_ASSERT(DS_TWR_RX_PATH_DELTA_US == 76u,
             "Update the DS-TWR equal-reply timing calculation when UWB frame sizes change");
#if DWM3000_DS_TWR_REPLY_DLY_UUS_USES_PROTOCOL_DEFAULT
BUILD_ASSERT(DS_TWR_REPLY_DLY_UUS == UWB_RANGE_REPLY_DELAY_UUS,
             "UWB schedule validation must match the fixed DWM3000 DS-TWR reply delay");
#endif
#if DWM3000_REPLY_DELAY_FROM_ROUND_INDEX
BUILD_ASSERT(DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS >=
             DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS,
             "Delay calibration maximum must be >= minimum");
BUILD_ASSERT(DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS > 0u,
             "Delay calibration step must be nonzero");
BUILD_ASSERT(((DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS -
               DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS) /
              DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS) <= UINT8_MAX,
             "Delay calibration range must fit in the UWB round_index field");
#endif
BUILD_ASSERT((((uint64_t)DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS *
               DWM3000_UUS_TO_DWT_TIME) >> 8) <= UINT32_MAX,
             "Maximum DS-TWR reply delay must fit in DX_TIME units");
BUILD_ASSERT(DWM3000_FIRST_PATH_NTM_LOW <= IP_CONFIG_LO_IP_NTM_BIT_MASK,
             "DWM3000 first-path threshold must fit in IP_CONFIG_LO.IP_NTM");
BUILD_ASSERT(DWM3000_STATUS_POLL_INTERVAL_US <= DS_TWR_RX_PATH_DELTA_US,
             "DWM3000 status polling must remain inside the equal-reply DS-TWR timing margin");
BUILD_ASSERT(DWM3000_WAKE_PHY_SFD_TIMEOUT >= DWM3000_PHY_SFD_TIMEOUT,
             "wake/discovery open-window RX must not be shorter than the ranging SFD timeout");

static dwt_config_t default_config = {
    DWM3000_PHY_CHANNEL,
    DWM3000_PHY_PREAMBLE_LENGTH,
    DWM3000_PHY_RX_PAC,
    DWM3000_PHY_TX_CODE,
    DWM3000_PHY_RX_CODE,
    DWM3000_PHY_SFD_TYPE,
    DWM3000_PHY_DATA_RATE,
    DWM3000_PHY_PHR_MODE,
    DWM3000_PHY_PHR_RATE,
    DWM3000_PHY_SFD_TIMEOUT,
    DWM3000_PHY_STS_MODE,
    DWM3000_PHY_STS_LENGTH,
    DWM3000_PHY_PDOA_MODE,
};

static dwt_config_t wake_config = {
    DWM3000_PHY_CHANNEL,
    DWM3000_PHY_PREAMBLE_LENGTH,
    DWM3000_PHY_RX_PAC,
    DWM3000_PHY_TX_CODE,
    DWM3000_PHY_RX_CODE,
    DWM3000_PHY_SFD_TYPE,
    DWM3000_PHY_DATA_RATE,
    DWM3000_PHY_PHR_MODE,
    DWM3000_PHY_PHR_RATE,
    DWM3000_WAKE_PHY_SFD_TIMEOUT,
    DWM3000_PHY_STS_MODE,
    DWM3000_PHY_STS_LENGTH,
    DWM3000_PHY_PDOA_MODE,
};

static dwt_config_t mesh_payload_config = {
    UWB_CHANNEL_MESH_PAYLOAD,
    DWM3000_PHY_PREAMBLE_LENGTH,
    DWM3000_PHY_RX_PAC,
    DWM3000_PHY_TX_CODE,
    DWM3000_PHY_RX_CODE,
    DWM3000_PHY_SFD_TYPE,
    DWM3000_PHY_DATA_RATE,
    DWM3000_PHY_PHR_MODE,
    DWM3000_PHY_PHR_RATE,
    DWM3000_PHY_SFD_TIMEOUT,
    DWM3000_PHY_STS_MODE,
    DWM3000_PHY_STS_LENGTH,
    DWM3000_PHY_PDOA_MODE,
};

static dwt_txconfig_t default_txconfig = {
    DWM3000_TX_PG_DELAY,
    DWM3000_TX_POWER,
    DWM3000_TX_PG_COUNT,
};

enum dwm3000_phy_mode {
    DWM3000_PHY_NONE = 0,
    DWM3000_PHY_RANGE = 1,
    DWM3000_PHY_WAKE = 2,
    DWM3000_PHY_MESH_PAYLOAD = 3,
};

static bool radio_configured;
static bool radio_awake;
static enum dwm3000_phy_mode active_phy_mode;
static struct dwm3000_driver_stats driver_stats;

static void clear_all_events(void);

#if defined(CONFIG_IMEC_HIGH_DEBUG)
static const char *dwm3000_debug_role_name(void)
{
    switch (DEVICE_ROLE) {
    case ROLE_CLICKER:
#if defined(CONFIG_IMEC_ROLE_TAG)
        if (IS_ENABLED(CONFIG_IMEC_ROLE_TAG)) {
            return "tag";
        }
#endif
        return "clicker";
    case ROLE_ANCHOR:
        return "anchor";
    case ROLE_GATEWAY:
        return "gateway";
    default:
        return "unknown";
    }
}
#endif

static void dwm3000_debug_event(bool warning, const char *event, const char *fmt, ...)
{
#if defined(CONFIG_IMEC_HIGH_DEBUG)
    char prefix[96];
    char message[160];
    va_list args;
    int ret;

    ret = debug_log_format_prefix(prefix,
                                  sizeof(prefix),
                                  k_uptime_get_32(),
                                  dwm3000_debug_role_name(),
                                  DEVICE_ID,
                                  CONFIG_IMEC_BENCH_STAGE,
                                  event);
    if (ret < 0) {
        return;
    }

    va_start(args, fmt);
    ret = vsnprintk(message, sizeof(message), fmt, args);
    va_end(args);
    if (ret < 0) {
        return;
    }

    if (warning) {
        LOG_WRN("%s %s", prefix, message);
    } else {
        LOG_DBG("%s %s", prefix, message);
    }
#else
    ARG_UNUSED(warning);
    ARG_UNUSED(event);
    ARG_UNUSED(fmt);
#endif
}

static void configure_first_path_sensitivity(void)
{
    /*
     * Use the lower recommended CIA first-path threshold. It can increase
     * outliers, but the ranging/report path keeps sample-level rejection.
     */
    dwt_modify8bitoffsetreg(IP_CONFIG_LO_ID,
                            0,
                            (uint8_t)~IP_CONFIG_LO_IP_NTM_BIT_MASK,
                            DWM3000_FIRST_PATH_NTM_LOW);
}

static uint16_t short_addr_from_id(uint64_t device_id)
{
    uint16_t short_addr = (uint16_t)(device_id & 0xffffu);

    return short_addr == 0u ? 1u : short_addr;
}

static uint64_t timestamp_to_u64(const uint8_t timestamp[5])
{
    return ((uint64_t)timestamp[0]) |
           ((uint64_t)timestamp[1] << 8) |
           ((uint64_t)timestamp[2] << 16) |
           ((uint64_t)timestamp[3] << 24) |
           ((uint64_t)timestamp[4] << 32);
}

static uint64_t read_tx_timestamp_u64(void)
{
    uint8_t timestamp[5];

    dwt_readtxtimestamp(timestamp);
    return timestamp_to_u64(timestamp);
}

static uint64_t read_rx_timestamp_u64(void)
{
    uint8_t timestamp[5];

    dwt_readrxtimestamp(timestamp);
    return timestamp_to_u64(timestamp);
}

static uint16_t dwt_delta_to_uus(uint32_t start_ts, uint32_t end_ts)
{
    uint32_t delta = end_ts - start_ts;
    uint32_t uus = (uint32_t)(((uint64_t)delta +
                               (DWM3000_UUS_TO_DWT_TIME / 2u)) /
                              DWM3000_UUS_TO_DWT_TIME);

    return uus > UINT16_MAX ? UINT16_MAX : (uint16_t)uus;
}

static uint32_t delayed_trx_offset_from_uus(uint16_t delay_uus)
{
    return (uint32_t)(((uint64_t)delay_uus * DWM3000_UUS_TO_DWT_TIME) >> 8);
}

static uint64_t delayed_tx_timestamp_from_rx_reference(uint64_t rx_reference_ts,
                                                       uint32_t dx_time)
{
    return rx_reference_ts + (((uint64_t)(dx_time & 0xfffffffeUL)) << 8) +
           DWM3000_TX_ANT_DLY;
}

static uint16_t reply_delay_uus_from_round_index(uint8_t round_index)
{
#if DWM3000_REPLY_DELAY_FROM_ROUND_INDEX
    uint32_t span_uus = DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS -
                        DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS;
    uint32_t max_index = span_uus / DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS;
    uint32_t index = MIN((uint32_t)round_index, max_index);
    uint32_t delay_uus = DWM3000_REPLY_DELAY_CALIBRATION_MAX_UUS -
                         (index * DWM3000_REPLY_DELAY_CALIBRATION_STEP_UUS);

    return (uint16_t)MAX(delay_uus, DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS);
#else
    (void)round_index;
    return DS_TWR_REPLY_DLY_UUS;
#endif
}

static int validate_driver_reply_timing(uint16_t poll_to_resp_uus,
                                        uint16_t resp_to_final_uus,
                                        uint16_t expected_uus)
{
#if DWM3000_DS_TWR_REPLY_DLY_UUS_USES_PROTOCOL_DEFAULT
    return uwb_session_validate_reply_timing(poll_to_resp_uus,
                                             resp_to_final_uus,
                                             expected_uus,
                                             DWM3000_REPLY_TIMING_TOLERANCE_US);
#else
    uint16_t tolerance_uus = DWM3000_REPLY_TIMING_TOLERANCE_US;
    uint16_t poll_error_uus = poll_to_resp_uus > expected_uus ?
        poll_to_resp_uus - expected_uus : expected_uus - poll_to_resp_uus;
    uint16_t final_error_uus = resp_to_final_uus > expected_uus ?
        resp_to_final_uus - expected_uus : expected_uus - resp_to_final_uus;
    uint16_t reply_delta_uus = poll_to_resp_uus > resp_to_final_uus ?
        poll_to_resp_uus - resp_to_final_uus : resp_to_final_uus - poll_to_resp_uus;

    if (poll_error_uus > tolerance_uus ||
        final_error_uus > tolerance_uus ||
        reply_delta_uus > tolerance_uus) {
        LOG_WRN("DS-TWR timing invalid: poll_to_resp_uus=%u resp_to_final_uus=%u expected_uus=%u tolerance_uus=%u",
                poll_to_resp_uus,
                resp_to_final_uus,
                expected_uus,
                tolerance_uus);
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
#endif
}

static int ensure_local_data(void)
{
    return dwt_setlocaldataptr(0) == DWT_SUCCESS ? 0 : -EINVAL;
}

static int initialise_radio(bool idle_after_init)
{
    int ret;
    int mode = idle_after_init ? DWT_DW_IDLE : DWT_DW_INIT;

    ret = dwm3000_port_init();
    if (ret < 0) {
        return ret;
    }

    ret = ensure_local_data();
    if (ret < 0) {
        return ret;
    }

    (void)dwm3000_port_set_slow_spi();
    if (dwt_initialise(mode) != DWT_SUCCESS) {
        return -EIO;
    }
    radio_awake = true;

    return dwm3000_port_set_fast_spi();
}

static int apply_radio_config(const dwt_config_t *config,
                              enum dwm3000_phy_mode phy_mode)
{
    if (config == NULL) {
        return -EINVAL;
    }

    if (dwt_configure((dwt_config_t *)config) != DWT_SUCCESS) {
        return -EIO;
    }

    configure_first_path_sensitivity();
    dwt_configuretxrf(&default_txconfig);
    dwt_configciadiag(DWM3000_CIA_DIAG_MODE);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC);
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();

    radio_configured = true;
    radio_awake = true;
    active_phy_mode = phy_mode;
    return 0;
}

static int restore_txrx_after_sleep(void)
{
    int ret;

    dwt_restore_common();
    ret = dwt_restore_txrx(DWT_RESTORE_TXRX_MODE);
    if (ret != DWT_SUCCESS) {
        return -EIO;
    }

    configure_first_path_sensitivity();
    dwt_configuretxrf(&default_txconfig);
    dwt_configciadiag(DWM3000_CIA_DIAG_MODE);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC);
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();

    return ensure_local_data();
}

static uint16_t effective_sfd_timeout(const dwt_config_t *config)
{
    return config->sfdTO == 0u ? DWT_SFDTOC_DEF : config->sfdTO;
}

static const dwt_config_t *config_for_phy(enum dwm3000_phy_mode phy_mode)
{
    switch (phy_mode) {
    case DWM3000_PHY_WAKE:
        return &wake_config;
    case DWM3000_PHY_MESH_PAYLOAD:
        return &mesh_payload_config;
    case DWM3000_PHY_RANGE:
    case DWM3000_PHY_NONE:
    default:
        return &default_config;
    }
}

static bool phy_configs_equal(const dwt_config_t *a, const dwt_config_t *b)
{
    return a->chan == b->chan &&
           a->txPreambLength == b->txPreambLength &&
           a->rxPAC == b->rxPAC &&
           a->txCode == b->txCode &&
           a->rxCode == b->rxCode &&
           a->sfdType == b->sfdType &&
           a->dataRate == b->dataRate &&
           a->phrMode == b->phrMode &&
           a->phrRate == b->phrRate &&
           effective_sfd_timeout(a) == effective_sfd_timeout(b) &&
           a->stsMode == b->stsMode &&
           a->stsLength == b->stsLength &&
           a->pdoaMode == b->pdoaMode;
}

static int wake_configured_radio(void)
{
    enum dwm3000_phy_mode requested_phy = active_phy_mode == DWM3000_PHY_NONE ?
        DWM3000_PHY_RANGE : active_phy_mode;
    uint32_t wake_start_us;
    uint32_t wake_elapsed_us;
    int ret;

    if (!radio_configured || radio_awake) {
        return 0;
    }

    wake_start_us = k_cyc_to_us_floor32(k_cycle_get_32());
    driver_stats.sleep_wake_count++;

    (void)dwm3000_port_set_slow_spi();
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        return ret;
    }

    for (uint32_t waited_us = 0u; waited_us <= DWM3000_WAKE_IDLE_RC_TIMEOUT_US;
         waited_us += DWM3000_STATUS_POLL_INTERVAL_US) {
        if (dwt_checkidlerc()) {
            ret = 0;
            break;
        }
        k_busy_wait(DWM3000_STATUS_POLL_INTERVAL_US);
        ret = -ETIMEDOUT;
    }
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        return ret;
    }

    ret = restore_txrx_after_sleep();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        return ret;
    }
    radio_awake = true;
    active_phy_mode = requested_phy;

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        return ret;
    }
    wake_elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32()) - wake_start_us;
    driver_stats.sleep_wake_total_us += wake_elapsed_us;
    if (wake_elapsed_us > driver_stats.sleep_wake_max_us) {
        driver_stats.sleep_wake_max_us = wake_elapsed_us;
    }
    LOG_DBG("DWM3000 PHY mode %d restored after sleep wake without full PHY reconfigure",
            active_phy_mode);
    return 0;
}

static int ensure_phy_mode(enum dwm3000_phy_mode phy_mode)
{
    const dwt_config_t *active_config;
    const dwt_config_t *config;
    int ret;

    if (!radio_configured) {
        ret = dwm3000_driver_configure_default();
        if (ret < 0) {
            return ret;
        }
    } else {
        ret = wake_configured_radio();
        if (ret < 0) {
            return ret;
        }
    }

    if (active_phy_mode == phy_mode) {
        return 0;
    }

    config = config_for_phy(phy_mode);
    active_config = config_for_phy(active_phy_mode);
    if (phy_configs_equal(active_config, config)) {
        active_phy_mode = phy_mode;
        LOG_DBG("DWM3000 PHY mode %d selected without redundant reconfigure",
                phy_mode);
        return 0;
    }

    return apply_radio_config(config, phy_mode);
}

static int ensure_current_phy_or_range(void)
{
    if (!radio_configured) {
        return dwm3000_driver_configure_default();
    }

    return wake_configured_radio();
}

static void clear_status(uint32_t mask)
{
    dwt_write32bitreg(SYS_STATUS_ID, mask);
}

static void clear_all_events(void)
{
    clear_status(0xffffffffu);
}

static int wait_status_internal(uint32_t mask,
                                uint32_t timeout_ms,
                                uint32_t *status,
                                bool log_events)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint32_t read_status;
    uint32_t start_cycles = k_cycle_get_32();
    uint32_t poll_loops = 0u;
    int64_t now_ms;

    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_SYS_STATUS_POLL_START",
                            "mask=0x%08x timeout_ms=%u",
                            mask,
                            timeout_ms);
    }
    do {
        read_status = dwt_read32bitreg(SYS_STATUS_ID);
        poll_loops++;
        if ((read_status & mask) != 0u) {
            uint32_t elapsed_us = (uint32_t)k_cyc_to_us_floor64(
                (uint32_t)(k_cycle_get_32() - start_cycles));

            driver_stats.sys_status_poll_loops += poll_loops;
            if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
                driver_stats.sys_status_poll_max_duration_us = elapsed_us;
            }
            if (status != NULL) {
                *status = read_status;
            }
            if (log_events) {
                dwm3000_debug_event(false,
                                    "UWB_SYS_STATUS_POLL_DONE",
                                    "mask=0x%08x status=0x%08x loops=%u duration_us=%u",
                                    mask,
                                    read_status,
                                    poll_loops,
                                    elapsed_us);
            }
            return 0;
        }

        now_ms = k_uptime_get();
        if (now_ms >= deadline) {
            break;
        }
        k_busy_wait(DWM3000_STATUS_POLL_INTERVAL_US);
    } while (k_uptime_get() <= deadline);

    if (status != NULL) {
        *status = dwt_read32bitreg(SYS_STATUS_ID);
    }
    driver_stats.sys_status_poll_loops += poll_loops;
    driver_stats.sys_status_poll_timeouts++;
    {
        uint32_t elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - start_cycles));

        if (elapsed_us > driver_stats.sys_status_poll_max_duration_us) {
            driver_stats.sys_status_poll_max_duration_us = elapsed_us;
        }
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_SYS_STATUS_POLL_TIMEOUT",
                                "mask=0x%08x status=0x%08x loops=%u duration_us=%u timeout_ms=%u",
                                mask,
                                status == NULL ? 0u : *status,
                                poll_loops,
                                elapsed_us,
                                timeout_ms);
        }
    }
    return -ETIMEDOUT;
}

static int wait_status(uint32_t mask, uint32_t timeout_ms, uint32_t *status)
{
    return wait_status_internal(mask, timeout_ms, status, true);
}

static int wait_tx_complete(uint32_t timeout_ms)
{
    uint32_t status;
    int ret;

    ret = wait_status(SYS_STATUS_TXFRS_BIT_MASK, timeout_ms, &status);
    if (ret < 0) {
        driver_stats.tx_failures++;
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=timeout ret=%d sys_status=0x%08x timeout_ms=%u",
                            ret,
                            status,
                            timeout_ms);
        return ret;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    driver_stats.tx_dones++;
    dwm3000_debug_event(false,
                        "UWB_TX_DONE",
                        "status=ok sys_status=0x%08x",
                        status);
    return 0;
}

static enum range_status status_to_range_status(uint32_t status)
{
    if ((status & SYS_STATUS_ALL_RX_TO) != 0u) {
        return RANGE_RX_TIMEOUT;
    }
    if ((status & SYS_STATUS_ALL_RX_ERR) != 0u) {
        return RANGE_RX_ERROR;
    }
    return RANGE_INTERNAL_ERROR;
}

static enum dwm3000_rx_failure status_to_rx_failure(uint32_t status)
{
    if ((status & SYS_STATUS_RXPTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    }
    if ((status & SYS_STATUS_RXSTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_SFD_TIMEOUT;
    }
    if ((status & SYS_STATUS_RXFTO_BIT_MASK) != 0u) {
        return DWM3000_RX_FAILURE_FRAME_TIMEOUT;
    }
    if ((status & (SYS_STATUS_RXFCE_BIT_MASK |
                   SYS_STATUS_RXPHE_BIT_MASK |
                   SYS_STATUS_RXFSL_BIT_MASK)) != 0u) {
        return DWM3000_RX_FAILURE_CRC_OR_PHY;
    }
    return DWM3000_RX_FAILURE_NONE;
}

static uint8_t floor_log2_u32(uint32_t value)
{
    uint8_t log2 = 0u;

    while (value > 1u) {
        value >>= 1;
        log2++;
    }
    return log2;
}

static int32_t log2_q8_u32(uint32_t value)
{
    uint8_t log2;
    uint32_t normalized;
    uint32_t fraction_q8;

    if (value == 0u) {
        return 0;
    }

    log2 = floor_log2_u32(value);
    normalized = value << (31u - log2);
    fraction_q8 = (normalized - 0x80000000u) >> 23;
    return ((int32_t)log2 << 8) + (int32_t)fraction_q8;
}

static int8_t clamp_i8(int32_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static int8_t estimate_ipatov_rsl_dbm(const dwt_rxdiag_t *diagnostics)
{
    int32_t power_db_q8;
    int32_t accum_db_q8;
    int32_t rsl_q8;
    int32_t rounded_dbm;

    if (diagnostics == NULL ||
        diagnostics->ipatovPower == 0u ||
        diagnostics->ipatovAccumCount == 0u) {
        return 0;
    }

    power_db_q8 = (log2_q8_u32(diagnostics->ipatovPower) * DWM3000_LOG2_TO_DB_Q8) >> 8;
    accum_db_q8 = (log2_q8_u32(diagnostics->ipatovAccumCount) * DWM3000_LOG2_TO_DB_Q8) >> 8;
    rsl_q8 = power_db_q8 +
             DWM3000_IPATOV_POWER_SCALE_DB_Q8 -
             (2 * accum_db_q8) -
             DWM3000_IPATOV_RSL_OFFSET_DB_Q8;
    rounded_dbm = rsl_q8 >= 0 ? (rsl_q8 + 128) >> 8 : -(((-rsl_q8) + 128) >> 8);
    return clamp_i8(rounded_dbm);
}

static bool read_cir_sample(const dwt_rxdiag_t *diagnostics,
                            uint8_t cir_sample[UWB_CIR_SAMPLE_LEN])
{
    uint8_t accum[DWM3000_ACCUM_CIR_SAMPLE_READ_LEN];
    uint16_t fp_int;

    if (diagnostics == NULL ||
        cir_sample == NULL ||
        diagnostics->ipatovAccumCount == 0u) {
        return false;
    }

    fp_int = diagnostics->ipatovFpIndex >> 6;
    if ((uint32_t)fp_int + DWM3000_ACCUM_CIR_SAMPLE_READ_LEN > ACC_BUFFER_MAX_LEN) {
        return false;
    }

    dwt_readaccdata(accum, sizeof(accum), fp_int);
    memcpy(cir_sample, &accum[1], UWB_CIR_SAMPLE_LEN);
    return true;
}

static void read_rx_diagnostics(int8_t *rsl_dbm,
                                uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                                bool *cir_sampled)
{
    dwt_rxdiag_t diagnostics;

    if (rsl_dbm == NULL && cir_sample == NULL) {
        return;
    }
    if (cir_sampled != NULL) {
        *cir_sampled = false;
    }

    memset(&diagnostics, 0, sizeof(diagnostics));
    dwt_readdiagnostics(&diagnostics);
    if (rsl_dbm != NULL) {
        *rsl_dbm = estimate_ipatov_rsl_dbm(&diagnostics);
    }
    if (read_cir_sample(&diagnostics, cir_sample)) {
        if (cir_sampled != NULL) {
            *cir_sampled = true;
        }
    }
}

static uint16_t read_rx_frame(uint8_t *buffer, size_t buffer_len)
{
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFLEN_BIT_MASK;

    if (frame_len > buffer_len) {
        return 0u;
    }

    dwt_readrxdata(buffer, (uint16_t)frame_len, 0);
    return (uint16_t)frame_len;
}

static size_t payload_len_without_fcs(size_t frame_len)
{
    if (frame_len >= FCS_LEN) {
        return frame_len - FCS_LEN;
    }
    return frame_len;
}

static int decode_poll_frame(const uint8_t *buffer, size_t frame_len,
                             struct uwb_range_header *header)
{
    int ret;

    ret = uwb_decode_poll(buffer, frame_len, header);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_poll(buffer, payload_len_without_fcs(frame_len), header);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_response_frame(const uint8_t *buffer, size_t frame_len,
                                 struct uwb_response_frame *frame)
{
    int ret;

    ret = uwb_decode_response(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_response(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_final_frame(const uint8_t *buffer, size_t frame_len,
                              struct uwb_final_frame *frame)
{
    int ret;

    ret = uwb_decode_final(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_final(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_report_frame(const uint8_t *buffer, size_t frame_len,
                               struct uwb_report_frame *frame)
{
    int ret;

    ret = uwb_decode_report(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_report(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_clicker_diag_frame(const uint8_t *buffer, size_t frame_len,
                                     struct uwb_clicker_diag_frame *frame)
{
    int ret;

    ret = uwb_decode_clicker_diag(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_clicker_diag(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int write_tx_frame(const uint8_t *frame, size_t frame_len)
{
    if (frame == NULL || frame_len > UINT16_MAX) {
        return -EINVAL;
    }

    if (dwt_writetxdata((uint16_t)frame_len, (uint8_t *)frame, 0) != DWT_SUCCESS) {
        return -EIO;
    }

    dwt_writetxfctrl((uint16_t)(frame_len + FCS_LEN), 0, 1);
    return 0;
}

static bool header_matches_request(const struct uwb_range_header *header,
                                   const struct dwm3000_range_request *request,
                                   uint8_t expected_type)
{
    uint16_t responder_short_addr;

    if (header == NULL || request == NULL) {
        return false;
    }
    if (header->type != expected_type ||
        header->seq != request->seq ||
        header->round_index != request->round_index ||
        header->network_id != request->network_id ||
        header->session_id != request->session_id ||
        header->session_nonce != request->session_nonce ||
        header->initiator_short_addr != short_addr_from_id(request->initiator_id) ||
        header->initiator_id != request->initiator_id ||
        header->flags != request->flags) {
        return false;
    }

    responder_short_addr = request->responder_short_addr != 0u ?
                           request->responder_short_addr :
                           short_addr_from_id(request->responder_id);
    return header->responder_short_addr == responder_short_addr &&
           header->responder_id == request->responder_id;
}

static bool poll_targets_anchor(const struct uwb_range_header *header,
                                uint64_t local_anchor_id)
{
    return header->responder_short_addr == short_addr_from_id(local_anchor_id) &&
           header->responder_id == local_anchor_id;
}

static bool poll_matches_expected(const struct uwb_range_header *poll,
                                  const struct dwm3000_range_request *expected)
{
    uint16_t expected_responder_short_addr;

    expected_responder_short_addr = expected->responder_short_addr != 0u ?
                                    expected->responder_short_addr :
                                    short_addr_from_id(expected->responder_id);
    return poll->initiator_short_addr == short_addr_from_id(expected->initiator_id) &&
           poll->responder_short_addr == expected_responder_short_addr &&
           poll->initiator_id == expected->initiator_id &&
           poll->responder_id == expected->responder_id &&
           poll->network_id == expected->network_id &&
           poll->session_id == expected->session_id &&
           poll->session_nonce == expected->session_nonce &&
           poll->seq == expected->seq &&
#if DWM3000_REPLY_DELAY_FROM_ROUND_INDEX
           reply_delay_uus_from_round_index(poll->round_index) >=
           DWM3000_REPLY_DELAY_CALIBRATION_MIN_UUS &&
#else
           poll->round_index == expected->round_index &&
#endif
           poll->flags == expected->flags;
}

static bool range_flags_valid(uint8_t flags)
{
    return flags == FLAG_DIAGNOSTIC ||
           flags == FLAG_COUNT_AS_CLICK;
}

static void result_set_request_metadata(struct dwm3000_range_result *result,
                                        const struct dwm3000_range_request *request)
{
    if (result == NULL || request == NULL) {
        return;
    }

    result->initiator_id = request->initiator_id;
    result->responder_id = request->responder_id;
    result->session_id = request->session_id;
    result->seq = request->seq;
    result->round_index = request->round_index;
    result->flags = request->flags;
}

static void result_set_poll_metadata(struct dwm3000_range_result *result,
                                     const struct uwb_range_header *poll,
                                     uint64_t local_anchor_id)
{
    if (result == NULL || poll == NULL) {
        return;
    }

    result->initiator_id = poll->initiator_id;
    result->responder_id = local_anchor_id;
    result->session_id = poll->session_id;
    result->seq = poll->seq;
    result->round_index = poll->round_index;
    result->flags = poll->flags;
}

static int validate_range_request(const struct dwm3000_range_request *request)
{
    if (request == NULL ||
        request->initiator_id == 0u ||
        request->responder_id == 0u ||
        request->initiator_id == request->responder_id ||
        request->network_id == 0u ||
        request->session_id == 0u ||
        request->session_nonce == 0u ||
        request->seq == 0u) {
        return -EINVAL;
    }
    if (request->responder_short_addr == 0u ||
        request->responder_short_addr != short_addr_from_id(request->responder_id)) {
        return -EINVAL;
    }
    if (!range_flags_valid(request->flags)) {
        return -EINVAL;
    }
    return 0;
}

static int send_range_frame(const uint8_t *frame, size_t frame_len, uint8_t tx_mode)
{
    int ret;

    driver_stats.tx_starts++;
    dwm3000_debug_event(false,
                        "UWB_TX_START",
                        "frame_len=%u tx_mode=0x%02x",
                        (unsigned int)frame_len,
                        tx_mode);
    ret = write_tx_frame(frame, frame_len);
    if (ret < 0) {
        driver_stats.tx_failures++;
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=write-fail ret=%d frame_len=%u",
                            ret,
                            (unsigned int)frame_len);
        return ret;
    }

    if (dwt_starttx(tx_mode) != DWT_SUCCESS) {
        driver_stats.tx_failures++;
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=start-fail frame_len=%u tx_mode=0x%02x",
                            (unsigned int)frame_len,
                            tx_mode);
        return -EIO;
    }
    return 0;
}

static int receive_frame(uint32_t timeout_ms, uint32_t *status,
                         uint8_t *buffer, size_t buffer_len, size_t *frame_len,
                         uint64_t *rx_timestamp, uint8_t *quality,
                         int8_t *rsl_dbm, uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                         bool *cir_sampled, bool capture_rsl, bool log_events)
{
    int ret;

    driver_stats.rx_starts++;
    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_RX_START",
                            "timeout_ms=%u buffer_len=%u",
                            timeout_ms,
                            (unsigned int)buffer_len);
    }
    ret = wait_status_internal(SYS_STATUS_RXFCG_BIT_MASK |
                               SYS_STATUS_ALL_RX_TO |
                               SYS_STATUS_ALL_RX_ERR,
                               timeout_ms,
                               status,
                               log_events);
    if (ret < 0) {
        driver_stats.rx_timeouts++;
        dwt_forcetrxoff();
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_RX_TIMEOUT",
                                "ret=%d timeout_ms=%u sys_status=0x%08x",
                                ret,
                                timeout_ms,
                                status == NULL ? 0u : *status);
        }
        return ret;
    }

    if ((*status & SYS_STATUS_RXFCG_BIT_MASK) == 0u) {
        if ((*status & (SYS_STATUS_RXFCE_BIT_MASK |
                        SYS_STATUS_RXPHE_BIT_MASK |
                        SYS_STATUS_RXFSL_BIT_MASK)) != 0u) {
            driver_stats.rx_crc_failures++;
        } else {
            driver_stats.rx_failures++;
        }
        clear_status(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff();
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_RX_DONE",
                                "status=error sys_status=0x%08x",
                                *status);
        }
        return -EIO;
    }

    if (rx_timestamp != NULL) {
        *rx_timestamp = read_rx_timestamp_u64();
    }
    if (quality != NULL) {
        *quality = 100u;
    }
    if (capture_rsl) {
        read_rx_diagnostics(rsl_dbm, cir_sample, cir_sampled);
    }

    *frame_len = read_rx_frame(buffer, buffer_len);
    clear_status(SYS_STATUS_RXFCG_BIT_MASK);
    if (*frame_len == 0u) {
        driver_stats.rx_failures++;
        dwt_forcetrxoff();
        if (log_events) {
            dwm3000_debug_event(true,
                                "UWB_RX_DONE",
                                "status=empty-frame sys_status=0x%08x",
                                *status);
        }
        return -EMSGSIZE;
    }

    driver_stats.rx_dones++;
    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_RX_DONE",
                            "status=ok sys_status=0x%08x frame_len=%u",
                            *status,
                            (unsigned int)*frame_len);
    }
    return 0;
}

static int receive_response(const struct dwm3000_range_request *request,
                            struct uwb_response_frame *response,
                            uint64_t *resp_rx_ts,
                            uint8_t *quality,
                            int8_t *rsl_dbm,
                            uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                            bool *cir_sampled,
                            enum range_status *status_out,
                            bool capture_rsl)
{
    uint8_t rx_buffer[UWB_RESP_LEN + FCS_LEN];
    uint32_t status;
    size_t frame_len;
    int ret;

    ret = wait_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                      request->timeout_ms == 0u ? RESP_RX_TIMEOUT_MS : request->timeout_ms,
                      &status);
    if (ret < 0) {
        dwt_forcetrxoff();
        *status_out = RANGE_RX_TIMEOUT;
        return ret;
    }

    if ((status & SYS_STATUS_RXFCG_BIT_MASK) == 0u) {
        *status_out = status_to_range_status(status);
        clear_status(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff();
        return -EIO;
    }

    *resp_rx_ts = read_rx_timestamp_u64();
    if (quality != NULL) {
        *quality = 100u;
    }
    if (capture_rsl) {
        read_rx_diagnostics(rsl_dbm, cir_sample, cir_sampled);
    }

    frame_len = read_rx_frame(rx_buffer, sizeof(rx_buffer));
    clear_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_TXFRS_BIT_MASK);
    if (frame_len == 0u) {
        dwt_forcetrxoff();
        *status_out = RANGE_BAD_FRAME;
        return -EMSGSIZE;
    }

    ret = decode_response_frame(rx_buffer, frame_len, response);
    if (ret < 0) {
        *status_out = RANGE_BAD_FRAME;
        return ret;
    }

    if (!header_matches_request(&response->header, request, MSG_UWB_RESP)) {
        *status_out = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }

    *status_out = RANGE_OK;
    return 0;
}

static int receive_report(const struct dwm3000_range_request *request,
                          uint64_t responder_id,
                          struct dwm3000_range_result *result)
{
    struct uwb_report_frame report;
    uint8_t rx_buffer[UWB_REPORT_LEN + FCS_LEN];
    uint32_t status;
    size_t frame_len;
    int ret;

    ret = receive_frame(REPORT_RX_TIMEOUT_MS, &status,
                        rx_buffer, sizeof(rx_buffer), &frame_len, NULL, NULL,
                        NULL, NULL, NULL, false, true);
    if (ret < 0) {
        result_set_request_metadata(result, request);
        result->responder_id = responder_id;
        result->distance_mm = 0;
        result->quality = 0u;
        result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : status_to_range_status(status);
        return ret;
    }

    ret = decode_report_frame(rx_buffer, frame_len, &report);
    if (ret < 0) {
        result->status = RANGE_BAD_FRAME;
        return ret;
    }

    if (!header_matches_request(&report.header, request, MSG_UWB_REPORT) ||
        report.header.responder_short_addr != short_addr_from_id(responder_id) ||
        report.header.responder_id != responder_id) {
        result->status = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }

    result->initiator_id = request->initiator_id;
    result->responder_id = responder_id;
    result->session_id = report.header.session_id;
    result->seq = report.header.seq;
    result->round_index = report.header.round_index;
    result->flags = report.header.flags;
    result->distance_mm = report.distance_mm;
    result->quality = report.quality;
    result->rsl_dbm = report.rsl_dbm;
    result->rsl_sampled = true;
    result->status = report.status;
    return report.status == RANGE_OK ? 0 : -EIO;
}

static bool clicker_diag_matches_poll(const struct uwb_clicker_diag_frame *diag,
                                      const struct uwb_range_header *poll,
                                      uint64_t local_anchor_id)
{
    if (diag == NULL || poll == NULL) {
        return false;
    }

    return diag->header.seq == poll->seq &&
           diag->header.round_index == poll->round_index &&
           diag->header.network_id == poll->network_id &&
           diag->header.session_id == poll->session_id &&
           diag->header.session_nonce == poll->session_nonce &&
           diag->header.initiator_short_addr == poll->initiator_short_addr &&
           diag->header.responder_short_addr == short_addr_from_id(local_anchor_id) &&
           diag->header.flags == poll->flags &&
           diag->header.initiator_id == poll->initiator_id &&
           diag->header.responder_id == local_anchor_id;
}

static void store_clicker_diag_result(struct dwm3000_range_result *result,
                                      const struct uwb_clicker_diag_frame *diag)
{
    uint8_t raw_len;
    uint8_t copy_len;

    if (result == NULL || diag == NULL) {
        return;
    }

    raw_len = diag->diag_len;
    copy_len = raw_len;
    if (copy_len > UWB_CLICKER_DIAG_MAX_BYTES - 15u) {
        copy_len = UWB_CLICKER_DIAG_MAX_BYTES - 15u;
    }

    proto_put_u32_le(&result->clicker_diag[0], diag->final_tx_ts_32);
    proto_put_u32_le(&result->clicker_diag[4], diag->status_flags);
    proto_put_u32_le(&result->clicker_diag[8], diag->status_detect_latency_us);
    result->clicker_diag[12] = diag->resp_quality;
    result->clicker_diag[13] = (uint8_t)diag->resp_rsl_dbm;
    result->clicker_diag[14] = raw_len;
    if (copy_len > 0u) {
        memcpy(&result->clicker_diag[15], diag->diag_bytes, copy_len);
    }
    result->clicker_diag_len = 15u + copy_len;
    result->clicker_diag_status_flags = diag->status_flags;
    result->clicker_diag_status_detect_latency_us = diag->status_detect_latency_us;
    result->clicker_diag_received = true;
    result->clicker_diag_truncated = copy_len != raw_len;
}

static int send_clicker_diag(const struct dwm3000_range_request *request,
                             const struct uwb_response_frame *response,
                             const struct uwb_final_frame *final,
                             uint64_t resp_rx_ts,
                             uint8_t resp_quality,
                             int8_t resp_rsl_dbm,
                             bool resp_rsl_sampled)
{
    struct uwb_clicker_diag_frame diag = {0};
    uint8_t tx_buffer[UWB_CLICKER_DIAG_MAX_LEN];
    size_t tx_len = 0u;
    int ret;

    if (request == NULL || response == NULL || final == NULL) {
        return -EINVAL;
    }

    diag.header.type = MSG_UWB_CLICKER_DIAG;
    diag.header.seq = request->seq;
    diag.header.round_index = request->round_index;
    diag.header.network_id = request->network_id;
    diag.header.session_id = request->session_id;
    diag.header.session_nonce = request->session_nonce;
    diag.header.initiator_short_addr = short_addr_from_id(request->initiator_id);
    diag.header.responder_short_addr = response->header.responder_short_addr;
    diag.header.flags = request->flags;
    diag.header.initiator_id = request->initiator_id;
    diag.header.responder_id = response->header.responder_id;
    diag.final_tx_ts_32 = final->final_tx_ts_32;
    diag.status_flags = UWB_CLICKER_DIAG_STATUS_RESP_RX_PRESENT |
                        UWB_CLICKER_DIAG_STATUS_COMPACT_BYTES_PRESENT;
    if (resp_rsl_sampled) {
        diag.status_flags |= UWB_CLICKER_DIAG_STATUS_RESP_RSL_PRESENT;
    }
    diag.status_detect_latency_us = 0u;
    diag.resp_quality = resp_quality;
    diag.resp_rsl_dbm = resp_rsl_dbm;
    diag.diag_len = CLICKER_DIAG_COMPACT_BYTES_LEN;
    proto_put_u32_le(&diag.diag_bytes[0], (uint32_t)resp_rx_ts);
    proto_put_u32_le(&diag.diag_bytes[4], response->poll_rx_ts_32);
    proto_put_u32_le(&diag.diag_bytes[8], response->resp_tx_ts_32);

    ret = uwb_encode_clicker_diag(&diag, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        return ret;
    }
    return wait_tx_complete(CLICKER_DIAG_TX_TIMEOUT_MS);
}

static int receive_clicker_diag(const struct uwb_range_header *poll,
                                uint64_t local_anchor_id,
                                struct dwm3000_range_result *result)
{
    struct uwb_clicker_diag_frame diag;
    uint8_t rx_buffer[UWB_CLICKER_DIAG_MAX_LEN + FCS_LEN];
    uint32_t status = 0u;
    size_t frame_len = 0u;
    int ret;

    if (poll == NULL) {
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(0u);
    dwt_setrxtimeout(0u);
    dwt_setpreambledetecttimeout(IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(CLICKER_DIAG_RX_TIMEOUT_MS,
                        &status,
                        rx_buffer,
                        sizeof(rx_buffer),
                        &frame_len,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        false,
                        true);
    if (ret < 0) {
        if (result != NULL) {
            result->clicker_diag_dropped = true;
        }
        return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

    memset(&diag, 0, sizeof(diag));
    ret = decode_clicker_diag_frame(rx_buffer, frame_len, &diag);
    if (ret < 0 || !clicker_diag_matches_poll(&diag, poll, local_anchor_id)) {
        if (result != NULL) {
            result->clicker_diag_dropped = true;
        }
        return -EBADMSG;
    }

    store_clicker_diag_result(result, &diag);
    return 0;
}

static int compute_distance_mm(const struct uwb_final_frame *final,
                               uint64_t poll_rx_ts,
                               uint64_t resp_tx_ts,
                               uint64_t final_rx_ts,
                               int32_t *distance_mm)
{
    uint32_t poll_tx_ts_32 = final->poll_tx_ts_32;
    uint32_t resp_rx_ts_32 = final->resp_rx_ts_32;
    uint32_t final_tx_ts_32 = final->final_tx_ts_32;
    uint32_t poll_rx_ts_32 = (uint32_t)poll_rx_ts;
    uint32_t resp_tx_ts_32 = (uint32_t)resp_tx_ts;
    uint32_t final_rx_ts_32 = (uint32_t)final_rx_ts;
    double ra = (double)(uint32_t)(resp_rx_ts_32 - poll_tx_ts_32);
    double rb = (double)(uint32_t)(final_rx_ts_32 - resp_tx_ts_32);
    double da = (double)(uint32_t)(final_tx_ts_32 - resp_rx_ts_32);
    double db = (double)(uint32_t)(resp_tx_ts_32 - poll_rx_ts_32);
    double denominator = ra + rb + da + db;
    double tof_dtu;
    double distance_m;

    if (distance_mm == NULL || denominator == 0.0) {
        return -EINVAL;
    }

    tof_dtu = ((ra * rb) - (da * db)) / denominator;
    distance_m = tof_dtu * DWM3000_TIME_UNITS * DWM3000_SPEED_OF_LIGHT_MPS;
    *distance_mm = (int32_t)(distance_m * 1000.0);
    return 0;
}

int dwm3000_driver_probe(uint32_t *dev_id)
{
    uint32_t read_id;
    int ret;

    if (dev_id == NULL) {
        return -EINVAL;
    }

    ret = dwm3000_port_init();
    if (ret < 0) {
        return ret;
    }

    ret = ensure_local_data();
    if (ret < 0) {
        return ret;
    }

    (void)dwm3000_port_set_slow_spi();
    read_id = dwt_readdevid();
    if (!dwm3000_port_dev_id_supported(read_id)) {
        return -ENODEV;
    }

    *dev_id = read_id;
    radio_awake = true;
    return 0;
}

int dwm3000_driver_initialise(bool idle_after_init)
{
    return initialise_radio(idle_after_init);
}

int dwm3000_driver_configure_default(void)
{
    int ret;

    ret = dwm3000_port_init();
    if (ret < 0) {
        return ret;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        return ret;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        return ret;
    }

    ret = initialise_radio(false);
    if (ret < 0) {
        return ret;
    }

    ret = apply_radio_config(&default_config, DWM3000_PHY_RANGE);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("DWM3000 configured for channel %u no-STS DS-TWR at 850 kbps, %u Hz SPI; polling SYS_STATUS",
            DWM3000_PHY_CHANNEL,
            (unsigned int)dwm3000_port_current_spi_hz());
    return 0;
}

int dwm3000_driver_configure_wake_mode(void)
{
    int ret;

    ret = ensure_phy_mode(DWM3000_PHY_WAKE);
    if (ret < 0) {
        return ret;
    }

    if (!focused_anchor_rx_logs_enabled()) {
        LOG_DBG("DWM3000 configured for UWB wake/discovery mode: sfd_timeout_symbols=%u",
                (unsigned int)wake_config.sfdTO);
    }
    return 0;
}

int dwm3000_driver_configure_range_mode(void)
{
    return ensure_phy_mode(DWM3000_PHY_RANGE);
}

int dwm3000_driver_configure_mesh_payload_mode(void)
{
    int ret;

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        return ret;
    }
    ret = apply_radio_config(&mesh_payload_config, DWM3000_PHY_MESH_PAYLOAD);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("DWM3000 configured for channel %u 1024-symbol no-STS mesh payload at 850 kbps",
            UWB_CHANNEL_MESH_PAYLOAD);
    return 0;
}

int dwm3000_driver_idle(void)
{
    if (!radio_configured || !radio_awake) {
        return 0;
    }

    dwt_forcetrxoff();
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();
    return 0;
}

int dwm3000_driver_standby(void)
{
    int ret;

    if (!radio_configured || !radio_awake) {
        return 0;
    }

    ret = dwm3000_driver_idle();
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_port_set_slow_spi();
    if (ret < 0) {
        return ret;
    }
    dwt_configuresleep(DWM3000_SLEEP_MODE, DWM3000_SLEEP_WAKE_FLAGS);
    dwt_entersleep(DWT_DW_IDLE_RC);
    radio_awake = false;
    if (!focused_anchor_rx_logs_enabled()) {
        LOG_INF("DWM3000 entered sleep with retained config; wakeup pin required before next UWB window");
    }
    return 0;
}

int dwm3000_driver_send_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t timeout_ms)
{
    int ret;

    if (frame == NULL || frame_len == 0u || frame_len > UINT16_MAX) {
        return -EINVAL;
    }

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        return ret;
    }

    clear_all_events();
    ret = send_range_frame(frame, frame_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        return ret;
    }

    return wait_tx_complete(timeout_ms == 0u ? REPORT_RX_TIMEOUT_MS : timeout_ms);
}

int dwm3000_driver_receive_frame(uint32_t timeout_ms,
                                 uint8_t *frame,
                                 size_t frame_cap,
                                 size_t *frame_len,
                                 uint8_t *quality,
                                 int8_t *rsl_dbm)
{
    return dwm3000_driver_receive_frame_detailed(timeout_ms,
                                                 frame,
                                                 frame_cap,
                                                 frame_len,
                                                 quality,
                                                 rsl_dbm,
                                                 NULL);
}

static int receive_frame_with_preamble_timeout(uint32_t timeout_ms,
                                               uint16_t preamble_timeout_pac,
                                               uint8_t *frame,
                                               size_t frame_cap,
                                               size_t *frame_len,
                                               uint8_t *quality,
                                               int8_t *rsl_dbm,
                                               enum dwm3000_rx_failure *failure,
                                               bool log_events)
{
    uint8_t rx_buffer[UWB_MESH_MAX_FRAME_LEN + FCS_LEN];
    uint32_t status = 0u;
    size_t raw_len = 0u;
    size_t payload_len;
    uint8_t rx_quality = 0u;
    int8_t rx_rsl_dbm = 0;
    int ret;

    if (frame == NULL || frame_len == NULL || frame_cap == 0u || timeout_ms == 0u) {
        return -EINVAL;
    }
    *frame_len = 0u;
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_NONE;
    }

    ret = ensure_current_phy_or_range();
    if (ret < 0) {
        return ret;
    }

    dwt_setpreambledetecttimeout(preamble_timeout_pac);
    dwt_setrxtimeout(0u);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(timeout_ms,
                        &status,
                        rx_buffer,
                        sizeof(rx_buffer),
                        &raw_len,
                        NULL,
                        &rx_quality,
                        &rx_rsl_dbm,
                        NULL,
                        NULL,
                        rsl_dbm != NULL,
                        log_events);
    if (ret < 0) {
        if (failure != NULL) {
            *failure = status_to_rx_failure(status);
            if (*failure == DWM3000_RX_FAILURE_NONE) {
                if (ret == -EMSGSIZE) {
                    *failure = DWM3000_RX_FAILURE_BAD_FRAME;
                }
            }
        }
        if (status_to_rx_failure(status) == DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT) {
            return -ETIMEDOUT;
        }
        return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

    payload_len = payload_len_without_fcs(raw_len);
    if (payload_len > frame_cap) {
        dwt_forcetrxoff();
        if (failure != NULL) {
            *failure = DWM3000_RX_FAILURE_BAD_FRAME;
        }
        return -EMSGSIZE;
    }

    memcpy(frame, rx_buffer, payload_len);
    *frame_len = payload_len;
    if (quality != NULL) {
        *quality = rx_quality;
    }
    if (rsl_dbm != NULL) {
        *rsl_dbm = rx_rsl_dbm;
    }
    return 0;
}

int dwm3000_driver_receive_frame_detailed(uint32_t timeout_ms,
                                          uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure)
{
    return receive_frame_with_preamble_timeout(timeout_ms,
                                               IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC,
                                               frame,
                                               frame_cap,
                                               frame_len,
                                               quality,
                                               rsl_dbm,
                                               failure,
                                               true);
}

int dwm3000_driver_receive_frame_continuous(uint32_t timeout_ms,
                                            uint8_t *frame,
                                            size_t frame_cap,
                                            size_t *frame_len,
                                            uint8_t *quality,
                                            int8_t *rsl_dbm,
                                            enum dwm3000_rx_failure *failure)
{
    bool log_events = !focused_anchor_rx_logs_enabled();

    if (log_events) {
        dwm3000_debug_event(false,
                            "UWB_RX_START",
                            "mode=continuous_open timeout_ms=%u preamble_timeout_pac=0",
                            timeout_ms);
    }
    return receive_frame_with_preamble_timeout(timeout_ms,
                                               0u,
                                               frame,
                                               frame_cap,
                                               frame_len,
                                               quality,
                                               rsl_dbm,
                                               failure,
                                               log_events);
}

int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
                                   struct dwm3000_range_result *result)
{
    struct uwb_response_frame response;
    struct uwb_final_frame final;
    struct uwb_range_header poll_header;
    uint8_t tx_buffer[UWB_CLICKER_DIAG_MAX_LEN];
    size_t tx_len;
    uint64_t poll_tx_ts;
    uint64_t resp_rx_ts;
    uint64_t final_tx_ts;
    uint32_t final_tx_offset;
    uint16_t reply_delay_uus;
    uint8_t quality = 0u;
    int8_t rsl_dbm = 0;
    enum range_status range_status = RANGE_INTERNAL_ERROR;
    int ret;

    if (result == NULL) {
        return -EINVAL;
    }

    memset(result, 0, sizeof(*result));
    ret = validate_range_request(request);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        return ret;
    }

    ret = ensure_phy_mode(DWM3000_PHY_RANGE);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        return ret;
    }
    reply_delay_uus = reply_delay_uus_from_round_index(request->round_index);

    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

    poll_header.type = MSG_UWB_POLL;
    poll_header.seq = request->seq;
    poll_header.round_index = request->round_index;
    poll_header.network_id = request->network_id;
    poll_header.session_id = request->session_id;
    poll_header.session_nonce = request->session_nonce;
    poll_header.initiator_short_addr = short_addr_from_id(request->initiator_id);
    poll_header.responder_short_addr = request->responder_short_addr;
    poll_header.flags = request->flags;
    poll_header.initiator_id = request->initiator_id;
    poll_header.responder_id = request->responder_id;

    ret = uwb_encode_poll(&poll_header, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        result->status = RANGE_INTERNAL_ERROR;
        return -EINVAL;
    }

    clear_all_events();
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        result->status = RANGE_INTERNAL_ERROR;
        return ret;
    }
    result_set_request_metadata(result, request);
    result->exchange_start_ms = k_uptime_get();
    result->exchange_started = true;

    ret = receive_response(request, &response, &resp_rx_ts, &quality, &rsl_dbm,
                           NULL, NULL, &range_status, false);
    if (ret < 0) {
        result_set_request_metadata(result, request);
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->status = range_status;
        return ret;
    }
    poll_tx_ts = read_tx_timestamp_u64();

    final_tx_offset = delayed_trx_offset_from_uus(reply_delay_uus);
    dwt_setdelayedtrxtime(final_tx_offset);
    final_tx_ts = delayed_tx_timestamp_from_rx_reference(resp_rx_ts,
                                                         final_tx_offset);

    final.header.type = MSG_UWB_FINAL;
    final.header.seq = request->seq;
    final.header.round_index = request->round_index;
    final.header.network_id = request->network_id;
    final.header.session_id = request->session_id;
    final.header.session_nonce = request->session_nonce;
    final.header.initiator_short_addr = poll_header.initiator_short_addr;
    final.header.responder_short_addr = response.header.responder_short_addr;
    final.header.flags = request->flags;
    final.header.initiator_id = request->initiator_id;
    final.header.responder_id = response.header.responder_id;
    final.poll_tx_ts_32 = (uint32_t)poll_tx_ts;
    final.resp_rx_ts_32 = (uint32_t)resp_rx_ts;
    final.final_tx_ts_32 = (uint32_t)final_tx_ts;

    ret = validate_driver_reply_timing(
        dwt_delta_to_uus(response.poll_rx_ts_32, response.resp_tx_ts_32),
        dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
        reply_delay_uus);
    if (ret != PROTO_OK) {
        result_set_request_metadata(result, request);
        result->responder_id = request->responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->status = RANGE_TIMING_INVALID;
        return -ETIME;
    }

    ret = uwb_encode_final(&final, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        result->status = RANGE_INTERNAL_ERROR;
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(0u);
    dwt_setrxtimeout(REPORT_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

    ret = send_range_frame(tx_buffer, tx_len,
                           DWT_START_TX_DLY_RS | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        result_set_request_metadata(result, request);
        result->responder_id = request->responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->status = RANGE_DELAYED_TX_MISSED;
        return -ETIME;
    }

    ret = receive_report(request, response.header.responder_id, result);
    if (ret == 0 && result->status == RANGE_OK && DWM3000_CLICKER_DIAG_ENABLED) {
        int diag_ret = send_clicker_diag(request,
                                         &response,
                                         &final,
                                         resp_rx_ts,
                                         quality,
                                         rsl_dbm,
                                         false);

        if (diag_ret < 0) {
            LOG_WRN("UWB clicker diagnostic TX failed: anchor=0x%016llx seq=%u ret=%d",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    diag_ret);
        }
    }
    if (ret < 0 && result->status == RANGE_OK) {
        result->status = RANGE_INTERNAL_ERROR;
    }
    if (result->quality == 0u) {
        result->quality = quality;
    }
    if (result->rsl_dbm == 0) {
        result->rsl_dbm = rsl_dbm;
    }
    return ret;
}

static int responder_poll_once(uint64_t local_anchor_id,
                               const struct dwm3000_range_request *expected,
                               uint32_t timeout_ms,
                               struct dwm3000_range_result *result)
{
    struct uwb_range_header poll;
    struct uwb_response_frame response;
    struct uwb_final_frame final;
    struct uwb_report_frame report;
    uint8_t rx_buffer[UWB_FINAL_LEN + FCS_LEN];
    uint8_t tx_buffer[UWB_RESP_LEN];
    size_t frame_len;
    size_t tx_len;
    uint32_t status;
    uint32_t resp_tx_offset;
    uint64_t poll_rx_ts;
    uint64_t resp_tx_ts;
    uint64_t final_rx_ts;
    uint16_t reply_delay_uus;
    uint8_t quality = 0u;
    int8_t rsl_dbm = 0;
    uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {0};
    int32_t distance_mm = 0;
    enum range_status report_status = RANGE_OK;
    bool cir_sampled = false;
    bool capture_final_rsl;
    int ret;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = RANGE_INTERNAL_ERROR;
    }

    if (local_anchor_id == 0u || expected == NULL) {
        return -EINVAL;
    }

    ret = ensure_phy_mode(DWM3000_PHY_RANGE);
    if (ret < 0) {
        return ret;
    }

    dwt_setpreambledetecttimeout(0u);
    dwt_setrxtimeout(0u);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(timeout_ms == 0u ? DEFAULT_RESPONDER_WINDOW_MS : timeout_ms,
                        &status, rx_buffer, sizeof(rx_buffer), &frame_len,
                        &poll_rx_ts, &quality, &rsl_dbm, NULL, NULL, false, true);
    if (ret < 0) {
        if (ret == -ETIMEDOUT) {
            return -ETIMEDOUT;
        }
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = ret == -EMSGSIZE ? RANGE_BAD_FRAME :
                             RANGE_RX_ERROR;
        }
        return -EAGAIN;
    }

    ret = decode_poll_frame(rx_buffer, frame_len, &poll);
    if (ret < 0) {
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_BAD_FRAME;
        }
        return -EAGAIN;
    }
    if (!poll_targets_anchor(&poll, local_anchor_id)) {
        if (result != NULL) {
            result_set_poll_metadata(result, &poll, poll.responder_id);
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_WRONG_TARGET;
        }
        return -EAGAIN;
    }
    if (!poll_matches_expected(&poll, expected)) {
        if (result != NULL) {
            result_set_poll_metadata(result, &poll, local_anchor_id);
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_WRONG_TARGET;
        }
        return -EAGAIN;
    }
    if (result != NULL) {
        result_set_poll_metadata(result, &poll, local_anchor_id);
        result->exchange_start_ms = k_uptime_get();
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->exchange_started = true;
        result->status = RANGE_INTERNAL_ERROR;
    }
    reply_delay_uus = reply_delay_uus_from_round_index(poll.round_index);

    resp_tx_offset = delayed_trx_offset_from_uus(reply_delay_uus);
    dwt_setdelayedtrxtime(resp_tx_offset);
    resp_tx_ts = delayed_tx_timestamp_from_rx_reference(poll_rx_ts,
                                                        resp_tx_offset);

    response.header.type = MSG_UWB_RESP;
    response.header.seq = poll.seq;
    response.header.round_index = poll.round_index;
    response.header.network_id = poll.network_id;
    response.header.session_id = poll.session_id;
    response.header.session_nonce = poll.session_nonce;
    response.header.initiator_short_addr = poll.initiator_short_addr;
    response.header.responder_short_addr = short_addr_from_id(local_anchor_id);
    response.header.flags = poll.flags;
    response.header.initiator_id = poll.initiator_id;
    response.header.responder_id = local_anchor_id;
    response.poll_rx_ts_32 = (uint32_t)poll_rx_ts;
    response.resp_tx_ts_32 = (uint32_t)resp_tx_ts;

    ret = uwb_encode_response(&response, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        if (result != NULL) {
            result->status = RANGE_INTERNAL_ERROR;
        }
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(RESP_TX_TO_FINAL_RX_DLY_UUS);
    dwt_setrxtimeout(FINAL_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

    ret = send_range_frame(tx_buffer, tx_len,
                           DWT_START_TX_DLY_RS | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        if (result != NULL) {
            result->status = RANGE_DELAYED_TX_MISSED;
        }
        return -ETIME;
    }

    capture_final_rsl = expected->capture_rsl;
    ret = receive_frame(FINAL_RX_TIMEOUT_MS, &status,
                        rx_buffer, sizeof(rx_buffer), &frame_len,
                        &final_rx_ts, &quality, &rsl_dbm,
                        cir_sample, &cir_sampled,
                        capture_final_rsl,
                        true);
    if (ret < 0) {
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : RANGE_RX_ERROR;
        }
        return ret;
    }

    ret = decode_final_frame(rx_buffer, frame_len, &final);
    if (ret < 0) {
        report_status = RANGE_BAD_FRAME;
    } else if (final.header.seq != poll.seq ||
               final.header.round_index != poll.round_index ||
               final.header.network_id != poll.network_id ||
               final.header.session_id != poll.session_id ||
               final.header.session_nonce != poll.session_nonce ||
               final.header.initiator_short_addr != poll.initiator_short_addr ||
               final.header.responder_short_addr != short_addr_from_id(local_anchor_id) ||
               final.header.flags != poll.flags ||
               final.header.initiator_id != poll.initiator_id ||
               final.header.responder_id != local_anchor_id) {
        report_status = RANGE_WRONG_TARGET;
    } else if (validate_driver_reply_timing(
                   dwt_delta_to_uus((uint32_t)poll_rx_ts, (uint32_t)resp_tx_ts),
                   dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
                   reply_delay_uus) != PROTO_OK) {
        report_status = RANGE_TIMING_INVALID;
    } else {
        ret = compute_distance_mm(&final, poll_rx_ts, resp_tx_ts, final_rx_ts, &distance_mm);
        if (ret < 0) {
            report_status = RANGE_INTERNAL_ERROR;
        }
    }

    report.header.type = MSG_UWB_REPORT;
    report.header.seq = poll.seq;
    report.header.round_index = poll.round_index;
    report.header.network_id = poll.network_id;
    report.header.session_id = poll.session_id;
    report.header.session_nonce = poll.session_nonce;
    report.header.initiator_short_addr = poll.initiator_short_addr;
    report.header.responder_short_addr = short_addr_from_id(local_anchor_id);
    report.header.flags = poll.flags;
    report.header.initiator_id = poll.initiator_id;
    report.header.responder_id = local_anchor_id;
    report.distance_mm = distance_mm;
    report.quality = quality;
    report.status = report_status;
    report.rsl_dbm = rsl_dbm;

    ret = uwb_encode_report(&report, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        return ret;
    }

    ret = wait_tx_complete(REPORT_RX_TIMEOUT_MS);
    if (ret < 0) {
        return ret;
    }

    if (report_status == RANGE_OK && DWM3000_CLICKER_DIAG_ENABLED) {
        int diag_ret = receive_clicker_diag(&poll, local_anchor_id, result);

        if (diag_ret < 0) {
            LOG_DBG("UWB clicker diagnostic not received: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    diag_ret);
        }
    }

    LOG_INF("UWB report sent to short=0x%04x: status=%u distance_mm=%d quality=%u rsl=%d dBm",
            poll.initiator_short_addr,
            report.status,
            report.distance_mm,
            report.quality,
            report.rsl_dbm);
    if (result != NULL) {
        result_set_poll_metadata(result, &poll, local_anchor_id);
        result->distance_mm = distance_mm;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = capture_final_rsl;
        if (cir_sampled) {
            memcpy(result->cir_sample, cir_sample, UWB_CIR_SAMPLE_LEN);
        }
        result->cir_sampled = cir_sampled;
        result->status = report_status;
    }
    return report_status == RANGE_OK ? 0 : -EIO;
}

int dwm3000_driver_responder_poll_expected(uint64_t local_anchor_id,
                                           const struct dwm3000_range_request *expected,
                                           uint32_t timeout_ms,
                                           struct dwm3000_range_result *result)
{
    int ret;

    if (expected == NULL) {
        return -EINVAL;
    }
    ret = validate_range_request(expected);
    if (ret < 0) {
        return ret;
    }
    if (expected->responder_id != local_anchor_id ||
        expected->responder_short_addr != short_addr_from_id(local_anchor_id)) {
        return -EINVAL;
    }

    return responder_poll_once(local_anchor_id, expected, timeout_ms, result);
}

int dwm3000_driver_listen_activity(uint32_t timeout_ms, bool *activity_detected)
{
    uint32_t status = 0u;
    int ret;

    if (activity_detected == NULL || timeout_ms == 0u) {
        return -EINVAL;
    }

    *activity_detected = false;
    ret = ensure_phy_mode(DWM3000_PHY_RANGE);
    if (ret < 0) {
        return ret;
    }

    dwt_setpreambledetecttimeout(0u);
    dwt_setrxtimeout(0u);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = wait_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                      timeout_ms,
                      &status);
    dwt_forcetrxoff();
    clear_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
    if (ret == -ETIMEDOUT) {
        return 0;
    }
    if (ret < 0) {
        return ret;
    }

    *activity_detected = (status & (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR)) != 0u;
    return 0;
}

void dwm3000_driver_stats_reset(void)
{
    memset(&driver_stats, 0, sizeof(driver_stats));
}

void dwm3000_driver_stats_get(struct dwm3000_driver_stats *stats)
{
    if (stats != NULL) {
        *stats = driver_stats;
    }
}
