#include "dwm3000_driver.h"

#include "dwm3000_port.h"
#include "uwb.h"

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

#define POLL_TX_TO_RESP_RX_DLY_UUS 690u
#define RESP_RX_TO_FINAL_TX_DLY_UUS 880u
#define RESP_RX_TIMEOUT_UUS 300u
#define RESP_RX_TIMEOUT_MS 8u
#define POLL_RX_TO_RESP_TX_DLY_UUS 900u
#define RESP_TX_TO_FINAL_RX_DLY_UUS 670u
#define FINAL_RX_TIMEOUT_UUS 300u
#define FINAL_RX_TIMEOUT_MS 8u
#define REPORT_RX_TIMEOUT_UUS 700u
#define REPORT_RX_TIMEOUT_MS 12u
#define PREAMBLE_TIMEOUT_PAC 5u
#define POLL_INTERVAL_US 100u
#define DEFAULT_RESPONDER_WINDOW_MS 50u
#define DWM3000_SLEEP_MODE DWT_CONFIG
#define DWM3000_SLEEP_WAKE_FLAGS (DWT_PRES_SLEEP | DWT_WAKE_WUP | DWT_SLP_EN)

static dwt_config_t default_config = {
    5,
    DWT_PLEN_64,
    DWT_PAC8,
    9,
    9,
    1,
    DWT_BR_6M8,
    DWT_PHRMODE_STD,
    DWT_PHRRATE_STD,
    65,
    DWT_STS_MODE_1 | DWT_STS_MODE_SDC,
    DWT_STS_LEN_64,
    DWT_PDOA_M0,
};

static dwt_txconfig_t default_txconfig = {
    0x34,
    0xfdfdfdfd,
    0x0,
};

static bool radio_configured;
static bool radio_awake;

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

static void clear_status(uint32_t mask)
{
    dwt_write32bitreg(SYS_STATUS_ID, mask);
}

static void clear_all_events(void)
{
    clear_status(0xffffffffu);
}

static int wait_status(uint32_t mask, uint32_t timeout_ms, uint32_t *status)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint32_t read_status;

    do {
        read_status = dwt_read32bitreg(SYS_STATUS_ID);
        if ((read_status & mask) != 0u) {
            if (status != NULL) {
                *status = read_status;
            }
            return 0;
        }
        k_busy_wait(POLL_INTERVAL_US);
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
    if (header->type != expected_type ||
        header->seq != request->seq ||
        header->session_id != request->session_id ||
        header->initiator_id != request->initiator_id) {
        return false;
    }

    return request->responder_id == DWM3000_BROADCAST_ID ||
           header->responder_id == request->responder_id;
}

static bool poll_targets_anchor(const struct uwb_range_header *header,
                                uint64_t local_anchor_id)
{
    return header->responder_id == local_anchor_id ||
           header->responder_id == DWM3000_BROADCAST_ID;
}

static int validate_range_request(const struct dwm3000_range_request *request)
{
    if (request == NULL ||
        request->initiator_id == 0u ||
        request->responder_id == 0u ||
        request->initiator_id == request->responder_id ||
        request->session_id == 0u) {
        return -EINVAL;
    }
    if (((request->flags & FLAG_DIAGNOSTIC) != 0u) &&
        ((request->flags & FLAG_COUNT_AS_CLICK) != 0u)) {
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
                         uint64_t *rx_timestamp, uint8_t *quality)
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
                            enum range_status *status_out)
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
                        rx_buffer, sizeof(rx_buffer), &frame_len, NULL, NULL);
    if (ret < 0) {
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

    if (report.header.seq != request->seq ||
        report.header.session_id != request->session_id ||
        report.header.initiator_id != request->initiator_id ||
        report.header.responder_id != responder_id) {
        result->status = RANGE_WRONG_TARGET;
        return -EADDRNOTAVAIL;
    }

    result->responder_id = report.header.responder_id;
    result->distance_mm = report.distance_mm;
    result->quality = report.quality;
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

    if (dwt_configure(&default_config) != DWT_SUCCESS) {
        return -EIO;
    }

    dwt_configuretxrf(&default_txconfig);
    dwt_setrxantennadelay(DWM3000_RX_ANT_DLY);
    dwt_settxantennadelay(DWM3000_TX_ANT_DLY);
    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);
    clear_all_events();

    radio_configured = true;
    radio_awake = true;
    LOG_INF("DWM3000 configured for channel 5 STS-SDC DS-TWR at %u Hz SPI",
            (unsigned int)dwm3000_port_current_spi_hz());
    return 0;
}

