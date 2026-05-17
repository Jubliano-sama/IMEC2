#include "dwm3000_driver.h"

#include "dwm3000_port.h"
#include "uwb.h"
#include "uwb_session.h"

#include "deca_device_api.h"
#include "deca_regs.h"
#include "deca_vals.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(dwm3000_driver, LOG_LEVEL_INF);

#define DWM3000_TX_ANT_DLY 16385u
#define DWM3000_RX_ANT_DLY 16385u
#define DWM3000_UUS_TO_DWT_TIME 63898ULL
#define DWM3000_TIME_UNITS (1.0 / (499.2e6 * 128.0))
#define DWM3000_SPEED_OF_LIGHT_MPS 299702547.0
/* Q8.8 constants for 10*log10(ipatovPower*2^17/ipatovAccumCount^2)-121.74 dBm. */
#define DWM3000_LOG2_TO_DB_Q8 771
#define DWM3000_IPATOV_POWER_SCALE_DB_Q8 13101
#define DWM3000_IPATOV_RSL_OFFSET_DB_Q8 31165
#define DWM3000_ACCUM_CIR_SAMPLE_READ_LEN (UWB_CIR_SAMPLE_LEN + 1u)

#define POLL_TX_TO_RESP_RX_DLY_UUS 690u
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
 * path delta is ceil(8 B * 8 bits * 1e6 / 850e3) = 76 us. The shared 900 uus
 * delay intentionally lets the shorter poll path wait, keeping both reply
 * times equal. Optional RX diagnostics must stay out of the RX-to-delayed-TX
 * critical path; delayed TX misses mean this common value must be raised.
 */
#define UWB_PHY_DATA_RATE_BPS 850000u
#define DS_TWR_RX_PATH_DELTA_BYTES (UWB_RESP_LEN - UWB_POLL_LEN)
#define DS_TWR_RX_PATH_DELTA_US (((DS_TWR_RX_PATH_DELTA_BYTES * 8u * 1000000u) + \
                                  UWB_PHY_DATA_RATE_BPS - 1u) / UWB_PHY_DATA_RATE_BPS)
#define DS_TWR_REPLY_DLY_UUS 900u
#define RESP_RX_TO_FINAL_TX_DLY_UUS DS_TWR_REPLY_DLY_UUS
#define RESP_RX_TIMEOUT_UUS 2000u
#define RESP_RX_TIMEOUT_MS 8u
#define POLL_RX_TO_RESP_TX_DLY_UUS DS_TWR_REPLY_DLY_UUS
#define RESP_TX_TO_FINAL_RX_DLY_UUS 670u
#define FINAL_RX_TIMEOUT_UUS 2000u
#define FINAL_RX_TIMEOUT_MS 8u
#define REPORT_RX_TIMEOUT_UUS 2500u
#define REPORT_RX_TIMEOUT_MS 12u
#define PREAMBLE_TIMEOUT_PAC 5u
#define DEFAULT_RESPONDER_WINDOW_MS 50u
#define DWM3000_SLEEP_MODE DWT_CONFIG
#define DWM3000_SLEEP_WAKE_FLAGS (DWT_PRES_SLEEP | DWT_WAKE_WUP | DWT_SLP_EN)
#define DWM3000_IRQ_EVENT_MASK (SYS_ENABLE_LO_TXFRS_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXFCG_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXFTO_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXPTO_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXPHE_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXFCE_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXFSL_ENABLE_BIT_MASK | \
                                SYS_ENABLE_LO_RXSTO_ENABLE_BIT_MASK)

BUILD_ASSERT(DS_TWR_RX_PATH_DELTA_US == 76u,
             "Update the DS-TWR equal-reply timing calculation when UWB frame sizes change");
BUILD_ASSERT(DS_TWR_REPLY_DLY_UUS == UWB_DS_TWR_REPLY_DELAY_US,
             "UWB schedule validation must match the fixed DWM3000 DS-TWR reply delay");

