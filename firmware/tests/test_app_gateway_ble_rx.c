#include "gateway_ble_transport.h"
#include "gateway_command.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ROLE_GATEWAY 1
#define DEVICE_ROLE ROLE_GATEWAY
#define CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE 0
#define IS_ENABLED(value) (value)
#define ARG_UNUSED(value) ((void)(value))
#define LOG_ERR(...) ((void)0)
struct k_work { int unused; };

static struct gateway_ble_ingress ingress;
static int gateway_ble_host_receipt_ingress_count;
static unsigned credits;
static unsigned reservations;
static unsigned releases;
static bool worker_pending;
static unsigned receipt_dispatches;
static unsigned valid_receipts;
static unsigned failures;
static unsigned commands;
static uint16_t command_order[8];
static bool inject_command_during_receipt;
static bool inject_receipt_during_reservation;
static struct gateway_host_receipt_identity expected_identity;

static void status_debug_printf(const char *format, ...)
{
    (void)format;
}
static int atomic_get(const int *value) { return *value; }
static void atomic_dec(int *value) { --*value; }
static bool gateway_ble_transport_enabled(void) { return true; }
static uint8_t gateway_ble_rx_depth(void) { return ingress.count; }
static int gateway_ble_rx_take(bool receipt,
                               struct gateway_ble_ingress_frame *pending)
{
    return gateway_ble_ingress_take(&ingress, receipt, pending);
}

static void enqueue(uint8_t type, uint16_t sequence, bool valid)
{
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    uint8_t payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES];
    struct proto_packet packet = {
        .msg_type = type,
        .src_id = 123u,
        .dst_id = 456u,
        .session_id = expected_identity.session_id,
        .seq = sequence,
        .ttl = 1u,
    };
    size_t payload_len = 0u;
    size_t frame_len;
    uint8_t receipts;

    if (type == MSG_GATEWAY_HOST_RECEIPT) {
        assert(gateway_host_receipt_identity_append_tlv(payload, sizeof(payload),
            &payload_len, &expected_identity) == PROTO_OK);
        if (!valid) {
            /* A valid packet with a malformed receipt TLV stays untrusted. */
            payload[0] = 0xffu;
        }
    }
    packet.payload_len = (uint16_t)payload_len;
    assert(serial_frame_encode_packet(&packet, payload, frame, sizeof(frame),
                                       &frame_len) == PROTO_OK);
    assert(gateway_ble_ingress_write(&ingress, frame, frame_len, &receipts) == 0);
    gateway_ble_host_receipt_ingress_count += receipts;
    worker_pending = true;
}

static int gateway_command_result_reserve_ingress(uint32_t *token)
{
    reservations++;
    if (inject_receipt_during_reservation) {
        inject_receipt_during_reservation = false;
        enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq, true);
    }
    if (credits == 0u) {
        return -ENOSPC;
    }
    credits--;
    *token = 1u;
    return 0;
}
static void gateway_command_result_release_ingress(uint32_t token)
{
    assert(token != 0u);
    credits++;
    releases++;
    /* Production release calls gateway_ble_resume_rx(), including from
     * inside the currently executing worker. */
    worker_pending = true;
}
static void gateway_ble_schedule_failed(const char *owner, int ret)
{
    (void)owner;
    (void)ret;
    failures++;
}
static bool gateway_handle_ble_frame(const uint8_t *frame, size_t len,
                                      uint32_t token)
{
    struct proto_packet packet;
    struct gateway_host_receipt_identity identity;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len;

    assert(serial_frame_decode_packet(frame, len, &packet, payload,
        sizeof(payload), &payload_len) == PROTO_OK);
    if (packet.msg_type == MSG_GATEWAY_HOST_RECEIPT) {
        assert(token == 0u);
        receipt_dispatches++;
        if (gateway_host_receipt_packet_validate(&packet, payload, payload_len,
                                                  &identity) == PROTO_OK &&
            identity.seq == expected_identity.seq &&
            packet.seq == identity.seq &&
            memcmp(identity.stream_record_digest,
                   expected_identity.stream_record_digest,
                   sizeof(identity.stream_record_digest)) == 0) {
            valid_receipts++;
            credits++;
        }
        if (inject_command_during_receipt) {
            inject_command_during_receipt = false;
            enqueue(MSG_COMMAND, 3u, true);
        }
        return false;
    }
    assert(packet.msg_type == MSG_COMMAND && token != 0u);
    command_order[commands++] = packet.seq;
    return true;
}

/* Extracted verbatim at configure time: this is the production worker,
 * with kernel dispatch and command-result credit represented by the seams above. */
#include "gateway_ble_rx_worker.inc"

