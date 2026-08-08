#include "app_gateway_ble_stream.h"
#include "survey.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_GATEWAY_ID UINT64_C(0x9000000000000001)
#define TEST_OPERATION_SESSION UINT32_C(0x456789ab)
#define TEST_PAYLOAD_LEN 8u
#define TEST_DISCONNECT_MS (SURVEY_PAIR_RESULT_FLOW_CONTROL_GUARD_MS / 2u)

struct producer_record {
    struct proto_packet packet;
    uint8_t payload[TEST_PAYLOAD_LEN];
    uint16_t ack_count;
    uint16_t gui_notification_count;
    uint16_t gui_receipt_count;
    uint16_t retired_count;
    bool source_owned;
};

struct gateway_flow_model {
    struct gateway_ble_stream_state stream;
    uint16_t semantic_apply_count;
    uint16_t busy_count;
};

static struct producer_record records[SURVEY_PAIR_RESULT_MAX_BURST_RECORDS];

static void fail_at(int line, const char *condition)
{
    fprintf(stderr, "FAIL line=%d check=(%s)\n", line, condition);
    exit(EXIT_FAILURE);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fail_at(__LINE__, #condition); \
        } \
    } while (0)

static bool packet_identity_equal(const struct proto_packet *left,
                                  const struct proto_packet *right)
{
    return left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->ttl == right->ttl &&
           left->payload_len == right->payload_len;
}

static bool gateway_ram_record_matches(
    const struct gateway_flow_model *gateway,
    const struct producer_record *record)
{
    struct proto_packet head = {0};
    const uint8_t *encoded = NULL;
    size_t encoded_len = 0u;

    return gateway != NULL && record != NULL &&
           gateway_ble_stream_depth(&gateway->stream) == 1u &&
           gateway_ble_stream_head_packet(&gateway->stream, &head) == 0 &&
           packet_identity_equal(&head, &record->packet) &&
           gateway_ble_stream_peek(&gateway->stream,
                                   &encoded,
                                   &encoded_len) == 0 &&
           encoded != NULL &&
           encoded_len == GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                              record->packet.payload_len &&
           memcmp(&encoded[GATEWAY_BLE_STREAM_RECORD_HEADER_LEN],
                  record->payload,
                  record->packet.payload_len) == 0;
}

static size_t copy_gateway_ram_record(
    const struct gateway_flow_model *gateway,
    uint8_t *destination,
    size_t destination_cap)
{
    const uint8_t *encoded = NULL;
    size_t encoded_len = 0u;

    CHECK(gateway_ble_stream_peek(&gateway->stream,
                                  &encoded,
                                  &encoded_len) == 0);
    CHECK(encoded != NULL && encoded_len <= destination_cap);
    memcpy(destination, encoded, encoded_len);
    return encoded_len;
}

/* Admit one source packet into the bounded gateway RAM stream. Admission is
 * deliberately not semantic acceptance: the source still owns the packet
 * until the exact GUI receipt is accepted. */
static int gateway_accept_source(struct gateway_flow_model *gateway,
                                 struct producer_record *record,
                                 uint32_t now_ms)
{
    int ret;

    if (gateway->stream.count > 0u) {
        if (gateway_ram_record_matches(gateway, record)) {
            /* An exact source retry sees its existing RAM record. */
            return 0;
        }
        gateway->busy_count++;
        return -EBUSY;
    }

    ret = gateway_ble_stream_reserve_packet(&gateway->stream,
                                            &record->packet,
                                            record->payload,
                                            sizeof(record->payload),
                                            now_ms,
                                            now_ms,
                                            false);
    if (ret != 1) {
        return ret;
    }
    ret = gateway_ble_stream_commit_reservation(&gateway->stream,
                                                &record->packet,
                                                record->payload,
                                                sizeof(record->payload));
    CHECK(ret == 1);
    return 1;
}

/* Model the notification-complete callback. The stream remains in RAM and
 * becomes HOST_NOTIFIED; only a later exact GUI receipt may advance it. */
static void gateway_notify_gui(struct gateway_flow_model *gateway,
                               struct producer_record *record)
{
    const uint8_t *encoded = NULL;
    size_t encoded_len = 0u;

    CHECK(gateway_ram_record_matches(gateway, record));
    CHECK(gateway_ble_stream_begin_send_view(&gateway->stream,
                                             &encoded,
                                             &encoded_len) == 0);
    CHECK(encoded != NULL &&
              encoded_len == GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                                  sizeof(record->payload));
    /* app_gateway_ble marks this phase when all ATT chunks complete. */
    CHECK(gateway_ble_stream_mark_host_notified(&gateway->stream) == 0);
    record->gui_notification_count++;
}

static int gateway_accept_gui_receipt(struct gateway_flow_model *gateway,
                                      struct producer_record *record)
{
    int ret;

    if (!gateway_ram_record_matches(gateway, record)) {
        return -ESTALE;
    }
    ret = gateway_ble_stream_accept_host_receipt(&gateway->stream);
    if (ret != 0) {
        return ret;
    }