static dwt_config_t default_config = {
    5,
    DWT_PLEN_64,
    DWT_PAC8,
    9,
    9,
    1,
    DWT_BR_850K,
    DWT_PHRMODE_STD,
    DWT_PHRRATE_STD,
    65,
    DWT_STS_MODE_1 | DWT_STS_MODE_SDC,
    DWT_STS_LEN_64,
    DWT_PDOA_M0,
};

static dwt_config_t wake_config = {
    5,
    DWT_PLEN_1024,
    DWT_PAC32,
    9,
    9,
    1,
    DWT_BR_850K,
    DWT_PHRMODE_STD,
    DWT_PHRRATE_STD,
    1057,
    DWT_STS_MODE_1 | DWT_STS_MODE_SDC,
    DWT_STS_LEN_64,
    DWT_PDOA_M0,
};

static dwt_txconfig_t default_txconfig = {
    0x34,
    0xfdfdfdfd,
    0x0,
};

enum dwm3000_phy_mode {
    DWM3000_PHY_NONE = 0,
    DWM3000_PHY_RANGE = 1,
    DWM3000_PHY_WAKE = 2,
};

static bool radio_configured;
static bool radio_awake;
static enum dwm3000_phy_mode active_phy_mode;

static void clear_all_events(void);

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

    dwt_configuretxrf(&default_txconfig);
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);
    dwt_setinterrupt(DWM3000_IRQ_EVENT_MASK, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();

    radio_configured = true;
    radio_awake = true;
    active_phy_mode = phy_mode;
    return 0;
}

static int wake_configured_radio(void)
{
    int ret;

    if (!radio_configured || radio_awake) {
        return 0;
    }

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_port_set_fast_spi();
    if (ret < 0) {
        return ret;
    }
    ret = ensure_local_data();
    if (ret < 0) {
        return ret;
    }

    dwt_restoreconfig();
    dwt_setinterrupt(DWM3000_IRQ_EVENT_MASK, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();
    radio_awake = true;
    return 0;
}

static int ensure_phy_mode(enum dwm3000_phy_mode phy_mode)
{
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

    config = phy_mode == DWM3000_PHY_WAKE ? &wake_config : &default_config;
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
    dwm3000_port_irq_reset();
}

static int wait_status(uint32_t mask, uint32_t timeout_ms, uint32_t *status)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint32_t read_status;
    int64_t now_ms;
    uint32_t wait_ms;
    int ret;

    do {
        read_status = dwt_read32bitreg(SYS_STATUS_ID);
        if ((read_status & mask) != 0u) {
            if (status != NULL) {
                *status = read_status;
            }
            return 0;
        }

        now_ms = k_uptime_get();
        if (now_ms >= deadline) {
            break;
        }
        wait_ms = (uint32_t)(deadline - now_ms);
        ret = dwm3000_port_wait_for_irq(wait_ms == 0u ? 1u : wait_ms);
        if (ret < 0 && ret != -ETIMEDOUT) {
            return ret;
        }
    } while (k_uptime_get() <= deadline);

    if (status != NULL) {
        *status = dwt_read32bitreg(SYS_STATUS_ID);
    }
    return -ETIMEDOUT;
}

