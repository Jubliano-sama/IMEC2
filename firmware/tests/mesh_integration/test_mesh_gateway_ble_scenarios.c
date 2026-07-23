#include "app_gateway_ble_stream.h"
#include "gateway_ble_transport.h"
#include "mesh_sim.h"
#include "report.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MESH_SEED UINT32_C(0x6d657368)
#define TEST_CIR_SEED UINT32_C(0x43495231)
#define TEST_ROUTE_EPOCH UINT32_C(23)
#define TEST_EVENT_SEQ UINT32_C(0x10203040)
#define TEST_BATCH_ID UINT32_C(0x55667788)
#define TEST_BATCH_FLAG_FINAL 0x01u
#define TEST_ANCHOR_ID UINT64_C(0xa100000000000001)
#define TEST_CLICKER_ID UINT64_C(0xc100000000000002)
#define TEST_GATEWAY_ID UINT64_C(0x9000000000000003)
#define TEST_TIMESTAMP_MS UINT64_C(0x0000018f12345678)
#define TEST_FIRST_PATH_INDEX UINT16_C(93)
#define TEST_CIR_START_INDEX UINT16_C(64)
#define TEST_BLE_MTU UINT16_C(247)
#define TEST_BLE_CREDIT_CAPACITY 1u
#define TEST_MESH_EVENT_WINDOW_MS 30u
#define TEST_MAX_MESH_EVENTS 24u
#define TEST_MAX_BLE_RECORDS 3u
#define TEST_MAX_CHUNKS_PER_RECORD 16u
#define TEST_MAX_TIMELINE_STEPS 128u
#define TEST_BACKPRESSURE_HOLD_US UINT64_C(150000)
#define TEST_DISCONNECT_HOLD_US UINT64_C(220000)
#define TEST_WATCHDOG_TIMEOUT_US UINT64_C(350000)
#define TEST_MAX_GATEWAY_DELIVERY_LATENCY_US UINT64_C(650000)
#define TEST_MAX_SCENARIO_TIME_US UINT64_C(1200000)
#define TEST_MAX_GATEWAY_MESH_QUEUE_DEPTH 1u

enum test_record_index {
    TEST_RECORD_RANGE = 0,
    TEST_RECORD_CIR_FIRST = 1,
    TEST_RECORD_CIR_REMAINDER = 2,
    TEST_RECORD_COUNT = 3,
};

struct expected_record {
    struct proto_packet packet;
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t payload_len;
    uint32_t stream_received_at_ms;
    uint32_t stream_queued_at_ms;
};

struct shared_timeline_state {
    struct gateway_ble_tx_cursor cursor;
    const uint8_t *record;
    const uint8_t *chunk;
    size_t record_len;
    size_t chunk_len;
    uint8_t retry_copy[GATEWAY_BLE_ATT_MAX_MTU];
    size_t retry_len;
    size_t retry_offset;
    uint64_t mesh_action_end_us;
    uint64_t backpressure_release_us;
    uint64_t reconnect_at_us;
    uint64_t first_ble_submit_us;
    uint64_t first_ble_completion_us;
    uint64_t last_ble_completion_us;
    uint64_t mesh_quiesced_at_us;
    uint64_t disconnect_at_us;
    uint64_t reconnect_completed_at_us;
    size_t deliveries_enqueued;
    size_t records_retired;
    size_t timeline_steps;
    size_t max_anchor_mesh_queue;
    size_t max_gateway_mesh_queue;
    uint32_t mesh_events_credit_exhausted;
    uint32_t mesh_events_disconnected;
    uint32_t deliveries_while_ble_blocked;
    uint32_t backpressure_events;
    uint32_t cir_credit_starvation_events;
    uint32_t credit_completion_events;
    uint8_t max_stream_depth;
    uint8_t record_retire_count[TEST_RECORD_COUNT];
    bool mesh_action_scheduled;
    bool record_active;
    bool chunk_submitted;
    bool backpressure_active;
    bool backpressure_exercised;
    bool cir_credit_starved;
    bool disconnect_exercised;
    bool reconnect_pending;
    bool retry_verified;
};

static struct mesh_sim_world test_world;
static struct gateway_ble_stream_state test_stream;
static struct gateway_ble_link test_link;
static struct shared_timeline_state timeline;
static struct expected_record expected_records[TEST_RECORD_COUNT];
static uint8_t cir_bytes[RANGE_REPORT_CIR_WINDOW_RAW_BYTES];
static uint8_t gui_stream_bytes[GATEWAY_BLE_STREAM_RECORD_POOL_BYTES];
static size_t gui_stream_len;
static uint8_t anchor_index = UINT8_MAX;
static uint8_t gateway_index = UINT8_MAX;
static uint16_t mesh_connection_index = UINT16_MAX;
static size_t mesh_events_run;
static size_t ble_events_run;
static size_t ble_chunks_completed;
static size_t current_record_index;
static size_t current_chunk_index;
static const char *test_phase = "startup";

