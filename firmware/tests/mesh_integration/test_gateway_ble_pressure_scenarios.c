#include "app_gateway_ble_stream.h"
#include "gateway_ble_transport.h"
#include "report.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_EVENT_SEQ UINT32_C(0x10203040)
#define TEST_CLICKER_ID UINT64_C(0xc100000000000002)
#define TEST_GATEWAY_ID UINT64_C(0x9000000000000003)
#define TEST_BLE_MTU UINT16_C(247)
#define TEST_BLE_PRESSURE_PAYLOAD_LEN 8u
#define TEST_BLE_PRESSURE_RECORD_COUNT \
    (GATEWAY_BLE_STREAM_QUEUE_DEPTH + 1u)
#define TEST_BLE_NOTIFY_FAILURE_RESET_THRESHOLD 4u

static struct gateway_ble_stream_state test_stream;
static struct gateway_ble_link test_link;
static const char *test_phase = "startup";

static void fail_at(int line, const char *condition, const char *format, ...)
{
    va_list args;

    fprintf(stderr,
            "FAIL phase=%s line=%d check=(%s): ",
            test_phase,
            line,
            condition);
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

static void run_ble_durable_priority_guard(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diagnostics;
    struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = TEST_GATEWAY_ID,
        .dst_id = TEST_CLICKER_ID,
        .session_id = TEST_EVENT_SEQ,
        .ttl = 4u,
        .payload_len = 2u,
    };
    struct proto_packet click = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_COUNT_AS_CLICK,
        .src_id = TEST_CLICKER_ID,
        .dst_id = TEST_GATEWAY_ID,
        .session_id = TEST_EVENT_SEQ + 1u,
        .seq = 100u,
        .ttl = 4u,
        .payload_len = 2u,
    };
    const uint8_t result_payload[] = {0xa5u, 0x5au};
    const uint8_t click_payload[] = {0x3cu, 0xc3u};
    struct proto_packet head = {0};
    int ret;

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        result.seq = i;
        ret = gateway_ble_stream_enqueue_packet(&state,
                                                &result,
                                                result_payload,
                                                sizeof(result_payload),
                                                10u,
                                                20u + i,
                                                true);
        CHECK(ret == 1,
              "durable result enqueue failed: index=%u ret=%d",
              i,
              ret);
    }
    CHECK(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH,
          "durable result queue did not reach capacity: depth=%u",
          gateway_ble_stream_depth(&state));
    ret = gateway_ble_stream_reserve_packet(&state,
                                            &click,
                                            click_payload,
                                            sizeof(click_payload),
                                            30u,
                                            40u,
                                            true);
    CHECK(ret == -ENOSPC && !state.reservation_active &&
              gateway_ble_stream_depth(&state) ==
                  GATEWAY_BLE_STREAM_QUEUE_DEPTH,
          "click reservation evicted durable results: ret=%d depth=%u "
          "reservation=%u",
          ret,
          gateway_ble_stream_depth(&state),
          state.reservation_active ? 1u : 0u);
    CHECK(gateway_ble_stream_head_packet(&state, &head) == 0 &&
              head.msg_type == MSG_COMMAND_RESULT && head.seq == 0u,
          "durable result FIFO changed after click refusal: msg=0x%02x seq=%u",
          head.msg_type,
          head.seq);
    gateway_ble_stream_get_diagnostics(&state, 40u, &diagnostics);
    CHECK(diagnostics.drops_queue_full == 1u &&
              diagnostics.drops_priority == 0u &&
              diagnostics.last_drop_reason ==
                  GATEWAY_BLE_STREAM_DROP_QUEUE_FULL,
          "durable priority refusal diagnostics mismatch: full=%u priority=%u "
          "last=%d",
          diagnostics.drops_queue_full,
          diagnostics.drops_priority,
          (int)diagnostics.last_drop_reason);
}

