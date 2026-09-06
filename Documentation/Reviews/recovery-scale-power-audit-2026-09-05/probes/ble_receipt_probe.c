#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Fake kernel/transport boundary; the worker below is extracted verbatim. */
#define ROLE_GATEWAY 1
#define DEVICE_ROLE ROLE_GATEWAY
#define CONFIG_IMEC_CLICK_HANDOFF_RTT_TRACE 0
#define IS_ENABLED(value) (value)
#define ARG_UNUSED(value) ((void)(value))
#define K_NO_WAIT 0
#define status_debug_printf(...) ((void)0)
#define LOG_ERR(...) ((void)0)

struct k_work { int unused; };
struct gateway_ble_frame_pending { uint8_t frame[1]; uint16_t len; };
struct fake_msgq {
    struct gateway_ble_frame_pending frames[4];
    unsigned count;
};
static struct fake_msgq gateway_ble_rx_msgq;
static int gateway_ble_host_receipt_ingress_count;
static unsigned reserve_calls;
static unsigned release_calls;
static unsigned commands_handled;
static unsigned receipts_handled;
static unsigned schedule_errors;
static unsigned credits;
static int last_reserve_return;

static int atomic_get(const int *value) { return *value; }
static void atomic_dec(int *value) { --*value; }
static unsigned k_msgq_num_used_get(const struct fake_msgq *queue)
{
    return queue->count;
}
static int k_msgq_peek(const struct fake_msgq *queue,
                       struct gateway_ble_frame_pending *pending)
{
    if (queue->count == 0u) {
        return -ENOMSG;
    }
    *pending = queue->frames[0];
    return 0;
}
static int k_msgq_get(struct fake_msgq *queue,
                      struct gateway_ble_frame_pending *pending,
                      int timeout)
{
    ARG_UNUSED(timeout);
    if (k_msgq_peek(queue, pending) < 0) {
        return -ENOMSG;
    }
    --queue->count;
    memmove(queue->frames, queue->frames + 1u,
            queue->count * sizeof(queue->frames[0]));
    return 0;
}
static bool gateway_ble_transport_enabled(void) { return true; }
static bool gateway_ble_pending_is_host_receipt(
    const struct gateway_ble_frame_pending *pending)
{
    /* A tag represents an already valid decoded frame; codecs are not tested. */
    return pending->frame[0] == MSG_GATEWAY_HOST_RECEIPT;
}
static int gateway_command_result_reserve_ingress(uint32_t *token)
{
    ++reserve_calls;
    last_reserve_return = credits == 0u ? -ENOSPC : 0;
    if (last_reserve_return == 0) {
        --credits;
        *token = reserve_calls;
    }
    return last_reserve_return;
}
static void gateway_command_result_release_ingress(uint32_t token)
{
    assert(token != 0u);
    ++release_calls;
    ++credits;
}
static void gateway_ble_schedule_failed(const char *owner, int ret)
{
    ARG_UNUSED(owner);
    ARG_UNUSED(ret);
    ++schedule_errors;
}
static bool gateway_handle_ble_frame(const uint8_t *frame, uint16_t length,
                                      uint32_t token)
{
    assert(length == 1u);
    if (frame[0] == MSG_GATEWAY_HOST_RECEIPT) {
        assert(token == 0u);
        ++receipts_handled;
        /* Model a valid receipt freeing the retained output blocking a credit. */
        ++credits;
        return false;
    }
    assert(frame[0] == MSG_COMMAND && token != 0u);
    ++commands_handled;
    return true;
}

#include "ble_receipt_worker.inc"

static void enqueue(uint8_t type)
{
    assert(gateway_ble_rx_msgq.count < 4u);
    gateway_ble_rx_msgq.frames[gateway_ble_rx_msgq.count++] =
        (struct gateway_ble_frame_pending) {.frame = {type}, .len = 1u};
    if (type == MSG_GATEWAY_HOST_RECEIPT) {
        ++gateway_ble_host_receipt_ingress_count;
    }
}

int main(int argc, char **argv)
{
    bool reordered = false;
    bool receipt_expected = true;

    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "receipt_first_no_credit") == 0) {
        enqueue(MSG_GATEWAY_HOST_RECEIPT);
        enqueue(MSG_COMMAND);
    } else if (strcmp(argv[1], "command_only_no_credit") == 0) {
        enqueue(MSG_COMMAND);
        receipt_expected = false;
    } else {
        enqueue(MSG_COMMAND);
        enqueue(MSG_GATEWAY_HOST_RECEIPT);
        if (strcmp(argv[1], "command_first_one_credit") == 0) {
            credits = 1u;
        } else if (strcmp(argv[1], "command_first_then_reorder") == 0) {
            reordered = true;
        } else if (strcmp(argv[1], "command_first_no_credit") != 0) {
            return 2;
        }
    }
    /* Repeated worker submissions alone must not masquerade as recovery. */
    for (unsigned invocation = 0u; invocation < 10u; ++invocation) {
        gateway_ble_rx_work_handler(NULL);
    }
    printf("case=%s invocations=10 queued=%u receipts_pending=%d "
           "receipts_handled=%u commands_handled=%u reserve_calls=%u "
           "last_reserve_return=%d credits=%u\n", argv[1],
           k_msgq_num_used_get(&gateway_ble_rx_msgq),
           atomic_get(&gateway_ble_host_receipt_ingress_count),
           receipts_handled, commands_handled, reserve_calls,
           last_reserve_return, credits);
    if (reordered) {
        struct gateway_ble_frame_pending saved = gateway_ble_rx_msgq.frames[0];

        assert(gateway_ble_rx_msgq.count == 2u && receipts_handled == 0u);
        gateway_ble_rx_msgq.frames[0] = gateway_ble_rx_msgq.frames[1];
        gateway_ble_rx_msgq.frames[1] = saved;
        gateway_ble_rx_work_handler(NULL);
        printf("after_fixture_reorder queued=%u receipts_pending=%d "
               "receipts_handled=%u commands_handled=%u credits=%u\n",
               k_msgq_num_used_get(&gateway_ble_rx_msgq),
               atomic_get(&gateway_ble_host_receipt_ingress_count),
               receipts_handled, commands_handled, credits);
    }
    assert(release_calls == 0u && schedule_errors == 0u);
    if (!receipt_expected) {
        assert(gateway_ble_rx_msgq.count == 1u && commands_handled == 0u);
        return 0;
    }
    if (receipts_handled != 1u) {
        puts("FAIL: valid queued receipt was not dispatched under result backpressure");
        return 1;
    }
    assert(gateway_ble_rx_msgq.count == 0u && commands_handled == 1u);
    return 0;
}