static void fail_at(int line, const char *condition, const char *format, ...)
{
    va_list args;
    size_t gateway_deliveries = 0u;
    size_t stream_depth = 0u;

    if (gateway_index < test_world.role_count) {
        gateway_deliveries = test_world.roles[gateway_index].delivery_count;
    }
    stream_depth = gateway_ble_stream_depth(&test_stream);
    fprintf(stderr,
            "FAIL phase=%s line=%d check=(%s) mesh_seed=0x%08x "
            "cir_seed=0x%08x now_us=%llu mesh_error=%d mesh_events=%zu "
            "deliveries=%zu stream_depth=%zu record=%zu chunk=%zu "
            "gui_bytes=%zu ble_events=%zu: ",
            test_phase,
            line,
            condition,
            TEST_MESH_SEED,
            TEST_CIR_SEED,
            (unsigned long long)test_world.now_us,
            test_world.last_error,
            mesh_events_run,
            gateway_deliveries,
            stream_depth,
            current_record_index,
            current_chunk_index,
            gui_stream_len,
            ble_events_run);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

#define CHECK(condition, ...) \
    do { \
        if (!(condition)) { \
            fail_at(__LINE__, #condition, __VA_ARGS__); \
        } \
    } while (0)

static void fill_cir_bytes(void)
{
    uint32_t state = TEST_CIR_SEED;

    for (size_t i = 0u; i < sizeof(cir_bytes); i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        cir_bytes[i] = (uint8_t)(state >> 24);
    }
}

static void append_batch_metadata(uint8_t *payload,
                                  size_t payload_cap,
                                  size_t *payload_len,
                                  bool final_packet)
{
    int ret;

    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CH9_BATCH_ID,
                         TEST_BATCH_ID);
    CHECK(ret == PROTO_OK,
          "batch ID append failed: ret=%d len=%zu cap=%zu",
          ret,
          *payload_len,
          payload_cap);
    ret = tlv_append_u8(payload,
                        payload_cap,
                        payload_len,
                        TLV_MESH_CH9_BATCH_FLAGS,
                        final_packet ? TEST_BATCH_FLAG_FINAL : 0u);
    CHECK(ret == PROTO_OK,
          "batch flags append failed: ret=%d len=%zu cap=%zu",
          ret,
          *payload_len,
          payload_cap);
}

static void build_range_record(void)
{
    static const int32_t distance_samples[] = {4182, 4175, 4191};
    static const uint8_t round_indices[] = {2u, 5u, 9u};
    static const uint64_t sequence_timestamps[] = {
        TEST_TIMESTAMP_MS,
        TEST_TIMESTAMP_MS + 17u,
        TEST_TIMESTAMP_MS + 34u,
    };
    static const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {
        0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u,
    };
    struct range_report_diagnostics diagnostics = {
        .status_flags = RANGE_DIAG_CLICKER_PRESENT |
                        RANGE_DIAG_ANCHOR_PRESENT |
                        RANGE_DIAG_CHANNEL9_DELIVERED,
        .burst_id = UINT32_C(0x2468ace0),
        .exchange_stride_us = 1750u,
        .burst_duration_ms = 43u,
        .click_latency_ms = 27u,
        .uwb_awake_time_us = 31415u,
        .diag_bytes_captured = RANGE_REPORT_CIR_WINDOW_RAW_BYTES,
        .diag_bytes_transmitted = RANGE_REPORT_CIR_WINDOW_RAW_BYTES,
        .diag_bytes_truncated = 0u,
        .diag_frames_dropped = 0u,
        .report_fragment_count = TEST_RECORD_COUNT,
        .channel9_report_latency_ms = 61u,
        .gateway_ack_latency_ms = 94u,
        .phy_config_id = 9u,
        .clock_offset_raw = -321,
        .clicker_clock_offset_raw = 222,
        .carrier_integrator = -7654321,
        .click_latency_present = true,
        .channel9_report_latency_present = true,
        .gateway_ack_latency_present = true,
        .clock_offset_present = true,
        .clicker_clock_offset_present = true,
        .carrier_integrator_present = true,
    };
    struct range_report_fields fields = {
        .clicker_id = TEST_CLICKER_ID,
        .anchor_id = TEST_ANCHOR_ID,
        .event_seq = TEST_EVENT_SEQ,
        .timestamp_ms = TEST_TIMESTAMP_MS,
        .distance_mm = 4182,
        .quality = 97u,
        .rsl_dbm = -73,
        .cir_sample = cir_sample,
        .range_status = RANGE_OK,
        .distance_samples_mm = distance_samples,
        .range_round_indices = round_indices,
        .sequence_start_timestamps_ms = sequence_timestamps,
        .sample_index = 0u,
        .sample_count = 3u,
        .distance_sample_count = 3u,
        .diagnostics = &diagnostics,
    };
    struct expected_record *record = &expected_records[TEST_RECORD_RANGE];
    int ret;

    ret = report_append_range_tlvs(record->payload,
                                   PACKET_MAX_PAYLOAD_LEN,
                                   &record->payload_len,
                                   &fields);
    CHECK(ret == PROTO_OK,
          "range TLV build failed: ret=%d len=%zu",
          ret,
          record->payload_len);
    append_batch_metadata(record->payload,
                          PACKET_MAX_PAYLOAD_LEN,
                          &record->payload_len,
                          false);
    CHECK(record->payload_len <= PACKET_MAX_PAYLOAD_LEN,
          "range payload exceeded standard packet: len=%zu max=%u",
          record->payload_len,
          PACKET_MAX_PAYLOAD_LEN);
    ret = report_init_range_packet(&record->packet,
                                   TEST_ANCHOR_ID,
                                   TEST_GATEWAY_ID,
                                   TEST_EVENT_SEQ,
                                   UINT16_C(0x2101),
                                   FLAG_COUNT_AS_CLICK,
                                   (uint16_t)record->payload_len);
    CHECK(ret == PROTO_OK, "range packet init failed: ret=%d", ret);
}

static void build_cir_record(enum test_record_index record_index,
                             uint16_t byte_offset,
                             uint16_t chunk_len,
                             bool final_packet)
{
    struct range_report_cir_fragment fragment = {
        .clicker_id = TEST_CLICKER_ID,
        .anchor_id = TEST_ANCHOR_ID,
        .event_seq = TEST_EVENT_SEQ,
        .timestamp_ms = TEST_TIMESTAMP_MS,
        .fragment_index = (uint16_t)(record_index - TEST_RECORD_CIR_FIRST),
        .fragment_count = 2u,
        .byte_offset = byte_offset,
        .total_bytes = RANGE_REPORT_CIR_WINDOW_RAW_BYTES,
        .first_path_index = TEST_FIRST_PATH_INDEX,
        .start_index = TEST_CIR_START_INDEX,
        .chunk = &cir_bytes[byte_offset],
        .chunk_len = chunk_len,
    };
    struct expected_record *record = &expected_records[record_index];
    int ret;

    ret = report_append_cir_fragment_tlvs(record->payload,
                                          sizeof(record->payload),
                                          &record->payload_len,
                                          &fragment);
    CHECK(ret == PROTO_OK,
          "CIR fragment build failed: record=%u ret=%d offset=%u raw=%u",
          (unsigned int)record_index,
          ret,
          byte_offset,
          chunk_len);
    append_batch_metadata(record->payload,
                          sizeof(record->payload),
                          &record->payload_len,
                          final_packet);
    ret = report_init_range_packet(
        &record->packet,
        TEST_ANCHOR_ID,
        TEST_GATEWAY_ID,
        TEST_EVENT_SEQ,
        (uint16_t)(UINT16_C(0x2101) + (uint16_t)record_index),
        FLAG_DIAGNOSTIC,
        (uint16_t)record->payload_len);
    CHECK(ret == PROTO_OK,
          "CIR packet init failed: record=%u ret=%d payload=%zu",
          (unsigned int)record_index,
          ret,
          record->payload_len);
}

static void build_expected_records(void)
{
    test_phase = "build-records";
    memset(expected_records, 0, sizeof(expected_records));
    fill_cir_bytes();
    build_range_record();
    build_cir_record(TEST_RECORD_CIR_FIRST,
                     0u,
                     RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES,
                     false);
    build_cir_record(TEST_RECORD_CIR_REMAINDER,
                     RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES,
                     RANGE_REPORT_CIR_REMAINDER_RAW_BYTES,
                     true);

    CHECK(RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES == 881u,
          "production first CIR raw chunk changed: actual=%u expected=881",
          RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES);
    CHECK(RANGE_REPORT_CIR_REMAINDER_RAW_BYTES == 271u,
          "production CIR remainder changed: actual=%u expected=271",
          RANGE_REPORT_CIR_REMAINDER_RAW_BYTES);
    CHECK(expected_records[TEST_RECORD_CIR_FIRST].payload_len ==
              PACKET_EXT_MAX_PAYLOAD_LEN,
          "first CIR payload must exactly fill extended packet: actual=%zu expected=%u",
          expected_records[TEST_RECORD_CIR_FIRST].payload_len,
          PACKET_EXT_MAX_PAYLOAD_LEN);
    CHECK(expected_records[TEST_RECORD_CIR_REMAINDER].payload_len ==
              RANGE_REPORT_CIR_REMAINDER_PAYLOAD_BYTES +
                  MESH_CH9_BATCH_METADATA_TLV_BYTES,
          "CIR remainder payload mismatch: actual=%zu expected=%u",
          expected_records[TEST_RECORD_CIR_REMAINDER].payload_len,
          RANGE_REPORT_CIR_REMAINDER_PAYLOAD_BYTES +
              MESH_CH9_BATCH_METADATA_TLV_BYTES);
}

static struct mesh_event_params connection_params(void)
{
    return (struct mesh_event_params) {
        .event_interval_ms = 100u,
        .event_window_ms = TEST_MESH_EVENT_WINDOW_MS,
        .first_event_time_ms = 100u,
        .guard_ms = 4u,
        .peer_clock_skew_estimate_ppm = 20,
        .max_missed_events = 3u,
        .supervision_timeout_ms = 20000u,
    };
}

static bool mesh_delivery_complete(void)
{
    const struct mesh_sim_role_instance *anchor;
    const struct mesh_sim_role_instance *gateway;

    if (anchor_index >= test_world.role_count ||
        gateway_index >= test_world.role_count) {
        return false;
    }
    anchor = &test_world.roles[anchor_index];
    gateway = &test_world.roles[gateway_index];
    return gateway->delivery_count == TEST_RECORD_COUNT &&
           anchor->tx_queue_count == 0u &&
           gateway->tx_queue_count == 0u &&
           anchor->relay.pending.state == MESH_RELAY_TX_IDLE;
}

static void observe_queue_depths(void)
{
    size_t anchor_depth;
    size_t gateway_depth;
    uint8_t stream_depth;

    CHECK(anchor_index < test_world.role_count &&
              gateway_index < test_world.role_count,
          "queue observation before role setup: anchor=%u gateway=%u roles=%zu",
          anchor_index,
          gateway_index,
          test_world.role_count);
    anchor_depth = test_world.roles[anchor_index].tx_queue_count;
    gateway_depth = test_world.roles[gateway_index].tx_queue_count;
    stream_depth = gateway_ble_stream_depth(&test_stream);
    if (anchor_depth > timeline.max_anchor_mesh_queue) {
        timeline.max_anchor_mesh_queue = anchor_depth;
    }
    if (gateway_depth > timeline.max_gateway_mesh_queue) {
        timeline.max_gateway_mesh_queue = gateway_depth;
    }
    if (stream_depth > timeline.max_stream_depth) {
        timeline.max_stream_depth = stream_depth;
    }
}

static void setup_shared_scenario(void)
{
    struct mesh_event_params params = connection_params();
    int ret;

    test_phase = "shared-setup";
    memset(&timeline, 0, sizeof(timeline));
    memset(&test_stream, 0, sizeof(test_stream));
    memset(&test_link, 0, sizeof(test_link));
    memset(gui_stream_bytes, 0, sizeof(gui_stream_bytes));
    anchor_index = UINT8_MAX;
    gateway_index = UINT8_MAX;
    mesh_connection_index = UINT16_MAX;
    mesh_events_run = 0u;
    ble_events_run = 0u;
    ble_chunks_completed = 0u;
    gui_stream_len = 0u;
    current_record_index = 0u;
    current_chunk_index = 0u;
    mesh_sim_init(&test_world, TEST_MESH_SEED);
    CHECK((uint64_t)test_world.channel9_tx_offset_us +
              mesh_sim_frame_duration_us(
                  MESH_SIM_PHY_CHANNEL9_MESH,
                  proto_packet_encoded_len(PACKET_EXT_MAX_PAYLOAD_LEN)) <=
              (uint64_t)TEST_MESH_EVENT_WINDOW_MS * 1000u,
          "maximum CIR frame does not fit mesh event: offset=%u airtime=%u "
          "window_ms=%u",
          test_world.channel9_tx_offset_us,
          mesh_sim_frame_duration_us(
              MESH_SIM_PHY_CHANNEL9_MESH,
              proto_packet_encoded_len(PACKET_EXT_MAX_PAYLOAD_LEN)),
          TEST_MESH_EVENT_WINDOW_MS);
    gateway_ble_stream_init(&test_stream);
    gateway_ble_link_init(&test_link,
                          GATEWAY_BLE_DEFAULT_CONNECTION_INTERVAL_US,
                          TEST_BLE_CREDIT_CAPACITY);
    ret = gateway_ble_link_connect(&test_link,
                                   test_world.now_us,
                                   TEST_BLE_MTU,
                                   true);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "initial BLE link connect failed: ret=%d now=%llu",
          ret,
          (unsigned long long)test_world.now_us);
    CHECK(gateway_ble_att_payload_max(TEST_BLE_MTU) == 244u,
          "unexpected ATT payload max: mtu=%u payload=%u",
          TEST_BLE_MTU,
          gateway_ble_att_payload_max(TEST_BLE_MTU));
    ret = mesh_sim_add_role(&test_world,
                            MESH_SIM_ROLE_ANCHOR,
                            TEST_ANCHOR_ID,
                            TEST_GATEWAY_ID,
                            TEST_ROUTE_EPOCH,
                            &anchor_index);
    CHECK(ret == MESH_SIM_OK, "anchor add failed: ret=%d", ret);
    ret = mesh_sim_add_role(&test_world,
                            MESH_SIM_ROLE_GATEWAY,
                            TEST_GATEWAY_ID,
                            TEST_GATEWAY_ID,
                            TEST_ROUTE_EPOCH,
                            &gateway_index);
    CHECK(ret == MESH_SIM_OK, "gateway add failed: ret=%d", ret);
    ret = mesh_sim_set_link(&test_world,
                            anchor_index,
                            gateway_index,
                            99u,
                            0u);
    CHECK(ret == MESH_SIM_OK, "mesh link setup failed: ret=%d", ret);
    ret = mesh_sim_install_route(&test_world,
                                 anchor_index,
                                 gateway_index,
                                 0u,
                                 TEST_ROUTE_EPOCH);
    CHECK(ret == PROTO_OK, "anchor route install failed: ret=%d", ret);
    ret = mesh_sim_add_connection(&test_world,
                                  anchor_index,
                                  gateway_index,
                                  &params,
                                  true,
                                  &mesh_connection_index);
    CHECK(ret == MESH_SIM_OK,
          "mesh connection add failed: ret=%d",
          ret);
    ret = mesh_sim_watchdog_arm(&test_world,
                                anchor_index,
                                TEST_WATCHDOG_TIMEOUT_US,
                                MESH_SIM_WATCHDOG_FAIL);
    CHECK(ret == MESH_SIM_OK,
          "anchor watchdog arm failed: ret=%d",
          ret);
    ret = mesh_sim_watchdog_arm(&test_world,
                                gateway_index,
                                TEST_WATCHDOG_TIMEOUT_US,
                                MESH_SIM_WATCHDOG_FAIL);
    CHECK(ret == MESH_SIM_OK,
          "gateway watchdog arm failed: ret=%d",
          ret);

    for (size_t i = 0u; i < TEST_RECORD_COUNT; i++) {
        ret = mesh_sim_queue_originated(&test_world,
                                        anchor_index,
                                        &expected_records[i].packet,
                                        expected_records[i].payload,
                                        expected_records[i].payload_len);
        CHECK(ret == MESH_SIM_OK,
              "originated queue failed: record=%zu seq=%u payload=%zu ret=%d",
              i,
              expected_records[i].packet.seq,
              expected_records[i].payload_len,
              ret);
    }
    observe_queue_depths();
}

