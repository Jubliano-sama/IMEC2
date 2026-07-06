#include "dwm3000_driver.h"

#include "app_board.h"
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
#define DWM3000_ACCUM_CIR_SAMPLE_READ_LEN (DWM3000_FULL_CIR_SAMPLE_BYTES + 1u)
#define DWM3000_FULL_CIR_READ_CHUNK_SAMPLES \
    (UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES / DWM3000_FULL_CIR_SAMPLE_BYTES)
#define DWM3000_FULL_CIR_READ_CHUNK_BYTES \
    (DWM3000_FULL_CIR_READ_CHUNK_SAMPLES * DWM3000_FULL_CIR_SAMPLE_BYTES)
#define DWM3000_CIR_WINDOW_ANCHOR_BUDGET_COUNT 8u

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
#define POLL_RX_TO_RESP_TX_DLY_UUS DS_TWR_REPLY_DLY_UUS

#ifndef DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS
#define DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS 4000u
#endif
#ifndef DWM3000_DS_TWR_RX_GUARD_AFTER_UUS
#define DWM3000_DS_TWR_RX_GUARD_AFTER_UUS 5000u
#endif
#ifndef DWM3000_DS_TWR_RX_WAIT_MARGIN_MS
#define DWM3000_DS_TWR_RX_WAIT_MARGIN_MS 8u
#endif
#define DS_TWR_UUS_TO_MS_CEIL(value_uus) (((uint32_t)(value_uus) + 999u) / 1000u)
#define DS_TWR_RX_DEFAULT_GUARD_BEFORE_UUS \
    (DS_TWR_REPLY_DLY_UUS > DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS ? \
     DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS : DS_TWR_REPLY_DLY_UUS)
#define DS_TWR_RX_DEFAULT_AFTER_TX_DLY_UUS \
    (DS_TWR_REPLY_DLY_UUS - DS_TWR_RX_DEFAULT_GUARD_BEFORE_UUS)
#define DS_TWR_RX_DEFAULT_TIMEOUT_UUS \
    (DS_TWR_RX_DEFAULT_GUARD_BEFORE_UUS + DWM3000_DS_TWR_RX_GUARD_AFTER_UUS)

#ifndef DWM3000_POLL_TX_TO_RESP_RX_DLY_UUS
#define DWM3000_POLL_TX_TO_RESP_RX_DLY_UUS DS_TWR_RX_DEFAULT_AFTER_TX_DLY_UUS
#endif
#define POLL_TX_TO_RESP_RX_DLY_UUS DWM3000_POLL_TX_TO_RESP_RX_DLY_UUS
#ifndef DWM3000_RESP_RX_TIMEOUT_UUS
#define DWM3000_RESP_RX_TIMEOUT_UUS DS_TWR_RX_DEFAULT_TIMEOUT_UUS
#endif
#define RESP_RX_TIMEOUT_UUS DWM3000_RESP_RX_TIMEOUT_UUS
#ifndef DWM3000_RESP_RX_TIMEOUT_MS
#define DWM3000_RESP_RX_TIMEOUT_MS \
    (DS_TWR_UUS_TO_MS_CEIL(POLL_TX_TO_RESP_RX_DLY_UUS + RESP_RX_TIMEOUT_UUS) + \
     DWM3000_DS_TWR_RX_WAIT_MARGIN_MS)
#endif
#define RESP_RX_TIMEOUT_MS DWM3000_RESP_RX_TIMEOUT_MS
#ifndef DWM3000_RESP_TX_TO_FINAL_RX_DLY_UUS
#define DWM3000_RESP_TX_TO_FINAL_RX_DLY_UUS DS_TWR_RX_DEFAULT_AFTER_TX_DLY_UUS
#endif
#define RESP_TX_TO_FINAL_RX_DLY_UUS DWM3000_RESP_TX_TO_FINAL_RX_DLY_UUS
#ifndef DWM3000_FINAL_RX_TIMEOUT_UUS
#define DWM3000_FINAL_RX_TIMEOUT_UUS DS_TWR_RX_DEFAULT_TIMEOUT_UUS
#endif
#define FINAL_RX_TIMEOUT_UUS DWM3000_FINAL_RX_TIMEOUT_UUS
#ifndef DWM3000_FINAL_RX_TIMEOUT_MS
#define DWM3000_FINAL_RX_TIMEOUT_MS \
    (DS_TWR_UUS_TO_MS_CEIL(RESP_TX_TO_FINAL_RX_DLY_UUS + FINAL_RX_TIMEOUT_UUS) + \
     DWM3000_DS_TWR_RX_WAIT_MARGIN_MS)
#endif
#define FINAL_RX_TIMEOUT_MS DWM3000_FINAL_RX_TIMEOUT_MS
#ifndef DWM3000_REPORT_RX_TIMEOUT_UUS
#define DWM3000_REPORT_RX_TIMEOUT_UUS 16000u
#endif
#define REPORT_RX_TIMEOUT_UUS DWM3000_REPORT_RX_TIMEOUT_UUS
#ifndef DWM3000_REPORT_RX_TIMEOUT_MS
#define DWM3000_REPORT_RX_TIMEOUT_MS \
    (DS_TWR_UUS_TO_MS_CEIL(REPORT_RX_TIMEOUT_UUS) + \
     DWM3000_DS_TWR_RX_WAIT_MARGIN_MS)
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
#define CLICKER_DIAG_TX_TIMEOUT_MS 16u
#define CLICKER_DIAG_RX_TIMEOUT_MS 16u
#define CLICKER_DIAG_TX_AFTER_FINAL_DELAY_US 2000u
#define CLICKER_DIAG_RX_PREAMBLE_TIMEOUT_PAC 0u
#define CLICKER_DIAG_COMPACT_BYTES_LEN 12u
#define ANCHOR_DIAG_TX_TIMEOUT_MS 40u
#define ANCHOR_DIAG_RX_TIMEOUT_MS 80u
#define ANCHOR_DIAG_FRAGMENT_TX_TIMEOUT_MS 40u
#define ANCHOR_DIAG_FRAGMENT_RX_TIMEOUT_MS 2000u
#define ANCHOR_DIAG_FRAGMENT_TX_GAP_US 1500u
#define CLICKER_DIAG_FULL_ANCHOR_DELAY_US 25000u
#define ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC 0u
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
#define DWM3000_PHY_RX_PAC DWT_PAC16
#endif
#ifndef DWM3000_PHY_TX_CODE
#define DWM3000_PHY_TX_CODE 9
#endif
#ifndef DWM3000_PHY_RX_CODE
#define DWM3000_PHY_RX_CODE 9
#endif
#ifndef DWM3000_PHY_SFD_TYPE
#define DWM3000_PHY_SFD_TYPE DWT_SFD_DW_16
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
#define DWM3000_PHY_SFD_TIMEOUT 4097
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
#ifndef DWM3000_MESH_PHY_PREAMBLE_LENGTH
#define DWM3000_MESH_PHY_PREAMBLE_LENGTH DWT_PLEN_1024
#endif
#ifndef DWM3000_MESH_PHY_RX_PAC
#define DWM3000_MESH_PHY_RX_PAC DWT_PAC8
#endif
#ifndef DWM3000_MESH_PHY_TX_CODE
#define DWM3000_MESH_PHY_TX_CODE 9
#endif
#ifndef DWM3000_MESH_PHY_RX_CODE
#define DWM3000_MESH_PHY_RX_CODE 9
#endif
#ifndef DWM3000_MESH_PHY_SFD_TYPE
#define DWM3000_MESH_PHY_SFD_TYPE DWT_SFD_IEEE_4Z
#endif
#ifndef DWM3000_MESH_PHY_SFD_TIMEOUT
#define DWM3000_MESH_PHY_SFD_TIMEOUT (1024 + 1 + 8 - 8)
#endif
#ifndef DWM3000_MESH_PHY_PHR_MODE
#define DWM3000_MESH_PHY_PHR_MODE DWT_PHRMODE_EXT
#endif
#ifndef DWM3000_MESH_PHY_PHR_RATE
#define DWM3000_MESH_PHY_PHR_RATE DWT_PHRRATE_STD
#endif
#ifndef DWM3000_MESH_PHY_STS_MODE
#define DWM3000_MESH_PHY_STS_MODE DWT_STS_MODE_OFF
#endif
#ifndef DWM3000_MESH_PHY_STS_LENGTH
#define DWM3000_MESH_PHY_STS_LENGTH DWT_STS_LEN_64
#endif
#ifndef DWM3000_MESH_PHY_PDOA_MODE
#define DWM3000_MESH_PHY_PDOA_MODE DWT_PDOA_M0
#endif
#ifndef DWM3000_MESH_TX_POWER
#define DWM3000_MESH_TX_POWER 0xfefefefeu
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
BUILD_ASSERT(DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS <= UINT16_MAX,
             "DS-TWR delayed-RX leading guard must fit DWM3000 timeout units");
BUILD_ASSERT((DWM3000_CIR_ACCUM_SAMPLE_COUNT * DWM3000_CIR_SAMPLE_BYTES) + 1u ==
             ACC_BUFFER_MAX_LEN,
             "DWM3000 accumulator constants must match the SDK buffer size plus dummy byte");