    /* These are all receipt-gated side effects. */
    gateway->semantic_apply_count++;
    record->gui_receipt_count++;
    record->ack_count++;
    record->source_owned = false;
    return 0;
}

static int gateway_retire_after_receipt(struct gateway_flow_model *gateway,
                                        struct producer_record *record,
                                        uint32_t now_ms)
{
    if (!gateway_ram_record_matches(gateway, record)) {
        return -ESTALE;
    }
    if (gateway->stream.head_send_phase !=
        GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED) {
        return -EAGAIN;
    }

    gateway_ble_stream_mark_sent(&gateway->stream, now_ms);
    record->retired_count++;
    return 0;
}

static void gateway_disconnect_before_receipt(
    struct gateway_flow_model *gateway)
{
    CHECK(gateway->stream.head_send_phase ==
          GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED);
    CHECK(gateway_ble_stream_rewind_host_notification(&gateway->stream) == 0);
    CHECK(gateway->stream.head_send_phase == GATEWAY_BLE_STREAM_HEAD_IDLE);
}

/* RAM custody is intentionally lost across reset. No record survives in flash;
 * the still-owning source must retry and reconstruct the stream item. */
static void gateway_reset(struct gateway_flow_model *gateway)
{
    gateway_ble_stream_init(&gateway->stream);
}

static void initialize_burst(void)
{
    for (size_t i = 0u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        records[i].packet = (struct proto_packet) {
            .msg_type = MSG_SURVEY_PAIR_RESULT,
            .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
            .src_id = UINT64_C(0xa000000000000000) +
                      (i / SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) + 1u,
            .dst_id = TEST_GATEWAY_ID,
            .session_id = TEST_OPERATION_SESSION,
            .seq = (uint16_t)(i + 1u),
            .ttl = SURVEY_DEFAULT_TTL,
            .payload_len = TEST_PAYLOAD_LEN,
        };
        proto_put_u32_le(&records[i].payload[0], (uint32_t)i);
        proto_put_u32_le(&records[i].payload[4],
                         UINT32_C(0x5a5a0000) | (uint32_t)i);
        records[i].ack_count = 0u;
        records[i].gui_notification_count = 0u;
        records[i].gui_receipt_count = 0u;
        records[i].retired_count = 0u;
        records[i].source_owned = true;
    }
}

