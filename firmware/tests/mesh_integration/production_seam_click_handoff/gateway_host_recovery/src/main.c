/*
 * This deliberately compiles the production BLE receipt and mesh delivery
 * owners into one test translation unit.  The assertions drive the exact
 * receipt callback and its registered mesh work handler, rather than a
 * recovery-model substitute.
 */
#include "app_gateway_ble.c"
/* app_gateway_ble.c already establishes a log module for this TU. */
#undef LOG_MODULE_REGISTER
#define LOG_MODULE_REGISTER(...)
#include "app_mesh_report.c"
/* The ambiguous-save result conversion is owned by the gateway assignment
 * control state, so compile that real owner beside the BLE/runtime boundary.
 * Its otherwise-unused role paths are linker-elided by this focused seam. */
#undef LOG_MODULE_REGISTER
#define LOG_MODULE_REGISTER(...)
#include "app_anchor.c"

#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#define TEST_SOURCE_A UINT64_C(0x1112131415161718)
#define TEST_SOURCE_B UINT64_C(0x2122232425262728)
#define TEST_HOST_ID UINT64_C(0x3132333435363738)
#define TEST_CURRENT_GATEWAY_EPOCH 8u
#define TEST_STALE_GATEWAY_EPOCH 7u

static uint32_t test_watchdog_stops;
static uint32_t test_dwm_tx_count;
static uint32_t test_dwm_rx_count;

/* Result custody may request that BLE receive work resumes after it has
 * converted a reservation.  This seam injects packets directly into the
 * production receipt boundary, so it needs a valid inert RX work item rather
 * than an uninitialized work structure; no test case asks the fixture to
 * emulate a separate host ingress frame. */
static void test_gateway_ble_rx_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
}

/* The production durable owner stays opaque.  This seam supplies only its
 * device boundary, including the one failure mode that matters here: write
 * commits, mandatory readback times out, and the caller must adopt the exact
 * candidate without reporting a false terminal. */
#define TEST_DURABLE_SLOT_COUNT 4u
struct test_durable_slot {
    uint16_t id;
    uint8_t bytes[APP_DURABLE_STATE_MAX_RECORD_SIZE];
    size_t len;
    bool present;
};

struct test_durable_store {
    struct test_durable_slot slots[TEST_DURABLE_SLOT_COUNT];
    bool fail_next_read_after_write;
    int next_read_error;
};

static struct test_durable_slot *test_durable_slot_find(
    struct test_durable_store *store,
    uint16_t id,
    bool allocate)
{
    struct test_durable_slot *free_slot = NULL;

    if (store == NULL) {
        return NULL;
    }
    for (size_t index = 0u; index < ARRAY_SIZE(store->slots); index++) {
        if (store->slots[index].present && store->slots[index].id == id) {
            return &store->slots[index];
        }
        if (!store->slots[index].present && free_slot == NULL) {
            free_slot = &store->slots[index];
        }
    }
    if (allocate && free_slot != NULL) {
        free_slot->id = id;
    }
    return allocate ? free_slot : NULL;
}

static int test_durable_mount(void *context)
{
    return context == NULL ? -EINVAL : 0;
}

static ssize_t test_durable_read(void *context,
                                 uint16_t id,
                                 void *data,
                                 size_t len)
{
    struct test_durable_store *store = context;
    struct test_durable_slot *slot;

    if (store == NULL || data == NULL) {
        return -EINVAL;
    }
    if (store->next_read_error != 0) {
        int ret = store->next_read_error;

        store->next_read_error = 0;
        return ret;
    }
    slot = test_durable_slot_find(store, id, false);
    if (slot == NULL) {
        return -ENOENT;
    }
    memcpy(data, slot->bytes, MIN(len, slot->len));
    return (ssize_t)slot->len;
}

static ssize_t test_durable_write(void *context,
                                  uint16_t id,
                                  const void *data,
                                  size_t len)
{
    struct test_durable_store *store = context;
    struct test_durable_slot *slot;

    if (store == NULL || data == NULL ||
        len > APP_DURABLE_STATE_MAX_RECORD_SIZE) {
        return -EINVAL;
    }
    slot = test_durable_slot_find(store, id, true);
    if (slot == NULL) {
        return -ENOSPC;
    }
    memcpy(slot->bytes, data, len);
    slot->len = len;
    slot->present = true;
    if (store->fail_next_read_after_write) {
        store->fail_next_read_after_write = false;
        store->next_read_error = -ETIMEDOUT;
    }
    return (ssize_t)len;
}

static int test_durable_erase(void *context, uint16_t id)
{
    struct test_durable_store *store = context;
    struct test_durable_slot *slot;

    if (store == NULL) {
        return -EINVAL;
    }
    slot = test_durable_slot_find(store, id, false);
    if (slot == NULL) {
        return -ENOENT;
    }
    memset(slot, 0, sizeof(*slot));
    return 0;
}

static void test_durable_install(struct test_durable_store *store)
{
    const struct app_durable_state_test_backend backend = {
        .context = store,
        .mount = test_durable_mount,
        .read = test_durable_read,
        .write = test_durable_write,
        .erase = test_durable_erase,
    };

    zassert_not_null(store);
    app_durable_state_test_reset();
    zassert_ok(app_durable_state_test_install_backend(
        &backend, APP_DURABLE_STATE_ROLE_GATEWAY));
    zassert_ok(app_durable_state_init(DEVICE_ID));
    zassert_ok(app_durable_state_begin_boot());
}

#define TEST_PUBLISH_CAPTURE_CAP 8u
static struct gateway_command_event
    test_published_events[TEST_PUBLISH_CAPTURE_CAP];
static size_t test_published_event_count;
static uint32_t test_published_next_sequence;

static int test_publisher_reserve_sequence(struct gateway_command_event *event,
                                           void *context)
{
    ARG_UNUSED(context);
    if (event == NULL) {
        return -EINVAL;
    }
    test_published_next_sequence++;
    if (test_published_next_sequence == 0u) {
        test_published_next_sequence++;
    }
    event->event_seq = test_published_next_sequence;
    return 0;
}

static int test_publisher_capture_emit(struct gateway_command_event *event,
                                       bool terminal,
                                       void *context)
{
    ARG_UNUSED(terminal);
    ARG_UNUSED(context);
    if (event == NULL ||
        test_published_event_count == ARRAY_SIZE(test_published_events)) {
        return -ENOSPC;
    }
    test_published_events[test_published_event_count++] = *event;
    return 0;
}

static void test_publisher_capture_reset(void)
{
    const struct app_gateway_assignment_publisher_ops ops = {
        .emit_if_available = test_publisher_capture_emit,
        .reserve_event_seq = test_publisher_reserve_sequence,
    };

    memset(test_published_events, 0, sizeof(test_published_events));
    test_published_event_count = 0u;
    test_published_next_sequence = 0u;
    zassert_ok(app_gateway_assignment_publisher_init(&ops));
}

/* The receipt-domain tests drive the stream/receipt boundary directly, so a
 * non-null connection sentinel must never reach the real GATT sender. The
 * stream work is deliberately inert while the test advances each exact head
 * itself; production stream ownership and packet encoding remain unchanged. */