static void run_ble_pressure_recovery_scenario(void)
{
    struct gateway_ble_tx_cursor cursor;
    struct gateway_ble_stream_diagnostics diagnostics;
    struct proto_packet packets[TEST_BLE_PRESSURE_RECORD_COUNT];
    uint8_t payloads[TEST_BLE_PRESSURE_RECORD_COUNT]
                    [TEST_BLE_PRESSURE_PAYLOAD_LEN];
    size_t expected_bytes = 0u;
    uint8_t completed = 0u;
    int ret;

    test_phase = "ble-pressure-recovery";
    gateway_ble_stream_init(&test_stream);
    gateway_ble_link_init(&test_link,
                          GATEWAY_BLE_DEFAULT_CONNECTION_INTERVAL_US,
                          1u);
    ret = gateway_ble_link_connect(&test_link, 0u, TEST_BLE_MTU, true);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "pressure link connect failed: ret=%d",
          ret);

    for (size_t i = 0u; i < TEST_BLE_PRESSURE_RECORD_COUNT; i++) {
        packets[i] = (struct proto_packet) {
            .msg_type = MSG_CLICK_REPORT,
            .flags = FLAG_COUNT_AS_CLICK,
            .src_id = TEST_CLICKER_ID + (uint64_t)i,
            .dst_id = TEST_GATEWAY_ID,
            .session_id = TEST_EVENT_SEQ + (uint32_t)i,
            .seq = (uint16_t)(UINT16_C(0x0300) + (uint16_t)i),
            .ttl = 4u,
            .payload_len = TEST_BLE_PRESSURE_PAYLOAD_LEN,
        };
        for (size_t j = 0u; j < TEST_BLE_PRESSURE_PAYLOAD_LEN; j++) {
            payloads[i][j] = (uint8_t)(0x80u +
                                       (uint8_t)(i *
                                                 TEST_BLE_PRESSURE_PAYLOAD_LEN) +
                                       (uint8_t)j);
        }
    }

    for (size_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        ret = gateway_ble_stream_enqueue_packet(
            &test_stream,
            &packets[i],
            payloads[i],
            sizeof(payloads[i]),
            (uint32_t)(10u + i),
            (uint32_t)(20u + i),
            true);
        CHECK(ret == 1,
              "pressure enqueue failed: record=%zu ret=%d depth=%u",
              i,
              ret,
              gateway_ble_stream_depth(&test_stream));
    }
    CHECK(gateway_ble_stream_depth(&test_stream) ==
              GATEWAY_BLE_STREAM_QUEUE_DEPTH,
          "pressure queue did not reach capacity: depth=%u capacity=%u",
          gateway_ble_stream_depth(&test_stream),
          GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    ret = gateway_ble_stream_enqueue_packet(
        &test_stream,
        &packets[GATEWAY_BLE_STREAM_QUEUE_DEPTH],
        payloads[GATEWAY_BLE_STREAM_QUEUE_DEPTH],
        sizeof(payloads[0]),
        13u,
        23u,
        true);
    CHECK(ret == -ENOSPC,
          "full stream accepted an extra record: ret=%d depth=%u",
          ret,
          gateway_ble_stream_depth(&test_stream));

    CHECK(gateway_ble_link_disconnect(&test_link) == 0u,
          "disconnect with no in-flight notification dropped custody");
    ret = gateway_ble_link_connect(&test_link, 0u, TEST_BLE_MTU, false);
    CHECK(ret == GATEWAY_BLE_LINK_OK,
          "CCC-disabled reconnect failed: ret=%d",
          ret);
    CHECK(gateway_ble_link_try_notify(&test_link, 1u) ==
              GATEWAY_BLE_LINK_ERR_NOTIFY_DISABLED,
          "CCC-disabled notify was accepted");
    ret = gateway_ble_stream_enqueue_packet(
        &test_stream,
        &packets[GATEWAY_BLE_STREAM_QUEUE_DEPTH],
        payloads[GATEWAY_BLE_STREAM_QUEUE_DEPTH],
        sizeof(payloads[0]),
        14u,
        24u,
        false);
    CHECK(ret == -ENOTCONN,
          "CCC-disabled full stream returned wrong refusal: ret=%d",
          ret);
    CHECK(gateway_ble_stream_depth(&test_stream) ==
              GATEWAY_BLE_STREAM_QUEUE_DEPTH,
          "CCC-disabled refusal changed queue depth: depth=%u",
          gateway_ble_stream_depth(&test_stream));
    CHECK(gateway_ble_link_disconnect(&test_link) == 0u,
          "CCC-disabled disconnect dropped an unexpected notification");
    CHECK(gateway_ble_link_connect(&test_link, 0u, TEST_BLE_MTU, true) ==
              GATEWAY_BLE_LINK_OK,
          "pressure recovery connect failed");

    for (size_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        struct proto_packet head = {0};
        const uint8_t *record = NULL;
        const uint8_t *chunk = NULL;
        const uint8_t *retry_chunk = NULL;
        size_t record_len = 0u;
        size_t chunk_len = 0u;
        size_t retry_len = 0u;
        uint64_t event_us;

        CHECK(gateway_ble_stream_head_packet(&test_stream, &head) == 0,
              "pressure stream head unavailable: record=%zu depth=%u",
              i,
              gateway_ble_stream_depth(&test_stream));
        CHECK(head.seq == packets[i].seq && head.src_id == packets[i].src_id,
              "stream FIFO changed: record=%zu seq=%u expected=%u src=0x%016llx",
              i,
              head.seq,
              packets[i].seq,
              (unsigned long long)head.src_id);
        ret = gateway_ble_stream_begin_send_view(&test_stream,
                                                 &record,
                                                 &record_len);
        CHECK(ret == 0,
              "pressure stream send begin failed: record=%zu ret=%d",
              i,
              ret);
        CHECK(record_len == GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                                TEST_BLE_PRESSURE_PAYLOAD_LEN &&
                  proto_get_u16_le(&record[10]) == packets[i].seq &&
                  proto_get_u16_le(&record[36]) ==
                      TEST_BLE_PRESSURE_PAYLOAD_LEN &&
                  memcmp(&record[GATEWAY_BLE_STREAM_RECORD_HEADER_LEN],
                         payloads[i],
                         TEST_BLE_PRESSURE_PAYLOAD_LEN) == 0,
              "stream record bytes changed: record=%zu len=%zu seq=%u",
              i,
              record_len,
              proto_get_u16_le(&record[10]));
        gateway_ble_tx_cursor_init(&cursor,
                                   record,
                                   record_len,
                                   test_link.negotiated_mtu);
        CHECK(gateway_ble_tx_cursor_begin(&cursor, &chunk, &chunk_len) ==
                  PROTO_OK &&
                  chunk == record && chunk_len == record_len,
              "pressure ATT chunk mismatch: record=%zu chunk=%zu frame=%zu",
              i,
              chunk_len,
              record_len);
        ret = gateway_ble_tx_cursor_begin(&cursor, &retry_chunk, &retry_len);
        CHECK(ret == PROTO_ERR_BUSY,
              "busy ATT cursor accepted a second submit: record=%zu ret=%d",
              i,
              ret);
        CHECK(gateway_ble_link_try_notify(&test_link, chunk_len) ==
                  GATEWAY_BLE_LINK_OK,
              "pressure notify submit failed: record=%zu credits=%u",
              i,
              test_link.available_credits);

        if (i == 0u) {
            CHECK(gateway_ble_link_try_notify(&test_link, 1u) ==
                      GATEWAY_BLE_LINK_ERR_NO_CREDIT,
                  "credit refusal missing while first record is in flight");
            CHECK(gateway_ble_link_disconnect(&test_link) == 1u,
                  "disconnect did not reject first in-flight record");
            CHECK(cursor.offset == 0u && cursor.in_flight,
                  "disconnect advanced first cursor: offset=%zu in_flight=%u",
                  cursor.offset,
                  cursor.in_flight ? 1u : 0u);
            CHECK(gateway_ble_tx_cursor_complete(&cursor, false) == PROTO_OK &&
                      cursor.offset == 0u && !cursor.in_flight,
                  "rejected first record advanced cursor: offset=%zu",
                  cursor.offset);
            CHECK(gateway_ble_link_connect(&test_link, 0u, TEST_BLE_MTU, true) ==
                      GATEWAY_BLE_LINK_OK,
                  "first record retry reconnect failed");
            CHECK(gateway_ble_tx_cursor_begin(&cursor,
                                              &retry_chunk,
                                              &retry_len) == PROTO_OK &&
                      retry_chunk == chunk && retry_len == chunk_len,
                  "disconnect retry changed first record chunk: len=%zu",
                  retry_len);
            CHECK(gateway_ble_link_try_notify(&test_link, retry_len) ==
                      GATEWAY_BLE_LINK_OK,
                  "disconnect retry notify failed");
        } else if (i == 1u) {
            gateway_ble_link_set_stalled(&test_link, true);
        }

        event_us = test_link.next_event_us;
        CHECK(event_us > 0u,
              "pressure link did not schedule completion: record=%zu",
              i);
        CHECK(gateway_ble_link_run_connection_event(&test_link,
                                                    event_us,
                                                    &completed) ==
                  GATEWAY_BLE_LINK_OK,
              "pressure connection event failed: record=%zu event=%llu",
              i,
              (unsigned long long)event_us);
        if (i == 1u) {
            struct proto_packet retained_head = {0};
            const uint8_t *retained_record = NULL;
            size_t retained_record_len = 0u;

            CHECK(completed == 0u && test_link.available_credits == 0u,
                  "stalled central unexpectedly completed record: completed=%u credits=%u",
                  completed,
                  test_link.available_credits);
            CHECK(gateway_ble_link_try_notify(&test_link, 1u) ==
                      GATEWAY_BLE_LINK_ERR_NO_CREDIT,
                  "stalled central did not refuse a retryable notify");

            /*
             * A real transport must bound this state.  Model its deadline
             * owner by resetting the link, rejecting the cursor completion,
             * and cancelling the stream view.  Durable custody must remain
             * at the exact queue head and restart at byte zero.
             */
            CHECK(gateway_ble_link_disconnect(&test_link) == 1u,
                  "stalled-link deadline did not reject its notification");
            CHECK(gateway_ble_tx_cursor_complete(&cursor, false) == PROTO_OK &&
                      cursor.offset == 0u && !cursor.in_flight,
                  "stalled-link reset advanced the cursor: offset=%zu",
                  cursor.offset);
            gateway_ble_stream_cancel_send(&test_stream);
            CHECK(gateway_ble_stream_head_packet(&test_stream,
                                                 &retained_head) == 0 &&
                      retained_head.seq == packets[i].seq &&
                      retained_head.src_id == packets[i].src_id,
                  "stalled-link reset changed the durable queue head");
            CHECK(gateway_ble_stream_begin_send_view(&test_stream,
                                                     &retained_record,
                                                     &retained_record_len) == 0 &&
                      retained_record_len == record_len &&
                      memcmp(retained_record, record, record_len) == 0,
                  "stalled-link reset changed the retained record bytes");
            CHECK(gateway_ble_link_connect(&test_link,
                                           event_us,
                                           TEST_BLE_MTU,
                                           true) == GATEWAY_BLE_LINK_OK,
                  "stalled-link reconnect failed");
            CHECK(gateway_ble_tx_cursor_begin(&cursor,
                                              &retry_chunk,
                                              &retry_len) == PROTO_OK &&
                      retry_chunk == chunk && retry_len == chunk_len,
                  "stalled-link retry did not restart the first chunk");
            CHECK(gateway_ble_link_try_notify(&test_link, retry_len) ==
                      GATEWAY_BLE_LINK_OK,
                  "stalled-link retry notify failed");
            CHECK(gateway_ble_link_run_connection_event(&test_link,
                                                        test_link.next_event_us,
                                                        &completed) ==
                      GATEWAY_BLE_LINK_OK &&
                      completed == 1u,
                  "credits did not return after central resumed: completed=%u",
                  completed);
        } else {
            CHECK(completed == 1u,
                  "pressure record did not complete: record=%zu completed=%u",
                  i,
                  completed);
        }
        CHECK(gateway_ble_tx_cursor_complete(&cursor, true) == PROTO_OK &&
                  gateway_ble_tx_cursor_done(&cursor),
              "pressure cursor did not retire: record=%zu offset=%zu len=%zu",
              i,
              cursor.offset,
              cursor.frame_len);
        expected_bytes += record_len;
        gateway_ble_stream_mark_sent(
            &test_stream,
            (uint32_t)(test_link.next_event_us / 1000u));
    }

    gateway_ble_stream_get_diagnostics(&test_stream,
                                       (uint32_t)(test_link.next_event_us / 1000u),
                                       &diagnostics);
    CHECK(gateway_ble_stream_depth(&test_stream) == 0u &&
              test_stream.pool_used == 0u &&
                  test_stream.head_send_phase ==
                      GATEWAY_BLE_STREAM_HEAD_IDLE,
          "pressure stream did not drain: depth=%u pool=%u active=%u",
          gateway_ble_stream_depth(&test_stream),
          test_stream.pool_used,
          test_stream.head_send_phase != GATEWAY_BLE_STREAM_HEAD_IDLE ? 1u :
                                                                         0u);
    CHECK(diagnostics.enqueue_attempts ==
              GATEWAY_BLE_STREAM_QUEUE_DEPTH + 2u &&
              diagnostics.packets_sent == GATEWAY_BLE_STREAM_QUEUE_DEPTH &&
              diagnostics.bytes_sent == expected_bytes &&
              diagnostics.drops_queue_full == 1u &&
              diagnostics.drops_not_ready == 1u &&
              diagnostics.drops_too_large == 0u &&
              diagnostics.drops_priority == 0u &&
              diagnostics.max_queue_depth_observed ==
                  GATEWAY_BLE_STREAM_QUEUE_DEPTH &&
              diagnostics.last_drop_reason ==
                  GATEWAY_BLE_STREAM_DROP_NOT_READY,
          "pressure diagnostics mismatch: attempts=%u sent=%u bytes=%u "
          "expected_bytes=%zu full=%u not_ready=%u max_depth=%u last=%d",
          diagnostics.enqueue_attempts,
          diagnostics.packets_sent,
          diagnostics.bytes_sent,
          expected_bytes,
          diagnostics.drops_queue_full,
          diagnostics.drops_not_ready,
          diagnostics.max_queue_depth_observed,
          (int)diagnostics.last_drop_reason);
    CHECK(test_link.notifications_dropped_disconnect == 2u &&
              test_link.notifications_submitted ==
                  GATEWAY_BLE_STREAM_QUEUE_DEPTH + 2u &&
              test_link.notifications_completed ==
                  GATEWAY_BLE_STREAM_QUEUE_DEPTH &&
              test_link.connected && test_link.notify_enabled &&
              test_link.available_credits == 1u && test_link.in_flight == 0u,
          "pressure BLE accounting mismatch: dropped=%u submitted=%u "
          "completed=%u connected=%u notify=%u credits=%u inflight=%u",
          test_link.notifications_dropped_disconnect,
          test_link.notifications_submitted,
          test_link.notifications_completed,
          test_link.connected ? 1u : 0u,
          test_link.notify_enabled ? 1u : 0u,
          test_link.available_credits,
          test_link.in_flight);
    CHECK(test_link.connection_generation == 5u,
          "pressure BLE generation mismatch: actual=%u expected=5",
          test_link.connection_generation);
}

