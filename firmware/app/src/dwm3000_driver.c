#include "dwm3000_driver.h"

#include "app_board.h"
#include "app_device_identity.h"
#include "debug_log.h"
#include "dwm3000_port.h"
#include "dwm3000_timing.h"
#include "uwb.h"
#include "uwb_session.h"

#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DWM3000_DRIVER_LOG_LEVEL LOG_LEVEL_INF

LOG_MODULE_REGISTER(dwm3000_driver, DWM3000_DRIVER_LOG_LEVEL);

static atomic_t receive_abort_owners;
static atomic_t receive_abort_enabled;

#define ROLE_CLICKER 1
#define ROLE_ANCHOR 2
#define ROLE_GATEWAY 3

#ifndef DEVICE_ROLE
#define DEVICE_ROLE ROLE_CLICKER
#endif

#define DWM3000_DS_TWR_RTT_DEBUG_ENABLED \
    (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || \
     IS_ENABLED(CONFIG_IMEC_DS_TWR_RTT_DEBUG))

#ifndef GATEWAY_ID
#define GATEWAY_ID 0x9999888877776666ull
#endif

#ifndef DEVICE_ID
#if IMEC_USE_HARDWARE_DEVICE_ID
#define DEVICE_ID app_device_id()
#elif DEVICE_ROLE == ROLE_ANCHOR
#define DEVICE_ID 0x2222222222222222ull
#elif DEVICE_ROLE == ROLE_GATEWAY
#define DEVICE_ID GATEWAY_ID
#else
#define DEVICE_ID 0x1111111111111111ull
#endif
#endif

#ifndef DWM3000_TX_ANT_DLY
#define DWM3000_TX_ANT_DLY 16345u
#endif
#ifndef DWM3000_RX_ANT_DLY
#define DWM3000_RX_ANT_DLY 16345u
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
 * Current timing-critical frame sizes before the radio FCS are 42 B for a
 * diagnostic poll, 46 B for a normal-click poll, 50 B for a response,
 * 57 B for a final, and 50 B for a report. The worst case remains the compact
 * poll: the initiator receives an 8 B longer response before scheduling the
 * final than the responder receives before scheduling the response. At
 * 850 kbps that path delta is ceil(8 B * 8 bits * 1e6 / 850e3) = 76 us. The shared
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

#define DWM3000_RX_ERROR_STATUS_MASK \
    (SYS_STATUS_ALL_RX_ERR | SYS_STATUS_RXOVRR_BIT_MASK)
#define DWM3000_SYS_STATUS_HI_FATAL_MASK \
    (SYS_STATUS_HI_SPIERR_BIT_MASK | SYS_STATUS_HI_SPI_UNF_BIT_MASK | \
     SYS_STATUS_HI_SPI_OVF_BIT_MASK | SYS_STATUS_HI_CMD_ERR_BIT_MASK)
#define RX_TERMINAL_STATUS_MASK \
    (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | \
     DWM3000_RX_ERROR_STATUS_MASK)
#define RX_ACTIVITY_STATUS_MASK \
    (SYS_STATUS_RXPRD_BIT_MASK | SYS_STATUS_RXSFDD_BIT_MASK | \
     SYS_STATUS_RXPHD_BIT_MASK | SYS_STATUS_RXFR_BIT_MASK)
#define RX_CLEAR_STATUS_MASK (RX_TERMINAL_STATUS_MASK | RX_ACTIVITY_STATUS_MASK)
#define DWM3000_DEADLINE_TX_LEAD_UUS 5000u
#define DWM3000_STANDARD_FRAME_MAX_LEN 127u

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
/*
 * Same-channel PHY reconfigure without a hardware reset.  OFF: run20
 * (logs/click_two_anchor_bench_20260903/run20) showed dwt_configure() failing
 * on every attempt with DWT_ERROR and SYS_STATUS == 0 - 20 of 20 on anchor B,
 * zero fast=1 markers across both anchors - so the attempt only added ~1 ms in
 * front of the full reset it then had to do anyway.
 *
 * Cause: dwt_configure() clears SYS_STATUS CP_LOCK and then calls
 * dwt_setdwstate(DWT_DW_IDLE), which only ORs the SEQ_CTRL auto-INIT2IDLE bit.
 * That starts a PLL calibration on the IDLE_RC -> IDLE_PLL edge only.  A radio
 * that is already in IDLE_PLL - either still awake, or woken from retained
 * sleep with SEQ_CTRL restored from the AON array - has the bit set already,
 * so no calibration runs, CP_LOCK never re-asserts, the six 20 us polls expire
 * and dwt_configure() returns DWT_ERROR.  Re-enabling this needs
 * dwt_setdwstate(DWT_DW_IDLE_RC) plus a dwt_checkidlerc() wait before
 * dwt_configure(), which is exactly the precondition configure_radio_from_reset()
 * gets for free from dwt_initialise(); that has not been proven on the bench.
 */