static uint32_t test_ble_connection_sentinel;

static void test_gateway_ble_stream_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
}

static void test_gateway_mesh_receipt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
}

static void test_enable_receipt_stream_fixture(void)
{
    k_work_init_delayable(&gateway_ble_stream_work,
                          test_gateway_ble_stream_work_handler);
    gateway_ble_conn = (struct bt_conn *)(void *)&test_ble_connection_sentinel;
    gateway_ble_packet_notify_enabled = true;
}

static void test_disable_receipt_stream_fixture(void)
{
    (void)k_work_cancel_delayable(&gateway_ble_stream_work);
    gateway_ble_conn = NULL;
    gateway_ble_packet_notify_enabled = false;
}

static void test_publisher_stream_reset(void)
{
    const struct app_gateway_assignment_publisher_ops ops = {
        .emit_if_available = gateway_publish_assignment_event_if_available,
        .reserve_event_seq = gateway_reserve_command_event_sequence,
    };

    zassert_ok(app_gateway_assignment_publisher_init(&ops));
}

static void test_start_reliable_assignment_publisher(void)
{
    const uint64_t anchor_id = TEST_SOURCE_A;
    const uint8_t slot = 0u;
    const struct gateway_command_event base_event = {
        .schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION,
        .record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN,
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .gateway_epoch = TEST_CURRENT_GATEWAY_EPOCH,
        .correlation_id = UINT32_C(0x44556677),
        .gateway_sequence = UINT32_C(0x10203040),
        .host_session_id = UINT32_C(0x44556677),
        .host_seq = 37u,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };

    test_publisher_stream_reset();
    zassert_ok(app_gateway_assignment_publisher_prepare_table(
        &base_event, &anchor_id, &slot, NULL, 1u, UINT64_C(1), 0u));
    zassert_ok(app_gateway_assignment_publisher_commit_prepared_batch(
        &base_event));
}

static void test_enqueue_distinct_mesh_result(struct proto_packet *packet,
                                              uint8_t *payload,
                                              size_t *payload_len)
{
    zassert_not_null(packet);
    zassert_not_null(payload);
    zassert_not_null(payload_len);

    *payload_len = 0u;
    zassert_ok(mesh_append_command_result(
        payload, APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN, payload_len,
        CMD_GET_STATUS, COMMAND_OK, 0u));
    zassert_ok(mesh_init_command_result(packet,
                                        TEST_SOURCE_B,
                                        DEVICE_ID,
                                        UINT32_C(0x55667788),
                                        41u,
                                        (uint8_t)*payload_len,
                                        false));
    zassert_equal(gateway_ble_reserve_stream_packet(
                      packet, payload, *payload_len, k_uptime_get_32()),
                  GATEWAY_BLE_STREAM_RESERVATION_ACQUIRED);
    zassert_equal(gateway_ble_commit_stream_reservation(
                      packet, payload, *payload_len),
                  GATEWAY_BLE_STREAM_RESERVATION_ACQUIRED);
}

/* Board and radio effects are intentionally inert.  The test still executes
 * production frame construction, radio leasing, C5 politeness, flood repeat,
 * and retained BLE retirement; only the hardware device boundary is stubbed. */
void app_watchdog_stop_feeding(void)
{
    test_watchdog_stops++;
}

void app_watchdog_note_radio_progress(void)
{
}

void status_debug_note(const char *text)
{
    ARG_UNUSED(text);
}

void status_debug_printf(const char *fmt, ...)
{
    /* Route-test diagnostics are intentionally noisy.  The fixture asserts
     * the causal effects below, so suppressing them keeps a failed virtual
     * scheduler run bounded instead of generating an unbounded runner log. */
    ARG_UNUSED(fmt);
}

void status_debug_uwb_rx_channel_pulse(uint8_t channel)
{
    ARG_UNUSED(channel);
}

void status_debug_gateway_uwb_rx_channel_pulse(uint8_t channel)
{
    ARG_UNUSED(channel);
}

void status_debug_uwb_tx_channel_pulse(uint8_t channel)
{
    ARG_UNUSED(channel);
}

void status_debug_tx_packet_sent_pulse(void)
{
}

void status_debug_tx_wake_claim_sent_pulse(void)
{
}

void status_debug_tx_mesh_frame_sent_pulse(void)
{
}

void status_debug_tx_gateway_ack_rx_pulse(void)
{
}

void status_debug_gateway_ack_tx_pulse(void)
{
}

int dwm3000_driver_configure_mesh_payload_mode(void)
{
    return 0;
}

int dwm3000_driver_configure_wake_mesh_control_mode(void)
{
    return 0;
}

int dwm3000_driver_configure_wake_mode(void)
{
    return 0;
}

int dwm3000_driver_ensure_wake_mode(void)
{
    return 0;
}

int dwm3000_driver_idle(void)
{
    return 0;
}

int dwm3000_driver_standby(void)
{
    return 0;
}

int dwm3000_driver_force_recovery(void)
{
    return 0;
}

int dwm3000_driver_send_frame(const uint8_t *frame,
                              size_t frame_len,
                              uint32_t timeout_ms)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(timeout_ms);
    test_dwm_tx_count++;
    return 0;
}

int dwm3000_driver_send_frame_tracked(const uint8_t *frame,
                                      size_t frame_len,
                                      uint32_t timeout_ms,
                                      bool *rf_started)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(timeout_ms);
    test_dwm_tx_count++;
    if (rf_started != NULL) {
        *rf_started = true;
    }
    return 0;
}

int dwm3000_driver_send_frame_tracked_until(
    const uint8_t *frame,
    size_t frame_len,
    uint32_t timeout_ms,
    uint64_t absolute_deadline_ms,
    struct dwm3000_tx_observation *observation)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_len);
    ARG_UNUSED(timeout_ms);
    ARG_UNUSED(absolute_deadline_ms);
    test_dwm_tx_count++;
    if (observation != NULL) {
        *observation = (struct dwm3000_tx_observation) {
            .rf_started_at_ms = (uint64_t)k_uptime_get(),
            .tx_completed_at_ms = (uint64_t)k_uptime_get(),
            .rf_started = true,
            .tx_completed = true,
        };
    }
    return 0;
}

static int test_dwm_receive_timeout(uint8_t *frame,
                                    size_t frame_cap,
                                    size_t *frame_len,
                                    uint8_t *quality,
                                    int8_t *rsl_dbm,
                                    enum dwm3000_rx_failure *failure)
{
    ARG_UNUSED(frame);
    ARG_UNUSED(frame_cap);
    if (frame_len != NULL) {
        *frame_len = 0u;
    }
    if (quality != NULL) {
        *quality = 0u;
    }
    if (rsl_dbm != NULL) {
        *rsl_dbm = 0;
    }
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_FRAME_TIMEOUT;
    }
    return -ETIMEDOUT;
}