static void schedule_next_mesh_action(void)
{
    struct mesh_sim_connection_action action;
    int ret;

    if (timeline.mesh_action_scheduled || mesh_delivery_complete()) {
        return;
    }
    CHECK(mesh_events_run < TEST_MAX_MESH_EVENTS,
          "mesh delivery did not quiesce within %u events: delivered=%zu "
          "anchor_queue=%zu gateway_queue=%zu anchor_pending=%d",
          TEST_MAX_MESH_EVENTS,
          test_world.roles[gateway_index].delivery_count,
          test_world.roles[anchor_index].tx_queue_count,
          test_world.roles[gateway_index].tx_queue_count,
          (int)test_world.roles[anchor_index].relay.pending.state);
    ret = mesh_sim_connection_next_action(&test_world,
                                          mesh_connection_index,
                                          &action);
    CHECK(ret == MESH_SIM_OK,
          "next mesh action failed: event=%zu ret=%d",
          mesh_events_run,
          ret);
    CHECK(action.kind == MESH_SIM_CONNECTION_ACTION_CHANNEL9_EVENT,
          "unexpected mesh action: event=%zu kind=%d start=%llu end=%llu",
          mesh_events_run,
          (int)action.kind,
          (unsigned long long)action.start_us,
          (unsigned long long)action.end_us);
    CHECK(!action.already_scheduled && action.skipped_events == 0u &&
              action.start_us >= test_world.now_us &&
              action.end_us > test_world.now_us,
          "invalid shared-timeline mesh action: event=%zu now=%llu start=%llu "
          "end=%llu scheduled=%u skipped=%u",
          mesh_events_run,
          (unsigned long long)test_world.now_us,
          (unsigned long long)action.start_us,
          (unsigned long long)action.end_us,
          action.already_scheduled ? 1u : 0u,
          action.skipped_events);
    ret = mesh_sim_schedule_next_connection_event(&test_world,
                                                  mesh_connection_index,
                                                  false);
    CHECK(ret == MESH_SIM_OK,
          "mesh event schedule failed: event=%zu ret=%d",
          mesh_events_run,
          ret);
    timeline.mesh_action_end_us = action.end_us;
    timeline.mesh_action_scheduled = true;
}

static bool packets_equal(const struct proto_packet *left,
                          const struct proto_packet *right)
{
    return left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->ttl == right->ttl &&
           left->payload_len == right->payload_len &&
           left->message_age_ms == right->message_age_ms;
}

static const struct mesh_sim_transmission *find_origin_transmission(
    const struct proto_packet *packet,
    size_t *match_count)
{
    const struct mesh_sim_transmission *match = NULL;

    *match_count = 0u;
    for (size_t i = 0u; i < test_world.transmission_count; i++) {
        const struct mesh_sim_transmission *tx = &test_world.transmissions[i];

        if (tx->valid && tx->has_outbound && tx->node_index == anchor_index &&
            tx->outbound.packet.src_id == packet->src_id &&
            tx->outbound.packet.session_id == packet->session_id &&
            tx->outbound.packet.seq == packet->seq &&
            tx->outbound.packet.msg_type == packet->msg_type) {
            match = tx;
            (*match_count)++;
        }
    }
    return match;
}

static void verify_mesh_deliveries(void)
{
    const struct mesh_sim_role_instance *gateway =
        &test_world.roles[gateway_index];
    uint64_t previous_tx_start = 0u;

    test_phase = "verify-mesh-deliveries";
    for (size_t i = 0u; i < TEST_RECORD_COUNT; i++) {
        const struct expected_record *expected = &expected_records[i];
        const struct mesh_sim_delivery *delivery = &gateway->deliveries[i];
        const struct mesh_sim_transmission *tx;
        struct proto_packet decoded = {0};
        const uint8_t *decoded_payload = NULL;
        size_t decoded_payload_len = 0u;
        size_t tx_matches = 0u;
        uint16_t stored_crc;
        uint16_t calculated_crc;
        int ret;

        current_record_index = i;
        CHECK(delivery->previous_hop_id == TEST_ANCHOR_ID,
              "delivery previous hop mismatch: actual=0x%016llx expected=0x%016llx",
              (unsigned long long)delivery->previous_hop_id,
              (unsigned long long)TEST_ANCHOR_ID);
        CHECK(delivery->packet.msg_type == expected->packet.msg_type &&
                  delivery->packet.flags == expected->packet.flags &&
                  delivery->packet.src_id == expected->packet.src_id &&
                  delivery->packet.dst_id == expected->packet.dst_id &&
                  delivery->packet.session_id == expected->packet.session_id &&
                  delivery->packet.seq == expected->packet.seq &&
                  delivery->packet.ttl == expected->packet.ttl &&
                  delivery->packet.payload_len == expected->packet.payload_len,
              "delivery packet metadata mismatch: seq=%u expected_seq=%u "
              "flags=0x%02x expected_flags=0x%02x ttl=%u expected_ttl=%u "
              "payload=%u expected_payload=%u",
              delivery->packet.seq,
              expected->packet.seq,
              delivery->packet.flags,
              expected->packet.flags,
              delivery->packet.ttl,
              expected->packet.ttl,
              delivery->packet.payload_len,
              expected->packet.payload_len);
        CHECK(delivery->payload_len == expected->payload_len,
              "mesh payload length mismatch: actual=%u expected=%zu",
              delivery->payload_len,
              expected->payload_len);
        CHECK(memcmp(delivery->payload,
                     expected->payload,
                     expected->payload_len) == 0,
              "mesh payload bytes changed: seq=%u len=%zu expected_crc=0x%04x "
              "actual_crc=0x%04x",
              expected->packet.seq,
              expected->payload_len,
              proto_crc16_ccitt_false(expected->payload, expected->payload_len),
              proto_crc16_ccitt_false(delivery->payload, delivery->payload_len));

        tx = find_origin_transmission(&expected->packet, &tx_matches);
        CHECK(tx != NULL && tx_matches == 1u,
              "origin transmission count mismatch: seq=%u matches=%zu total_tx=%zu",
              expected->packet.seq,
              tx_matches,
              test_world.transmission_count);
        CHECK(tx->start_us > previous_tx_start,
              "origin transmission order mismatch: seq=%u start=%llu previous=%llu",
              expected->packet.seq,
              (unsigned long long)tx->start_us,
              (unsigned long long)previous_tx_start);
        previous_tx_start = tx->start_us;
        CHECK(tx->frame_len == proto_packet_encoded_len(expected->payload_len),
              "wire frame length mismatch: actual=%u expected=%zu payload=%zu",
              tx->frame_len,
              proto_packet_encoded_len((uint16_t)expected->payload_len),
              expected->payload_len);
        stored_crc = proto_get_u16_le(&tx->frame[tx->frame_len - PACKET_CRC_LEN]);
        calculated_crc = proto_crc16_ccitt_false(tx->frame,
                                                 tx->frame_len - PACKET_CRC_LEN);
        CHECK(stored_crc == calculated_crc,
              "wire CRC mismatch: stored=0x%04x calculated=0x%04x seq=%u frame_len=%u",
              stored_crc,
              calculated_crc,
              expected->packet.seq,
              tx->frame_len);
        ret = proto_packet_decode(tx->frame,
                                  tx->frame_len,
                                  &decoded,
                                  &decoded_payload,
                                  &decoded_payload_len);
        CHECK(ret == PROTO_OK,
              "wire packet decode failed: seq=%u ret=%d",
              expected->packet.seq,
              ret);
        CHECK(packets_equal(&decoded, &tx->outbound.packet),
              "wire/outbound packet mismatch: seq=%u wire_age=%u outbound_age=%u",
              expected->packet.seq,
              decoded.message_age_ms,
              tx->outbound.packet.message_age_ms);
        CHECK(decoded_payload_len == expected->payload_len &&
                  memcmp(decoded_payload,
                         expected->payload,
                         expected->payload_len) == 0,
              "wire payload mismatch: seq=%u actual_len=%zu expected_len=%zu",
              expected->packet.seq,
              decoded_payload_len,
              expected->payload_len);
        CHECK(packets_equal(&delivery->packet, &decoded),
              "gateway delivery metadata differs from decoded wire packet: seq=%u",
              expected->packet.seq);
        CHECK(delivery->packet.message_age_ms ==
                  expected->packet.message_age_ms,
              "message age metadata changed: actual=%u expected=%u tx_start=%llu",
              delivery->packet.message_age_ms,
              expected->packet.message_age_ms,
              (unsigned long long)tx->start_us);
        CHECK(delivery->delivered_at_us == tx->end_us,
              "delivery timing mismatch: delivered=%llu tx_end=%llu",
              (unsigned long long)delivery->delivered_at_us,
              (unsigned long long)tx->end_us);
        CHECK(delivery->delivered_at_us <=
                  TEST_MAX_GATEWAY_DELIVERY_LATENCY_US,
              "gateway mesh latency exceeded bound: record=%zu delivered=%llu "
              "bound=%llu",
              i,
              (unsigned long long)delivery->delivered_at_us,
              (unsigned long long)TEST_MAX_GATEWAY_DELIVERY_LATENCY_US);
    }
}