BUILD_ASSERT(DWM3000_CIR_WINDOW_PRE_SAMPLES < DWM3000_CIR_WINDOW_SAMPLE_COUNT,
             "CIR window must include samples after the first path");
BUILD_ASSERT(DWM3000_CIR_WINDOW_SAMPLE_COUNT <= DWM3000_CIR_ACCUM_SAMPLE_COUNT,
             "CIR window must fit inside the DWM3000 accumulator");
BUILD_ASSERT((DWM3000_CIR_WINDOW_BYTES * DWM3000_CIR_WINDOW_ANCHOR_BUDGET_COUNT) <=
             (DWM3000_CIR_ACCUM_SAMPLE_COUNT * DWM3000_CIR_SAMPLE_BYTES),
             "Eight CIR windows must fit in the memory used by one full accumulator");
BUILD_ASSERT(DWM3000_FULL_CIR_READ_CHUNK_SAMPLES > 0u,
             "CIR window read chunks must include at least one complex sample");
BUILD_ASSERT(DWM3000_FULL_CIR_READ_CHUNK_BYTES <= UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES,
             "CIR window read chunk must fit the anchor diagnostic fragment payload");
BUILD_ASSERT(((DWM3000_FULL_CIR_BYTES + UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES - 1u) /
              UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES) <= UINT8_MAX,
             "CIR window UWB fragment count must fit in the fragment header");
BUILD_ASSERT(DWM3000_DS_TWR_RX_GUARD_AFTER_UUS <= UINT16_MAX,
             "DS-TWR delayed-RX trailing guard must fit DWM3000 timeout units");
BUILD_ASSERT(POLL_TX_TO_RESP_RX_DLY_UUS <= DS_TWR_REPLY_DLY_UUS,
             "initiator response RX must not start after the scheduled response");
BUILD_ASSERT((POLL_TX_TO_RESP_RX_DLY_UUS + RESP_RX_TIMEOUT_UUS) >=
             (DS_TWR_REPLY_DLY_UUS + DWM3000_DS_TWR_RX_GUARD_AFTER_UUS),
             "initiator response RX window must cover the selected reply delay");
BUILD_ASSERT(RESP_TX_TO_FINAL_RX_DLY_UUS <= DS_TWR_REPLY_DLY_UUS,
             "responder final RX must not start after the scheduled final");
BUILD_ASSERT((RESP_TX_TO_FINAL_RX_DLY_UUS + FINAL_RX_TIMEOUT_UUS) >=
             (DS_TWR_REPLY_DLY_UUS + DWM3000_DS_TWR_RX_GUARD_AFTER_UUS),
             "responder final RX window must cover the selected reply delay");
BUILD_ASSERT(RESP_RX_TIMEOUT_MS >=
             DS_TWR_UUS_TO_MS_CEIL(POLL_TX_TO_RESP_RX_DLY_UUS + RESP_RX_TIMEOUT_UUS),
             "initiator response wait timeout must cover delayed RX");
BUILD_ASSERT(FINAL_RX_TIMEOUT_MS >=
             DS_TWR_UUS_TO_MS_CEIL(RESP_TX_TO_FINAL_RX_DLY_UUS + FINAL_RX_TIMEOUT_UUS),
             "responder final wait timeout must cover delayed RX");
BUILD_ASSERT(REPORT_RX_TIMEOUT_MS >= DS_TWR_UUS_TO_MS_CEIL(REPORT_RX_TIMEOUT_UUS),
             "initiator report wait timeout must cover report RX timeout");
BUILD_ASSERT((UWB_MESH_MAX_FRAME_LEN + UWB_PHY_FCS_LEN) <= UWB_PHY_EXTENDED_FRAME_MAX_LEN,
             "channel-9 mesh frames must fit the DW3000 extended PHR frame limit");

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

static dwt_config_t wake_mesh_control_config = {
    DWM3000_PHY_CHANNEL,
    DWM3000_PHY_PREAMBLE_LENGTH,
    DWM3000_PHY_RX_PAC,
    DWM3000_PHY_TX_CODE,
    DWM3000_PHY_RX_CODE,
    DWM3000_PHY_SFD_TYPE,
    DWM3000_PHY_DATA_RATE,
    DWM3000_MESH_PHY_PHR_MODE,
    DWM3000_MESH_PHY_PHR_RATE,
    DWM3000_WAKE_PHY_SFD_TIMEOUT,
    DWM3000_PHY_STS_MODE,
    DWM3000_PHY_STS_LENGTH,
    DWM3000_PHY_PDOA_MODE,
};

static dwt_config_t mesh_payload_config = {
    UWB_CHANNEL_MESH_PAYLOAD,
    DWM3000_MESH_PHY_PREAMBLE_LENGTH,
    DWM3000_MESH_PHY_RX_PAC,
    DWM3000_MESH_PHY_TX_CODE,
    DWM3000_MESH_PHY_RX_CODE,
    DWM3000_MESH_PHY_SFD_TYPE,
    DWM3000_PHY_DATA_RATE,
    DWM3000_MESH_PHY_PHR_MODE,
    DWM3000_MESH_PHY_PHR_RATE,
    DWM3000_MESH_PHY_SFD_TIMEOUT,
    DWM3000_MESH_PHY_STS_MODE,
    DWM3000_MESH_PHY_STS_LENGTH,
    DWM3000_MESH_PHY_PDOA_MODE,
};

static dwt_txconfig_t default_txconfig = {
    DWM3000_TX_PG_DELAY,
    DWM3000_TX_POWER,
    DWM3000_TX_PG_COUNT,
};

static dwt_txconfig_t mesh_payload_txconfig = {
    DWM3000_TX_PG_DELAY,
    DWM3000_MESH_TX_POWER,
    DWM3000_TX_PG_COUNT,
};

enum dwm3000_phy_mode {
    DWM3000_PHY_NONE = 0,
    DWM3000_PHY_RANGE = 1,
    DWM3000_PHY_WAKE = 2,
    DWM3000_PHY_MESH_PAYLOAD = 3,
    DWM3000_PHY_WAKE_MESH_CONTROL = 4,
};

static bool radio_configured;
static bool radio_awake;
static bool radio_restored_from_sleep;
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

static uint32_t dwt_time32_delta_to_uus(uint32_t start_time32, uint32_t end_time32)
{
    uint32_t delta = end_time32 - start_time32;

    return (uint32_t)((((uint64_t)delta << 8) +
                       (DWM3000_UUS_TO_DWT_TIME / 2u)) /
                      DWM3000_UUS_TO_DWT_TIME);
}

static uint32_t delayed_tx_time_from_rx_reference(uint64_t rx_reference_ts,
                                                  uint16_t delay_uus)
{
    return (uint32_t)((rx_reference_ts +
                       ((uint64_t)delay_uus * DWM3000_UUS_TO_DWT_TIME)) >> 8);
}

static uint64_t delayed_tx_timestamp_from_programmed_time(uint32_t tx_time)
{
    return (((uint64_t)(tx_time & 0xfffffffeUL)) << 8) + DWM3000_TX_ANT_DLY;
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

static uint16_t request_reply_delay_uus(const struct dwm3000_range_request *request)
{
    if (request != NULL && request->reply_delay_uus != 0u) {
        return request->reply_delay_uus;
    }
    return reply_delay_uus_from_round_index(request == NULL ? 0u : request->round_index);
}

static uint16_t ds_twr_rx_guard_before_uus(uint16_t reply_delay_uus)
{
    return reply_delay_uus > DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS ?
           DWM3000_DS_TWR_RX_GUARD_BEFORE_UUS : reply_delay_uus;
}

static uint16_t ds_twr_rx_after_tx_delay_uus(uint16_t reply_delay_uus)
{
    return reply_delay_uus - ds_twr_rx_guard_before_uus(reply_delay_uus);
}

static uint16_t ds_twr_rx_timeout_uus(uint16_t reply_delay_uus)
{
    uint32_t timeout_uus = (uint32_t)ds_twr_rx_guard_before_uus(reply_delay_uus) +
                           DWM3000_DS_TWR_RX_GUARD_AFTER_UUS;

    return timeout_uus > UINT16_MAX ? UINT16_MAX : (uint16_t)timeout_uus;
}

static uint32_t ds_twr_rx_wait_timeout_ms(uint16_t reply_delay_uus)
{
    uint32_t timeout_uus = (uint32_t)ds_twr_rx_after_tx_delay_uus(reply_delay_uus) +
                           ds_twr_rx_timeout_uus(reply_delay_uus);

    return DS_TWR_UUS_TO_MS_CEIL(timeout_uus) + DWM3000_DS_TWR_RX_WAIT_MARGIN_MS;
}

static int validate_driver_reply_timing(uint16_t poll_to_resp_uus,
                                        uint16_t resp_to_final_uus,
                                        uint16_t expected_uus)
{
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

static dwt_txconfig_t *txconfig_for_phy(enum dwm3000_phy_mode phy_mode)
{
    return phy_mode == DWM3000_PHY_MESH_PAYLOAD ?
           &mesh_payload_txconfig :
           &default_txconfig;
}

static int apply_radio_config(const dwt_config_t *config,
                              enum dwm3000_phy_mode phy_mode)
{
    if (config == NULL) {
        return -EINVAL;
    }

    if (dwt_configure((dwt_config_t *)config) != DWT_SUCCESS) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_CONFIG_FAIL phy=%d ret=%d sys=0x%08x\n",
                                phy_mode, -EIO, (unsigned int)sys_status);
        }
        return -EIO;
    }

    configure_first_path_sensitivity();
    dwt_configuretxrf(txconfig_for_phy(phy_mode));
    dwt_configciadiag(DWM3000_CIA_DIAG_MODE);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC);
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();

    radio_configured = true;
    radio_awake = true;
    radio_restored_from_sleep = false;
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
    case DWM3000_PHY_WAKE_MESH_CONTROL:
        return &wake_mesh_control_config;
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