int dwm3000_driver_receive_frame(uint32_t timeout_ms,
                                 uint8_t *frame,
                                 size_t frame_cap,
                                 size_t *frame_len,
                                 uint8_t *quality,
                                 int8_t *rsl_dbm)
{
    int ret = test_dwm_receive_timeout(frame, frame_cap, frame_len, quality,
                                       rsl_dbm, NULL);

    test_dwm_rx_count++;
    /* An RF timeout consumes its programmed window.  Returning it at the
     * same virtual timestamp makes the gateway's normal RX rearm spin
     * forever, which would model neither hardware nor the production timer
     * contract. */
    k_msleep(timeout_ms == 0u ? 1u : timeout_ms);
    return ret;
}

int dwm3000_driver_receive_frame_detailed(uint32_t timeout_ms,
                                          uint8_t *frame,
                                          size_t frame_cap,
                                          size_t *frame_len,
                                          uint8_t *quality,
                                          int8_t *rsl_dbm,
                                          enum dwm3000_rx_failure *failure)
{
    int ret = test_dwm_receive_timeout(frame, frame_cap, frame_len, quality,
                                       rsl_dbm, failure);

    test_dwm_rx_count++;
    k_msleep(timeout_ms == 0u ? 1u : timeout_ms);
    return ret;
}

int dwm3000_driver_receive_frame_detailed_quiet(
    uint32_t timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    return dwm3000_driver_receive_frame_detailed(timeout_ms, frame, frame_cap,
                                                 frame_len, quality, rsl_dbm,
                                                 failure);
}

int dwm3000_driver_sniff_activity(uint32_t timeout_ms,
                                  enum dwm3000_rx_failure *failure)
{
    ARG_UNUSED(timeout_ms);
    if (failure != NULL) {
        *failure = DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT;
    }
    return -ETIMEDOUT;
}

int dwm3000_driver_receive_frame_continuous(
    uint32_t timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    return dwm3000_driver_receive_frame_detailed(timeout_ms, frame, frame_cap,
                                                 frame_len, quality, rsl_dbm,
                                                 failure);
}

int dwm3000_driver_receive_frame_continuous_extend_on_activity(
    uint32_t acquire_timeout_ms,
    uint32_t completion_timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure)
{
    ARG_UNUSED(acquire_timeout_ms);
    return dwm3000_driver_receive_frame_detailed(
        completion_timeout_ms, frame, frame_cap, frame_len, quality, rsl_dbm,
        failure);
}

int dwm3000_driver_receive_frame_continuous_timed(
    uint32_t timeout_ms,
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint8_t *quality,
    int8_t *rsl_dbm,
    enum dwm3000_rx_failure *failure,
    struct dwm3000_rx_frame_timing *timing)
{
    if (timing != NULL) {
        memset(timing, 0, sizeof(*timing));
    }
    return dwm3000_driver_receive_frame_detailed(timeout_ms, frame, frame_cap,
                                                 frame_len, quality, rsl_dbm,
                                                 failure);
}

void dwm3000_driver_request_receive_abort(uint32_t owner_mask)
{
    ARG_UNUSED(owner_mask);
}

void dwm3000_driver_clear_receive_abort(uint32_t owner_mask)
{
    ARG_UNUSED(owner_mask);
}

bool dwm3000_driver_receive_abort_pending(void)
{
    return false;
}

void dwm3000_driver_last_rx_debug_get(struct dwm3000_rx_debug_snapshot *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

struct recovery_fixture {
    struct proto_packet packet;
    uint8_t payload[256];
    size_t payload_len;
};

static void build_stale_result(struct recovery_fixture *fixture,
                               uint64_t source_id,
                               uint32_t command_seq,
                               uint16_t result_seq,
                               uint32_t collection_epoch)
{
    const struct command_result_id result_id = {
        .gateway_id = DEVICE_ID,
        .gateway_epoch = TEST_STALE_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .node_id = source_id,
        .node_boot_counter = 3u,
        .result_seq = result_seq,
    };

    memset(fixture, 0, sizeof(*fixture));
    zassert_ok(command_result_id_append_tlvs(
        fixture->payload, sizeof(fixture->payload), &fixture->payload_len,
        &result_id));
    zassert_ok(mesh_append_command_result(
        fixture->payload, sizeof(fixture->payload), &fixture->payload_len,
        CMD_GET_STATUS, COMMAND_OK, 0u));
    zassert_ok(tlv_append_u32(fixture->payload, sizeof(fixture->payload),
                              &fixture->payload_len,
                              TLV_COLLECTION_EPOCH_ID, collection_epoch));
    zassert_ok(mesh_init_command_result(&fixture->packet, source_id,
                                        DEVICE_ID, command_seq, result_seq,
                                        (uint8_t)fixture->payload_len, false));
}

static void build_stale_bundle(struct recovery_fixture *fixture,
                               uint64_t source_id,
                               uint32_t command_seq,
                               uint16_t packet_seq,
                               uint16_t bundle_id,
                               uint32_t collection_epoch)
{
    const struct command_result_id result_id = {
        .gateway_id = DEVICE_ID,
        .gateway_epoch = TEST_STALE_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .node_id = source_id,
        .node_boot_counter = 4u,
        .result_seq = packet_seq,
    };
    struct result_bundle_header bundle = {
        .gateway_id = DEVICE_ID,
        .gateway_epoch = TEST_STALE_GATEWAY_EPOCH,
        .command_seq = command_seq,
        .collection_epoch_id = collection_epoch,
        .bundle_id = bundle_id,
        .record_count = 1u,
    };
    uint8_t record_payload[128] = {0};
    uint8_t records[160] = {0};
    size_t record_payload_len = 0u;
    size_t records_len = 0u;

    zassert_equal(bundle_id, packet_seq,
                  "wire bundle identity must match its packet sequence");
    memset(fixture, 0, sizeof(*fixture));
    zassert_ok(command_result_id_append_tlvs(
        record_payload, sizeof(record_payload), &record_payload_len,
        &result_id));
    zassert_ok(mesh_append_command_result(record_payload,
                                          sizeof(record_payload),
                                          &record_payload_len,
                                          CMD_GET_STATUS, COMMAND_OK, 0u));
    zassert_ok(tlv_append_u32(record_payload, sizeof(record_payload),
                              &record_payload_len,
                              TLV_COLLECTION_EPOCH_ID, collection_epoch));
    {
        const struct result_bundle_record record = {
            .result_id = result_id,
            .payload_len = (uint16_t)record_payload_len,
            .payload_crc = proto_crc16_ccitt_false(record_payload,
                                                   record_payload_len),
            .payload = record_payload,
        };

        zassert_ok(result_bundle_record_append_tlv(
            records, sizeof(records), &records_len, &record));
    }
    bundle.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    zassert_ok(result_bundle_header_append_tlvs(
        fixture->payload, sizeof(fixture->payload), &fixture->payload_len,
        &bundle));
    zassert_true(sizeof(fixture->payload) - fixture->payload_len >= records_len);
    memcpy(&fixture->payload[fixture->payload_len], records, records_len);
    fixture->payload_len += records_len;
    fixture->packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_BUNDLE,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = source_id,
        .dst_id = DEVICE_ID,
        .session_id = command_seq,
        .seq = packet_seq,
        .ttl = MESH_DEFAULT_TTL,
        .payload_len = (uint16_t)fixture->payload_len,
    };
}