static void enqueue_new_gateway_deliveries(void)
{
    const struct mesh_sim_role_instance *gateway =
        &test_world.roles[gateway_index];

    test_phase = "shared-gateway-enqueue";
    CHECK(gateway->delivery_count <= TEST_RECORD_COUNT,
          "gateway delivered duplicate records: actual=%zu expected_max=%u",
          gateway->delivery_count,
          TEST_RECORD_COUNT);
    while (timeline.deliveries_enqueued < gateway->delivery_count) {
        size_t i = timeline.deliveries_enqueued;
        const struct mesh_sim_delivery *delivery = &gateway->deliveries[i];
        uint32_t received_at_ms = (uint32_t)(delivery->delivered_at_us / 1000u);
        uint32_t now_ms = (uint32_t)(test_world.now_us / 1000u);
        uint8_t depth_before = gateway_ble_stream_depth(&test_stream);
        bool ble_ready = test_link.connected && test_link.notify_enabled;
        int ret;

        CHECK(delivery->delivered_at_us <= test_world.now_us,
              "delivery observed before simulator time: record=%zu delivered=%llu now=%llu",
              i,
              (unsigned long long)delivery->delivered_at_us,
              (unsigned long long)test_world.now_us);
        expected_records[i].stream_received_at_ms = received_at_ms;
        expected_records[i].stream_queued_at_ms = now_ms;
        ret = gateway_ble_stream_enqueue_packet(&test_stream,
                                                &delivery->packet,
                                                delivery->payload,
                                                delivery->payload_len,
                                                received_at_ms,
                                                now_ms,
                                                ble_ready);
        CHECK(ret == 1,
              "gateway stream enqueue failed: record=%zu seq=%u payload=%u ret=%d",
              i,
              delivery->packet.seq,
              delivery->payload_len,
              ret);
        CHECK(gateway_ble_stream_depth(&test_stream) == depth_before + 1u,
              "gateway stream depth mismatch after enqueue: actual=%u expected=%zu",
              gateway_ble_stream_depth(&test_stream),
              (size_t)depth_before + 1u);
        if (!test_link.connected || test_link.available_credits == 0u) {
            timeline.deliveries_while_ble_blocked++;
        }
        timeline.deliveries_enqueued++;
        observe_queue_depths();
    }
}

static void append_gui_chunk(const uint8_t *chunk, size_t chunk_len)
{
    CHECK(chunk != NULL && chunk_len > 0u,
          "invalid GUI chunk: ptr=%p len=%zu",
          (const void *)chunk,
          chunk_len);
    CHECK(gui_stream_len + chunk_len <= sizeof(gui_stream_bytes),
          "GUI stream buffer overflow: used=%zu chunk=%zu cap=%zu",
          gui_stream_len,
          chunk_len,
          sizeof(gui_stream_bytes));
    memcpy(&gui_stream_bytes[gui_stream_len], chunk, chunk_len);
    gui_stream_len += chunk_len;
}

static bool shared_timeline_complete(void)
{
    return mesh_delivery_complete() &&
           timeline.deliveries_enqueued == TEST_RECORD_COUNT &&
           timeline.records_retired == TEST_RECORD_COUNT &&
           gateway_ble_stream_depth(&test_stream) == 0u &&
           !timeline.record_active && !timeline.chunk_submitted &&
           !timeline.reconnect_pending;
}

static void retire_active_ble_record(void)
{
    uint8_t depth_before = gateway_ble_stream_depth(&test_stream);

    CHECK(timeline.record_active &&
              timeline.records_retired < TEST_RECORD_COUNT &&
              current_record_index == timeline.records_retired,
          "invalid record retirement: active=%u retired=%zu record=%zu",
          timeline.record_active ? 1u : 0u,
          timeline.records_retired,
          current_record_index);
    CHECK(gateway_ble_tx_cursor_done(&timeline.cursor),
          "record retired before cursor completion: record=%zu offset=%zu len=%zu",
          current_record_index,
          timeline.cursor.offset,
          timeline.cursor.frame_len);
    CHECK(depth_before > 0u,
          "record retirement with empty stream: record=%zu",
          current_record_index);

    gateway_ble_stream_mark_sent(&test_stream,
                                 (uint32_t)(test_world.now_us / 1000u));
    CHECK(gateway_ble_stream_depth(&test_stream) == depth_before - 1u,
          "stream record did not retire: before=%u after=%u record=%zu",
          depth_before,
          gateway_ble_stream_depth(&test_stream),
          current_record_index);
    timeline.record_retire_count[current_record_index]++;
    timeline.records_retired++;
    timeline.record_active = false;
    timeline.record = NULL;
    timeline.record_len = 0u;
    timeline.chunk = NULL;
    timeline.chunk_len = 0u;
    current_record_index = timeline.records_retired;
    current_chunk_index = 0u;
}

static void run_ble_connection_event(void)
{
    bool chunk_was_submitted = timeline.chunk_submitted;
    uint8_t completed = 0u;
    int ret;

    test_phase = "shared-ble-completion";
    CHECK(test_link.connected &&
              test_link.next_event_us == test_world.now_us,
          "BLE event is not on shared clock: connected=%u next=%llu now=%llu",
          test_link.connected ? 1u : 0u,
          (unsigned long long)test_link.next_event_us,
          (unsigned long long)test_world.now_us);
    ret = gateway_ble_link_run_connection_event(&test_link,
                                                test_world.now_us,
                                                &completed);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "BLE connection event failed: ret=%d now=%llu",
          ret,
          (unsigned long long)test_world.now_us);
    ble_events_run++;

    if (completed == 0u) {
        if (chunk_was_submitted) {
            CHECK(timeline.backpressure_active &&
                      test_link.central_stalled &&
                      test_link.available_credits == 0u &&
                      test_link.in_flight == 1u,
                  "in-flight chunk made no progress without modeled backpressure: "
                  "active=%u stalled=%u credits=%u inflight=%u",
                  timeline.backpressure_active ? 1u : 0u,
                  test_link.central_stalled ? 1u : 0u,
                  test_link.available_credits,
                  test_link.in_flight);
            ret = gateway_ble_link_try_notify(&test_link, 1u);
            CHECK(ret == GATEWAY_BLE_LINK_ERR_NO_CREDIT,
                  "credit exhaustion did not backpressure notify: ret=%d credits=%u",
                  ret,
                  test_link.available_credits);
            timeline.backpressure_events++;
            if (current_record_index >= TEST_RECORD_CIR_FIRST) {
                timeline.cir_credit_starved = true;
                timeline.cir_credit_starvation_events++;
            }
        } else {
            CHECK(test_link.available_credits == TEST_BLE_CREDIT_CAPACITY &&
                      test_link.in_flight == 0u,
                  "idle BLE event changed credits: credits=%u inflight=%u",
                  test_link.available_credits,
                  test_link.in_flight);
        }
        return;
    }

    CHECK(completed == 1u && chunk_was_submitted &&
              timeline.cursor.in_flight &&
              test_link.available_credits == TEST_BLE_CREDIT_CAPACITY &&
              test_link.in_flight == 0u,
          "BLE completion/credit mismatch: completed=%u submitted=%u "
          "cursor_inflight=%u credits=%u link_inflight=%u",
          completed,
          chunk_was_submitted ? 1u : 0u,
          timeline.cursor.in_flight ? 1u : 0u,
          test_link.available_credits,
          test_link.in_flight);
    append_gui_chunk(timeline.chunk, timeline.chunk_len);
    ret = gateway_ble_tx_cursor_complete(&timeline.cursor, true);
    CHECK(ret == PROTO_OK,
          "accepted BLE chunk did not complete cursor: ret=%d record=%zu chunk=%zu",
          ret,
          current_record_index,
          current_chunk_index);
    timeline.chunk_submitted = false;
    ble_chunks_completed++;
    timeline.credit_completion_events++;
    if (timeline.first_ble_completion_us == 0u) {
        timeline.first_ble_completion_us = test_world.now_us;
    }
    timeline.last_ble_completion_us = test_world.now_us;
    current_chunk_index++;
    if (gateway_ble_tx_cursor_done(&timeline.cursor)) {
        retire_active_ble_record();
    }
}

static void disconnect_active_ble_chunk(void)
{
    uint8_t dropped;
    int ret;

    test_phase = "shared-ble-disconnect";
    CHECK(timeline.chunk_submitted && timeline.cursor.in_flight &&
              timeline.chunk != NULL && timeline.chunk_len > 0u,
          "disconnect without an active BLE chunk");
    CHECK(timeline.chunk_len <= sizeof(timeline.retry_copy),
          "retry capture too large: len=%zu cap=%zu",
          timeline.chunk_len,
          sizeof(timeline.retry_copy));
    memcpy(timeline.retry_copy, timeline.chunk, timeline.chunk_len);
    timeline.retry_len = timeline.chunk_len;
    timeline.retry_offset = timeline.cursor.offset;

    dropped = gateway_ble_link_disconnect(&test_link);
    CHECK(dropped == 1u,
          "disconnect did not reject exactly one in-flight chunk: dropped=%u",
          dropped);
    CHECK(timeline.cursor.in_flight &&
              timeline.cursor.offset == timeline.retry_offset,
          "disconnect advanced or cleared cursor: offset=%zu expected=%zu inflight=%u",
          timeline.cursor.offset,
          timeline.retry_offset,
          timeline.cursor.in_flight ? 1u : 0u);
    ret = gateway_ble_tx_cursor_complete(&timeline.cursor, false);
    CHECK(ret == PROTO_OK &&
              timeline.cursor.offset == timeline.retry_offset &&
              !timeline.cursor.in_flight,
          "rejected chunk advanced cursor: ret=%d offset=%zu expected=%zu",
          ret,
          timeline.cursor.offset,
          timeline.retry_offset);

    timeline.chunk_submitted = false;
    timeline.disconnect_exercised = true;
    timeline.reconnect_pending = true;
    timeline.disconnect_at_us = test_world.now_us;
    timeline.reconnect_at_us = test_world.now_us + TEST_DISCONNECT_HOLD_US;
    CHECK(timeline.reconnect_at_us <= TEST_MAX_SCENARIO_TIME_US,
          "reconnect deadline exceeds scenario bound: reconnect=%llu",
          (unsigned long long)timeline.reconnect_at_us);
}