static void run_maximum_burst_ram_receipt_flow(void)
{
    struct gateway_flow_model gateway = {0};
    struct producer_record conflicting = {0};
    uint8_t first_encoded[GATEWAY_BLE_STREAM_RECORD_MAX_LEN] = {0};
    uint8_t retry_encoded[GATEWAY_BLE_STREAM_RECORD_MAX_LEN] = {0};
    size_t first_encoded_len;
    size_t retry_encoded_len;
    uint32_t now_ms = 0u;

    CHECK(SURVEY_PAIR_RESULT_MAX_BURST_RECORDS == 250u);
    CHECK(SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS ==
          (SURVEY_PAIR_RESULT_MAX_BURST_RECORDS *
               SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS) +
              SURVEY_PAIR_RESULT_FLOW_CONTROL_GUARD_MS);
    initialize_burst();
    gateway_ble_stream_init(&gateway.stream);

    CHECK(gateway_accept_source(&gateway, &records[0], now_ms) == 1);
    CHECK(gateway.semantic_apply_count == 0u);
    CHECK(records[0].source_owned && records[0].ack_count == 0u);

    /* A duplicate source retry is idempotent while the exact RAM item lives. */
    CHECK(gateway_accept_source(&gateway, &records[0], now_ms) == 0);
    CHECK(gateway.semantic_apply_count == 0u);
    CHECK(records[0].source_owned && records[0].ack_count == 0u);
    conflicting = records[0];
    conflicting.payload[0] ^= 0xffu;
    CHECK(gateway_accept_source(&gateway, &conflicting, now_ms) == -EBUSY);
    CHECK(gateway.semantic_apply_count == 0u);

    /* Every different source remains owner-held behind the one RAM item. */
    for (size_t i = 1u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        CHECK(gateway_accept_source(&gateway, &records[i], now_ms) == -EBUSY);
        CHECK(records[i].source_owned);
        CHECK(records[i].ack_count == 0u);
    }
    CHECK(gateway.semantic_apply_count == 0u);

    gateway_notify_gui(&gateway, &records[0]);
    CHECK(records[0].gui_notification_count == 1u);
    CHECK(records[0].source_owned && records[0].ack_count == 0u);
    CHECK(gateway.semantic_apply_count == 0u);
    first_encoded_len = copy_gateway_ram_record(&gateway,
                                                first_encoded,
                                                sizeof(first_encoded));

    /* Retirement before receipt is rejected and leaves source custody intact. */
    CHECK(gateway_retire_after_receipt(&gateway, &records[0], now_ms) ==
          -EAGAIN);
    CHECK(gateway_ble_stream_depth(&gateway.stream) == 1u);
    CHECK(records[0].source_owned);

    /* Disconnect rewinds the phase and resends the same bounded RAM bytes. */
    gateway_disconnect_before_receipt(&gateway);
    now_ms += TEST_DISCONNECT_MS;
    gateway_notify_gui(&gateway, &records[0]);
    retry_encoded_len = copy_gateway_ram_record(&gateway,
                                                retry_encoded,
                                                sizeof(retry_encoded));
    CHECK(retry_encoded_len == first_encoded_len);
    CHECK(memcmp(first_encoded, retry_encoded, first_encoded_len) == 0);
    CHECK(records[0].gui_notification_count == 2u);
    CHECK(records[0].source_owned && records[0].ack_count == 0u);

    /* Reset drops the RAM item. The source retry reconstructs it from custody. */
    gateway_reset(&gateway);
    CHECK(gateway_ble_stream_depth(&gateway.stream) == 0u);
    CHECK(records[0].source_owned && records[0].ack_count == 0u);
    CHECK(gateway.semantic_apply_count == 0u);
    CHECK(gateway_accept_source(&gateway, &records[0], now_ms) == 1);
    CHECK(gateway.semantic_apply_count == 0u);

    gateway_notify_gui(&gateway, &records[0]);
    CHECK(records[0].gui_notification_count == 3u);
    CHECK(records[0].source_owned && records[0].ack_count == 0u);
    CHECK(gateway_retire_after_receipt(&gateway, &records[0], now_ms) ==
          -EAGAIN);
    CHECK(gateway_accept_gui_receipt(&gateway, &records[0]) == 0);
    CHECK(gateway.semantic_apply_count == 1u);
    CHECK(records[0].gui_receipt_count == 1u);
    CHECK(records[0].ack_count == 1u);
    CHECK(!records[0].source_owned);

    /* A duplicate source frame cannot reapply an already receipt-gated item. */
    CHECK(gateway_accept_source(&gateway, &records[0], now_ms) == 0);
    CHECK(gateway.semantic_apply_count == 1u);
    CHECK(records[0].ack_count == 1u);

    /* A duplicate GUI receipt is harmless and cannot reapply or re-ACK. */
    CHECK(gateway_accept_gui_receipt(&gateway, &records[0]) == -EALREADY);
    CHECK(gateway.semantic_apply_count == 1u);
    CHECK(records[0].gui_receipt_count == 1u);
    CHECK(records[0].ack_count == 1u);
    CHECK(gateway_retire_after_receipt(&gateway, &records[0], now_ms) == 0);
    CHECK(records[0].retired_count == 1u);
    CHECK(gateway_ble_stream_depth(&gateway.stream) == 0u);

    for (size_t i = 1u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        now_ms += SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS;
        CHECK(gateway_accept_source(&gateway, &records[i], now_ms) == 1);
        CHECK(gateway.semantic_apply_count == i);
        CHECK(records[i].source_owned && records[i].ack_count == 0u);

        if (i + 1u < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS) {
            CHECK(gateway_accept_source(&gateway,
                                        &records[i + 1u],
                                        now_ms) == -EBUSY);
            CHECK(records[i + 1u].source_owned);
            CHECK(records[i + 1u].ack_count == 0u);
        }

        gateway_notify_gui(&gateway, &records[i]);
        CHECK(records[i].source_owned && records[i].ack_count == 0u);
        CHECK(gateway.semantic_apply_count == i);
        CHECK(gateway_retire_after_receipt(&gateway, &records[i], now_ms) ==
              -EAGAIN);
        CHECK(gateway_accept_gui_receipt(&gateway, &records[i]) == 0);
        CHECK(gateway.semantic_apply_count == i + 1u);
        CHECK(records[i].gui_receipt_count == 1u);
        CHECK(records[i].ack_count == 1u);
        CHECK(!records[i].source_owned);
        CHECK(gateway_retire_after_receipt(&gateway, &records[i], now_ms) ==
              0);
        CHECK(records[i].retired_count == 1u);
    }

    CHECK(now_ms < SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS);
    CHECK(gateway.semantic_apply_count ==
          SURVEY_PAIR_RESULT_MAX_BURST_RECORDS);
    CHECK(gateway.busy_count ==
          2u * (SURVEY_PAIR_RESULT_MAX_BURST_RECORDS - 1u));
    CHECK(gateway_ble_stream_depth(&gateway.stream) == 0u);
    for (size_t i = 0u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        CHECK(!records[i].source_owned);
        CHECK(records[i].ack_count == 1u);
        CHECK(records[i].gui_receipt_count == 1u);
        CHECK(records[i].gui_notification_count == (i == 0u ? 3u : 1u));
        CHECK(records[i].retired_count == 1u);
    }
}

int main(void)
{
    run_maximum_burst_ram_receipt_flow();
    puts("PASS gateway_survey_host_flow_control");
    return EXIT_SUCCESS;
}