static void reset_gateway_owner(void)
{
    k_msgq_purge(&mesh_rx_msgq);
    zassert_ok(app_mesh_report_init(NULL));
    /* This test application reinitializes the production mesh owner between
     * cases. app_mesh_report_init() resets its paused-state flag, while the
     * radio admission gate is intentionally process-lifetime state in the
     * production image. Balance a prior fixture pause explicitly so a stale
     * test case cannot make the next recovery C5 transaction self-defer. */
    radio_guard_uwb_admission_resume();
    mesh_relay_init(&mesh_runtime, MESH_RELAY_ROLE_GATEWAY, DEVICE_ID,
                    DEVICE_ID, TEST_CURRENT_GATEWAY_EPOCH);
    zassert_ok(app_mesh_report_attach_gateway_ack_store());

    gateway_ble_stream_init(&gateway_ble_stream_state);
    gateway_ble_stream_initialized = true;
    gateway_ble_conn = NULL;
    gateway_ble_packet_notify_enabled = false;
    k_work_init(&gateway_ble_rx_work, test_gateway_ble_rx_work_handler);
    k_work_init_delayable(&gateway_ble_stream_work,
                          gateway_ble_stream_work_handler);
    k_work_init_delayable(&gateway_ble_host_receipt_timeout_work,
                          gateway_ble_host_receipt_timeout_work_handler);
    k_work_init_delayable(&gateway_persistence_retry_work,
                          gateway_persistence_retry_work_handler);
    atomic_clear(&gateway_persistence_retry_schedule_deferred);
    atomic_clear(&gateway_persistence_retry_deferred_delay_ms);
    app_gateway_command_result_queue_init(&gateway_host_command_results);
    gateway_command_result_validation_clear(
        &gateway_command_result_validation_state);
    gateway_collection_clear(&gateway_collection_state);
    app_gateway_collection_recovery_reset(&gateway_collection_recovery);
    gateway_membership_restore_pending = false;
    gateway_membership_clear_pending = false;
    atomic_clear(&gateway_assignment_publication_pending_state);
    zassert_ok(app_gateway_control_sequence_init());
    test_watchdog_stops = 0u;
    test_dwm_tx_count = 0u;
    test_dwm_rx_count = 0u;
}

static void reset_assignment_adoption_fixture(bool retain_restored_membership)
{
    reset_gateway_owner();
    /* This is the production startup owner for the result queue, persistence
     * retry work, and restore-before-admission state. The test installs the
     * fake durable backend first, so its real restore path is safe to use. */
    gateway_command_result_tracking_init();
    /* app_mesh_report_init() deliberately resets role-owned work structures
     * in this amalgamated seam. Reinstall this real handler after that reset,
     * exactly as gateway startup does before a save can schedule adoption. */
    k_work_init_delayable(&gateway_persistence_retry_work,
                          gateway_persistence_retry_work_handler);
    /* Keep the local result in its production bounded command-result queue
     * during this ownership test. The receipt-domain cases below separately
     * exercise its real BLE retirement without pulling the whole ingress
     * work handler into this narrow adoption fixture. */
    gateway_ble_stream_initialized = false;
    gateway_ble_conn = NULL;
    gateway_ble_packet_notify_enabled = false;

    if (!retain_restored_membership) {
        gateway_membership_clear(&gateway_membership_roster_state);
        gateway_membership_clear_durable_evidence();
        gateway_membership_clear_pending = false;
        gateway_membership_restore_pending = false;
        gateway_membership_publication_live_owner = false;
        gateway_membership_publication_owner_release_pending = false;
        atomic_clear(&gateway_assignment_publication_pending_state);
    }
    memset(&gateway_discovery_assignment_state,
           0,
           sizeof(gateway_discovery_assignment_state));
    memset(&gateway_assignment_operation_lease,
           0,
           sizeof(gateway_assignment_operation_lease));
    memset(&gateway_operation_owner, 0, sizeof(gateway_operation_owner));
    test_publisher_capture_reset();
}

static void queue_received_fixture(const struct recovery_fixture *fixture)
{
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN] = {0};
    size_t frame_len = 0u;
    bool valid_mesh_frame = false;
    uint64_t previous_hop_id = 0u;

    zassert_not_null(fixture);
    zassert_ok(uwb_mesh_frame_encode(NETWORK_ID, fixture->packet.src_id,
                                     DEVICE_ID, &fixture->packet,
                                     fixture->payload, frame, sizeof(frame),
                                     &frame_len));
    zassert_true(mesh_queue_from_frame_deferred(
        frame, frame_len, 90u, UWB_CHANNEL_MESH_PAYLOAD, &valid_mesh_frame,
        &previous_hop_id));
    zassert_true(valid_mesh_frame);
    zassert_equal(previous_hop_id, fixture->packet.src_id);
}

static void mark_exact_head_host_notified(void)
{
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    k_spinlock_key_t key = k_spin_lock(&gateway_ble_stream_lock);

    zassert_ok(gateway_ble_stream_begin_send_view(&gateway_ble_stream_state,
                                                  &record, &record_len));
    zassert_not_null(record);
    zassert_true(record_len > GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    zassert_ok(gateway_ble_stream_mark_host_notified(&gateway_ble_stream_state));
    k_spin_unlock(&gateway_ble_stream_lock, key);
}

static uint32_t drain_mesh_owner(const char *owner)
{
    uint32_t handled;

    zassert_ok(k_mutex_lock(&mesh_rx_handler_lock, K_FOREVER));
    atomic_set(&mesh_rx_handler_active_state, 1);
    mesh_rx_handler_lock_note_owner(owner);
    handled = mesh_drain_rx_queue_locked(owner);
    atomic_clear(&mesh_rx_handler_active_state);
    mesh_rx_handler_lock_clear_owner();
    k_mutex_unlock(&mesh_rx_handler_lock);
    return handled;
}

static void capture_head_host_receipt_identity(
    struct gateway_host_receipt_identity *identity,
    struct proto_packet *head)
{
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    k_spinlock_key_t key = k_spin_lock(&gateway_ble_stream_lock);

    zassert_not_null(identity);
    zassert_not_null(head);
    memset(identity, 0, sizeof(*identity));
    memset(head, 0, sizeof(*head));
    zassert_ok(gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                              head));
    zassert_ok(gateway_ble_stream_peek(&gateway_ble_stream_state, &record,
                                       &record_len));
    zassert_true(semantic_digest_sha256(record, record_len,
                                        identity->stream_record_digest));
    k_spin_unlock(&gateway_ble_stream_lock, key);

    identity->original_msg_type = head->msg_type;
    identity->original_flags = head->flags;
    identity->src_id = head->src_id;
    identity->dst_id = head->dst_id;
    identity->session_id = head->session_id;
    identity->seq = head->seq;
}