#define DWM3000_ENABLE_SAME_CHANNEL_FAST_SWITCH 0
#define IMMEDIATE_RX_PREAMBLE_TIMEOUT_PAC 5u
/*
 * The preamble-detect timeout is counted in PACs, so the same PAC count means
 * a different wall-clock acquisition budget on each PHY.  Five PACs of the
 * channel-5 wake/range PHY (PAC16) is ~81 us; the mesh-control PHY runs PAC8,
 * so it needs ten to keep the same tolerance for arming just before a sender
 * starts.  Shortening the control preamble must change airtime, not how long
 * an immediate receive is willing to hunt.
 */
#define CONTROL_RX_PREAMBLE_TIMEOUT_PAC 10u
#define DELAYED_RX_PREAMBLE_TIMEOUT_PAC 0u
#define DEFAULT_RESPONDER_WINDOW_MS UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS
#define DWM3000_SLEEP_MODE DWT_CONFIG
#define DWM3000_SLEEP_WAKE_FLAGS \
    (DWT_PRES_SLEEP | DWT_WAKE_CSN | DWT_WAKE_WUP | DWT_SLP_EN)
BUILD_ASSERT((DWM3000_SLEEP_MODE & DWT_CONFIG) != 0u,
             "DW3000 retained sleep must restore saved configuration on wake");
BUILD_ASSERT((DWM3000_SLEEP_WAKE_FLAGS & (DWT_WAKE_CSN | DWT_WAKE_WUP | DWT_SLP_EN)) ==
             (DWT_WAKE_CSN | DWT_WAKE_WUP | DWT_SLP_EN),
             "DW3000 retained sleep must wake via CSn or WAKEUP pin");
#define DWM3000_WAKE_IDLE_RC_TIMEOUT_US 3000u
#ifndef DWM3000_FIRST_PATH_NTM
#define DWM3000_FIRST_PATH_NTM 12u
#endif
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
/* PLEN 4096 + one SFD search symbol + 16-symbol SFD - PAC16. */
#define DWM3000_PHY_SFD_TIMEOUT 4097u
#endif
#ifndef DWM3000_WAKE_PHY_SFD_TIMEOUT
#define DWM3000_WAKE_PHY_SFD_TIMEOUT DWM3000_PHY_SFD_TIMEOUT
#endif
/*
 * Channel-5 mesh-control PHY (wake_mesh_control_config).  Mesh control frames
 * are only ever exchanged between nodes that are already awake and listening -
 * the gateway in continuous RX, or an anchor right after a wake train, a
 * ranging burst or inside a post-burst listen window.  None of those receivers
 * needs the 4096-symbol acquisition train the duty-cycled sniffers rely on, so
 * this PHY keeps every wake-PHY field except the preamble, its matching PAC and
 * the SFD timeout.  That removes 3126 us of SHR from every control frame
 * (4185 us -> 1059 us).  The wake PHY itself stays at PLEN4096.
 */
#ifndef DWM3000_CONTROL_PHY_PREAMBLE_LENGTH
#define DWM3000_CONTROL_PHY_PREAMBLE_LENGTH DWT_PLEN_1024
#endif
#ifndef DWM3000_CONTROL_PHY_RX_PAC
/* Qorvo pairs PLEN1024 with PAC8; this is the same pairing the proven
 * channel-9 payload PHY (DWM3000_MESH_PHY_*) already ships. */
#define DWM3000_CONTROL_PHY_RX_PAC DWT_PAC8
#endif
#ifndef DWM3000_CONTROL_PHY_SFD_TIMEOUT
/* PLEN 1024 + one SFD search symbol + the 16-symbol DW SFD - PAC8. The
 * channel-9 tuple derives the same way but with its 8-symbol IEEE 4z SFD,
 * so the two timeouts are not interchangeable. */
#define DWM3000_CONTROL_PHY_SFD_TIMEOUT (1024u + 1u + 16u - 8u)
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
BUILD_ASSERT(DWM3000_FIRST_PATH_NTM <= IP_CONFIG_LO_IP_NTM_BIT_MASK,
             "DWM3000 first-path threshold must fit in IP_CONFIG_LO.IP_NTM");
BUILD_ASSERT(DWM3000_PHY_PREAMBLE_LENGTH == DWT_PLEN_4096 &&
             DWM3000_PHY_RX_PAC == DWT_PAC16 &&
             DWM3000_PHY_SFD_TYPE == DWT_SFD_DW_16 &&
             DWM3000_PHY_SFD_TIMEOUT == 4097u,
             "850 kbps range PHY requires the proven PLEN4096/PAC16/DW16 SFD tuple");
BUILD_ASSERT(DWM3000_STATUS_POLL_INTERVAL_US <= DS_TWR_RX_PATH_DELTA_US,
             "DWM3000 status polling must remain inside the equal-reply DS-TWR timing margin");
