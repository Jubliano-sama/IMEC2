#include "app_mesh_report_encode.h"

#include "app_board.h"
#include "app_config.h"
#include "app_mesh_report.h"
#include "app_stack_workload_diag.h"
#include "app_state.h"
#include "protocol.h"
#include "report.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_DECLARE(app_mesh_report, LOG_LEVEL_DBG);

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(DWM3000_FULL_CIR_BYTES <= UINT16_MAX,
             "CIR reassembly length must fit its protocol field");
BUILD_ASSERT(RANGE_REPORT_CIR_PACKET_METADATA_BYTES +
             UWB_FULL_CIR_REPORT_PACKET_BYTES +
             (2u * RANGE_REPORT_CIR_PACKET_CHUNK_TLV_COUNT) +
             MESH_CH9_BATCH_METADATA_TLV_BYTES ==
             PACKET_EXT_MAX_PAYLOAD_LEN,
             "routed CIR payload must exactly fit one extended packet");
BUILD_ASSERT(RANGE_REPORT_CIR_WINDOW_RAW_BYTES == DWM3000_FULL_CIR_BYTES,
             "report CIR window must match the captured DWM3000 window");
#endif

static struct app_mesh_report_encode_ops report_encode_ops;

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
struct anchor_cir_report_stream {
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint64_t timestamp_ms;
    uint32_t event_seq;
    uint32_t generation;
    uint16_t captured_len;
    uint16_t total_len;
    uint16_t first_path_index;
    uint16_t start_index;
    uint16_t next_offset;
    uint16_t next_fragment_index;
    uint16_t fragment_count;
    uint16_t next_seq;
    bool active;
};

static uint8_t anchor_click_cir_buffer[DWM3000_FULL_CIR_BYTES];
static struct anchor_cir_report_stream anchor_cir_report_stream;
static struct k_spinlock anchor_cir_report_lock;
#endif

void app_mesh_report_encode_init(
    const struct app_mesh_report_encode_ops *ops)
{
    if (ops == NULL) {
        memset(&report_encode_ops, 0, sizeof(report_encode_ops));
        return;
    }
    report_encode_ops = *ops;
}

int anchor_append_sequence_time_tlvs(uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len,
                                     int64_t local_ms)
{
    uint64_t timestamp_ms = 0u;

    anchor_sequence_timestamp_at(local_ms, &timestamp_ms);

    return tlv_append_u64(payload,
                          payload_cap,
                          payload_len,
                          TLV_TIMESTAMP_MS,
                          timestamp_ms);
}

static bool range_result_has_raw_timestamps(const struct dwm3000_range_result *result)
{
    return result != NULL &&
           (result->poll_tx_ts_32 != 0u ||
            result->poll_rx_ts_32 != 0u ||
            result->resp_tx_ts_32 != 0u ||
            result->resp_rx_ts_32 != 0u ||
            result->final_tx_ts_32 != 0u ||
            result->final_rx_ts_32 != 0u);
}