static void build_exact_host_receipt(struct proto_packet *receipt,
                                     uint8_t *payload,
                                     size_t payload_cap,
                                     size_t *payload_len)
{
    struct proto_packet head = {0};
    struct gateway_host_receipt_identity identity;

    capture_head_host_receipt_identity(&identity, &head);
    *payload_len = 0u;
    zassert_ok(gateway_host_receipt_identity_append_tlv(
        payload, payload_cap, payload_len, &identity));
    *receipt = (struct proto_packet) {
        .msg_type = MSG_GATEWAY_HOST_RECEIPT,
        .src_id = TEST_HOST_ID,
        .dst_id = DEVICE_ID,
        .session_id = head.session_id,
        .seq = head.seq,
        .ttl = 1u,
        .payload_len = (uint16_t)*payload_len,
    };
}

static void accept_receipt_and_run_owner(const struct proto_packet *receipt,
                                         const uint8_t *payload,
                                         size_t payload_len)
{
    struct k_work_sync sync = {0};

    /* Exercise the production boundary exactly: BLE accepts the receipt,
     * queues mesh_rx_work, and that worker owns semantic completion, C5, and
     * BLE retirement.  Holding mesh_rx_handler_lock here would manufacture a
     * workqueue lock-busy loop that the real BT callback never creates. */
    zassert_ok(gateway_ble_accept_host_receipt(receipt, payload, payload_len));
    zassert_true(atomic_get(&mesh_gateway_host_receipt_received_state) != 0);
    zassert_true(k_work_flush(&mesh_rx_work, &sync),
                 "exact receipt did not submit the production mesh owner");

    /* The recovery completion deliberately restarts normal gateway RX after
     * it retires the exact BLE head.  This fixture has no RF peer, so leave
     * that unrelated perpetual scan paused between controlled receipts; the
     * next call resumes it immediately before exercising the next real
     * callback. */
    zassert_ok(mesh_transport_pause_preserving_queued());
}

ZTEST(production_seam_gateway_host_recovery,
      test_ambiguous_assignment_save_adopts_one_local_result_and_cold_replays)
{
    static struct test_durable_store durable;
    const uint64_t node_ids[] = {TEST_SOURCE_A};
    const uint8_t slots[] = {0u};
    const uint32_t assignment_epoch = 91u;
    const uint32_t table_sequence = 73u;
    const struct discovery_assignment_table_commitment commitment = {
        .bytes = {[0] = 0x5au, [31] = 0xa5u},
    };
    struct proto_packet host_command = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = TEST_HOST_ID,
        .dst_id = DEVICE_ID,
        .session_id = UINT32_C(0x33445566),
        .seq = 29u,
        .ttl = 3u,
        .payload_len = PROTO_TLV_U16_ENCODED_LEN,
    };
    struct gateway_membership_publication publication = {
        .host_command = host_command,
        .committed_mask = UINT64_C(1),
        .acknowledged_mask = UINT64_C(1),
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .event_gateway_epoch = TEST_CURRENT_GATEWAY_EPOCH,
        .claimed_count = 1u,
        .claimed_slot_span = 1u,
        .table_round = 1u,
        .publish_pending = 1u,
    };
    struct gateway_command_event base_event = {
        .schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION,
        .record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN,
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .gateway_epoch = TEST_CURRENT_GATEWAY_EPOCH,
        .correlation_id = host_command.session_id,
        .gateway_sequence = assignment_epoch,
        .host_session_id = host_command.session_id,
        .host_seq = host_command.seq,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
    struct app_gateway_command_result_item local_result = {0};
    const uint8_t *tlv = NULL;
    uint8_t tlv_len = 0u;
    uint32_t reservation_token = 0u;
    const uint8_t reservation_depth_before = 1u;

    memset(&durable, 0, sizeof(durable));
    publication.claimed_node_ids[0] = node_ids[0];
    test_durable_install(&durable);
    reset_assignment_adoption_fixture(false);

    /* This is the exact prepared publisher/assignment ownership that exists
     * just before the durable commit in the real completion path. */
    zassert_ok(app_gateway_assignment_publisher_prepare_table(
        &base_event, node_ids, slots, NULL, ARRAY_SIZE(node_ids),
        publication.acknowledged_mask, publication.duplicate_count));
    zassert_ok(gateway_command_result_reserve_ingress(&reservation_token));
    zassert_ok(gateway_command_result_bind_ingress(
        reservation_token, &host_command, CMD_ASSIGN_DISCOVERY_SLOTS));
    gateway_discovery_assignment_state =
        (struct gateway_discovery_assignment_state) {
            .host_command = host_command,
            .result_reservation_token = reservation_token,
            .anchor_ids = {node_ids[0]},
            .anchor_slots = {slots[0]},
            .ack_mask = UINT64_C(1),
            .epoch = assignment_epoch,
            .table_commitment = commitment,
            .table_command_seq = table_sequence,
            .claim_count = ARRAY_SIZE(node_ids),
            .stage = GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS,
            .table_round = publication.table_round,
            .table_delivery_succeeded = true,
        };

    /* Keep the automatic retry deferred until the test explicitly drives the
     * owning adoption transition; otherwise it can race the pre-adoption
     * custody assertions from the mesh route workqueue. */
    zassert_ok(mesh_transport_pause_preserving_queued());
    durable.fail_next_read_after_write = true;
    zassert_equal(gateway_set_registered_membership_roster(
                      discovery_assignment_membership_epoch(assignment_epoch),
                      node_ids, slots, ARRAY_SIZE(node_ids), assignment_epoch,
                      table_sequence, &commitment, &publication),
                  GATEWAY_MEMBERSHIP_COMMIT_ADOPTION_PENDING,
                  "write/readback ambiguity must retain the exact assignment");
    zassert_true(gateway_membership_adoption_pending);
    zassert_false(gateway_membership_durable_receipt_valid);
    zassert_equal(gateway_discovery_assignment_state.result_reservation_token,
                  reservation_token);
    zassert_equal(app_gateway_command_result_reservation_depth(
                      &gateway_host_command_results),
                  reservation_depth_before,
                  "no local result may consume the reservation before adoption");
    zassert_equal(app_gateway_command_result_queue_depth(
                      &gateway_host_command_results),
                  0u);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);
    zassert_equal(test_published_event_count, 0u);

    zassert_ok(gateway_membership_adopt_pending_commit());
    zassert_false(gateway_membership_adoption_pending);
    zassert_true(gateway_membership_durable_receipt_valid);
    zassert_equal(gateway_discovery_assignment_state.result_reservation_token,
                  0u,
                  "queue custody must consume the original reservation once");
    zassert_equal(app_gateway_command_result_reservation_depth(
                      &gateway_host_command_results),
                  0u);
    zassert_equal(app_gateway_command_result_queue_depth(
                      &gateway_host_command_results),
                  1u,
                  "the committed local result must retain bounded queue custody");
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);
    zassert_ok(app_gateway_command_result_queue_peek(
        &gateway_host_command_results, &local_result));
    zassert_equal(local_result.packet.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(local_result.packet.src_id, DEVICE_ID);
    zassert_equal(local_result.packet.dst_id, DEVICE_ID);
    zassert_true((local_result.packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    zassert_equal(tlv_find_unique(local_result.payload,
                                  local_result.payload_len,
                                  TLV_COMMAND_ID,
                                  &tlv,
                                  &tlv_len),
                  PROTO_OK);
    zassert_equal(tlv_len, sizeof(uint16_t));
    zassert_equal(proto_get_u16_le(tlv), CMD_ASSIGN_DISCOVERY_SLOTS);
    zassert_equal(tlv_find_unique(local_result.payload,
                                  local_result.payload_len,
                                  TLV_COMMAND_STATUS,
                                  &tlv,
                                  &tlv_len),
                  PROTO_OK);
    zassert_equal(tlv_len, sizeof(uint16_t));
    zassert_equal(proto_get_u16_le(tlv), COMMAND_OK);
    zassert_equal(tlv_find_unique(local_result.payload,
                                  local_result.payload_len,
                                  TLV_REASON,
                                  &tlv,
                                  &tlv_len),
                  PROTO_OK);
    zassert_equal(tlv_len, sizeof(uint8_t));
    zassert_equal(tlv[0], 1u,
                  "assignment result reason must carry acknowledged count");

    zassert_ok(app_anchor_gateway_assignment_adopted_result_commit(
        &gateway_membership_assignment_identity, 1u));
    zassert_equal(app_gateway_command_result_queue_depth(
                      &gateway_host_command_results),
                  1u,
                  "retrying the exact anchor hook must not create a terminal");

    /* Retrying the post-readback work must not synthesize another terminal or
     * spend another command-result credit. */
    zassert_ok(gateway_membership_adopt_pending_commit());
    zassert_equal(app_gateway_command_result_queue_depth(
                      &gateway_host_command_results),
                  1u);
    zassert_equal(app_gateway_command_result_reservation_depth(
                      &gateway_host_command_results),
                  0u);

    zassert_equal(gateway_replay_pending_assignment_publication(), 1);
    zassert_equal(test_published_event_count, 1u);
    zassert_true((test_published_events[0].flags &
                  GATEWAY_COMMAND_EVENT_FLAG_REPLAY) == 0u,
                 "same-boot adoption retains the original publisher semantics");

    /* A power loss has no RAM reservation or local result to reconstruct.
     * It restores only the immutable roster/publication proof and tags the
     * newly built publisher stream as a cold replay. */
    app_durable_state_test_reset();
    test_durable_install(&durable);
    reset_assignment_adoption_fixture(true);
    zassert_false(gateway_membership_restore_pending);
    zassert_true(gateway_membership_roster_state.valid);
    zassert_false(gateway_membership_publication_live_owner);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);
    zassert_equal(app_gateway_command_result_reservation_depth(
                      &gateway_host_command_results),
                  0u);
    zassert_equal(gateway_discovery_assignment_state.result_reservation_token,
                  0u);

    zassert_equal(gateway_replay_pending_assignment_publication(), 1);
    for (size_t event_index = 0u; event_index < 4u; event_index++) {
        zassert_equal(test_published_event_count, event_index + 1u,
                      "cold replay must emit each recovered publisher item");
        zassert_true((test_published_events[event_index].flags &
                      GATEWAY_COMMAND_EVENT_FLAG_REPLAY) != 0u,
                     "every cold reconstructed publisher event needs REPLAY");
        zassert_equal(app_gateway_assignment_publisher_note_host_receipt(
                          &test_published_events[event_index]),
                      1,
                      "each captured cold event must advance exactly once");
        app_gateway_assignment_publisher_pump();
    }
    zassert_equal(test_published_event_count, 4u,
                  "one mapping plus all aggregate/terminal events must replay");
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);

    /* Balance the deliberate transport pause that made the ambiguous durable
     * write deterministic. The retry was driven manually above, so do not
     * let a fixture-only deferred route submission leak into the next ZTEST. */
    (void)k_work_cancel_delayable(&gateway_persistence_retry_work);
    atomic_clear(&gateway_persistence_retry_schedule_deferred);
    atomic_clear(&gateway_persistence_retry_deferred_delay_ms);
    mesh_transport_resume();
    app_durable_state_test_reset();
}