static void start_or_submit_ble_chunk(void)
{
    const uint8_t *chunk = NULL;
    size_t chunk_len = 0u;
    int ret;

    if (!test_link.connected || !test_link.notify_enabled ||
        timeline.chunk_submitted || test_link.available_credits == 0u) {
        return;
    }
    if (!timeline.record_active) {
        if (gateway_ble_stream_depth(&test_stream) == 0u) {
            return;
        }
        CHECK(timeline.records_retired < timeline.deliveries_enqueued &&
                  timeline.records_retired < TEST_RECORD_COUNT,
              "stream head has no matching delivery: retired=%zu enqueued=%zu depth=%u",
              timeline.records_retired,
              timeline.deliveries_enqueued,
              gateway_ble_stream_depth(&test_stream));
        current_record_index = timeline.records_retired;
        current_chunk_index = 0u;
        ret = gateway_ble_stream_begin_send_view(&test_stream,
                                                 &timeline.record,
                                                 &timeline.record_len);
        CHECK(ret == 0,
              "begin stream record failed: record=%zu depth=%u ret=%d",
              current_record_index,
              gateway_ble_stream_depth(&test_stream),
              ret);
        CHECK(timeline.record_len ==
                  GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                      expected_records[current_record_index].payload_len,
              "stream record length mismatch before ATT chunking: actual=%zu "
              "expected=%zu record=%zu",
              timeline.record_len,
              GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                  expected_records[current_record_index].payload_len,
              current_record_index);
        gateway_ble_tx_cursor_init(&timeline.cursor,
                                   timeline.record,
                                   timeline.record_len,
                                   test_link.negotiated_mtu);
        timeline.record_active = true;
    }

    CHECK(current_chunk_index < TEST_MAX_CHUNKS_PER_RECORD,
          "ATT cursor exceeded chunk bound: record=%zu chunk=%zu offset=%zu len=%zu",
          current_record_index,
          current_chunk_index,
          timeline.cursor.offset,
          timeline.cursor.frame_len);
    ret = gateway_ble_tx_cursor_begin(&timeline.cursor, &chunk, &chunk_len);
    CHECK(ret == PROTO_OK,
          "ATT cursor begin failed: record=%zu chunk=%zu offset=%zu ret=%d",
          current_record_index,
          current_chunk_index,
          timeline.cursor.offset,
          ret);
    CHECK(chunk == &timeline.record[timeline.cursor.offset] &&
              chunk_len > 0u &&
              chunk_len <= gateway_ble_att_payload_max(test_link.negotiated_mtu),
          "ATT chunk invalid: delta=%td offset=%zu len=%zu max=%u",
          chunk - timeline.record,
          timeline.cursor.offset,
          chunk_len,
          gateway_ble_att_payload_max(test_link.negotiated_mtu));
    ret = gateway_ble_link_try_notify(&test_link, chunk_len);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "controller notify submit failed: record=%zu chunk=%zu len=%zu "
          "credits=%u inflight=%u ret=%d",
          current_record_index,
          current_chunk_index,
          chunk_len,
          test_link.available_credits,
          test_link.in_flight,
          ret);
    CHECK(test_link.available_credits == 0u && test_link.in_flight == 1u,
          "controller credit was not consumed: credits=%u inflight=%u",
          test_link.available_credits,
          test_link.in_flight);
    timeline.chunk = chunk;
    timeline.chunk_len = chunk_len;
    timeline.chunk_submitted = true;

    if (timeline.first_ble_submit_us == 0u) {
        timeline.first_ble_submit_us = test_world.now_us;
    }
    if (!timeline.backpressure_active &&
        current_record_index == TEST_RECORD_CIR_FIRST &&
        current_chunk_index == 0u) {
        timeline.backpressure_active = true;
        timeline.backpressure_exercised = true;
        timeline.backpressure_release_us =
            test_world.now_us + TEST_BACKPRESSURE_HOLD_US;
        gateway_ble_link_set_stalled(&test_link, true);
    }
    if (!timeline.disconnect_exercised &&
        current_record_index == TEST_RECORD_RANGE &&
        current_chunk_index == 1u) {
        disconnect_active_ble_chunk();
    }
}

static void release_ble_backpressure(void)
{
    test_phase = "shared-ble-credit-release";
    CHECK(timeline.backpressure_active &&
              timeline.backpressure_release_us == test_world.now_us &&
              test_link.connected && test_link.central_stalled &&
              test_link.available_credits == 0u,
          "invalid backpressure release: active=%u release=%llu now=%llu "
          "connected=%u stalled=%u credits=%u",
          timeline.backpressure_active ? 1u : 0u,
          (unsigned long long)timeline.backpressure_release_us,
          (unsigned long long)test_world.now_us,
          test_link.connected ? 1u : 0u,
          test_link.central_stalled ? 1u : 0u,
          test_link.available_credits);
    gateway_ble_link_set_stalled(&test_link, false);
    timeline.backpressure_active = false;
    timeline.backpressure_release_us = 0u;
}

static void reconnect_and_retry_ble_chunk(void)
{
    const uint8_t *retry_chunk = NULL;
    size_t retry_len = 0u;
    int ret;

    test_phase = "shared-ble-reconnect";
    CHECK(timeline.reconnect_pending &&
              timeline.reconnect_at_us == test_world.now_us &&
              !test_link.connected && timeline.record_active &&
              !timeline.chunk_submitted,
          "invalid reconnect state: pending=%u at=%llu now=%llu connected=%u "
          "record_active=%u submitted=%u",
          timeline.reconnect_pending ? 1u : 0u,
          (unsigned long long)timeline.reconnect_at_us,
          (unsigned long long)test_world.now_us,
          test_link.connected ? 1u : 0u,
          timeline.record_active ? 1u : 0u,
          timeline.chunk_submitted ? 1u : 0u);
    ret = gateway_ble_link_connect(&test_link,
                                   test_world.now_us,
                                   TEST_BLE_MTU,
                                   true);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "BLE reconnect failed: ret=%d now=%llu",
          ret,
          (unsigned long long)test_world.now_us);
    ret = gateway_ble_tx_cursor_set_mtu(&timeline.cursor,
                                        test_link.negotiated_mtu);
    CHECK(ret == PROTO_OK,
          "cursor MTU update after reconnect failed: ret=%d",
          ret);
    ret = gateway_ble_tx_cursor_begin(&timeline.cursor,
                                      &retry_chunk,
                                      &retry_len);
    CHECK(ret == PROTO_OK,
          "retry cursor begin failed: ret=%d offset=%zu",
          ret,
          timeline.cursor.offset);
    CHECK(retry_chunk == timeline.chunk &&
              retry_len == timeline.retry_len &&
              timeline.cursor.offset == timeline.retry_offset &&
              memcmp(retry_chunk, timeline.retry_copy, retry_len) == 0,
          "disconnect retry changed chunk: pointer_same=%u retry_len=%zu "
          "original_len=%zu offset=%zu expected_offset=%zu",
          retry_chunk == timeline.chunk ? 1u : 0u,
          retry_len,
          timeline.retry_len,
          timeline.cursor.offset,
          timeline.retry_offset);
    ret = gateway_ble_link_try_notify(&test_link, retry_len);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "retry notify submit failed: ret=%d len=%zu",
          ret,
          retry_len);
    CHECK(test_link.available_credits == 0u && test_link.in_flight == 1u,
          "reconnect credit state mismatch: credits=%u inflight=%u",
          test_link.available_credits,
          test_link.in_flight);

    timeline.chunk = retry_chunk;
    timeline.chunk_len = retry_len;
    timeline.chunk_submitted = true;
    timeline.reconnect_pending = false;
    timeline.retry_verified = true;
    timeline.reconnect_completed_at_us = test_world.now_us;
}

static uint64_t next_shared_event_us(void)
{
    uint64_t next_us = UINT64_MAX;

    if (timeline.mesh_action_scheduled &&
        timeline.mesh_action_end_us < next_us) {
        next_us = timeline.mesh_action_end_us;
    }
    if (test_link.connected && test_link.next_event_us < next_us) {
        next_us = test_link.next_event_us;
    }
    if (timeline.backpressure_active &&
        timeline.backpressure_release_us < next_us) {
        next_us = timeline.backpressure_release_us;
    }
    if (timeline.reconnect_pending && timeline.reconnect_at_us < next_us) {
        next_us = timeline.reconnect_at_us;
    }

    CHECK(next_us != UINT64_MAX && next_us > test_world.now_us &&
              next_us <= TEST_MAX_SCENARIO_TIME_US,
          "shared timeline has no valid next event: now=%llu next=%llu "
          "mesh_scheduled=%u mesh_end=%llu connected=%u ble_next=%llu "
          "backpressure=%u release=%llu reconnect=%u reconnect_at=%llu",
          (unsigned long long)test_world.now_us,
          (unsigned long long)next_us,
          timeline.mesh_action_scheduled ? 1u : 0u,
          (unsigned long long)timeline.mesh_action_end_us,
          test_link.connected ? 1u : 0u,
          (unsigned long long)test_link.next_event_us,
          timeline.backpressure_active ? 1u : 0u,
          (unsigned long long)timeline.backpressure_release_us,
          timeline.reconnect_pending ? 1u : 0u,
          (unsigned long long)timeline.reconnect_at_us);
    return next_us;
}

static void advance_shared_timeline(uint64_t next_us)
{
    const struct mesh_sim_connection *connection =
        &test_world.connections[mesh_connection_index];
    uint32_t completed_before = connection->completed_events;
    bool credits_exhausted = test_link.connected &&
                             test_link.available_credits == 0u;
    bool disconnected = timeline.reconnect_pending && !test_link.connected;
    int ret;

    test_phase = "shared-timeline-advance";
    ret = mesh_sim_run_until(&test_world, next_us);
    CHECK(ret == MESH_SIM_OK,
          "mesh simulator advance failed: ret=%d from=%llu to=%llu",
          ret,
          (unsigned long long)test_world.now_us,
          (unsigned long long)next_us);
    CHECK(connection->completed_events >= completed_before,
          "mesh completed-event counter regressed: before=%u after=%u",
          completed_before,
          connection->completed_events);
    if (credits_exhausted) {
        timeline.mesh_events_credit_exhausted +=
            connection->completed_events - completed_before;
    }
    if (disconnected) {
        timeline.mesh_events_disconnected +=
            connection->completed_events - completed_before;
    }
    if (timeline.mesh_action_scheduled &&
        timeline.mesh_action_end_us == test_world.now_us) {
        timeline.mesh_action_scheduled = false;
        mesh_events_run++;
    }
    observe_queue_depths();
}