int append_range_result_timing_tlvs(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    const struct dwm3000_range_result *result)
{
    const uint8_t *rx_diag = NULL;
    uint8_t rx_diag_len = 0u;
    int ret;

    if (payload == NULL || payload_len == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    if (result->rsl_sampled) {
        ret = tlv_append_i8(payload, payload_cap, payload_len,
                            TLV_UWB_RSL_DBM, result->rsl_dbm);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    if (result->clock_offset_sampled) {
        ret = tlv_append_u16(payload, payload_cap, payload_len,
                             TLV_UWB_CLOCK_OFFSET_RAW,
                             (uint16_t)result->clock_offset_raw);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (result->clicker_clock_offset_sampled) {
        ret = tlv_append_u16(payload, payload_cap, payload_len,
                             TLV_CLICKER_CLOCK_OFFSET_RAW,
                             (uint16_t)result->clicker_clock_offset_raw);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (result->carrier_integrator_sampled) {
        ret = tlv_append_i32(payload, payload_cap, payload_len,
                             TLV_UWB_CARRIER_INTEGRATOR,
                             result->carrier_integrator);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (range_result_has_raw_timestamps(result)) {
        uint8_t timestamps[6u * sizeof(uint32_t)];

        proto_put_u32_le(&timestamps[0], result->poll_tx_ts_32);
        proto_put_u32_le(&timestamps[4], result->poll_rx_ts_32);
        proto_put_u32_le(&timestamps[8], result->resp_tx_ts_32);
        proto_put_u32_le(&timestamps[12], result->resp_rx_ts_32);
        proto_put_u32_le(&timestamps[16], result->final_tx_ts_32);
        proto_put_u32_le(&timestamps[20], result->final_rx_ts_32);
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_UWB_RAW_TIMESTAMPS,
                               timestamps,
                               sizeof(timestamps));
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    if (result->clicker_rx_diag_sampled) {
        rx_diag = result->clicker_rx_diag_raw;
        rx_diag_len = result->clicker_rx_diag_raw_len;
    } else if (result->anchor_rx_diag_sampled) {
        rx_diag = result->anchor_rx_diag_raw;
        rx_diag_len = result->anchor_rx_diag_raw_len;
    }
    if (rx_diag != NULL && rx_diag_len > 0u) {
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_UWB_RX_DIAG_BYTES,
                               rx_diag,
                               rx_diag_len);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    return PROTO_OK;
}

static uint32_t anchor_status_bits(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0u;
    }

    return uwb_session_status_bits_from_diagnostics(&anchor_uwb_session.diagnostics);
}

int append_anchor_status_tlvs(uint8_t *payload,
                              size_t payload_cap,
                              size_t *payload_len)
{
    struct anchor_heartbeat_fields fields = {
        .device_role = (uint8_t)DEVICE_ROLE,
        .battery_mv = ANCHOR_BATTERY_MV_UNKNOWN,
        .status_bits = anchor_status_bits(),
        .uptime_ms = k_uptime_get_32(),
    };
    int ret;

    anchor_sequence_timestamp_at(k_uptime_get(), &fields.timestamp_ms);
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

uint8_t *mesh_anchor_click_cir_capture_begin(size_t *capacity)
{
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    k_spinlock_key_t key = k_spin_lock(&anchor_cir_report_lock);

    anchor_cir_report_stream.active = false;
    anchor_cir_report_stream.generation++;
    k_spin_unlock(&anchor_cir_report_lock, key);
    if (capacity != NULL) {
        *capacity = sizeof(anchor_click_cir_buffer);
    }
    return anchor_click_cir_buffer;
#else
    if (capacity != NULL) {
        *capacity = 0u;
    }
    return NULL;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
static int anchor_cir_report_queue_next(void)
{
    struct range_report_cir_fragment fragment = {0};
    struct mesh_outbound outbound = {0};
    uint8_t chunk[UWB_FULL_CIR_REPORT_PACKET_BYTES];
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    k_spinlock_key_t key;
    uint32_t generation;
    uint32_t queue_depth = 0u;
    size_t payload_len = 0u;
    uint16_t chunk_len;
    int ret;

    key = k_spin_lock(&anchor_cir_report_lock);
    if (!anchor_cir_report_stream.active) {
        k_spin_unlock(&anchor_cir_report_lock, key);
        return -ENOENT;
    }

    chunk_len = MIN((uint16_t)sizeof(chunk),
                    (uint16_t)(anchor_cir_report_stream.captured_len -
                               anchor_cir_report_stream.next_offset));
    fragment.clicker_id = anchor_cir_report_stream.clicker_id;
    fragment.anchor_id = anchor_cir_report_stream.anchor_id;
    fragment.event_seq = anchor_cir_report_stream.event_seq;
    fragment.timestamp_ms = anchor_cir_report_stream.timestamp_ms;
    fragment.fragment_index = anchor_cir_report_stream.next_fragment_index;
    fragment.fragment_count = anchor_cir_report_stream.fragment_count;
    fragment.byte_offset = anchor_cir_report_stream.next_offset;
    fragment.total_bytes = anchor_cir_report_stream.total_len;
    fragment.first_path_index = anchor_cir_report_stream.first_path_index;
    fragment.start_index = anchor_cir_report_stream.start_index;
    fragment.chunk = chunk;
    fragment.chunk_len = chunk_len;
    memcpy(chunk,
           &anchor_click_cir_buffer[anchor_cir_report_stream.next_offset],
           chunk_len);
    generation = anchor_cir_report_stream.generation;
    anchor_cir_report_stream.next_offset += chunk_len;
    anchor_cir_report_stream.next_fragment_index++;
    outbound.packet.seq = anchor_cir_report_stream.next_seq++;
    if (anchor_cir_report_stream.next_seq == 0u) {
        anchor_cir_report_stream.next_seq = 1u;
    }
    if (anchor_cir_report_stream.next_offset >=
        anchor_cir_report_stream.captured_len) {
        anchor_cir_report_stream.active = false;
    }
    k_spin_unlock(&anchor_cir_report_lock, key);

    ret = report_append_cir_fragment_tlvs(payload,
                                          sizeof(payload),
                                          &payload_len,
                                          &fragment);
    if (ret == PROTO_OK) {
        ret = report_init_range_packet(&outbound.packet,
                                       fragment.anchor_id,
                                       GATEWAY_ID,
                                       proto_click_report_session_id(
                                           fragment.clicker_id,
                                           fragment.event_seq),
                                       outbound.packet.seq,
                                       FLAG_DIAGNOSTIC,
                                       (uint16_t)payload_len);
    }
    if (ret != PROTO_OK) {
        ret = -EINVAL;
        goto fail_stream;
    }

    memcpy(outbound.payload, payload, payload_len);
    outbound.payload_len = (uint16_t)payload_len;
    if (report_encode_ops.queue_cir_fragment == NULL) {
        ret = -ENOSYS;
        goto fail_stream;
    }
    ret = report_encode_ops.queue_cir_fragment(&outbound, &queue_depth);
    if (ret == 0) {
        status_debug_printf("DBG_ANCHOR_CIR_QUEUE event=%u fragment=%u/%u offset=%u bytes=%u queue=%u\n",
                            fragment.event_seq,
                            fragment.fragment_index + 1u,
                            fragment.fragment_count,
                            fragment.byte_offset,
                            fragment.chunk_len,
                            queue_depth);
        report_tx_schedule(0u);
        return 0;
    }
    ret = -ENOSPC;

fail_stream:
    key = k_spin_lock(&anchor_cir_report_lock);
    if (anchor_cir_report_stream.generation == generation) {
        anchor_cir_report_stream.active = false;
    }
    k_spin_unlock(&anchor_cir_report_lock, key);
    LOG_WRN("anchor partial CIR stream stopped: event=%u fragment=%u/%u ret=%d",
            fragment.event_seq,
            fragment.fragment_index + 1u,
            fragment.fragment_count,
            ret);
    return ret;
}

static int anchor_cir_report_start(uint64_t clicker_id,
                                   uint32_t event_seq,
                                   uint64_t timestamp_ms,
                                   uint16_t next_seq,
                                   const struct dwm3000_range_result *result)
{
    k_spinlock_key_t key;

    if (result == NULL || !result->anchor_full_cir_sampled ||
        result->anchor_full_cir_len == 0u) {
        return 0;
    }
    if (result->anchor_full_cir_len > sizeof(anchor_click_cir_buffer) ||
        result->anchor_full_cir_total_len == 0u) {
        return -EMSGSIZE;
    }

    key = k_spin_lock(&anchor_cir_report_lock);
    anchor_cir_report_stream.clicker_id = clicker_id;
    anchor_cir_report_stream.anchor_id = result->responder_id;
    anchor_cir_report_stream.timestamp_ms = timestamp_ms;
    anchor_cir_report_stream.event_seq = event_seq;
    anchor_cir_report_stream.captured_len = result->anchor_full_cir_len;
    anchor_cir_report_stream.total_len = result->anchor_full_cir_total_len;
    anchor_cir_report_stream.first_path_index =
        result->anchor_full_cir_first_path_index;
    anchor_cir_report_stream.start_index = result->anchor_full_cir_start_index;
    anchor_cir_report_stream.next_offset = 0u;
    anchor_cir_report_stream.next_fragment_index = 0u;
    anchor_cir_report_stream.fragment_count =
        (uint16_t)((result->anchor_full_cir_len +
                    UWB_FULL_CIR_REPORT_PACKET_BYTES - 1u) /
                   UWB_FULL_CIR_REPORT_PACKET_BYTES);
    anchor_cir_report_stream.next_seq = next_seq == 0u ? 1u : next_seq;
    anchor_cir_report_stream.active = true;
    k_spin_unlock(&anchor_cir_report_lock, key);

    return anchor_cir_report_queue_next();
}
#endif

int app_mesh_report_encode_queue_next_cir(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    return anchor_cir_report_queue_next();
#else
    return -ENOENT;
#endif
}

static int build_range_report_samples(uint64_t clicker_id,
                                      uint32_t event_seq,
                                      uint8_t attempt_index,
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
    int64_t persistence_deadline_ms;
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
    persistence_deadline_ms =
        k_uptime_get() + ANCHOR_RANGE_REPORT_PERSISTENCE_DEADLINE_MS;
    ret = mesh_range_report_batch_reserve(clicker_id,
                                          event_seq,
                                          attempt_index);
    if (ret < 0) {
        return ret;
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
            range_local_ms = sample_sequence_start_ms[sample_index];
            if (range_local_ms < 0) {
                range_local_ms = k_uptime_get();
            }
            for (uint16_t i = 0u; i < chunk_count; i++) {
                int64_t sample_local_ms = sample_sequence_start_ms[sample_index + i];

                if (sample_local_ms < 0) {
                    sample_local_ms = k_uptime_get();
                }
                anchor_sequence_timestamp_at(sample_local_ms,
                                             &sequence_start_timestamps_ms[i]);
            }
            fields.timestamp_ms = sequence_start_timestamps_ms[0];
        } else {
            range_local_ms = range_result->exchange_started ?
                             range_result->exchange_start_ms :
                             k_uptime_get();
            anchor_sequence_timestamp_at(range_local_ms,
                                         &fields.timestamp_ms);
        }
        if (range_result->click_timestamp_present) {
            anchor_sequence_timestamp_at(range_result->click_timestamp_ms,
                                         &fields.timestamp_ms);
        }

        fields.clicker_id = clicker_id;
        fields.anchor_id = range_result->responder_id;
        fields.event_seq = event_seq;
        fields.attempt_index = attempt_index;
        fields.detection_source = DETECTION_SOURCE_UWB_WAKE_CLAIM;
        fields.detection_attempt_present = attempt_index != 0u;
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
        fields.burst_id = burst_id;
        fields.burst_id_present =
            (range_result->flags & FLAG_COUNT_AS_CLICK) != 0u;
        fields.omit_rsl = packet_index != 0u;
        fields.omit_cir = packet_index != 0u;
        fragmented = sample_count > chunk_count || packet_index != 0u;
        if (packet_index == 0u) {
            uint32_t anchor_diag_len = range_result->anchor_full_cir_sampled ?
                                       range_result->anchor_full_cir_len :
                                       range_result->cir_sampled ?
                                       UWB_CIR_SAMPLE_LEN : 0u;
            uint32_t anchor_diag_truncated =
                range_result->anchor_full_cir_total_len >
                range_result->anchor_full_cir_len ?
                range_result->anchor_full_cir_total_len -
                range_result->anchor_full_cir_len : 0u;
            uint16_t cir_fragment_count = range_result->anchor_full_cir_sampled ?
                (uint16_t)((range_result->anchor_full_cir_len +
                            UWB_FULL_CIR_REPORT_PACKET_BYTES - 1u) /
                           UWB_FULL_CIR_REPORT_PACKET_BYTES) : 0u;
            uint32_t clicker_diag_len = range_result->clicker_diag_received ?
                                        range_result->clicker_diag_len : 0u;
            uint32_t clicker_diag_copy_len = clicker_diag_len > 15u ?
                                             clicker_diag_len - 15u : 0u;
            uint32_t clicker_diag_raw_len =
                (range_result->clicker_diag_received && clicker_diag_len >= 15u) ?
                range_result->clicker_diag[14] : clicker_diag_copy_len;

            memset(&diagnostics, 0, sizeof(diagnostics));
            diagnostics.status_flags = (range_result->cir_sampled ||
                                        range_result->anchor_full_cir_sampled) ?
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
            diagnostics.diag_bytes_truncated = anchor_diag_truncated +
                (clicker_diag_raw_len > clicker_diag_copy_len ?
                 clicker_diag_raw_len - clicker_diag_copy_len : 0u);
            diagnostics.diag_frames_dropped = range_result->clicker_diag_dropped ?
                                              1u : 0u;
            diagnostics.report_fragment_count = cir_fragment_count +
                (fragmented ?
                 (uint16_t)(1u +
                            ((sample_count - chunk_count +
                              RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT - 1u) /
                             RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT)) :
                 1u);
            diagnostics.phy_config_id = UWB_CHANNEL_WAKE_CONTACT;
            diagnostics.clock_offset_raw = range_result->clock_offset_raw;
            diagnostics.clock_offset_present = range_result->clock_offset_sampled;
            diagnostics.clicker_clock_offset_raw =
                range_result->clicker_clock_offset_raw;
            diagnostics.clicker_clock_offset_present =
                range_result->clicker_clock_offset_sampled;
            diagnostics.carrier_integrator = range_result->carrier_integrator;
            diagnostics.carrier_integrator_present =
                range_result->carrier_integrator_sampled;
            diagnostics.clicker_diag = range_result->clicker_diag_received ?
                                       range_result->clicker_diag : NULL;
            diagnostics.clicker_diag_len = range_result->clicker_diag_received ?
                                           range_result->clicker_diag_len : 0u;
            diagnostics.anchor_diag = NULL;
            diagnostics.anchor_diag_len = 0u;
            fields.diagnostics = &diagnostics;
        }

        ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
        if (ret == PROTO_ERR_NO_SPACE && chunk_count > 1u) {
            chunk_count--;
            goto build_payload;
        }
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            goto fail;
        }

        ret = report_range_transport_seq(attempt_index,
                                         packet_index,
                                         &packet_seq);
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            goto fail;
        }
        ret = report_init_range_packet(&packet,
                                       range_result->responder_id,
                                       GATEWAY_ID,
                                       proto_click_report_session_id(
                                           clicker_id,
                                           event_seq),
                                       packet_seq,
                                       range_result->flags,
                                       (uint8_t)payload_len);
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            goto fail;
        }

        ret = proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len);
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            goto fail;
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
            uint16_t queue_depth;
            bool final_fragment =
                sample_index + chunk_count >= sample_count;

            memcpy(outbound.payload, payload, payload_len);
            ret = queue_anchor_range_report_fragment(&outbound,
                                                     clicker_id,
                                                     event_seq,
                                                     attempt_index,
                                                     final_fragment,
                                                     persistence_deadline_ms);
            if (ret < 0) {
                LOG_WRN("range report batch fragment could not be queued for mesh TX: %d",
                        ret);
                goto fail;
            }
            queue_depth = (uint16_t)report_tx_queue_used();
            app_stack_workload_diag_cir_admit(&packet, queue_depth, queue_depth);
            app_stack_workload_diag_cir_sample(&packet, queue_depth, queue_depth);
        }

        sample_index += chunk_count;
        packet_index++;
    } while (sample_index < sample_count);

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (range_result->anchor_full_cir_sampled) {
        uint64_t cir_timestamp_ms = 0u;
        uint16_t cir_seq;
        int64_t cir_local_ms = range_result->exchange_started ?
                               range_result->exchange_start_ms :
                               k_uptime_get();

        ret = report_range_transport_seq(attempt_index,
                                         packet_index,
                                         &cir_seq);
        if (ret != PROTO_OK) {
            ret = -EINVAL;
            goto fail;
        }
        anchor_sequence_timestamp_at(cir_local_ms, &cir_timestamp_ms);
        ret = anchor_cir_report_start(clicker_id,
                                      event_seq,
                                      cir_timestamp_ms,
                                      cir_seq,
                                      range_result);
        if (ret < 0) {
            goto fail;
        }
    }
#endif

    return 0;

fail:
    mesh_range_report_batch_abort(clicker_id, event_seq, attempt_index);
    return ret;
}

int build_uwb_schedule_report_if_relevant(
    const struct uwb_anchor_session *session,
    uint8_t schedule_flags,
    const struct anchor_range_window_report *report)
{
    int ret;

    if ((schedule_flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) == 0u) {
        return 0;
    }
    if (session == NULL || report == NULL || !report->have_result) {
        return 0;
    }

    ret = build_range_report_samples(session->epoch.clicker_id,
                                     session->epoch.click_event_id,
                                     session->epoch.attempt_index,
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
    return ret;
}
