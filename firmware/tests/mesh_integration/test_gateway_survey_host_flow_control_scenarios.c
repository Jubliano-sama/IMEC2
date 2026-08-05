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
    uint16_t host_count;
    bool source_owned;
};

struct gateway_flow_model {
    struct gateway_ble_stream_state stream;
    struct proto_packet journal_packet;
    uint8_t journal_payload[TEST_PAYLOAD_LEN];
    uint16_t semantic_accept_count;
    uint16_t busy_count;
    bool journal_valid;
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
           left->payload_len == right->payload_len;
}

static bool journal_matches(const struct gateway_flow_model *gateway,
                            const struct producer_record *record)
{
    return gateway->journal_valid &&
           packet_identity_equal(&gateway->journal_packet, &record->packet) &&
           memcmp(gateway->journal_payload,
                  record->payload,
                  sizeof(record->payload)) == 0;
}

static int gateway_try_accept(struct gateway_flow_model *gateway,
                              struct producer_record *record,
                              uint32_t now_ms)
{
    int ret;

    if (gateway->journal_valid) {
        if (!journal_matches(gateway, record)) {
            gateway->busy_count++;
            return -EBUSY;
        }
        record->ack_count++;
        record->source_owned = false;
        return 0;
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
    gateway->journal_packet = record->packet;
    memcpy(gateway->journal_payload,
           record->payload,
           sizeof(gateway->journal_payload));
    gateway->journal_valid = true;
    gateway->semantic_accept_count++;
    ret = gateway_ble_stream_commit_reservation(&gateway->stream,
                                                &record->packet,
                                                record->payload,
                                                sizeof(record->payload));
    if (ret != 1) {
        return ret;
    }
    record->ack_count++;
    record->source_owned = false;
    return 0;
}

static void gateway_reset_restore(struct gateway_flow_model *gateway,
                                  uint32_t now_ms)
{
    struct gateway_ble_stream_state restored;
    int ret;

    gateway_ble_stream_init(&restored);
    if (gateway->journal_valid) {
        ret = gateway_ble_stream_enqueue_retained_packet(
            &restored,
            &gateway->journal_packet,
            gateway->journal_payload,
            sizeof(gateway->journal_payload),
            now_ms,
            now_ms,
            false);
        CHECK(ret == 1);
    }
    gateway->stream = restored;
}

static void gateway_notify_and_retire(struct gateway_flow_model *gateway,
                                      uint32_t now_ms)
{
    struct proto_packet head = {0};
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    size_t record_index;

    CHECK(gateway->journal_valid);
    CHECK(gateway_ble_stream_depth(&gateway->stream) == 1u);
    CHECK(gateway_ble_stream_head_packet(&gateway->stream, &head) == 0);
    CHECK(packet_identity_equal(&head, &gateway->journal_packet));
    CHECK(gateway_ble_stream_begin_send_view(&gateway->stream,
                                             &record,
                                             &record_len) == 0);
    CHECK(record != NULL);
    CHECK(record_len >= GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                            sizeof(gateway->journal_payload));
    record_index = (size_t)(gateway->journal_packet.seq - 1u);
    CHECK(record_index < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS);
    records[record_index].host_count++;
    gateway_ble_stream_mark_sent(&gateway->stream, now_ms);
    CHECK(gateway_ble_stream_depth(&gateway->stream) == 0u);
    gateway->journal_valid = false;
    memset(&gateway->journal_packet, 0, sizeof(gateway->journal_packet));
    memset(gateway->journal_payload, 0, sizeof(gateway->journal_payload));
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
        records[i].host_count = 0u;
        records[i].source_owned = true;
    }
}

static void run_maximum_burst_reset_and_backpressure(void)
{
    struct gateway_flow_model gateway = {0};
    uint32_t now_ms = 0u;

    CHECK(SURVEY_PAIR_RESULT_MAX_BURST_RECORDS == 200u);
    CHECK(SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS ==
          (SURVEY_PAIR_RESULT_MAX_BURST_RECORDS *
               SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS) +
              SURVEY_PAIR_RESULT_FLOW_CONTROL_GUARD_MS);
    initialize_burst();
    gateway_ble_stream_init(&gateway.stream);

    CHECK(gateway_try_accept(&gateway, &records[0], now_ms) == 0);
    CHECK(gateway.semantic_accept_count == 1u);
    CHECK(!records[0].source_owned);

    /*
     * A disconnected host retains the first exact journal owner. Every
     * different producer remains unacknowledged and unchanged at its source.
     */
    for (size_t i = 1u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        CHECK(gateway_try_accept(&gateway, &records[i], now_ms) == -EBUSY);
        CHECK(records[i].source_owned);
        CHECK(records[i].ack_count == 0u);
    }
    CHECK(gateway.semantic_accept_count == 1u);

    /*
     * Reset restores the exact committed head. Its retry is ACK-sticky but
     * cannot append another host record or repeat the semantic mutation.
     */
    gateway_reset_restore(&gateway, now_ms);
    CHECK(gateway_ble_stream_depth(&gateway.stream) == 1u);
    CHECK(gateway_try_accept(&gateway, &records[0], now_ms) == 0);
    CHECK(gateway.semantic_accept_count == 1u);
    CHECK(records[0].ack_count == 2u);

    now_ms += TEST_DISCONNECT_MS;
    gateway_notify_and_retire(&gateway, now_ms);

    for (size_t i = 1u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        CHECK(gateway_try_accept(&gateway, &records[i], now_ms) == 0);
        CHECK(gateway.semantic_accept_count == i + 1u);
        if (i + 1u < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS) {
            CHECK(gateway_try_accept(&gateway,
                                     &records[i + 1u],
                                     now_ms) == -EBUSY);
            CHECK(records[i + 1u].source_owned);
            CHECK(records[i + 1u].ack_count == 0u);
        }
        now_ms += SURVEY_GATEWAY_HOST_RECORD_SERVICE_BUDGET_MS;
        gateway_notify_and_retire(&gateway, now_ms);
    }

    CHECK(now_ms < SURVEY_PAIR_RESULT_CUSTODY_HORIZON_MS);
    CHECK(gateway.semantic_accept_count ==
          SURVEY_PAIR_RESULT_MAX_BURST_RECORDS);
    CHECK(gateway.busy_count >=
          (2u * (SURVEY_PAIR_RESULT_MAX_BURST_RECORDS - 1u)) - 1u);
    CHECK(!gateway.journal_valid);
    CHECK(gateway_ble_stream_depth(&gateway.stream) == 0u);
    for (size_t i = 0u; i < SURVEY_PAIR_RESULT_MAX_BURST_RECORDS; i++) {
        CHECK(!records[i].source_owned);
        CHECK(records[i].ack_count >= 1u);
        CHECK(records[i].host_count == 1u);
    }
}

int main(void)
{
    run_maximum_burst_reset_and_backpressure();
    puts("PASS gateway_survey_host_flow_control");
    return EXIT_SUCCESS;
}