static void verify_shared_mesh_state(void)
{
    CHECK(mesh_delivery_complete(),
          "mesh delivery did not quiesce: delivered=%zu anchor_queue=%zu "
          "gateway_queue=%zu anchor_pending=%d",
          test_world.roles[gateway_index].delivery_count,
          test_world.roles[anchor_index].tx_queue_count,
          test_world.roles[gateway_index].tx_queue_count,
          (int)test_world.roles[anchor_index].relay.pending.state);
    CHECK(test_world.last_error == MESH_SIM_OK,
          "mesh simulator ended with error=%d",
          test_world.last_error);
    CHECK(test_world.roles[gateway_index].delivery_count == TEST_RECORD_COUNT &&
              timeline.deliveries_enqueued == TEST_RECORD_COUNT,
          "gateway delivery/enqueue count mismatch: delivered=%zu enqueued=%zu "
          "expected=%u",
          test_world.roles[gateway_index].delivery_count,
          timeline.deliveries_enqueued,
          TEST_RECORD_COUNT);
    CHECK(test_world.roles[gateway_index].partial_frames == 0u &&
              test_world.roles[gateway_index].collision_frames == 0u,
          "gateway radio loss: partial=%u collision=%u decoded=%u",
          test_world.roles[gateway_index].partial_frames,
          test_world.roles[gateway_index].collision_frames,
          test_world.roles[gateway_index].decoded_frames);
    CHECK(test_world.roles[anchor_index].partial_frames == 0u &&
          test_world.roles[anchor_index].collision_frames == 0u,
          "anchor ACK radio loss: partial=%u collision=%u decoded=%u",
          test_world.roles[anchor_index].partial_frames,
          test_world.roles[anchor_index].collision_frames,
          test_world.roles[anchor_index].decoded_frames);
    CHECK(test_world.roles[anchor_index].route_discovery_requests == 0u &&
              test_world.roles[gateway_index].route_discovery_requests == 0u,
          "unexpected route fallback: anchor=%u gateway=%u",
          test_world.roles[anchor_index].route_discovery_requests,
          test_world.roles[gateway_index].route_discovery_requests);
    CHECK(mesh_sim_count_transitions(&test_world,
                                     MESH_SIM_TRANSITION_PACKET_DELIVERED,
                                     TEST_GATEWAY_ID) == TEST_RECORD_COUNT,
          "gateway delivery transition count mismatch");
    CHECK(test_world.connections[mesh_connection_index].completed_events ==
              mesh_events_run,
          "mesh event accounting mismatch: connection=%u timeline=%zu",
          test_world.connections[mesh_connection_index].completed_events,
          mesh_events_run);
}

static void verify_adversarial_interleaving_guards(void)
{
    const struct mesh_sim_watchdog *anchor_watchdog =
        &test_world.roles[anchor_index].watchdog;
    const struct mesh_sim_watchdog *gateway_watchdog =
        &test_world.roles[gateway_index].watchdog;

    test_phase = "verify-adversarial-interleaving";
    CHECK(timeline.first_ble_submit_us >
              test_world.roles[gateway_index].deliveries[0].delivered_at_us &&
              timeline.first_ble_submit_us < timeline.mesh_quiesced_at_us,
          "sequential mesh-then-BLE mutation escaped: first_delivery=%llu "
          "first_ble_submit=%llu mesh_quiesced=%llu",
          (unsigned long long)
              test_world.roles[gateway_index].deliveries[0].delivered_at_us,
          (unsigned long long)timeline.first_ble_submit_us,
          (unsigned long long)timeline.mesh_quiesced_at_us);
    CHECK(timeline.disconnect_at_us > timeline.first_ble_submit_us &&
              timeline.disconnect_at_us < timeline.mesh_quiesced_at_us &&
              timeline.reconnect_completed_at_us > timeline.disconnect_at_us &&
              timeline.reconnect_completed_at_us < timeline.mesh_quiesced_at_us,
          "disconnect/reconnect did not interleave mesh: submit=%llu "
          "disconnect=%llu reconnect=%llu mesh_quiesced=%llu",
          (unsigned long long)timeline.first_ble_submit_us,
          (unsigned long long)timeline.disconnect_at_us,
          (unsigned long long)timeline.reconnect_completed_at_us,
          (unsigned long long)timeline.mesh_quiesced_at_us);
    CHECK(timeline.mesh_events_credit_exhausted > 0u,
          "BLE-blocks-mesh mutation escaped during exhausted credits");
    CHECK(timeline.mesh_events_disconnected > 0u,
          "BLE-blocks-mesh mutation escaped during disconnect");
    CHECK(timeline.deliveries_while_ble_blocked > 0u &&
              timeline.max_stream_depth >= 2u,
          "gateway ingress did not continue under BLE backpressure: "
          "blocked_deliveries=%u max_stream_depth=%u",
          timeline.deliveries_while_ble_blocked,
          timeline.max_stream_depth);
    CHECK(timeline.backpressure_exercised &&
              timeline.backpressure_events > 0u &&
              timeline.cir_credit_starved &&
              timeline.cir_credit_starvation_events > 0u &&
              timeline.retry_verified &&
              timeline.disconnect_exercised,
          "adversarial BLE paths missing: stalled=%u stall_events=%u "
          "cir_starved=%u cir_events=%u retry=%u disconnect=%u",
          timeline.backpressure_exercised ? 1u : 0u,
          timeline.backpressure_events,
          timeline.cir_credit_starved ? 1u : 0u,
          timeline.cir_credit_starvation_events,
          timeline.retry_verified ? 1u : 0u,
          timeline.disconnect_exercised ? 1u : 0u);
    CHECK(timeline.first_ble_completion_us > timeline.first_ble_submit_us &&
              timeline.last_ble_completion_us >
                  timeline.mesh_quiesced_at_us &&
              timeline.credit_completion_events == ble_chunks_completed,
          "BLE completion timeline mismatch: first_submit=%llu first_complete=%llu "
          "last_complete=%llu mesh_quiesced=%llu credit_events=%u chunks=%zu",
          (unsigned long long)timeline.first_ble_submit_us,
          (unsigned long long)timeline.first_ble_completion_us,
          (unsigned long long)timeline.last_ble_completion_us,
          (unsigned long long)timeline.mesh_quiesced_at_us,
          timeline.credit_completion_events,
          ble_chunks_completed);
    CHECK(timeline.max_anchor_mesh_queue <= TEST_RECORD_COUNT &&
              timeline.max_gateway_mesh_queue <=
                  TEST_MAX_GATEWAY_MESH_QUEUE_DEPTH &&
          timeline.max_stream_depth <= GATEWAY_BLE_STREAM_QUEUE_DEPTH,
          "queue bound exceeded: anchor=%zu gateway=%zu stream=%u",
          timeline.max_anchor_mesh_queue,
          timeline.max_gateway_mesh_queue,
          timeline.max_stream_depth);
    CHECK(anchor_watchdog->armed && !anchor_watchdog->expired &&
              anchor_watchdog->expirations == 0u &&
              anchor_watchdog->feeds == mesh_events_run &&
              anchor_watchdog->deadline_us > test_world.now_us,
          "anchor watchdog unhealthy: armed=%u expired=%u expirations=%u "
          "feeds=%u events=%zu deadline=%llu now=%llu",
          anchor_watchdog->armed ? 1u : 0u,
          anchor_watchdog->expired ? 1u : 0u,
          anchor_watchdog->expirations,
          anchor_watchdog->feeds,
          mesh_events_run,
          (unsigned long long)anchor_watchdog->deadline_us,
          (unsigned long long)test_world.now_us);
    CHECK(gateway_watchdog->armed && !gateway_watchdog->expired &&
              gateway_watchdog->expirations == 0u &&
              gateway_watchdog->feeds == mesh_events_run &&
              gateway_watchdog->deadline_us > test_world.now_us,
          "gateway watchdog unhealthy: armed=%u expired=%u expirations=%u "
          "feeds=%u events=%zu deadline=%llu now=%llu",
          gateway_watchdog->armed ? 1u : 0u,
          gateway_watchdog->expired ? 1u : 0u,
          gateway_watchdog->expirations,
          gateway_watchdog->feeds,
          mesh_events_run,
          (unsigned long long)gateway_watchdog->deadline_us,
          (unsigned long long)test_world.now_us);
    CHECK(mesh_sim_count_transitions(&test_world,
                                     MESH_SIM_TRANSITION_WATCHDOG_EXPIRED,
                                     0u) == 0u,
          "watchdog expiration occurred on shared timeline");

    for (size_t i = 0u; i < TEST_RECORD_COUNT; i++) {
        CHECK(timeline.record_retire_count[i] == 1u,
              "BLE record loss/duplicate: record=%zu retire_count=%u",
              i,
              timeline.record_retire_count[i]);
    }
}

static void run_shared_mesh_ble_timeline(void)
{
    test_phase = "shared-timeline";
    start_or_submit_ble_chunk();

    while (!shared_timeline_complete()) {
        uint64_t next_us;
        bool ble_event_due;

        CHECK(timeline.timeline_steps < TEST_MAX_TIMELINE_STEPS,
              "shared timeline did not quiesce within %u steps: now=%llu "
              "mesh_complete=%u deliveries=%zu retired=%zu stream=%u "
              "connected=%u reconnect=%u",
              TEST_MAX_TIMELINE_STEPS,
              (unsigned long long)test_world.now_us,
              mesh_delivery_complete() ? 1u : 0u,
              timeline.deliveries_enqueued,
              timeline.records_retired,
              gateway_ble_stream_depth(&test_stream),
              test_link.connected ? 1u : 0u,
              timeline.reconnect_pending ? 1u : 0u);
        schedule_next_mesh_action();
        next_us = next_shared_event_us();
        ble_event_due = test_link.connected &&
                        test_link.next_event_us == next_us;
        advance_shared_timeline(next_us);
        enqueue_new_gateway_deliveries();

        if (timeline.backpressure_active &&
            timeline.backpressure_release_us == test_world.now_us) {
            release_ble_backpressure();
        }
        if (timeline.reconnect_pending &&
            timeline.reconnect_at_us == test_world.now_us) {
            reconnect_and_retry_ble_chunk();
        }
        if (ble_event_due) {
            run_ble_connection_event();
        }
        start_or_submit_ble_chunk();
        observe_queue_depths();

        if (mesh_delivery_complete() &&
            timeline.mesh_quiesced_at_us == 0u) {
            timeline.mesh_quiesced_at_us = test_world.now_us;
        }
        timeline.timeline_steps++;
    }

    verify_shared_mesh_state();
    verify_adversarial_interleaving_guards();
    CHECK(gateway_ble_stream_depth(&test_stream) == 0u &&
              test_stream.pool_used == 0u && !test_stream.head_send_active,
          "stream did not drain cleanly: depth=%u pool=%u active=%u",
          gateway_ble_stream_depth(&test_stream),
          test_stream.pool_used,
          test_stream.head_send_active ? 1u : 0u);
    CHECK(test_link.connection_generation == 2u,
          "BLE connection generation mismatch: actual=%u expected=2",
          test_link.connection_generation);
    CHECK(test_link.notifications_dropped_disconnect == 1u,
          "expected one rejected in-flight notification at disconnect: actual=%u",
          test_link.notifications_dropped_disconnect);
    CHECK(test_link.notifications_submitted == ble_chunks_completed + 1u &&
              test_link.notifications_completed == ble_chunks_completed,
          "notification exact-once accounting mismatch: submitted=%u "
          "completed=%u chunks=%zu",
          test_link.notifications_submitted,
          test_link.notifications_completed,
          ble_chunks_completed);
}