BUILD_ASSERT(DWM3000_WAKE_PHY_SFD_TIMEOUT >= DWM3000_PHY_SFD_TIMEOUT,
             "wake/discovery open-window RX must not be shorter than the ranging SFD timeout");
BUILD_ASSERT(DWM3000_CONTROL_PHY_PREAMBLE_LENGTH == DWT_PLEN_1024 &&
             DWM3000_CONTROL_PHY_RX_PAC == DWT_PAC8 &&
             DWM3000_CONTROL_PHY_SFD_TIMEOUT == 1033u,
             "channel-5 mesh control PHY requires the PLEN1024/PAC8/DW16 SFD tuple");
BUILD_ASSERT(DWM3000_CONTROL_PHY_PREAMBLE_LENGTH != DWM3000_PHY_PREAMBLE_LENGTH,
             "the mesh control PHY exists to shorten the wake acquisition train");
BUILD_ASSERT(DWM3000_CONTROL_PHY_SFD_TIMEOUT < DWM3000_WAKE_PHY_SFD_TIMEOUT,
             "a shorter control preamble must shorten its SFD timeout as well");
BUILD_ASSERT(DWM3000_TIMING_CH5_CONTROL_PREAMBLE_SYMBOLS == 1024u &&
             DWM3000_TIMING_CH5_CONTROL_PAC_SYMBOLS == 8u &&
             DWM3000_TIMING_CH5_CONTROL_SFD_TIMEOUT_SYMBOLS ==
                 DWM3000_CONTROL_PHY_SFD_TIMEOUT,
             "the airtime model must describe the shipped mesh control tuple");
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
BUILD_ASSERT((UWB_RF_SCOPE_WIRE_LEN + UWB_MESH_MAX_FRAME_LEN +
              UWB_PHY_FCS_LEN) <= UWB_PHY_EXTENDED_FRAME_MAX_LEN,
             "channel-9 mesh frames must fit the DW3000 extended PHR frame limit");
BUILD_ASSERT(DWM3000_STANDARD_FRAME_MAX_LEN <= TX_FCTRL_TXFLEN_BIT_MASK,
             "standard PHR frame limit must fit the DW3000 TX length field");
BUILD_ASSERT((((uint64_t)DWM3000_DEADLINE_TX_LEAD_UUS *
               DWM3000_UUS_TO_DWT_TIME) >> 8) > 0u &&
             (((uint64_t)DWM3000_DEADLINE_TX_LEAD_UUS *
               DWM3000_UUS_TO_DWT_TIME) >> 8) <= UINT32_MAX,
             "deadline-bound delayed-TX lead must fit the DW3000 time register");

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

/*
 * Differs from wake_config in exactly five fields: txPreambLength, rxPAC,
 * sfdTO, phrMode and phrRate.  Channel, codes, PRF/SFD type, data rate, STS
 * and PDoA stay identical so ensure_phy_mode() can retune this PHY with a
 * plain dwt_configure() instead of a hardware reset.
 */
static dwt_config_t wake_mesh_control_config = {
    DWM3000_PHY_CHANNEL,
    DWM3000_CONTROL_PHY_PREAMBLE_LENGTH,
    DWM3000_CONTROL_PHY_RX_PAC,
    DWM3000_PHY_TX_CODE,
    DWM3000_PHY_RX_CODE,
    DWM3000_PHY_SFD_TYPE,
    DWM3000_PHY_DATA_RATE,
    DWM3000_MESH_PHY_PHR_MODE,
    DWM3000_MESH_PHY_PHR_RATE,
    DWM3000_CONTROL_PHY_SFD_TIMEOUT,
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
static bool radio_state_unknown = true;
static enum dwm3000_phy_mode active_phy_mode;
static atomic_t radio_transition_trace_seq;
static struct dwm3000_rx_debug_snapshot last_rx_debug;
static uint32_t last_rx_finfo_register;
static uint32_t last_rx_host_uptime_ms;
static bool last_rx_host_uptime_valid;
static struct dwm3000_driver_stats driver_stats;

static int dwm3000_local_rf_scope(struct uwb_rf_scope *scope)
{
    enum uwb_rf_scope_role role;
    uint8_t forced_relay_hops = 0u;

#if DEVICE_ROLE == ROLE_GATEWAY
    role = UWB_RF_SCOPE_ROLE_GATEWAY;
#elif DEVICE_ROLE == ROLE_ANCHOR
    role = UWB_RF_SCOPE_ROLE_ANCHOR;
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS)
    forced_relay_hops =
        CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS;
#endif
#else
    role = UWB_RF_SCOPE_ROLE_CLICKER;
#endif

    return uwb_rf_scope_build(role, forced_relay_hops, scope);
}


/* Implementation is split by responsibility but remains one translation unit. */
#include "dwm3000_driver_radio.inc"
#include "dwm3000_driver_ranging_frames.inc"
#include "dwm3000_driver_io.inc"
#include "dwm3000_driver_ds_twr.inc"