ZTEST(production_seam_gateway_host_recovery,
      test_generic_command_telemetry_never_claims_host_receipt_custody)
{
    const struct gateway_command_event event = {
        .schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION,
        .record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN,
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ACCEPTED,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .gateway_epoch = TEST_CURRENT_GATEWAY_EPOCH,
        .correlation_id = UINT32_C(0x11223344),
        .gateway_sequence = UINT32_C(0x55667788),
        .host_session_id = UINT32_C(0x11223344),
        .host_seq = 17u,
        .event_seq = UINT32_C(0x00001011),
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_COMMAND_EVENT,
        .flags = 0u,
        .src_id = DEVICE_ID,
        .dst_id = DEVICE_ID,
        .session_id = event.event_seq,
        .seq = (uint16_t)event.event_seq,
        .ttl = 1u,
        .payload_len = GATEWAY_COMMAND_EVENT_WIRE_LEN,
    };
    struct gateway_host_receipt_identity identity;
    struct proto_packet head;
    uint8_t event_payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    uint8_t receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    size_t event_payload_len = 0u;
    size_t receipt_payload_len = 0u;
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    k_spinlock_key_t key;

    reset_gateway_owner();
    zassert_ok(gateway_command_event_encode(&event,
                                            event_payload,
                                            sizeof(event_payload),
                                            &event_payload_len));
    zassert_equal(event_payload_len, sizeof(event_payload));
    zassert_ok(gateway_ble_stream_packet(&packet,
                                         event_payload,
                                         event_payload_len,
                                         k_uptime_get_32()));
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
    zassert_false(gateway_host_custody_supported(&packet));

    key = k_spin_lock(&gateway_ble_stream_lock);
    zassert_false(gateway_ble_stream_state.items[0].retain_until_sent);
    zassert_false(gateway_ble_stream_state.items[0].host_custody_owner);
    zassert_ok(gateway_ble_stream_begin_send_view(&gateway_ble_stream_state,
                                                  &record,
                                                  &record_len));
    zassert_not_null(record);
    zassert_not_equal(record_len, 0u);
    zassert_equal(gateway_ble_stream_mark_host_notified(&gateway_ble_stream_state),
                  -EPERM);
    gateway_ble_stream_cancel_send(&gateway_ble_stream_state);
    k_spin_unlock(&gateway_ble_stream_lock, key);

    capture_head_host_receipt_identity(&identity, &head);
    zassert_equal(gateway_host_receipt_identity_append_tlv(
                      receipt_payload,
                      sizeof(receipt_payload),
                      &receipt_payload_len,
                      &identity),
                  PROTO_ERR_MALFORMED,
                  "flags=0 telemetry must be unreceiptable by construction");

    key = k_spin_lock(&gateway_ble_stream_lock);
    gateway_ble_stream_mark_sent(&gateway_ble_stream_state, k_uptime_get_32());
    k_spin_unlock(&gateway_ble_stream_lock, key);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);
}