static const uint8_t *expect_tlv(const uint8_t *payload,
                                 size_t payload_len,
                                 size_t *offset,
                                 uint8_t expected_type,
                                 uint8_t expected_len,
                                 const char *name)
{
    uint8_t actual_type;
    uint8_t actual_len;
    const uint8_t *value;

    CHECK(*offset <= payload_len && payload_len - *offset >= 2u,
          "TLV header missing: name=%s offset=%zu payload_len=%zu",
          name,
          *offset,
          payload_len);
    actual_type = payload[*offset];
    actual_len = payload[*offset + 1u];
    CHECK(actual_type == expected_type && actual_len == expected_len,
          "TLV order/length mismatch: name=%s offset=%zu actual_type=0x%02x "
          "expected_type=0x%02x actual_len=%u expected_len=%u",
          name,
          *offset,
          actual_type,
          expected_type,
          actual_len,
          expected_len);
    CHECK(payload_len - *offset - 2u >= actual_len,
          "TLV value truncated: name=%s offset=%zu len=%u remaining=%zu",
          name,
          *offset,
          actual_len,
          payload_len - *offset - 2u);
    value = &payload[*offset + 2u];
    *offset += (size_t)actual_len + 2u;
    return value;
}

static void expect_tlv_u8(const uint8_t *payload,
                          size_t payload_len,
                          size_t *offset,
                          uint8_t type,
                          uint8_t expected,
                          const char *name)
{
    const uint8_t *value = expect_tlv(payload,
                                      payload_len,
                                      offset,
                                      type,
                                      sizeof(uint8_t),
                                      name);

    CHECK(value[0] == expected,
          "TLV u8 mismatch: name=%s actual=%u expected=%u",
          name,
          value[0],
          expected);
}

static void expect_tlv_u16(const uint8_t *payload,
                           size_t payload_len,
                           size_t *offset,
                           uint8_t type,
                           uint16_t expected,
                           const char *name)
{
    const uint8_t *value = expect_tlv(payload,
                                      payload_len,
                                      offset,
                                      type,
                                      sizeof(uint16_t),
                                      name);
    uint16_t actual = proto_get_u16_le(value);

    CHECK(actual == expected,
          "TLV u16 mismatch: name=%s actual=%u expected=%u",
          name,
          actual,
          expected);
}

static void expect_tlv_u32(const uint8_t *payload,
                           size_t payload_len,
                           size_t *offset,
                           uint8_t type,
                           uint32_t expected,
                           const char *name)
{
    const uint8_t *value = expect_tlv(payload,
                                      payload_len,
                                      offset,
                                      type,
                                      sizeof(uint32_t),
                                      name);
    uint32_t actual = proto_get_u32_le(value);

    CHECK(actual == expected,
          "TLV u32 mismatch: name=%s actual=0x%08x expected=0x%08x",
          name,
          actual,
          expected);
}

static void expect_tlv_u64(const uint8_t *payload,
                           size_t payload_len,
                           size_t *offset,
                           uint8_t type,
                           uint64_t expected,
                           const char *name)
{
    const uint8_t *value = expect_tlv(payload,
                                      payload_len,
                                      offset,
                                      type,
                                      sizeof(uint64_t),
                                      name);
    uint64_t actual = proto_get_u64_le(value);

    CHECK(actual == expected,
          "TLV u64 mismatch: name=%s actual=0x%016llx expected=0x%016llx",
          name,
          (unsigned long long)actual,
          (unsigned long long)expected);
}

static void verify_range_tlvs(const uint8_t *payload, size_t payload_len)
{
    static const int32_t distance_samples[] = {4182, 4175, 4191};
    static const uint8_t round_indices[] = {2u, 5u, 9u};
    static const uint64_t sequence_timestamps[] = {
        TEST_TIMESTAMP_MS,
        TEST_TIMESTAMP_MS + 17u,
        TEST_TIMESTAMP_MS + 34u,
    };
    static const uint8_t cir_sample[UWB_CIR_SAMPLE_LEN] = {
        0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u,
    };
    size_t offset = 0u;
    const uint8_t *value;

    expect_tlv_u64(payload, payload_len, &offset,
                   TLV_CLICKER_ID, TEST_CLICKER_ID, "clicker-id");
    expect_tlv_u64(payload, payload_len, &offset,
                   TLV_ANCHOR_ID, TEST_ANCHOR_ID, "anchor-id");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_EVENT_SEQ, TEST_EVENT_SEQ, "event-seq");
    expect_tlv_u64(payload, payload_len, &offset,
                   TLV_TIMESTAMP_MS, TEST_TIMESTAMP_MS, "timestamp");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_DISTANCE_MM, (uint32_t)4182, "distance-mm");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_SAMPLE_COUNT, 3u, "sample-count");
    value = expect_tlv(payload, payload_len, &offset,
                       TLV_DISTANCE_SAMPLES_MM,
                       (uint8_t)sizeof(distance_samples),
                       "distance-samples");
    for (size_t i = 0u; i < 3u; i++) {
        CHECK((int32_t)proto_get_u32_le(&value[i * sizeof(uint32_t)]) ==
                  distance_samples[i],
              "distance sample mismatch: index=%zu actual=%d expected=%d",
              i,
              (int32_t)proto_get_u32_le(&value[i * sizeof(uint32_t)]),
              distance_samples[i]);
    }
    value = expect_tlv(payload, payload_len, &offset,
                       TLV_RANGE_ROUND_INDICES,
                       (uint8_t)sizeof(round_indices),
                       "round-indices");
    CHECK(memcmp(value, round_indices, sizeof(round_indices)) == 0,
          "range round indices changed");
    value = expect_tlv(payload, payload_len, &offset,
                       TLV_SEQUENCE_START_TIMESTAMPS_MS,
                       (uint8_t)sizeof(sequence_timestamps),
                       "sequence-timestamps");
    for (size_t i = 0u; i < 3u; i++) {
        CHECK(proto_get_u64_le(&value[i * sizeof(uint64_t)]) ==
                  sequence_timestamps[i],
              "sequence timestamp mismatch: index=%zu actual=0x%016llx "
              "expected=0x%016llx",
              i,
              (unsigned long long)proto_get_u64_le(
                  &value[i * sizeof(uint64_t)]),
              (unsigned long long)sequence_timestamps[i]);
    }
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_DIAG_STATUS_FLAGS,
                   RANGE_DIAG_CLICKER_PRESENT | RANGE_DIAG_ANCHOR_PRESENT |
                       RANGE_DIAG_CHANNEL9_DELIVERED,
                   "diag-status");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_BURST_ID, UINT32_C(0x2468ace0), "burst-id");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_EXCHANGE_STRIDE_US, 1750u, "exchange-stride");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_BURST_DURATION_MS, 43u, "burst-duration");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_CLICK_LATENCY_MS, 27u, "click-latency");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_UWB_AWAKE_TIME_US, 31415u, "uwb-awake-time");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_DIAG_BYTES_CAPTURED,
                   RANGE_REPORT_CIR_WINDOW_RAW_BYTES,
                   "diag-captured");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_DIAG_BYTES_TRANSMITTED,
                   RANGE_REPORT_CIR_WINDOW_RAW_BYTES,
                   "diag-transmitted");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_DIAG_BYTES_TRUNCATED, 0u, "diag-truncated");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_DIAG_FRAMES_DROPPED, 0u, "diag-dropped");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_REPORT_FRAGMENT_COUNT,
                   TEST_RECORD_COUNT,
                   "report-fragment-count");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_CHANNEL9_REPORT_LATENCY_MS,
                   61u,
                   "channel9-latency");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_GATEWAY_ACK_LATENCY_MS,
                   94u,
                   "gateway-ack-latency");
    expect_tlv_u8(payload, payload_len, &offset,
                  TLV_PHY_CONFIG_ID, 9u, "phy-config");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_UWB_CLOCK_OFFSET_RAW,
                   (uint16_t)-321,
                   "clock-offset");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_CLICKER_CLOCK_OFFSET_RAW,
                   222u,
                   "clicker-clock-offset");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_UWB_CARRIER_INTEGRATOR,
                   (uint32_t)-7654321,
                   "carrier-integrator");
    expect_tlv_u8(payload, payload_len, &offset,
                  TLV_QUALITY, 97u, "quality");
    value = expect_tlv(payload, payload_len, &offset,
                       TLV_UWB_RSL_DBM, sizeof(uint8_t), "rsl");
    CHECK((int8_t)value[0] == -73,
          "RSL mismatch: actual=%d expected=-73",
          (int8_t)value[0]);
    value = expect_tlv(payload, payload_len, &offset,
                       TLV_UWB_CIR_SAMPLE,
                       UWB_CIR_SAMPLE_LEN,
                       "cir-sample");
    CHECK(memcmp(value, cir_sample, sizeof(cir_sample)) == 0,
          "range CIR sample changed");
    expect_tlv_u8(payload, payload_len, &offset,
                  TLV_RANGE_STATUS, RANGE_OK, "range-status");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_MESH_CH9_BATCH_ID, TEST_BATCH_ID, "batch-id");
    expect_tlv_u8(payload, payload_len, &offset,
                  TLV_MESH_CH9_BATCH_FLAGS, 0u, "batch-flags");
    CHECK(offset == payload_len,
          "unexpected trailing range payload bytes: parsed=%zu total=%zu",
          offset,
          payload_len);
}