static int wait_tx_complete(uint32_t timeout_ms)
{
    uint32_t status;
    int ret;

    ret = wait_status(SYS_STATUS_TXFRS_BIT_MASK, timeout_ms, &status);
    if (ret < 0) {
        return ret;
    }

    clear_status(SYS_STATUS_TXFRS_BIT_MASK);
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

static int check_sts_quality(uint8_t *quality)
{
    int16_t sts_quality;
    int ret;

    ret = dwt_readstsquality(&sts_quality);
    if (ret < 0) {
        if (quality != NULL) {
            *quality = 0u;
        }
        return -EBADMSG;
    }

    if (quality != NULL) {
        *quality = sts_quality >= 100 ? 100u : (uint8_t)sts_quality;
    }
    return 0;
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

    ret = write_tx_frame(frame, frame_len);
    if (ret < 0) {
        return ret;
    }

    return dwt_starttx(tx_mode) == DWT_SUCCESS ? 0 : -EIO;
}

static int receive_frame(uint32_t timeout_ms, uint32_t *status,
                         uint8_t *buffer, size_t buffer_len, size_t *frame_len,
                         uint64_t *rx_timestamp, uint8_t *quality,
                         int8_t *rsl_dbm, uint8_t cir_sample[UWB_CIR_SAMPLE_LEN],
                         bool *cir_sampled, bool capture_rsl)
{
    int ret;

    ret = wait_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR,
                      timeout_ms, status);
    if (ret < 0) {
        dwt_forcetrxoff();
        return ret;
    }

    if ((*status & SYS_STATUS_RXFCG_BIT_MASK) == 0u) {
        clear_status(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff();
        return -EIO;
    }

    if (rx_timestamp != NULL) {
        *rx_timestamp = read_rx_timestamp_u64();
    }
    if (check_sts_quality(quality) < 0) {
        clear_status(SYS_STATUS_RXFCG_BIT_MASK);
        dwt_forcetrxoff();
        return -EBADMSG;
    }
    if (capture_rsl) {
        read_rx_diagnostics(rsl_dbm, cir_sample, cir_sampled);
    }

    *frame_len = read_rx_frame(buffer, buffer_len);
    clear_status(SYS_STATUS_RXFCG_BIT_MASK);
    if (*frame_len == 0u) {
        dwt_forcetrxoff();
        return -EMSGSIZE;
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
    if (check_sts_quality(quality) < 0) {
        clear_status(SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_TXFRS_BIT_MASK);
        dwt_forcetrxoff();
        *status_out = RANGE_STS_QUALITY_FAIL;
        return -EBADMSG;
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
                        NULL, NULL, NULL, false);
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
    result->flags = report.header.flags;
    result->distance_mm = report.distance_mm;
    result->quality = report.quality;
    result->rsl_dbm = report.rsl_dbm;
    result->rsl_sampled = report.rsl_dbm != 0;
    result->status = report.status;
    return report.status == RANGE_OK ? 0 : -EIO;
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
    LOG_INF("DWM3000 configured for channel 5 STS-SDC DS-TWR at 850 kbps, %u Hz SPI; waiting on IRQ",
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

    LOG_DBG("DWM3000 configured for UWB wake/discovery long-preamble mode");
    return 0;
}

int dwm3000_driver_standby(void)
{
    if (!radio_configured || !radio_awake) {
        return 0;
    }

    dwt_forcetrxoff();
    dwt_setinterrupt(0u, 0u, DWT_ENABLE_INT_ONLY);
    clear_all_events();
    dwt_configuresleep(DWM3000_SLEEP_MODE, DWM3000_SLEEP_WAKE_FLAGS);
    dwt_entersleep(DWT_DW_IDLE);
    radio_awake = false;
    LOG_INF("DWM3000 entered sleep with retained config; wakeup pin required before next UWB window");
    return dwm3000_port_set_slow_spi();
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

int dwm3000_driver_receive_frame_detailed(uint32_t timeout_ms,
                                          uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure)
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

    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);
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
                        rsl_dbm != NULL);
    if (ret < 0) {
        if (failure != NULL) {
            *failure = status_to_rx_failure(status);
            if (*failure == DWM3000_RX_FAILURE_NONE) {
                if (ret == -EBADMSG) {
                    *failure = DWM3000_RX_FAILURE_STS_QUALITY;
                } else if (ret == -EMSGSIZE) {
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

int dwm3000_driver_range_initiator(const struct dwm3000_range_request *request,
                                   struct dwm3000_range_result *result)
{
    struct uwb_response_frame response;
    struct uwb_final_frame final;
    struct uwb_range_header poll_header;
    uint8_t tx_buffer[UWB_FINAL_LEN];
    size_t tx_len;
    uint64_t poll_tx_ts;
    uint64_t resp_rx_ts;
    uint64_t final_tx_ts;
    uint32_t final_tx_time;
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

    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);

    poll_header.type = MSG_UWB_POLL;
    poll_header.seq = request->seq;
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

    final_tx_time = (uint32_t)((resp_rx_ts +
                                (RESP_RX_TO_FINAL_TX_DLY_UUS *
                                 DWM3000_UUS_TO_DWT_TIME)) >> 8);
    dwt_setdelayedtrxtime(final_tx_time);
    final_tx_ts = (((uint64_t)(final_tx_time & 0xfffffffeUL)) << 8) +
                  DWM3000_TX_ANT_DLY;

    final.header.type = MSG_UWB_FINAL;
    final.header.seq = request->seq;
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

    ret = uwb_session_validate_reply_timing(
        dwt_delta_to_uus(response.poll_rx_ts_32, response.resp_tx_ts_32),
        dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
        DS_TWR_REPLY_DLY_UUS,
        UWB_SESSION_REPLY_DELAY_TOLERANCE_US);
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
    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);

    ret = send_range_frame(tx_buffer, tx_len,
                           DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
    if (ret < 0) {
        result_set_request_metadata(result, request);
        result->responder_id = request->responder_id;
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->status = RANGE_DELAYED_TX_MISSED;
        return -ETIME;
    }

    ret = receive_report(request, response.header.responder_id, result);
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
                        &poll_rx_ts, &quality, &rsl_dbm, NULL, NULL, false);
    if (ret < 0) {
        if (ret == -ETIMEDOUT) {
            return -ETIMEDOUT;
        }
        if (result != NULL) {
            result->quality = quality;
            result->rsl_dbm = rsl_dbm;
            result->status = ret == -EBADMSG ? RANGE_STS_QUALITY_FAIL :
                             ret == -EMSGSIZE ? RANGE_BAD_FRAME :
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
        result->quality = quality;
        result->rsl_dbm = rsl_dbm;
        result->exchange_started = true;
        result->status = RANGE_INTERNAL_ERROR;
    }

    resp_tx_time = (uint32_t)((poll_rx_ts +
                               (POLL_RX_TO_RESP_TX_DLY_UUS *
                                DWM3000_UUS_TO_DWT_TIME)) >> 8);
    dwt_setdelayedtrxtime(resp_tx_time);
    resp_tx_ts = (((uint64_t)(resp_tx_time & 0xfffffffeUL)) << 8) +
                 DWM3000_TX_ANT_DLY;

    response.header.type = MSG_UWB_RESP;
    response.header.seq = poll.seq;
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
    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);

    ret = send_range_frame(tx_buffer, tx_len,
                           DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
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
                        capture_final_rsl);
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
               final.header.network_id != poll.network_id ||
               final.header.session_id != poll.session_id ||
               final.header.session_nonce != poll.session_nonce ||
               final.header.initiator_short_addr != poll.initiator_short_addr ||
               final.header.responder_short_addr != short_addr_from_id(local_anchor_id) ||
               final.header.flags != poll.flags ||
               final.header.initiator_id != poll.initiator_id ||
               final.header.responder_id != local_anchor_id) {
        report_status = RANGE_WRONG_TARGET;
    } else if (uwb_session_validate_reply_timing(
                   dwt_delta_to_uus((uint32_t)poll_rx_ts, (uint32_t)resp_tx_ts),
                   dwt_delta_to_uus(final.resp_rx_ts_32, final.final_tx_ts_32),
                   DS_TWR_REPLY_DLY_UUS,
                   UWB_SESSION_REPLY_DELAY_TOLERANCE_US) != PROTO_OK) {
        report_status = RANGE_TIMING_INVALID;
    } else {
        ret = compute_distance_mm(&final, poll_rx_ts, resp_tx_ts, final_rx_ts, &distance_mm);
        if (ret < 0) {
            report_status = RANGE_INTERNAL_ERROR;
        }
    }

    report.header.type = MSG_UWB_REPORT;
    report.header.seq = poll.seq;
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