static int configure_radio_from_reset(enum dwm3000_phy_mode phy_mode)
{
    const dwt_config_t *config = config_for_phy(phy_mode);
    int ret;

    ret = dwm3000_port_init();
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_PORT_INIT_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_WAKE_PIN_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_RESET_HW_FAIL phy=%d ret=%d\n",
                                phy_mode, ret);
        }
        return ret;
    }

    ret = initialise_radio(false);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_RESET_INIT_FAIL phy=%d ret=%d sys=0x%08x\n",
                                phy_mode, ret, (unsigned int)sys_status);
        }
        return ret;
    }

    return apply_radio_config(config, phy_mode);
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

    ret = dwm3000_port_set_slow_spi();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_SLOW_SPI_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
        return ret;
    }
    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_PIN_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
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
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_WAKE_IDLE_RC_TIMEOUT phy=%d ret=%d sys=0x%08x\n",
                                requested_phy, ret, (unsigned int)sys_status);
        }
        return ret;
    }

    ret = restore_txrx_after_sleep();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t sys_status = dwt_read32bitreg(SYS_STATUS_ID);

            status_debug_printf("DBG_DWM_WAKE_RESTORE_FAIL phy=%d ret=%d sys=0x%08x\n",
                                requested_phy, ret, (unsigned int)sys_status);
        }
        return ret;
    }
    radio_awake = true;
    active_phy_mode = requested_phy;
    radio_restored_from_sleep = true;

    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        driver_stats.sleep_wake_failures++;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_FAST_SPI_FAIL phy=%d ret=%d\n",
                                requested_phy, ret);
        }
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
        ret = configure_radio_from_reset(phy_mode);
        if (ret < 0) {
            return ret;
        }
    } else {
        ret = wake_configured_radio();
        if (ret < 0) {
            return ret;
        }
    }

    if (active_phy_mode == phy_mode && !radio_restored_from_sleep) {
        return 0;
    }

    config = config_for_phy(phy_mode);
    active_config = config_for_phy(active_phy_mode);
    if (!radio_restored_from_sleep && phy_configs_equal(active_config, config)) {
        active_phy_mode = phy_mode;
        LOG_DBG("DWM3000 PHY mode %d selected without redundant reconfigure",
                phy_mode);
        return 0;
    }

    ret = apply_radio_config(config, phy_mode);
    if (ret == 0) {
        return 0;
    }
    if (!radio_restored_from_sleep) {
        return ret;
    }

    LOG_WRN("DWM3000 PHY configure failed after retained sleep restore: phy=%d ret=%d; forcing full reinit",
            phy_mode,
            ret);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DWM_RETAIN_REINIT phy=%d ret=%d\n", phy_mode, ret);
    }
    ret = configure_radio_from_reset(phy_mode);
    if (ret < 0) {
        LOG_WRN("DWM3000 full reinit after retained sleep configure failure failed: ret=%d",
                ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_REINIT_FAIL ret=%d\n", ret);
        }
        return ret;
    }
    LOG_INF("DWM3000 PHY mode %d configured directly after full reinit",
            phy_mode);
    return 0;
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

#if defined(CONFIG_IMEC_ML_ANCHOR) || defined(CONFIG_IMEC_HIGH_DEBUG)
static const char *rx_failure_debug_name(enum dwm3000_rx_failure failure)
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
#endif

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
    if (fp_int >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        return false;
    }

    dwt_readaccdata(accum, sizeof(accum), fp_int);
    memcpy(cir_sample, &accum[1], UWB_CIR_SAMPLE_LEN);
    return true;
}

static void pack_rx_diag_raw(const dwt_rxdiag_t *diagnostics,
                             uint8_t raw[DWM3000_RX_DIAG_RAW_LEN],
                             uint8_t *raw_len)
{
    size_t offset = 0u;

    if (raw_len != NULL) {
        *raw_len = 0u;
    }
    if (diagnostics == NULL || raw == NULL) {
        return;
    }

    memcpy(&raw[offset], diagnostics->ipatovRxTime, sizeof(diagnostics->ipatovRxTime));
    offset += sizeof(diagnostics->ipatovRxTime);
    raw[offset++] = diagnostics->ipatovRxStatus;
    proto_put_u16_le(&raw[offset], diagnostics->ipatovPOA);
    offset += sizeof(uint16_t);
    memcpy(&raw[offset], diagnostics->stsRxTime, sizeof(diagnostics->stsRxTime));
    offset += sizeof(diagnostics->stsRxTime);
    proto_put_u16_le(&raw[offset], diagnostics->stsRxStatus);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsPOA);
    offset += sizeof(uint16_t);
    memcpy(&raw[offset], diagnostics->sts2RxTime, sizeof(diagnostics->sts2RxTime));
    offset += sizeof(diagnostics->sts2RxTime);
    proto_put_u16_le(&raw[offset], diagnostics->sts2RxStatus);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2POA);
    offset += sizeof(uint16_t);
    memcpy(&raw[offset], diagnostics->tdoa, sizeof(diagnostics->tdoa));
    offset += sizeof(diagnostics->tdoa);
    proto_put_u16_le(&raw[offset], (uint16_t)diagnostics->pdoa);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], (uint16_t)diagnostics->xtalOffset);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->ciaDiag1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovPeak);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovPower);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovF1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovF2);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->ipatovF3);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->ipatovFpIndex);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->ipatovAccumCount);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsPeak);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsPower);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsF1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsF2);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->stsF3);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsFpIndex);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->stsAccumCount);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2Peak);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2Power);
    offset += sizeof(uint16_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2F1);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2F2);
    offset += sizeof(uint32_t);
    proto_put_u32_le(&raw[offset], diagnostics->sts2F3);
    offset += sizeof(uint32_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2FpIndex);
    offset += sizeof(uint16_t);
    proto_put_u16_le(&raw[offset], diagnostics->sts2AccumCount);
    offset += sizeof(uint16_t);

    if (offset == DWM3000_RX_DIAG_RAW_LEN && raw_len != NULL) {
        *raw_len = (uint8_t)offset;
    }
}

static bool capture_rx_diag_raw(uint8_t raw[DWM3000_RX_DIAG_RAW_LEN],
                                uint8_t *raw_len,
                                uint16_t *first_path_index)
{
    dwt_rxdiag_t diagnostics;

    if (raw_len != NULL) {
        *raw_len = 0u;
    }
    if (first_path_index != NULL) {
        *first_path_index = 0u;
    }
    if (raw == NULL) {
        return false;
    }

    memset(&diagnostics, 0, sizeof(diagnostics));
    dwt_readdiagnostics(&diagnostics);
    pack_rx_diag_raw(&diagnostics, raw, raw_len);
    if (first_path_index != NULL) {
        *first_path_index = diagnostics.ipatovFpIndex >> 6;
    }
    return raw_len == NULL || *raw_len == DWM3000_RX_DIAG_RAW_LEN;
}

static uint16_t cir_window_start_sample(uint16_t first_path_index,
                                        uint16_t window_samples)
{
    uint16_t start_sample;

    if (window_samples >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        return 0u;
    }
    if (first_path_index >= DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        first_path_index = DWM3000_CIR_ACCUM_SAMPLE_COUNT - 1u;
    }

    start_sample = first_path_index > DWM3000_CIR_WINDOW_PRE_SAMPLES ?
                   first_path_index - DWM3000_CIR_WINDOW_PRE_SAMPLES : 0u;
    if (start_sample + window_samples > DWM3000_CIR_ACCUM_SAMPLE_COUNT) {
        start_sample = DWM3000_CIR_ACCUM_SAMPLE_COUNT - window_samples;
    }
    return start_sample;
}