static void verify_cir_tlvs(const uint8_t *payload,
                            size_t payload_len,
                            size_t record_index)
{
    uint16_t fragment_index =
        (uint16_t)(record_index - TEST_RECORD_CIR_FIRST);
    uint16_t raw_offset = fragment_index == 0u ?
                          0u : RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES;
    uint16_t raw_len = fragment_index == 0u ?
                       RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES :
                       RANGE_REPORT_CIR_REMAINDER_RAW_BYTES;
    size_t offset = 0u;
    size_t raw_copied = 0u;

    expect_tlv_u64(payload, payload_len, &offset,
                   TLV_CLICKER_ID, TEST_CLICKER_ID, "CIR-clicker-id");
    expect_tlv_u64(payload, payload_len, &offset,
                   TLV_ANCHOR_ID, TEST_ANCHOR_ID, "CIR-anchor-id");
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_EVENT_SEQ, TEST_EVENT_SEQ, "CIR-event-seq");
    expect_tlv_u64(payload, payload_len, &offset,
                   TLV_TIMESTAMP_MS, TEST_TIMESTAMP_MS, "CIR-timestamp");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_DIAG_FRAGMENT_INDEX,
                   fragment_index,
                   "CIR-fragment-index");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_DIAG_FRAGMENT_COUNT, 2u, "CIR-fragment-count");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_UWB_CIR_BYTE_OFFSET, raw_offset, "CIR-byte-offset");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_UWB_CIR_TOTAL_BYTES,
                   RANGE_REPORT_CIR_WINDOW_RAW_BYTES,
                   "CIR-total-bytes");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_UWB_CIR_FIRST_PATH_INDEX,
                   TEST_FIRST_PATH_INDEX,
                   "CIR-first-path-index");
    expect_tlv_u16(payload, payload_len, &offset,
                   TLV_UWB_CIR_START_INDEX,
                   TEST_CIR_START_INDEX,
                   "CIR-start-index");

    while (raw_copied < raw_len) {
        size_t remaining = raw_len - raw_copied;
        uint8_t expected_chunk_len =
            (uint8_t)(remaining > UINT8_MAX ? UINT8_MAX : remaining);
        const uint8_t *value = expect_tlv(payload,
                                          payload_len,
                                          &offset,
                                          TLV_UWB_CIR_FULL_CHUNK,
                                          expected_chunk_len,
                                          "CIR-raw-chunk");

        CHECK(memcmp(value,
                     &cir_bytes[raw_offset + raw_copied],
                     expected_chunk_len) == 0,
              "CIR raw chunk bytes changed: fragment=%u raw_offset=%zu "
              "chunk_len=%u expected_crc=0x%04x actual_crc=0x%04x",
              fragment_index,
              raw_copied,
              expected_chunk_len,
              proto_crc16_ccitt_false(
                  &cir_bytes[raw_offset + raw_copied], expected_chunk_len),
              proto_crc16_ccitt_false(value, expected_chunk_len));
        raw_copied += expected_chunk_len;
    }
    expect_tlv_u32(payload, payload_len, &offset,
                   TLV_MESH_CH9_BATCH_ID, TEST_BATCH_ID, "CIR-batch-id");
    expect_tlv_u8(payload, payload_len, &offset,
                  TLV_MESH_CH9_BATCH_FLAGS,
                  record_index == TEST_RECORD_CIR_REMAINDER ?
                      TEST_BATCH_FLAG_FINAL : 0u,
                  "CIR-batch-flags");
    CHECK(raw_copied == raw_len,
          "CIR raw reconstruction length mismatch: copied=%zu expected=%u",
          raw_copied,
          raw_len);
    CHECK(offset == payload_len,
          "unexpected trailing CIR payload bytes: parsed=%zu total=%zu fragment=%u",
          offset,
          payload_len,
          fragment_index);
}

static void verify_gui_stream(void)
{
    size_t offset = 0u;

    test_phase = "verify-gui-stream";
    for (current_record_index = 0u;
         current_record_index < TEST_RECORD_COUNT;
         current_record_index++) {
        const struct expected_record *expected =
            &expected_records[current_record_index];
        const uint8_t *record;
        const uint8_t *payload;
        size_t record_len;
        uint16_t payload_len;
        uint16_t stored_crc;
        uint16_t expected_crc;

        CHECK(gui_stream_len - offset >= GATEWAY_BLE_STREAM_RECORD_HEADER_LEN,
              "GUI stream record header truncated: offset=%zu remaining=%zu",
              offset,
              gui_stream_len - offset);
        record = &gui_stream_bytes[offset];
        payload_len = proto_get_u16_le(&record[36]);
        record_len = GATEWAY_BLE_STREAM_RECORD_HEADER_LEN + payload_len;
        CHECK(gui_stream_len - offset >= record_len,
              "GUI stream record payload truncated: offset=%zu record_len=%zu "
              "remaining=%zu payload_len=%u",
              offset,
              record_len,
              gui_stream_len - offset,
              payload_len);
        payload = &record[GATEWAY_BLE_STREAM_RECORD_HEADER_LEN];

        CHECK(proto_get_u16_le(&record[0]) == GATEWAY_BLE_STREAM_MAGIC &&
                  record[2] == GATEWAY_BLE_STREAM_VERSION &&
                  record[3] == GATEWAY_BLE_STREAM_RECORD_HEADER_LEN &&
                  record[4] == GATEWAY_BLE_STREAM_RECORD_PACKET,
              "GUI stream envelope metadata mismatch: magic=0x%04x version=%u "
              "header=%u type=%u",
              proto_get_u16_le(&record[0]),
              record[2],
              record[3],
              record[4]);
        CHECK(record[5] == GATEWAY_BLE_STREAM_CLASS_CLICK &&
                  record[6] == 0u && record[7] == 0u,
              "GUI stream class/priority/truncation mismatch: class=%u "
              "priority=%u flags=0x%02x",
              record[5],
              record[6],
              record[7]);
        CHECK(record[8] == expected->packet.msg_type &&
                  record[9] == expected->packet.flags &&
                  proto_get_u16_le(&record[10]) == expected->packet.seq &&
                  proto_get_u32_le(&record[12]) == expected->packet.session_id &&
                  proto_get_u64_le(&record[16]) == expected->packet.src_id &&
                  proto_get_u64_le(&record[24]) == expected->packet.dst_id,
              "GUI stream packet metadata mismatch: msg=0x%02x flags=0x%02x "
              "seq=%u session=0x%08x src=0x%016llx dst=0x%016llx",
              record[8],
              record[9],
              proto_get_u16_le(&record[10]),
              proto_get_u32_le(&record[12]),
              (unsigned long long)proto_get_u64_le(&record[16]),
              (unsigned long long)proto_get_u64_le(&record[24]));
        CHECK(proto_get_u32_le(&record[32]) ==
                  expected->stream_queued_at_ms -
                      expected->stream_received_at_ms,
              "GUI stream latency mismatch: actual=%u expected=%u",
              proto_get_u32_le(&record[32]),
              expected->stream_queued_at_ms -
                  expected->stream_received_at_ms);
        CHECK(payload_len == expected->payload_len,
              "GUI payload length mismatch: actual=%u expected=%zu",
              payload_len,
              expected->payload_len);
        CHECK(record_len == GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                                expected->payload_len,
              "GUI record length mismatch: actual=%zu expected=%zu",
              record_len,
              GATEWAY_BLE_STREAM_RECORD_HEADER_LEN + expected->payload_len);
        CHECK(memcmp(payload, expected->payload, expected->payload_len) == 0,
              "GUI payload bytes changed: seq=%u len=%zu",
              expected->packet.seq,
              expected->payload_len);
        stored_crc = proto_get_u16_le(&record[38]);
        expected_crc = proto_crc16_ccitt_false(payload, payload_len);
        CHECK(stored_crc == expected_crc,
              "GUI payload CRC mismatch: stored=0x%04x calculated=0x%04x "
              "record=%zu seq=%u",
              stored_crc,
              expected_crc,
              current_record_index,
              expected->packet.seq);

        if (current_record_index == TEST_RECORD_RANGE) {
            verify_range_tlvs(payload, payload_len);
        } else {
            verify_cir_tlvs(payload, payload_len, current_record_index);
        }
        offset += record_len;
    }
    CHECK(offset == gui_stream_len,
          "GUI stream has trailing or missing bytes: parsed=%zu total=%zu",
          offset,
          gui_stream_len);
}

static void verify_final_diagnostics(void)
{
    struct gateway_ble_stream_diagnostics diagnostics;
    size_t expected_bytes = 0u;

    test_phase = "verify-final-diagnostics";
    for (size_t i = 0u; i < TEST_RECORD_COUNT; i++) {
        expected_bytes += GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                          expected_records[i].payload_len;
    }
    gateway_ble_stream_get_diagnostics(
        &test_stream,
        (uint32_t)(test_world.now_us / 1000u),
        &diagnostics);
    CHECK(diagnostics.enqueue_attempts == TEST_RECORD_COUNT &&
              diagnostics.packets_sent == TEST_RECORD_COUNT &&
              diagnostics.bytes_sent == expected_bytes &&
              diagnostics.max_queue_depth_observed ==
                  timeline.max_stream_depth,
          "final stream accounting mismatch: attempts=%u sent=%u bytes=%u "
          "expected_bytes=%zu max_depth=%u observed_max=%u",
          diagnostics.enqueue_attempts,
          diagnostics.packets_sent,
          diagnostics.bytes_sent,
          expected_bytes,
          diagnostics.max_queue_depth_observed,
          timeline.max_stream_depth);
    CHECK(diagnostics.drops_queue_full == 0u &&
              diagnostics.drops_too_large == 0u &&
              diagnostics.drops_not_ready == 0u &&
              diagnostics.drops_priority == 0u &&
              diagnostics.last_drop_reason == GATEWAY_BLE_STREAM_DROP_NONE &&
              diagnostics.oldest_queued_age_ms == 0u,
          "final stream drop/age diagnostics mismatch: full=%u large=%u "
          "not_ready=%u priority=%u last=%d oldest=%u",
          diagnostics.drops_queue_full,
          diagnostics.drops_too_large,
          diagnostics.drops_not_ready,
          diagnostics.drops_priority,
          (int)diagnostics.last_drop_reason,
          diagnostics.oldest_queued_age_ms);
}

int main(void)
{
    build_expected_records();
    setup_shared_scenario();
    run_shared_mesh_ble_timeline();
    verify_mesh_deliveries();
    verify_gui_stream();
    verify_final_diagnostics();

    printf("PASS mesh_gateway_ble_shared_timeline mesh_seed=0x%08x "
           "cir_seed=0x%08x "
           "mesh_events=%zu deliveries=%u cir_raw=%u+%u gui_bytes=%zu "
           "att_mtu=%u chunks=%zu ble_events=%zu disconnect_retries=1 "
           "credit_stalls=%u uwb_while_no_credit=%u "
           "uwb_while_disconnected=%u timeline_end_us=%llu stream_drops=0\n",
           TEST_MESH_SEED,
           TEST_CIR_SEED,
           mesh_events_run,
           TEST_RECORD_COUNT,
           RANGE_REPORT_CIR_PACKET_RAW_MAX_BYTES,
           RANGE_REPORT_CIR_REMAINDER_RAW_BYTES,
           gui_stream_len,
           TEST_BLE_MTU,
           ble_chunks_completed,
           ble_events_run,
           timeline.backpressure_events,
           timeline.mesh_events_credit_exhausted,
           timeline.mesh_events_disconnected,
           (unsigned long long)test_world.now_us);
    return EXIT_SUCCESS;
}