ZTEST(production_seam_gateway_host_recovery,
      test_publisher_receipt_advances_exact_inflight_event)
{
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    struct proto_packet receipt = {0};
    struct proto_packet head = {0};
    uint8_t receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    size_t receipt_payload_len = 0u;

    reset_gateway_owner();
    test_enable_receipt_stream_fixture();
    test_start_reliable_assignment_publisher();

    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
    zassert_ok(gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                              &head));
    zassert_equal(head.msg_type, MSG_GATEWAY_COMMAND_EVENT);
    zassert_equal(head.flags, FLAG_GATEWAY_ACK_REQUIRED);
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    zassert_true(diagnostics.active);
    zassert_not_equal(diagnostics.inflight_event_seq, 0u,
                      "publisher must install ownership before exposure");

    mark_exact_head_host_notified();
    build_exact_host_receipt(&receipt,
                             receipt_payload,
                             sizeof(receipt_payload),
                             &receipt_payload_len);
    k_sched_lock();
    zassert_ok(gateway_ble_accept_host_receipt(&receipt,
                                               receipt_payload,
                                               receipt_payload_len));
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    zassert_equal(diagnostics.inflight_event_seq, 0u);
    zassert_equal(diagnostics.sent_mappings, 1u);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);
    (void)k_work_cancel_delayable(&gateway_persistence_retry_work);
    test_disable_receipt_stream_fixture();
    k_sched_unlock();
}

ZTEST(production_seam_gateway_host_recovery,
      test_local_command_result_receipt_retires_without_mesh_handoff)
{
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = TEST_HOST_ID,
        .dst_id = DEVICE_ID,
        .session_id = UINT32_C(0x22334455),
        .seq = 31u,
        .ttl = 3u,
    };
    struct proto_packet receipt = {0};
    uint8_t receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    size_t receipt_payload_len = 0u;
    uint32_t token = 0u;

    reset_gateway_owner();
    zassert_ok(gateway_command_result_reserve_ingress(&token));
    zassert_ok(gateway_command_result_bind_ingress(
        token, &command, CMD_ASSIGN_DISCOVERY_SLOTS));
    zassert_ok(gateway_commit_host_command_result_reserved(
        token,
        &command,
        CMD_ASSIGN_DISCOVERY_SLOTS,
        COMMAND_OK,
        1u));
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
    /* Queue custody moved into the retained BLE head, so the source queue is
     * empty even though the exact local result remains recoverable there. */
    zassert_equal(app_gateway_command_result_queue_depth(
                      &gateway_host_command_results),
                  0u);
    zassert_true(gateway_local_command_result_valid(
        &gateway_ble_stream_state.items[0].packet,
        &gateway_ble_stream_state.record_pool[
            gateway_ble_stream_state.items[0].offset +
            GATEWAY_BLE_STREAM_RECORD_HEADER_LEN],
        gateway_ble_stream_state.items[0].packet.payload_len));

    atomic_set(&mesh_gateway_host_delivery_pending_state, 1);
    atomic_clear(&mesh_gateway_host_receipt_received_state);
    mark_exact_head_host_notified();
    build_exact_host_receipt(&receipt,
                             receipt_payload,
                             sizeof(receipt_payload),
                             &receipt_payload_len);
    zassert_ok(gateway_ble_accept_host_receipt(&receipt,
                                               receipt_payload,
                                               receipt_payload_len));
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u);
    zassert_true(atomic_get(&mesh_gateway_host_delivery_pending_state) != 0);
    zassert_equal(atomic_get(&mesh_gateway_host_receipt_received_state), 0,
                  "local result receipt must not signal unrelated mesh custody");
    atomic_clear(&mesh_gateway_host_delivery_pending_state);
}

ZTEST(production_seam_gateway_host_recovery,
      test_distinct_mesh_command_result_receipt_stays_with_mesh_owner)
{
    struct proto_packet packet = {0};
    struct proto_packet receipt = {0};
    uint8_t result_payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    uint8_t receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    size_t result_payload_len = 0u;
    size_t receipt_payload_len = 0u;

    reset_gateway_owner();
    test_enable_receipt_stream_fixture();
    test_enqueue_distinct_mesh_result(&packet,
                                      result_payload,
                                      &result_payload_len);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
    k_work_init(&mesh_rx_work, test_gateway_mesh_receipt_work_handler);
    atomic_set(&mesh_gateway_host_delivery_pending_state, 1);
    atomic_clear(&mesh_gateway_host_receipt_received_state);

    mark_exact_head_host_notified();
    build_exact_host_receipt(&receipt,
                             receipt_payload,
                             sizeof(receipt_payload),
                             &receipt_payload_len);
    zassert_ok(gateway_ble_accept_host_receipt(&receipt,
                                               receipt_payload,
                                               receipt_payload_len));
    zassert_true(atomic_get(&mesh_gateway_host_receipt_received_state) != 0,
                 "distinct-endpoint result must enter mesh receipt ownership");
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u,
                  "mesh owner, not BLE, must retire the accepted result");
    zassert_equal(gateway_ble_stream_state.head_send_phase,
                  GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED);

    atomic_clear(&mesh_gateway_host_delivery_pending_state);
    atomic_clear(&mesh_gateway_host_receipt_received_state);
    gateway_ble_stream_init(&gateway_ble_stream_state);
    test_disable_receipt_stream_fixture();
}

ZTEST(production_seam_gateway_host_recovery,
      test_publisher_receipt_cannot_release_unrelated_mesh_stream_head)
{
    struct app_gateway_assignment_publisher_diagnostics diagnostics;
    struct proto_packet mesh_packet = {0};
    struct proto_packet receipt = {0};
    struct proto_packet head = {0};
    uint8_t mesh_payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    uint8_t receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    size_t mesh_payload_len = 0u;
    size_t receipt_payload_len = 0u;

    reset_gateway_owner();
    test_enable_receipt_stream_fixture();
    test_start_reliable_assignment_publisher();
    test_enqueue_distinct_mesh_result(&mesh_packet,
                                      mesh_payload,
                                      &mesh_payload_len);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 2u);
    atomic_set(&mesh_gateway_host_delivery_pending_state, 1);
    atomic_clear(&mesh_gateway_host_receipt_received_state);

    mark_exact_head_host_notified();
    build_exact_host_receipt(&receipt,
                             receipt_payload,
                             sizeof(receipt_payload),
                             &receipt_payload_len);
    k_sched_lock();
    zassert_ok(gateway_ble_accept_host_receipt(&receipt,
                                               receipt_payload,
                                               receipt_payload_len));
    app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
    zassert_equal(diagnostics.inflight_event_seq, 0u);
    zassert_equal(diagnostics.sent_mappings, 1u);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
    zassert_ok(gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                              &head));
    zassert_equal(head.msg_type, MSG_COMMAND_RESULT);
    zassert_equal(head.src_id, TEST_SOURCE_B);
    zassert_true(atomic_get(&mesh_gateway_host_delivery_pending_state) != 0);
    zassert_equal(atomic_get(&mesh_gateway_host_receipt_received_state), 0,
                  "publisher receipt may not hand off an unrelated mesh head");

    atomic_clear(&mesh_gateway_host_delivery_pending_state);
    gateway_ble_stream_init(&gateway_ble_stream_state);
    (void)k_work_cancel_delayable(&gateway_persistence_retry_work);
    test_disable_receipt_stream_fixture();
    k_sched_unlock();
}