int dwm3000_driver_standby(void)
{
    if (!radio_configured && !radio_awake) {
        return 0;
    }

    if (radio_configured) {
        dwt_forcetrxoff();
    }
    clear_all_events();
    dwt_configuresleep(DWM3000_SLEEP_MODE, DWM3000_SLEEP_WAKE_FLAGS);
    dwt_entersleep(DWT_DW_IDLE);
    radio_configured = false;
    radio_awake = false;
    LOG_INF("DWM3000 entered deep sleep; wakeup pin required before next UWB window");
    return dwm3000_port_set_slow_spi();
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

    if (!radio_configured) {
        ret = dwm3000_driver_configure_default();
        if (ret < 0) {
            result->status = RANGE_INTERNAL_ERROR;
            return ret;
        }
    }

    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(PREAMBLE_TIMEOUT_PAC);

    poll_header.type = MSG_UWB_POLL;
    poll_header.seq = request->seq;
    poll_header.session_id = request->session_id;
    poll_header.initiator_id = request->initiator_id;
    poll_header.responder_id = request->responder_id;
    poll_header.flags = request->flags;

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

    ret = receive_response(request, &response, &resp_rx_ts, &quality, &range_status);
    if (ret < 0) {
        result->responder_id = request->responder_id;
        result->quality = quality;
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
    final.header.session_id = request->session_id;
    final.header.initiator_id = request->initiator_id;
    final.header.responder_id = response.header.responder_id;
    final.header.flags = request->flags;
    final.poll_tx_ts_32 = (uint32_t)poll_tx_ts;
    final.resp_rx_ts_32 = (uint32_t)resp_rx_ts;
    final.final_tx_ts_32 = (uint32_t)final_tx_ts;

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
        result->responder_id = response.header.responder_id;
        result->quality = quality;
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
    return ret;
}

int dwm3000_driver_responder_poll_once(uint64_t local_anchor_id,
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
    int32_t distance_mm = 0;
    enum range_status report_status = RANGE_OK;
    int ret;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = RANGE_INTERNAL_ERROR;
    }

    if (local_anchor_id == 0u) {
        return -EINVAL;
    }

    if (!radio_configured) {
        ret = dwm3000_driver_configure_default();
        if (ret < 0) {
            return ret;
        }
    }

    dwt_setpreambledetecttimeout(0u);
    dwt_setrxtimeout(0u);
    clear_all_events();
    if (dwt_rxenable(DWT_START_RX_IMMEDIATE) != DWT_SUCCESS) {
        return -EIO;
    }

    ret = receive_frame(timeout_ms == 0u ? DEFAULT_RESPONDER_WINDOW_MS : timeout_ms,
                        &status, rx_buffer, sizeof(rx_buffer), &frame_len,
                        &poll_rx_ts, &quality);
    if (ret < 0) {
        return ret == -ETIMEDOUT ? -ETIMEDOUT : ret;
    }

    ret = decode_poll_frame(rx_buffer, frame_len, &poll);
    if (ret < 0) {
        return ret;
    }
    if (!poll_targets_anchor(&poll, local_anchor_id)) {
        return -EAGAIN;
    }
    if (result != NULL) {
        result->responder_id = local_anchor_id;
        result->quality = quality;
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
    response.header.session_id = poll.session_id;
    response.header.initiator_id = poll.initiator_id;
    response.header.responder_id = local_anchor_id;
    response.header.flags = poll.flags;
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

    ret = receive_frame(FINAL_RX_TIMEOUT_MS, &status,
                        rx_buffer, sizeof(rx_buffer), &frame_len,
                        &final_rx_ts, &quality);
    if (ret < 0) {
        if (result != NULL) {
            result->quality = quality;
            result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT : RANGE_RX_ERROR;
        }
        return ret;
    }

    ret = decode_final_frame(rx_buffer, frame_len, &final);
    if (ret < 0) {
        report_status = RANGE_BAD_FRAME;
    } else if (final.header.seq != poll.seq ||
               final.header.session_id != poll.session_id ||
               final.header.initiator_id != poll.initiator_id ||
               final.header.responder_id != local_anchor_id) {
        report_status = RANGE_WRONG_TARGET;
    } else {
        ret = compute_distance_mm(&final, poll_rx_ts, resp_tx_ts, final_rx_ts, &distance_mm);
        if (ret < 0) {
            report_status = RANGE_INTERNAL_ERROR;
        }
    }

    report.header.type = MSG_UWB_REPORT;
    report.header.seq = poll.seq;
    report.header.session_id = poll.session_id;
    report.header.initiator_id = poll.initiator_id;
    report.header.responder_id = local_anchor_id;
    report.header.flags = poll.flags;
    report.distance_mm = distance_mm;
    report.quality = quality;
    report.status = report_status;

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

    LOG_INF("UWB report sent to 0x%016llx: status=%u distance_mm=%d quality=%u",
            (unsigned long long)poll.initiator_id,
            report.status,
            report.distance_mm,
            report.quality);
    if (result != NULL) {
        result->responder_id = local_anchor_id;
        result->distance_mm = distance_mm;
        result->quality = quality;
        result->status = report_status;
    }
    return report_status == RANGE_OK ? 0 : -EIO;
}