static void run_ble_repeated_submit_failure_reset_scenario(void)
{
    struct gateway_ble_stream_state stream;
    struct gateway_ble_link link;
    struct gateway_ble_tx_cursor cursor;
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = TEST_GATEWAY_ID,
        .dst_id = TEST_CLICKER_ID,
        .session_id = TEST_EVENT_SEQ,
        .seq = UINT16_C(0x4567),
        .ttl = 4u,
        .payload_len = TEST_BLE_PRESSURE_PAYLOAD_LEN,
    };
    const uint8_t payload[TEST_BLE_PRESSURE_PAYLOAD_LEN] = {
        0x01u, 0x23u, 0x45u, 0x67u,
        0x89u, 0xabu, 0xcdu, 0xefu,
    };
    const uint8_t *record = NULL;
    const uint8_t *chunk = NULL;
    const uint8_t *retry_record = NULL;
    const uint8_t *retry_chunk = NULL;
    size_t record_len = 0u;
    size_t chunk_len = 0u;
    size_t retry_record_len = 0u;
    size_t retry_chunk_len = 0u;
    uint8_t completed = 0u;
    int ret;

    test_phase = "ble-submit-failure-reset";
    gateway_ble_stream_init(&stream);
    gateway_ble_link_init(&link,
                          GATEWAY_BLE_DEFAULT_CONNECTION_INTERVAL_US,
                          1u);
    CHECK(gateway_ble_link_connect(&link, 0u, TEST_BLE_MTU, true) ==
              GATEWAY_BLE_LINK_OK,
          "submit-failure link connect failed");
    CHECK(gateway_ble_stream_enqueue_retained_packet(&stream,
                                                     &packet,
                                                     payload,
                                                     sizeof(payload),
                                                     10u,
                                                     20u,
                                                     true) == 1,
          "submit-failure retained enqueue failed");
    CHECK(gateway_ble_stream_begin_send_view(&stream,
                                             &record,
                                             &record_len) == 0,
          "submit-failure stream begin failed");
    gateway_ble_tx_cursor_init(&cursor,
                               record,
                               record_len,
                               link.negotiated_mtu);

    /*
     * A synchronous controller refusal leaves no callback owner.  Each
     * refusal must reject rather than advance the immutable cursor, and a
     * finite failure threshold must reset the link instead of retrying one
     * connection forever.
     */
    link.available_credits = 0u;
    for (uint8_t failure = 0u;
         failure < TEST_BLE_NOTIFY_FAILURE_RESET_THRESHOLD;
         failure++) {
        CHECK(gateway_ble_tx_cursor_begin(&cursor,
                                          &chunk,
                                          &chunk_len) == PROTO_OK,
              "submit-failure cursor begin failed: failure=%u",
              failure);
        CHECK(gateway_ble_link_try_notify(&link, chunk_len) ==
                  GATEWAY_BLE_LINK_ERR_NO_CREDIT,
              "submit-failure controller refusal missing: failure=%u",
              failure);
        CHECK(gateway_ble_tx_cursor_complete(&cursor, false) == PROTO_OK &&
                  cursor.offset == 0u && !cursor.in_flight,
              "submit-failure advanced retained cursor: failure=%u offset=%zu",
              failure,
              cursor.offset);
    }

    CHECK(gateway_ble_link_disconnect(&link) == 0u,
          "submit-failure reset invented an in-flight notification");
    gateway_ble_stream_cancel_send(&stream);
    CHECK(gateway_ble_stream_depth(&stream) == 1u &&
              stream.pool_used == record_len &&
                  stream.head_send_phase == GATEWAY_BLE_STREAM_HEAD_IDLE,
          "submit-failure reset released durable custody: depth=%u pool=%u active=%u",
          gateway_ble_stream_depth(&stream),
          stream.pool_used,
          stream.head_send_phase != GATEWAY_BLE_STREAM_HEAD_IDLE ? 1u : 0u);
    CHECK(gateway_ble_link_connect(&link, 1000u, TEST_BLE_MTU, true) ==
              GATEWAY_BLE_LINK_OK,
          "submit-failure reconnect failed");
    CHECK(gateway_ble_stream_begin_send_view(&stream,
                                             &retry_record,
                                             &retry_record_len) == 0 &&
              retry_record_len == record_len &&
              memcmp(retry_record, record, record_len) == 0,
          "submit-failure reconnect changed retained bytes");
    gateway_ble_tx_cursor_init(&cursor,
                               retry_record,
                               retry_record_len,
                               link.negotiated_mtu);
    CHECK(gateway_ble_tx_cursor_begin(&cursor,
                                      &retry_chunk,
                                      &retry_chunk_len) == PROTO_OK &&
              retry_chunk_len == chunk_len &&
              memcmp(retry_chunk, chunk, chunk_len) == 0,
          "submit-failure reconnect did not restart at byte zero");
    CHECK(gateway_ble_link_try_notify(&link, retry_chunk_len) ==
              GATEWAY_BLE_LINK_OK,
          "submit-failure retry notify failed");
    CHECK(gateway_ble_link_run_connection_event(&link,
                                                link.next_event_us,
                                                &completed) ==
                  GATEWAY_BLE_LINK_OK &&
              completed == 1u,
          "submit-failure retry did not complete");
    CHECK(gateway_ble_tx_cursor_complete(&cursor, true) == PROTO_OK &&
              gateway_ble_tx_cursor_done(&cursor),
          "submit-failure retry cursor did not complete");
    gateway_ble_stream_mark_sent(&stream,
                                 (uint32_t)(link.next_event_us / 1000u));
    CHECK(gateway_ble_stream_depth(&stream) == 0u &&
              stream.pool_used == 0u &&
              link.notifications_submitted == 1u &&
              link.notifications_completed == 1u &&
              link.connection_generation == 2u,
          "submit-failure recovery accounting mismatch: depth=%u pool=%u submitted=%u completed=%u generation=%u",
          gateway_ble_stream_depth(&stream),
          stream.pool_used,
          link.notifications_submitted,
          link.notifications_completed,
          link.connection_generation);
}


int main(void)
{
    run_ble_durable_priority_guard();
    run_ble_pressure_recovery_scenario();
    run_ble_repeated_submit_failure_reset_scenario();
    puts("PASS gateway_ble_pressure_recovery");
    return EXIT_SUCCESS;
}