static uint16_t read_cir_window(uint8_t *buffer,
                                uint16_t buffer_cap,
                                uint16_t requested_len,
                                uint16_t first_path_index,
                                uint16_t *start_index)
{
    uint8_t accum[DWM3000_FULL_CIR_READ_CHUNK_BYTES + 1u];
    uint16_t total_len;
    uint16_t total_samples;
    uint16_t start_sample;
    uint16_t sample_index;
    uint16_t byte_offset = 0u;

    if (start_index != NULL) {
        *start_index = 0u;
    }
    if (buffer == NULL || buffer_cap == 0u) {
        return 0u;
    }

    total_len = MIN(requested_len, buffer_cap);
    total_len -= total_len % DWM3000_FULL_CIR_SAMPLE_BYTES;
    total_samples = total_len / DWM3000_FULL_CIR_SAMPLE_BYTES;
    start_sample = cir_window_start_sample(first_path_index, total_samples);
    sample_index = start_sample;
    if (start_index != NULL) {
        *start_index = start_sample;
    }
    while (sample_index < start_sample + total_samples) {
        uint16_t chunk_samples =
            MIN((uint16_t)DWM3000_FULL_CIR_READ_CHUNK_SAMPLES,
                (uint16_t)(start_sample + total_samples - sample_index));
        uint16_t chunk_len = chunk_samples * DWM3000_FULL_CIR_SAMPLE_BYTES;

        dwt_readaccdata(accum, chunk_len + 1u, sample_index);
        memcpy(&buffer[byte_offset], &accum[1], chunk_len);
        sample_index += chunk_samples;
        byte_offset += chunk_len;
    }
    return byte_offset;
}