ZTEST(production_seam_gateway_host_recovery,
      test_two_stale_packets_wait_for_exact_receipt_c5_recovery_and_retirement)
{
    struct recovery_fixture first;
    struct recovery_fixture second;
    struct proto_packet receipt = {0};
    struct proto_packet first_receipt = {0};
    struct proto_packet head = {0};
    struct mesh_rx_pending queued = {0};
    uint8_t receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    uint8_t first_receipt_payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES] = {0};
    size_t receipt_payload_len = 0u;
    size_t first_receipt_payload_len = 0u;
    struct gateway_command_options blocked_collection = {
        .collection_required = true,
    };

    reset_gateway_owner();
    build_stale_result(&first, TEST_SOURCE_A, 91u, 19u, 44u);
    /* B is a canonical, complete RESULT_BUNDLE, not a header-only stand-in. */
    build_stale_bundle(&second, TEST_SOURCE_B, 92u, 20u, 20u, 45u);

    zassert_equal(gateway_preflight_result_semantic_delivery(
                      &first.packet, first.payload, first.payload_len,
                      k_uptime_get(), 0u, first.packet.src_id),
                  APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY,
                  "canonical stale result must enter reboot recovery");
    queue_received_fixture(&first);
    zassert_ok(k_msgq_peek(&mesh_rx_msgq, &queued));
    zassert_equal(mesh_gateway_preflight_semantic_delivery(&queued),
                  APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY,
                  "queued stale result must preserve reboot-recovery admission");
    zassert_equal(drain_mesh_owner("stale-first"), 1u);
    zassert_true(gateway_collection_recovery.host_custody_pending,
                 "A retry must retain recovery owner: active=%u complete=%u "
                 "next=%u sent=%u deferred=%u busy=%u rx=%u phase=%u paused=%u",
                 gateway_collection_recovery.active ? 1u : 0u,
                 gateway_collection_recovery.flood_progress.complete ? 1u : 0u,
                 gateway_collection_recovery.flood_progress.next_opportunity,
                 gateway_collection_recovery.flood_progress.result.sent_count,
                 gateway_collection_recovery.flood_progress.result.deferred_count,
                 gateway_collection_recovery.flood_progress.result.busy_skip_count,
                 test_dwm_rx_count,
                 (unsigned int)radio_guard_uwb_phase(),
                 mesh_transport_paused() ? 1u : 0u);
    zassert_false(gateway_collection_recovery.active);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
    zassert_true(atomic_get(&mesh_gateway_host_delivery_pending_state) != 0);
    zassert_equal(gateway_command_result_wait_reserve(&(uint32_t){0}), -EBUSY);
    zassert_equal(gateway_begin_command_collection(&blocked_collection), -EBUSY);

    queue_received_fixture(&second);
    zassert_equal(mesh_rx_pending_count(), 1u,
                  "packet B must remain queued behind packet A host custody");
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u,
                  "packet B may not replace packet A's retained BLE head");
    mark_exact_head_host_notified();
    build_exact_host_receipt(&receipt, receipt_payload,
                             sizeof(receipt_payload), &receipt_payload_len);
    first_receipt = receipt;
    memcpy(first_receipt_payload, receipt_payload, receipt_payload_len);
    first_receipt_payload_len = receipt_payload_len;
    accept_receipt_and_run_owner(&receipt, receipt_payload, receipt_payload_len);

    /* B may be admitted only after A's exact receipt ran the full C5
     * recovery and retired A's BLE item. The same owner drain then obtains
     * B's distinct stale-recovery reservation. */
    zassert_true(gateway_collection_recovery.host_custody_pending,
                 "A completion must retain/admit B: active=%u complete=%u "
                 "next=%u sent=%u deferred=%u busy=%u rx=%u phase=%u paused=%u",
                 gateway_collection_recovery.active ? 1u : 0u,
                 gateway_collection_recovery.flood_progress.complete ? 1u : 0u,
                 gateway_collection_recovery.flood_progress.next_opportunity,
                 gateway_collection_recovery.flood_progress.result.sent_count,
                 gateway_collection_recovery.flood_progress.result.deferred_count,
                 gateway_collection_recovery.flood_progress.result.busy_skip_count,
                 test_dwm_rx_count,
                 (unsigned int)radio_guard_uwb_phase(),
                 mesh_transport_paused() ? 1u : 0u);
    zassert_false(gateway_collection_recovery.active);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u,
                  "only packet B may remain after packet A recovery");
    zassert_equal(test_watchdog_stops, 0u);
    zassert_true(test_dwm_tx_count >= app_mesh_flood_repeat_limit(),
                 "real C5 flood did not transmit every required repeat");

    zassert_ok(gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                              &head));
    zassert_equal(head.msg_type, MSG_RESULT_BUNDLE);
    zassert_equal(head.src_id, TEST_SOURCE_B);

    /* A receipt is bound to its complete retained stream record. Once B is
     * the head, replaying A must neither advance B nor generate another C5
     * recovery flood. */
    {
        const uint32_t tx_before_stale_replay = test_dwm_tx_count;

        zassert_equal(gateway_ble_accept_host_receipt(
                          &first_receipt,
                          first_receipt_payload,
                          first_receipt_payload_len),
                      -ESTALE);
        zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 1u);
        zassert_equal(test_dwm_tx_count, tx_before_stale_replay);
        zassert_true(gateway_collection_recovery.host_custody_pending);
    }

    mark_exact_head_host_notified();
    build_exact_host_receipt(&receipt, receipt_payload,
                             sizeof(receipt_payload), &receipt_payload_len);
    mesh_transport_resume();
    accept_receipt_and_run_owner(&receipt, receipt_payload, receipt_payload_len);

    zassert_false(gateway_collection_recovery.host_custody_pending);
    zassert_false(gateway_collection_recovery.active);
    zassert_equal(gateway_ble_stream_depth(&gateway_ble_stream_state), 0u,
                  "packet B BLE custody must retire after its full EACK");
    zassert_true(test_dwm_tx_count >= 2u * app_mesh_flood_repeat_limit(),
                 "both exact stale packets must complete their full C5 flood");
}

ZTEST(production_seam_gateway_host_recovery,
      test_gateway_rx_still_defers_unrelated_c5_flood)
{
    struct mesh_c5_flood_tx_context unrelated = {
        .response_priority = true,
        .collection_recovery_candidate = NULL,
    };

    reset_gateway_owner();

    /* The route-test gateway is in its ordinary continuous-RX coordinator
     * state.  A null/non-frozen candidate must never borrow the recovery
     * exception merely because it is a priority C5 flood. */
    zassert_true(mesh_c5_flood_defer_active_cb(&unrelated));
}

ZTEST_SUITE(production_seam_gateway_host_recovery, NULL, NULL, NULL, NULL,
            NULL);