static void reset(void)
{
    memset(&ingress, 0, sizeof(ingress));
    gateway_ble_host_receipt_ingress_count = 0;
    credits = 0u;
    reservations = releases = 0u;
    worker_pending = false;
    receipt_dispatches = 0u;
    valid_receipts = 0u;
    commands = 0u;
    failures = 0u;
    inject_command_during_receipt = false;
    inject_receipt_during_reservation = false;
    expected_identity = (struct gateway_host_receipt_identity) {
        .original_msg_type = MSG_SURVEY_EVENT,
        .original_flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = 456u,
        .dst_id = 456u,
        .session_id = 44u,
        .seq = 77u,
        .stream_record_digest = {1u, 2u, 3u},
    };
}

static void test_receipt_bypasses_three_blocked_commands(void)
{
    reset();
    enqueue(MSG_COMMAND, 1u, true);
    enqueue(MSG_COMMAND, 2u, true);
    enqueue(MSG_COMMAND, 3u, true);
    enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq, true);
    gateway_ble_rx_work_handler(NULL);
    assert(receipt_dispatches == 1u && valid_receipts == 1u);
    assert(commands == 1u && command_order[0] == 1u && ingress.count == 2u);
    assert(gateway_ble_host_receipt_ingress_count == 0);
    credits = 2u;
    gateway_ble_rx_work_handler(NULL);
    assert(commands == 3u && command_order[1] == 2u && command_order[2] == 3u);
    assert(ingress.count == 0u && failures == 0u);
}

static void test_invalid_receipt_does_not_release_command_credit(void)
{
    reset();
    enqueue(MSG_COMMAND, 1u, true);
    enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq, false);
    gateway_ble_rx_work_handler(NULL);
    assert(receipt_dispatches == 1u && valid_receipts == 0u);
    assert(commands == 0u && ingress.count == 1u && credits == 0u);
    enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq + 1u, true);
    gateway_ble_rx_work_handler(NULL);
    assert(receipt_dispatches == 2u && valid_receipts == 0u);
    assert(commands == 0u && ingress.count == 1u && credits == 0u);
    enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq, true);
    gateway_ble_rx_work_handler(NULL);
    assert(commands == 1u && ingress.count == 0u && failures == 0u);
}

static void test_arrival_during_dispatch_keeps_command_fifo(void)
{
    reset();
    enqueue(MSG_COMMAND, 1u, true);
    enqueue(MSG_COMMAND, 2u, true);
    enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq, true);
    inject_command_during_receipt = true;
    credits = 2u;
    gateway_ble_rx_work_handler(NULL);
    assert(commands == 3u && ingress.count == 0u);
    assert(command_order[0] == 1u && command_order[1] == 2u && command_order[2] == 3u);
    assert(failures == 0u);
}

static void test_arrival_during_failed_reservation_runs_next_submission(void)
{
    reset();
    enqueue(MSG_COMMAND, 1u, true);
    inject_receipt_during_reservation = true;
    gateway_ble_rx_work_handler(NULL);
    assert(ingress.count == 2u && commands == 0u);
    /* ATT enqueue resubmits the same work item while it is running. */
    gateway_ble_rx_work_handler(NULL);
    assert(ingress.count == 0u && commands == 1u && valid_receipts == 1u);
    assert(failures == 0u);
}

static void run_until_quiescent(void)
{
    unsigned dispatches = 0u;

    while (worker_pending) {
        /* A retained self-submission is real work ownership, not an idle
         * return: cap dispatches so the old empty-queue loop fails promptly. */
        assert(dispatches++ < 8u);
        worker_pending = false;
        gateway_ble_rx_work_handler(NULL);
    }
}

static void test_empty_and_drained_queue_release_the_system_workqueue(void)
{
    reset();
    credits = 3u;
    worker_pending = true;
    run_until_quiescent();
    assert(reservations == 0u && releases == 0u && credits == 3u);

    enqueue(MSG_COMMAND, 1u, true);
    enqueue(MSG_GATEWAY_HOST_RECEIPT, expected_identity.seq, true);
    run_until_quiescent();
    assert(commands == 1u && valid_receipts == 1u && ingress.count == 0u);
    assert(reservations == 1u && releases == 0u && credits == 3u);
    assert(!worker_pending && failures == 0u);
}

int main(void)
{
    test_empty_and_drained_queue_release_the_system_workqueue();
    test_receipt_bypasses_three_blocked_commands();
    test_invalid_receipt_does_not_release_command_credit();
    test_arrival_during_dispatch_keeps_command_fifo();
    test_arrival_during_failed_reservation_runs_next_submission();
    puts("gateway BLE receipt worker tests passed");
    return 0;
}