static void read_rx_diagnostics(int8_t *rsl_dbm,
                                uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                                bool *cir_sampled,
                                int16_t *clock_offset_raw,
                                bool *clock_offset_sampled,
                                int32_t *carrier_integrator,
                                bool *carrier_integrator_sampled)
{
    dwt_rxdiag_t diagnostics;

    if (rsl_dbm == NULL && cir_sample == NULL &&
        clock_offset_raw == NULL && carrier_integrator == NULL) {
        return;
    }
    if (cir_sampled != NULL) {
        *cir_sampled = false;
    }
    if (clock_offset_sampled != NULL) {
        *clock_offset_sampled = false;
    }
    if (carrier_integrator_sampled != NULL) {
        *carrier_integrator_sampled = false;
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
    if (clock_offset_raw != NULL) {
        *clock_offset_raw = dwt_readclockoffset();
        if (clock_offset_sampled != NULL) {
            *clock_offset_sampled = true;
        }
    }
    if (carrier_integrator != NULL) {
        *carrier_integrator = dwt_readcarrierintegrator();
        if (carrier_integrator_sampled != NULL) {
            *carrier_integrator_sampled = true;
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

static int decode_anchor_diag_frame(const uint8_t *buffer, size_t frame_len,
                                    struct uwb_anchor_diag_frame *frame)
{
    int ret;

    ret = uwb_decode_anchor_diag(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_anchor_diag(buffer, payload_len_without_fcs(frame_len), frame);
    return ret == PROTO_OK ? 0 : -EBADMSG;
}

static int decode_anchor_diag_fragment_frame(
    const uint8_t *buffer,
    size_t frame_len,
    struct uwb_anchor_diag_fragment_frame *frame)
{
    int ret;

    ret = uwb_decode_anchor_diag_fragment(buffer, frame_len, frame);
    if (ret == PROTO_OK) {
        return 0;
    }

    ret = uwb_decode_anchor_diag_fragment(buffer,
                                          payload_len_without_fcs(frame_len),
                                          frame);
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
           flags == (FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY) ||
           flags == FLAG_COUNT_AS_CLICK;
}

static bool request_sends_clicker_diag(const struct dwm3000_range_request *request)
{
    return DWM3000_CLICKER_DIAG_ENABLED &&
           request != NULL &&
           request->send_clicker_diag &&
           request->round_index == 0u;
}

static bool request_expects_clicker_diag(const struct dwm3000_range_request *request)
{
    return DWM3000_CLICKER_DIAG_ENABLED &&
           request != NULL &&
           request->expect_clicker_diag &&
           request->round_index == 0u;
}

static bool request_expects_anchor_diag(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->expect_anchor_diag &&
           request->round_index == 0u;
}

static bool request_expects_anchor_diag_fragments(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->expect_anchor_diag_fragments &&
           request->anchor_full_cir != NULL &&
           request->anchor_full_cir_cap > 0u &&
           request->round_index == 0u;
}

static bool request_sends_anchor_diag(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->send_anchor_diag &&
           request->round_index == 0u;
}

static bool request_sends_anchor_diag_fragments(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->send_anchor_diag_fragments &&
           request->round_index == 0u;
}

static bool request_captures_anchor_full_cir(const struct dwm3000_range_request *request)
{
    return request != NULL &&
           request->anchor_full_cir != NULL &&
           request->anchor_full_cir_cap > 0u &&
           request->round_index == 0u;
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
        LOG_WRN("DWM3000 TX write failed: ret=%d frame_len=%u tx_mode=0x%02x",
                ret,
                (unsigned int)frame_len,
                tx_mode);
        dwm3000_debug_event(true,
                            "UWB_TX_DONE",
                            "status=write-fail ret=%d frame_len=%u",
                            ret,
                            (unsigned int)frame_len);
        return ret;
    }

    if (dwt_starttx(tx_mode) != DWT_SUCCESS) {
        driver_stats.tx_failures++;
        LOG_WRN("DWM3000 TX start failed: frame_len=%u tx_mode=0x%02x",
                (unsigned int)frame_len,
                tx_mode);
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
                         bool *cir_sampled, int16_t *clock_offset_raw,
                         bool *clock_offset_sampled,
                         int32_t *carrier_integrator,
                         bool *carrier_integrator_sampled,
                         bool capture_rsl, bool log_events)
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
        read_rx_diagnostics(rsl_dbm, cir_sample, cir_sampled,
                            clock_offset_raw, clock_offset_sampled,
                            carrier_integrator, carrier_integrator_sampled);
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
                            uint8_t rx_diag_raw[DWM3000_RX_DIAG_RAW_LEN],
                            uint8_t *rx_diag_raw_len,
                            bool *rx_diag_sampled,
                            int16_t *clock_offset_raw,
                            bool *clock_offset_sampled,
                            int32_t *carrier_integrator,
                            bool *carrier_integrator_sampled,
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
        read_rx_diagnostics(rsl_dbm, cir_sample, cir_sampled,
                            clock_offset_raw, clock_offset_sampled,
                            carrier_integrator, carrier_integrator_sampled);
        if (capture_rx_diag_raw(rx_diag_raw, rx_diag_raw_len, NULL)) {
            if (rx_diag_sampled != NULL) {
                *rx_diag_sampled = true;
            }
        } else if (rx_diag_sampled != NULL) {
            *rx_diag_sampled = false;
        }
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
                        NULL, NULL, NULL, NULL, NULL, NULL, NULL, false, true);
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
                             bool resp_rsl_sampled,
                             struct dwm3000_range_result *result)
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
    ret = wait_tx_complete(CLICKER_DIAG_TX_TIMEOUT_MS);
    if (ret == 0) {
        store_clicker_diag_result(result, &diag);
    }
    return ret;
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
    dwt_setpreambledetecttimeout(CLICKER_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    dwm3000_debug_event(false,
                        "CLICKER_DIAG_RX",
                        "short=0x%04x seq=%u round=%u timeout_ms=%u preamble_timeout_pac=%u",
                        poll->initiator_short_addr,
                        poll->seq,
                        poll->round_index,
                        CLICKER_DIAG_RX_TIMEOUT_MS,
                        CLICKER_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
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

static bool anchor_diag_matches_request(const struct uwb_anchor_diag_frame *diag,
                                        const struct dwm3000_range_request *request)
{
    if (diag == NULL || request == NULL) {
        return false;
    }

    return diag->header.type == MSG_UWB_ANCHOR_DIAG &&
           diag->header.seq == request->seq &&
           diag->header.round_index == request->round_index &&
           diag->header.network_id == request->network_id &&
           diag->header.session_id == request->session_id &&
           diag->header.session_nonce == request->session_nonce &&
           diag->header.initiator_short_addr == short_addr_from_id(request->initiator_id) &&
           diag->header.responder_short_addr == request->responder_short_addr &&
           diag->header.flags == request->flags &&
           diag->header.initiator_id == request->initiator_id &&
           diag->header.responder_id == request->responder_id;
}

static void store_anchor_diag_result(struct dwm3000_range_result *result,
                                     const struct uwb_anchor_diag_frame *diag)
{
    if (result == NULL || diag == NULL) {
        return;
    }

    result->distance_mm = diag->distance_mm;
    result->quality = diag->quality;
    result->status = diag->status;
    result->anchor_diag_status_flags = diag->status_flags;
    result->rsl_dbm = diag->rsl_dbm;
    result->rsl_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT) != 0u;
    result->clock_offset_raw = diag->clock_offset_raw;
    result->clock_offset_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_CLOCK_OFFSET_PRESENT) != 0u;
    result->carrier_integrator = diag->carrier_integrator;
    result->carrier_integrator_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_CARRIER_INTEGRATOR_PRESENT) != 0u;
    result->poll_rx_ts_32 = diag->poll_rx_ts_32;
    result->resp_tx_ts_32 = diag->resp_tx_ts_32;
    result->final_rx_ts_32 = diag->final_rx_ts_32;
    result->poll_tx_ts_32 = diag->poll_tx_ts_32;
    result->resp_rx_ts_32 = diag->resp_rx_ts_32;
    result->final_tx_ts_32 = diag->final_tx_ts_32;
    result->cir_sampled =
        (diag->status_flags & UWB_ANCHOR_DIAG_STATUS_CIR_SAMPLE_PRESENT) != 0u &&
        diag->diag_len >= UWB_CIR_SAMPLE_LEN;
    if (result->cir_sampled) {
        memcpy(result->cir_sample, diag->diag_bytes, UWB_CIR_SAMPLE_LEN);
    }
}

static int receive_anchor_diag(const struct dwm3000_range_request *request,
                               struct dwm3000_range_result *result)
{
    struct uwb_anchor_diag_frame diag;
    uint8_t rx_buffer[UWB_ANCHOR_DIAG_MAX_LEN + FCS_LEN];
    uint32_t status = 0u;
    size_t frame_len = 0u;
    int ret;

    if (request == NULL) {
        return -EINVAL;
    }

    dwt_setrxaftertxdelay(0u);
    dwt_setrxtimeout(0u);
    dwt_setpreambledetecttimeout(ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    dwm3000_debug_event(false,
                        "ANCHOR_DIAG_RX",
                        "anchor=0x%016llx seq=%u round=%u timeout_ms=%u preamble_timeout_pac=%u",
                        (unsigned long long)request->responder_id,
                        request->seq,
                        request->round_index,
                        ANCHOR_DIAG_RX_TIMEOUT_MS,
                        ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(ANCHOR_DIAG_RX_TIMEOUT_MS,
                        &status,
                        rx_buffer,
                        sizeof(rx_buffer),
                        &frame_len,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
                        false,
                        true);
    if (ret < 0) {
        return ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
    }

    memset(&diag, 0, sizeof(diag));
    ret = decode_anchor_diag_frame(rx_buffer, frame_len, &diag);
    if (ret < 0 || !anchor_diag_matches_request(&diag, request)) {
        return -EBADMSG;
    }

    store_anchor_diag_result(result, &diag);
    return 0;
}

static bool anchor_diag_fragment_matches_request(
    const struct uwb_anchor_diag_fragment_frame *fragment,
    const struct dwm3000_range_request *request)
{
    if (fragment == NULL || request == NULL) {
        return false;
    }

    return fragment->header.type == MSG_UWB_ANCHOR_DIAG_FRAGMENT &&
           fragment->header.seq == request->seq &&
           fragment->header.round_index == request->round_index &&
           fragment->header.network_id == request->network_id &&
           fragment->header.session_id == request->session_id &&
           fragment->header.session_nonce == request->session_nonce &&
           fragment->header.initiator_short_addr == short_addr_from_id(request->initiator_id) &&
           fragment->header.responder_short_addr == request->responder_short_addr &&
           fragment->header.flags == request->flags &&
           fragment->header.initiator_id == request->initiator_id &&
           fragment->header.responder_id == request->responder_id;
}

static int receive_anchor_diag_fragments(const struct dwm3000_range_request *request,
                                         struct dwm3000_range_result *result)
{
    uint8_t rx_buffer[UWB_ANCHOR_DIAG_FRAGMENT_MAX_LEN + FCS_LEN];
    int64_t deadline_ms;
    int last_error = -ETIMEDOUT;

    if (request == NULL || result == NULL || request->anchor_full_cir == NULL) {
        return -EINVAL;
    }

    result->anchor_full_cir_len = 0u;
    result->anchor_full_cir_total_len = 0u;
    result->anchor_full_cir_first_path_index = 0u;
    result->anchor_full_cir_start_index = 0u;
    result->anchor_rx_diag_raw_len = 0u;
    deadline_ms = k_uptime_get() + ANCHOR_DIAG_FRAGMENT_RX_TIMEOUT_MS;

    while (k_uptime_get() < deadline_ms) {
        bool expect_rx_diag =
            (result->anchor_diag_status_flags & UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT) != 0u;
        bool expect_full_cir =
            (result->anchor_diag_status_flags & UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT) != 0u;
        struct uwb_anchor_diag_fragment_frame fragment = {0};
        uint32_t status = 0u;
        size_t frame_len = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        int ret;

        dwt_setrxaftertxdelay(0u);
        dwt_setrxtimeout(0u);
        dwt_setpreambledetecttimeout(ANCHOR_DIAG_RX_PREAMBLE_TIMEOUT_PAC);
        clear_all_events();
        if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
            return -EIO;
        }

        ret = receive_frame((uint32_t)MAX(1, remaining_ms),
                            &status,
                            rx_buffer,
                            sizeof(rx_buffer),
                            &frame_len,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            false,
                            true);
        if (ret < 0) {
            last_error = ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
            if (ret == -ETIMEDOUT) {
                break;
            }
            continue;
        }

        ret = decode_anchor_diag_fragment_frame(rx_buffer, frame_len, &fragment);
        if (ret < 0) {
            last_error = -EBADMSG;
            LOG_DBG("ignored anchor diagnostic fragment decode miss: ret=%d len=%zu",
                    ret,
                    frame_len);
            continue;
        }
        if (!anchor_diag_fragment_matches_request(&fragment, request)) {
            last_error = -EBADMSG;
            LOG_DBG("ignored anchor diagnostic fragment mismatch: seq=%u round=%u session=%u type=%u",
                    fragment.header.seq,
                    fragment.header.round_index,
                    fragment.header.session_id,
                    fragment.header.type);
            continue;
        }

        if (fragment.block_type == UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_RX_DIAG) {
            if (fragment.total_len > sizeof(result->anchor_rx_diag_raw) ||
                fragment.offset != result->anchor_rx_diag_raw_len) {
                return -EMSGSIZE;
            }
            memcpy(&result->anchor_rx_diag_raw[fragment.offset],
                   fragment.chunk,
                   fragment.chunk_len);
            result->anchor_rx_diag_raw_len += fragment.chunk_len;
            if ((fragment.flags & UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST) != 0u) {
                result->anchor_rx_diag_sampled =
                    result->anchor_rx_diag_raw_len == fragment.total_len;
            }
        } else if (fragment.block_type == UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_CIR) {
            if (fragment.total_len > request->anchor_full_cir_cap ||
                fragment.offset != result->anchor_full_cir_len) {
                return -EMSGSIZE;
            }
            memcpy(&request->anchor_full_cir[fragment.offset],
                   fragment.chunk,
                   fragment.chunk_len);
            result->anchor_full_cir_len += fragment.chunk_len;
            result->anchor_full_cir_total_len = fragment.total_len;
            result->anchor_full_cir_first_path_index = fragment.first_path_index;
            result->anchor_full_cir_start_index =
                cir_window_start_sample(fragment.first_path_index,
                                        fragment.total_len / DWM3000_FULL_CIR_SAMPLE_BYTES);
            if ((fragment.flags & UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST) != 0u) {
                result->anchor_full_cir_sampled =
                    result->anchor_full_cir_len == fragment.total_len;
                result->anchor_full_cir_truncated =
                    result->anchor_full_cir_len < DWM3000_FULL_CIR_BYTES;
            }
        }

        if ((!expect_rx_diag || result->anchor_rx_diag_sampled) &&
            (!expect_full_cir || result->anchor_full_cir_sampled)) {
            return 0;
        }
    }

    return last_error;
}

static int send_anchor_diag_fragment_block(const struct uwb_range_header *poll,
                                           uint8_t block_type,
                                           const uint8_t *data,
                                           uint16_t data_len,
                                           uint16_t first_path_index)
{
    uint8_t tx_buffer[UWB_ANCHOR_DIAG_FRAGMENT_MAX_LEN];
    uint8_t fragment_count;
    uint16_t offset = 0u;

    if (poll == NULL || data == NULL || data_len == 0u) {
        return -EINVAL;
    }

    fragment_count = (uint8_t)((data_len + UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES - 1u) /
                               UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES);
    for (uint8_t fragment_index = 0u; fragment_index < fragment_count; fragment_index++) {
        struct uwb_anchor_diag_fragment_frame fragment = {0};
        size_t tx_len = 0u;
        uint16_t chunk_len = MIN((uint16_t)UWB_ANCHOR_DIAG_FRAGMENT_MAX_BYTES,
                                 (uint16_t)(data_len - offset));
        int ret;

        k_busy_wait(ANCHOR_DIAG_FRAGMENT_TX_GAP_US);
        fragment.header = *poll;
        fragment.header.type = MSG_UWB_ANCHOR_DIAG_FRAGMENT;
        fragment.block_type = block_type;
        fragment.offset = offset;
        fragment.total_len = data_len;
        fragment.first_path_index = first_path_index;
        fragment.fragment_index = fragment_index;
        fragment.fragment_count = fragment_count;
        fragment.flags = fragment_index == fragment_count - 1u ?
                         UWB_ANCHOR_DIAG_FRAGMENT_FLAG_LAST : 0u;
        fragment.chunk_len = (uint8_t)chunk_len;
        memcpy(fragment.chunk, &data[offset], chunk_len);

        ret = uwb_encode_anchor_diag_fragment(&fragment,
                                              tx_buffer,
                                              sizeof(tx_buffer),
                                              &tx_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        clear_status(SYS_STATUS_TXFRS_BIT_MASK);
        ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
        if (ret < 0) {
            return ret;
        }
        ret = wait_tx_complete(ANCHOR_DIAG_FRAGMENT_TX_TIMEOUT_MS);
        if (ret < 0) {
            return ret;
        }
        offset += chunk_len;
    }

    return 0;
}

static int send_anchor_diag_fragments(const struct uwb_range_header *poll,
                                      const struct dwm3000_range_request *expected,
                                      const struct dwm3000_range_result *result)
{
    int ret;

    if (poll == NULL || expected == NULL || result == NULL) {
        return -EINVAL;
    }

    if (result->anchor_rx_diag_sampled && result->anchor_rx_diag_raw_len > 0u) {
        ret = send_anchor_diag_fragment_block(poll,
                                              UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_RX_DIAG,
                                              result->anchor_rx_diag_raw,
                                              result->anchor_rx_diag_raw_len,
                                              result->anchor_full_cir_first_path_index);
        if (ret < 0) {
            return ret;
        }
    }
    if (result->anchor_full_cir_sampled &&
        expected->anchor_full_cir != NULL &&
        result->anchor_full_cir_len > 0u) {
        ret = send_anchor_diag_fragment_block(poll,
                                              UWB_ANCHOR_DIAG_FRAGMENT_BLOCK_CIR,
                                              expected->anchor_full_cir,
                                              result->anchor_full_cir_len,
                                              result->anchor_full_cir_first_path_index);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int send_anchor_diag(const struct uwb_range_header *poll,
                            const struct dwm3000_range_result *result)
{
    struct uwb_anchor_diag_frame diag = {0};
    uint8_t tx_buffer[UWB_ANCHOR_DIAG_MAX_LEN];
    size_t tx_len = 0u;
    int ret;

    if (poll == NULL || result == NULL) {
        return -EINVAL;
    }

    diag.header = *poll;
    diag.header.type = MSG_UWB_ANCHOR_DIAG;
    diag.distance_mm = result->distance_mm;
    diag.quality = result->quality;
    diag.status = result->status;
    diag.rsl_dbm = result->rsl_dbm;
    diag.poll_rx_ts_32 = result->poll_rx_ts_32;
    diag.resp_tx_ts_32 = result->resp_tx_ts_32;
    diag.final_rx_ts_32 = result->final_rx_ts_32;
    diag.poll_tx_ts_32 = result->poll_tx_ts_32;
    diag.resp_rx_ts_32 = result->resp_rx_ts_32;
    diag.final_tx_ts_32 = result->final_tx_ts_32;
    diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_RAW_TIMESTAMPS_PRESENT;
    if (result->rsl_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT;
    }
    if (result->cir_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_CIR_SAMPLE_PRESENT;
        diag.diag_len = UWB_CIR_SAMPLE_LEN;
        memcpy(diag.diag_bytes, result->cir_sample, UWB_CIR_SAMPLE_LEN);
    }
    if (result->clock_offset_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_CLOCK_OFFSET_PRESENT;
        diag.clock_offset_raw = result->clock_offset_raw;
    }
    if (result->carrier_integrator_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_CARRIER_INTEGRATOR_PRESENT;
        diag.carrier_integrator = result->carrier_integrator;
    }
    if (result->anchor_rx_diag_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT;
    }
    if (result->anchor_full_cir_sampled) {
        diag.status_flags |= UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT;
    }
    if (diag.status_flags == 0u) {
        diag.status_flags = UWB_ANCHOR_DIAG_STATUS_RSL_PRESENT;
    }

    ret = uwb_encode_anchor_diag(&diag, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        return ret;
    }
    return wait_tx_complete(ANCHOR_DIAG_TX_TIMEOUT_MS);
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

    ret = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
          configure_radio_from_reset(DWM3000_PHY_WAKE) :
          ensure_phy_mode(DWM3000_PHY_WAKE);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_RESET_FAIL ret=%d\n", ret);
        }
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

int dwm3000_driver_configure_wake_mesh_control_mode(void)
{
    int ret;

    ret = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
          configure_radio_from_reset(DWM3000_PHY_WAKE_MESH_CONTROL) :
          ensure_phy_mode(DWM3000_PHY_WAKE_MESH_CONTROL);
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_WAKE_MESH_CTRL_FAIL ret=%d\n", ret);
        }
        return ret;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DWM_WAKE_MESH_CTRL_OK sfd=%u\n",
                            (unsigned int)wake_mesh_control_config.sfdTO);
    }
    LOG_INF("DWM3000 configured for channel %u extended-PHR mesh control at 850 kbps",
            UWB_CHANNEL_WAKE_CONTACT);
    return 0;
}

int dwm3000_driver_configure_mesh_payload_mode(void)
{
    int ret;

    ret = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
          configure_radio_from_reset(DWM3000_PHY_MESH_PAYLOAD) :
          ensure_phy_mode(DWM3000_PHY_MESH_PAYLOAD);
    if (ret < 0) {
        return ret;
    }
#if DWM3000_MESH_PHY_STS_MODE == DWT_STS_MODE_OFF
    LOG_INF("DWM3000 configured for channel %u 1024-symbol no-STS mesh payload at 850 kbps",
            UWB_CHANNEL_MESH_PAYLOAD);
#else
    LOG_INF("DWM3000 configured for channel %u 1024-symbol STS-mode-1 mesh payload at 850 kbps",
            UWB_CHANNEL_MESH_PAYLOAD);
#endif
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
        LOG_WRN("DWM3000 send-frame PHY wake/restore failed: ret=%d frame_len=%u",
                ret,
                (unsigned int)frame_len);
        return ret;
    }

    clear_all_events();
    ret = send_range_frame(frame, frame_len, DWT_START_TX_IMMEDIATE);
    if (ret < 0) {
        LOG_WRN("DWM3000 send-frame immediate TX failed: ret=%d frame_len=%u active_phy=%d awake=%u restored=%u",
                ret,
                (unsigned int)frame_len,
                active_phy_mode,
                radio_awake ? 1u : 0u,
                radio_restored_from_sleep ? 1u : 0u);
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
                                               struct dwm3000_rx_frame_timing *timing,
                                               bool log_events)
{
    uint8_t rx_buffer[UWB_MESH_MAX_FRAME_LEN + FCS_LEN];
    uint64_t rx_timestamp = 0u;
    uint32_t rx_enable_time32 = 0u;
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
    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
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
    if (timing != NULL) {
        rx_enable_time32 = dwt_readsystimestamphi32();
    }

    ret = receive_frame(timeout_ms,
                        &status,
                        rx_buffer,
                        sizeof(rx_buffer),
                        &raw_len,
                        timing == NULL ? NULL : &rx_timestamp,
                        &rx_quality,
                        &rx_rsl_dbm,
                        NULL,
                        NULL,
                        NULL,
                        NULL,
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
    if (timing != NULL) {
        timing->rx_timestamp = rx_timestamp;
        timing->rx_enable_time32 = rx_enable_time32;
        timing->rx_timestamp_time32 = (uint32_t)(rx_timestamp >> 8);
        timing->rx_since_enable_uus =
            dwt_time32_delta_to_uus(rx_enable_time32, timing->rx_timestamp_time32);
        timing->valid = true;
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
                                               NULL,
                                               true);
}

int dwm3000_driver_receive_frame_detailed_quiet(uint32_t timeout_ms,
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
                                               NULL,
                                               false);
}

int dwm3000_driver_receive_frame_continuous(uint32_t timeout_ms,
                                            uint8_t *frame,
                                            size_t frame_cap,
                                            size_t *frame_len,
                                            uint8_t *quality,
                                            int8_t *rsl_dbm,
                                            enum dwm3000_rx_failure *failure)
{
    return dwm3000_driver_receive_frame_continuous_timed(timeout_ms,
                                                        frame,
                                                        frame_cap,
                                                        frame_len,
                                                        quality,
                                                        rsl_dbm,
                                                        failure,
                                                        NULL);
}

int dwm3000_driver_receive_frame_continuous_timed(uint32_t timeout_ms,
                                                  uint8_t *frame,
                                                  size_t frame_cap,
                                                  size_t *frame_len,
                                                  uint8_t *quality,
                                                  int8_t *rsl_dbm,
                                                  enum dwm3000_rx_failure *failure,
                                                  struct dwm3000_rx_frame_timing *timing)
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
                                               timing,
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
    uint32_t final_tx_time;
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
    reply_delay_uus = request_reply_delay_uus(request);

    dwm3000_debug_event(false,
                        "DS_TWR_TIMING",
                        "role=initiator reply_delay_uus=%u resp_rx_after_uus=%u resp_rx_timeout_uus=%u wait_timeout_ms=%u",
                        reply_delay_uus,
                        ds_twr_rx_after_tx_delay_uus(reply_delay_uus),
                        ds_twr_rx_timeout_uus(reply_delay_uus),
                        request->timeout_ms == 0u ?
                        RESP_RX_TIMEOUT_MS : request->timeout_ms);
    dwt_setrxaftertxdelay(ds_twr_rx_after_tx_delay_uus(reply_delay_uus));
    dwt_setrxtimeout(ds_twr_rx_timeout_uus(reply_delay_uus));
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
                           result->cir_sample, &result->cir_sampled,
                           result->clicker_rx_diag_raw,
                           &result->clicker_rx_diag_raw_len,
                           &result->clicker_rx_diag_sampled,
                           &result->clock_offset_raw,
                           &result->clock_offset_sampled,
                           &result->carrier_integrator,
                           &result->carrier_integrator_sampled,
                           &range_status, request->capture_rsl);
    if (ret < 0) {
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_WRN("UWB initiator RESP RX failed: ret=%d status=%u seq=%u round=%u timeout_ms=%u reply_delay_uus=%u",
                ret,
                range_status,
                request->seq,
                request->round_index,
                request->timeout_ms == 0u ? RESP_RX_TIMEOUT_MS : request->timeout_ms,
                reply_delay_uus);
#endif
        result_set_request_metadata(result, request);
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = request->capture_rsl;
        result->status = range_status;
        return ret;
    }
    if (request->capture_rsl) {
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = true;
    }
    poll_tx_ts = read_tx_timestamp_u64();

    final_tx_time = delayed_tx_time_from_rx_reference(resp_rx_ts,
                                                      reply_delay_uus);
    final_tx_ts = delayed_tx_timestamp_from_programmed_time(final_tx_time);

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
    result->poll_tx_ts_32 = final.poll_tx_ts_32;
    result->poll_rx_ts_32 = response.poll_rx_ts_32;
    result->resp_tx_ts_32 = response.resp_tx_ts_32;
    result->resp_rx_ts_32 = final.resp_rx_ts_32;
    result->final_tx_ts_32 = final.final_tx_ts_32;

    ret = validate_driver_reply_timing(
        dwt_delta_to_uus(response.poll_rx_ts_32, response.resp_tx_ts_32),
        dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
        reply_delay_uus);
    if (ret != PROTO_OK) {
        result_set_request_metadata(result, request);
        result->responder_id = request->responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = request->capture_rsl;
        result->status = RANGE_TIMING_INVALID;
        return -ETIME;
    }

    ret = uwb_encode_final(&final, tx_buffer, sizeof(tx_buffer), &tx_len);
    if (ret != PROTO_OK) {
        result->status = RANGE_INTERNAL_ERROR;
        return -EINVAL;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
    if (request->skip_responder_report) {
        dwt_setdelayedtrxtime(final_tx_time);
        ret = send_range_frame(tx_buffer, tx_len, DWT_START_TX_DELAYED);
    } else {
        dwt_setrxaftertxdelay(0u);
        dwt_setrxtimeout(REPORT_RX_TIMEOUT_UUS);
        dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

        dwt_setdelayedtrxtime(final_tx_time);
        ret = send_range_frame(tx_buffer, tx_len,
                               DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
    }
    if (ret < 0) {
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
            LOG_WRN("UWB initiator FINAL TX start failed: ret=%d anchor=0x%016llx seq=%u round=%u reply_delay_uus=%u",
                    ret,
                    (unsigned long long)request->responder_id,
                    request->seq,
                    request->round_index,
                    reply_delay_uus);
#endif
            result_set_request_metadata(result, request);
            result->responder_id = request->responder_id;
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->rsl_sampled = request->capture_rsl;
            result->status = RANGE_DELAYED_TX_MISSED;
            return -ETIME;
    }

#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
    LOG_INF("UWB initiator RESP RX ok: anchor=0x%016llx seq=%u round=%u quality=%u rsl=%d dBm poll_tx=0x%08x resp_rx=0x%08x",
            (unsigned long long)response.header.responder_id,
            request->seq,
            request->round_index,
            quality,
            rsl_dbm,
            (uint32_t)poll_tx_ts,
            (uint32_t)resp_rx_ts);
    LOG_INF("UWB initiator FINAL TX scheduled: anchor=0x%016llx seq=%u round=%u resp_to_final_uus=%u tx_time=0x%08x final_tx=0x%08x skip_report=%u",
            (unsigned long long)response.header.responder_id,
            request->seq,
            request->round_index,
            dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
            final_tx_time,
            final.final_tx_ts_32,
            request->skip_responder_report ? 1u : 0u);
    if (!request->skip_responder_report) {
        dwm3000_debug_event(false,
                            "DS_TWR_TIMING",
                            "role=initiator_report report_rx_after_uus=0 report_rx_timeout_uus=%u wait_timeout_ms=%u",
                            REPORT_RX_TIMEOUT_UUS,
                            REPORT_RX_TIMEOUT_MS);
    }
#endif

    if (request->skip_responder_report) {
        ret = wait_tx_complete(ds_twr_rx_wait_timeout_ms(reply_delay_uus));
        if (ret < 0) {
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
            LOG_WRN("UWB initiator FINAL TX complete failed: ret=%d anchor=0x%016llx seq=%u round=%u wait_ms=%u reply_delay_uus=%u",
                    ret,
                    (unsigned long long)request->responder_id,
                    request->seq,
                    request->round_index,
                    ds_twr_rx_wait_timeout_ms(reply_delay_uus),
                    reply_delay_uus);
#endif
            result_set_request_metadata(result, request);
            result->responder_id = request->responder_id;
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = RANGE_INTERNAL_ERROR;
            return ret;
        }
#if defined(CONFIG_IMEC_ML_CLICKER) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_INF("UWB initiator FINAL TX complete: anchor=0x%016llx seq=%u round=%u wait_ms=%u reply_delay_uus=%u",
                (unsigned long long)response.header.responder_id,
                request->seq,
                request->round_index,
                ds_twr_rx_wait_timeout_ms(reply_delay_uus),
                reply_delay_uus);
#endif

        result_set_request_metadata(result, request);
        result->responder_id = response.header.responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->rsl_sampled = request->capture_rsl;
        result->status = RANGE_OK;

        if (request_sends_clicker_diag(request)) {
            int diag_ret;

            k_busy_wait(CLICKER_DIAG_TX_AFTER_FINAL_DELAY_US);
            dwm3000_debug_event(false,
                                "CLICKER_DIAG_TX",
                                "anchor=0x%016llx seq=%u round=%u delay_after_final_us=%u",
                                (unsigned long long)response.header.responder_id,
                                request->seq,
                                request->round_index,
                                CLICKER_DIAG_TX_AFTER_FINAL_DELAY_US);
            diag_ret = send_clicker_diag(request,
                                         &response,
                                         &final,
                                         resp_rx_ts,
                                         quality,
                                         rsl_dbm,
                                         false,
                                         result);
            if (diag_ret < 0) {
                result->status = RANGE_INTERNAL_ERROR;
                LOG_WRN("UWB clicker diagnostic TX failed: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)response.header.responder_id,
                        request->seq,
                        diag_ret);
                return diag_ret;
            }
        }

        if (request_expects_anchor_diag(request)) {
            int anchor_diag_ret = receive_anchor_diag(request, result);

            if (anchor_diag_ret < 0) {
                LOG_WRN("UWB anchor diagnostic not received: anchor=0x%016llx seq=%u ret=%d",
                        (unsigned long long)response.header.responder_id,
                        request->seq,
                        anchor_diag_ret);
            } else {
                LOG_INF("UWB anchor diagnostic received: anchor=0x%016llx seq=%u bytes=%u",
                        (unsigned long long)response.header.responder_id,
                        request->seq,
                        result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
                if (request_expects_anchor_diag_fragments(request) &&
                    (result->anchor_diag_status_flags &
                     (UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT |
                      UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT)) != 0u) {
                    int fragment_ret = receive_anchor_diag_fragments(request, result);

                    if (fragment_ret < 0) {
                        LOG_WRN("UWB anchor diagnostic fragments not received: anchor=0x%016llx seq=%u ret=%d",
                                (unsigned long long)response.header.responder_id,
                                request->seq,
                                fragment_ret);
                    }
                }
            }
        }

        return 0;
    }

    ret = receive_report(request, response.header.responder_id, result);
    if (ret == 0 && result->status == RANGE_OK && request_sends_clicker_diag(request)) {
        int diag_ret = send_clicker_diag(request,
                                         &response,
                                         &final,
                                         resp_rx_ts,
                                         quality,
                                         rsl_dbm,
                                         false,
                                         result);

        if (diag_ret < 0) {
            LOG_WRN("UWB clicker diagnostic TX failed: anchor=0x%016llx seq=%u ret=%d",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    diag_ret);
        }
    }
    if (ret == 0 && result->status == RANGE_OK && request_expects_anchor_diag(request)) {
        int anchor_diag_ret = receive_anchor_diag(request, result);

        if (anchor_diag_ret < 0) {
            LOG_WRN("UWB anchor diagnostic not received: anchor=0x%016llx seq=%u ret=%d",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    anchor_diag_ret);
        } else {
            LOG_INF("UWB anchor diagnostic received: anchor=0x%016llx seq=%u bytes=%u",
                    (unsigned long long)response.header.responder_id,
                    request->seq,
                    result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
            if (request_expects_anchor_diag_fragments(request) &&
                (result->anchor_diag_status_flags &
                 (UWB_ANCHOR_DIAG_STATUS_RX_DIAG_PRESENT |
                  UWB_ANCHOR_DIAG_STATUS_FULL_CIR_PRESENT)) != 0u) {
                int fragment_ret = receive_anchor_diag_fragments(request, result);

                if (fragment_ret < 0) {
                    LOG_WRN("UWB anchor diagnostic fragments not received: anchor=0x%016llx seq=%u ret=%d",
                            (unsigned long long)response.header.responder_id,
                            request->seq,
                            fragment_ret);
                }
            }
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
    uint32_t resp_tx_time;
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
                        &poll_rx_ts, &quality, &rsl_dbm, NULL, NULL,
                        NULL, NULL, NULL, NULL, false, true);
    if (ret < 0) {
        if (ret == -ETIMEDOUT) {
            return -ETIMEDOUT;
        }
#if defined(CONFIG_IMEC_ML_ANCHOR) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_WRN("UWB responder POLL RX error: ret=%d sys_status=0x%08x failure=%s timeout_ms=%u",
                ret,
                status,
                rx_failure_debug_name(status_to_rx_failure(status)),
                timeout_ms == 0u ? DEFAULT_RESPONDER_WINDOW_MS : timeout_ms);
#endif
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
    reply_delay_uus = request_reply_delay_uus(expected);

    resp_tx_time = delayed_tx_time_from_rx_reference(poll_rx_ts,
                                                     reply_delay_uus);
    resp_tx_ts = delayed_tx_timestamp_from_programmed_time(resp_tx_time);

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

    dwt_setrxaftertxdelay(ds_twr_rx_after_tx_delay_uus(reply_delay_uus));
    dwt_setrxtimeout(ds_twr_rx_timeout_uus(reply_delay_uus));
    dwt_setpreambledetecttimeout(DELAYED_RX_PREAMBLE_TIMEOUT_PAC);

    dwt_setdelayedtrxtime(resp_tx_time);
    ret = send_range_frame(tx_buffer, tx_len,
                           DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        if (result != NULL) {
            result->status = RANGE_DELAYED_TX_MISSED;
        }
        return -ETIME;
    }
    dwm3000_debug_event(false,
                        "DS_TWR_TIMING",
                        "role=responder reply_delay_uus=%u final_rx_after_uus=%u final_rx_timeout_uus=%u wait_timeout_ms=%u",
                        reply_delay_uus,
                        ds_twr_rx_after_tx_delay_uus(reply_delay_uus),
                        ds_twr_rx_timeout_uus(reply_delay_uus),
                        ds_twr_rx_wait_timeout_ms(reply_delay_uus));

    capture_final_rsl = expected->capture_rsl;
    ret = receive_frame(ds_twr_rx_wait_timeout_ms(reply_delay_uus), &status,
                        rx_buffer, sizeof(rx_buffer), &frame_len,
                        &final_rx_ts, &quality, &rsl_dbm,
                        cir_sample, &cir_sampled,
                        result != NULL ? &result->clock_offset_raw : NULL,
                        result != NULL ? &result->clock_offset_sampled : NULL,
                        result != NULL ? &result->carrier_integrator : NULL,
                        result != NULL ? &result->carrier_integrator_sampled : NULL,
                        capture_final_rsl,
                        true);
    if (ret < 0) {
#if defined(CONFIG_IMEC_ML_ANCHOR) || defined(CONFIG_IMEC_HIGH_DEBUG)
        LOG_WRN("UWB responder FINAL RX failed: ret=%d sys_status=0x%08x failure=%s seq=%u round=%u wait_ms=%u reply_delay_uus=%u",
                ret,
                status,
                rx_failure_debug_name(status_to_rx_failure(status)),
                poll.seq,
                poll.round_index,
                ds_twr_rx_wait_timeout_ms(reply_delay_uus),
                reply_delay_uus);
#endif
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : RANGE_RX_ERROR;
        }
        return ret;
    }
    if (result != NULL) {
        result->poll_rx_ts_32 = (uint32_t)poll_rx_ts;
        result->resp_tx_ts_32 = (uint32_t)resp_tx_ts;
        result->final_rx_ts_32 = (uint32_t)final_rx_ts;
    }
    if (request_captures_anchor_full_cir(expected) && result != NULL) {
        result->anchor_rx_diag_sampled =
            capture_rx_diag_raw(result->anchor_rx_diag_raw,
                                &result->anchor_rx_diag_raw_len,
                                &result->anchor_full_cir_first_path_index);
        result->anchor_full_cir_total_len = DWM3000_FULL_CIR_BYTES;
        if (expected->anchor_full_cir != NULL && expected->anchor_full_cir_cap > 0u) {
            result->anchor_full_cir_len = read_cir_window(
                expected->anchor_full_cir,
                expected->anchor_full_cir_cap,
                DWM3000_FULL_CIR_BYTES,
                result->anchor_full_cir_first_path_index,
                &result->anchor_full_cir_start_index);
            result->anchor_full_cir_sampled = result->anchor_full_cir_len > 0u;
            result->anchor_full_cir_truncated =
                result->anchor_full_cir_len < DWM3000_FULL_CIR_BYTES;
        }
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
        if (result != NULL) {
            result->poll_tx_ts_32 = final.poll_tx_ts_32;
            result->resp_rx_ts_32 = final.resp_rx_ts_32;
            result->final_tx_ts_32 = final.final_tx_ts_32;
        }
        ret = compute_distance_mm(&final, poll_rx_ts, resp_tx_ts, final_rx_ts, &distance_mm);
        if (ret < 0) {
            report_status = RANGE_INTERNAL_ERROR;
        }
    }

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

    if (expected->skip_responder_report &&
        report_status == RANGE_OK &&
        request_expects_clicker_diag(expected)) {
        int diag_ret = receive_clicker_diag(&poll, local_anchor_id, result);

        if (diag_ret < 0) {
            LOG_DBG("UWB clicker diagnostic not received: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    diag_ret);
        } else {
            LOG_INF("UWB clicker diagnostic received: short=0x%04x seq=%u bytes=%u",
                    poll.initiator_short_addr,
                    poll.seq,
                    result != NULL ? result->clicker_diag_len : 0u);
        }
    }

    if (expected->skip_responder_report) {
        LOG_INF("UWB responder report skipped for short=0x%04x: status=%u distance_mm=%d quality=%u rsl=%d dBm",
                poll.initiator_short_addr,
                report_status,
                distance_mm,
                quality,
                rsl_dbm);
        if (report_status == RANGE_OK && request_sends_anchor_diag(expected)) {
            int anchor_diag_ret = send_anchor_diag(&poll, result);

            if (anchor_diag_ret < 0) {
                LOG_WRN("UWB anchor diagnostic TX failed: short=0x%04x seq=%u ret=%d",
                        poll.initiator_short_addr,
                        poll.seq,
                        anchor_diag_ret);
            } else {
                LOG_INF("UWB anchor diagnostic sent: short=0x%04x seq=%u bytes=%u",
                        poll.initiator_short_addr,
                        poll.seq,
                        result != NULL && result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
                if (request_sends_anchor_diag_fragments(expected)) {
                    int fragment_ret = send_anchor_diag_fragments(&poll, expected, result);

                    if (fragment_ret < 0) {
                        LOG_WRN("UWB anchor diagnostic fragments TX failed: short=0x%04x seq=%u ret=%d",
                                poll.initiator_short_addr,
                                poll.seq,
                                fragment_ret);
                    }
                }
            }
        }
        return report_status == RANGE_OK ? 0 : -EIO;
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

    LOG_INF("UWB report sent to short=0x%04x: status=%u distance_mm=%d quality=%u rsl=%d dBm",
            poll.initiator_short_addr,
            report.status,
            report.distance_mm,
            report.quality,
            report.rsl_dbm);
    if (report_status == RANGE_OK && request_expects_clicker_diag(expected)) {
        int diag_ret = receive_clicker_diag(&poll, local_anchor_id, result);

        if (diag_ret < 0) {
            LOG_DBG("UWB clicker diagnostic not received: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    diag_ret);
        } else {
            LOG_INF("UWB clicker diagnostic received: short=0x%04x seq=%u bytes=%u",
                    poll.initiator_short_addr,
                    poll.seq,
                    result != NULL ? result->clicker_diag_len : 0u);
        }
    }
    if (report_status == RANGE_OK && request_sends_anchor_diag(expected)) {
        int anchor_diag_ret = send_anchor_diag(&poll, result);

        if (anchor_diag_ret < 0) {
            LOG_WRN("UWB anchor diagnostic TX failed: short=0x%04x seq=%u ret=%d",
                    poll.initiator_short_addr,
                    poll.seq,
                    anchor_diag_ret);
        } else {
            LOG_INF("UWB anchor diagnostic sent: short=0x%04x seq=%u bytes=%u",
                    poll.initiator_short_addr,
                    poll.seq,
                    result != NULL && result->cir_sampled ? UWB_CIR_SAMPLE_LEN : 0u);
            if (request_sends_anchor_diag_fragments(expected)) {
                int fragment_ret = send_anchor_diag_fragments(&poll, expected, result);

                if (fragment_ret < 0) {
                    LOG_WRN("UWB anchor diagnostic fragments TX failed: short=0x%04x seq=%u ret=%d",
                            poll.initiator_short_addr,
                            poll.seq,
                            fragment_ret);
                }
            }
        }
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

void dwm3000_driver_stats_get(struct dwm3000_driver_stats *stats)
{
    if (stats != NULL) {
        *stats = driver_stats;
    }
}
