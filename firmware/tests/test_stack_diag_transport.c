#include "stack_diag_transport.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct fake_rtt {
    uint32_t now_ms;
    unsigned available;
    unsigned wait_count;
    unsigned drain_after_waits;
    unsigned write_count;
    unsigned zero_once_write;
    unsigned drain_during_write;
    bool partial_write;
};

static uint32_t fake_now_ms(void *context)
{
    return ((struct fake_rtt *)context)->now_ms;
}

static unsigned fake_available(void *context)
{
    return ((struct fake_rtt *)context)->available;
}

static unsigned fake_write(void *context, const char *data, size_t length)
{
    struct fake_rtt *fake = context;

    assert(data != NULL);
    fake->write_count++;
    if (fake->write_count == fake->zero_once_write) {
        return 0u;
    }
    if (fake->partial_write && length > 1u) {
        return (unsigned)length - 1u;
    }
    if (length > fake->available) {
        return 0u;
    }
    fake->available -= (unsigned)length;
    if (fake->write_count == fake->drain_during_write) {
        fake->available = 1023u;
    }
    return (unsigned)length;
}

static void fake_wait_ms(void *context, uint32_t delay_ms)
{
    struct fake_rtt *fake = context;

    fake->now_ms += delay_ms;
    fake->wait_count++;
    if (fake->drain_after_waits != 0u &&
        fake->wait_count >= fake->drain_after_waits) {
        fake->available = 1023u;
    }
}

static struct stack_diag_transport_ops fake_ops(struct fake_rtt *fake)
{
    return (struct stack_diag_transport_ops){
        .now_ms = fake_now_ms,
        .available = fake_available,
        .write = fake_write,
        .wait_ms = fake_wait_ms,
        .context = fake,
    };
}

static void establish_draining_host(struct stack_diag_transport *transport,
                                    struct fake_rtt *fake,
                                    const struct stack_diag_transport_ops *ops)
{
    assert(stack_diag_transport_begin(transport, ops) == 0);
    assert(stack_diag_transport_write(transport, ops, "boot\n") == 0);
    stack_diag_transport_end(transport, ops);
    fake->available = 1023u;
    assert(stack_diag_transport_begin(transport, ops) == 0);
    assert(transport->host_confirmed);
}

static void test_host_absent_probe_is_tiny_and_bounded(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {.available = 0u};
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    assert(stack_diag_transport_begin(&transport, &ops) == 0);
    assert(stack_diag_transport_write(&transport, &ops,
                                      "record-too-large\n") == -EAGAIN);
    assert(fake.wait_count == 1u);
    assert(fake.now_ms == STACK_DIAG_TRANSPORT_ATTACH_PROBE_MS);
    assert(!transport.host_confirmed);
    stack_diag_transport_end(&transport, &ops);
}

static void test_initial_zero_write_can_confirm_attached_host(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {
        .available = 0u,
        .drain_after_waits = 1u,
    };
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    assert(stack_diag_transport_begin(&transport, &ops) == 0);
    assert(stack_diag_transport_write(&transport, &ops, "first\n") == 0);
    assert(fake.wait_count == 1u);
    assert(transport.host_confirmed);
    stack_diag_transport_end(&transport, &ops);
}

static void test_drain_before_end_is_monotonic(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {.available = 1023u};
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    assert(stack_diag_transport_begin(&transport, &ops) == 0);
    assert(stack_diag_transport_write(&transport, &ops, "first\n") == 0);
    fake.available = 1023u;
    assert(stack_diag_transport_write(&transport, &ops, "second\n") == 0);
    assert(transport.host_confirmed);
    stack_diag_transport_end(&transport, &ops);
    assert(transport.host_confirmed);

    transport = (struct stack_diag_transport){0};
    fake = (struct fake_rtt){
        .available = 1023u,
        .drain_during_write = 1u,
    };
    ops = fake_ops(&fake);
    assert(stack_diag_transport_begin(&transport, &ops) == 0);
    assert(stack_diag_transport_write(&transport, &ops, "during\n") == 0);
    assert(transport.host_confirmed);
    stack_diag_transport_end(&transport, &ops);
}

static void test_confirmed_host_retries_complete_record(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {.available = 1023u};
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    establish_draining_host(&transport, &fake, &ops);
    fake.available = 0u;
    fake.drain_after_waits = 3u;
    assert(stack_diag_transport_write(&transport, &ops,
                                      "complete typed record\n") == 0);
    assert(fake.wait_count == 3u);
    stack_diag_transport_end(&transport, &ops);
}

static void test_partial_write_fails_closed(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {.available = 1023u};
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    establish_draining_host(&transport, &fake, &ops);
    fake.partial_write = true;
    assert(stack_diag_transport_write(&transport, &ops,
                                      "must stay atomic\n") == -EIO);
    assert(fake.wait_count == 0u);
    stack_diag_transport_end(&transport, &ops);
}

static void test_retry_deadline_is_bounded_and_wrap_safe(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {
        .now_ms = UINT32_MAX - 50u,
        .available = 1023u,
    };
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    establish_draining_host(&transport, &fake, &ops);
    fake.available = 0u;
    assert(stack_diag_transport_write(&transport, &ops,
                                      "deadline\n") == -ETIMEDOUT);
    assert(fake.wait_count == STACK_DIAG_TRANSPORT_RETRY_WINDOW_MS);
    assert(!transport.host_confirmed);
    stack_diag_transport_end(&transport, &ops);
}

static void test_every_record_position_recovers_from_one_zero_write(void)
{
    static const char *const records[] = {
        "run-begin\n",
        "sample-begin\n",
        "isr\n",
        "thread-a\n",
        "thread-b\n",
        "sample-end\n",
        "run-end\n",
    };

    for (size_t zero_index = 0u;
         zero_index < sizeof(records) / sizeof(records[0]); zero_index++) {
        struct stack_diag_transport transport = {0};
        struct fake_rtt fake = {.available = 1023u};
        struct stack_diag_transport_ops ops = fake_ops(&fake);

        establish_draining_host(&transport, &fake, &ops);
        fake.zero_once_write = fake.write_count + (unsigned)zero_index + 1u;
        for (size_t record_index = 0u;
             record_index < sizeof(records) / sizeof(records[0]);
             record_index++) {
            assert(stack_diag_transport_write(&transport, &ops,
                                               records[record_index]) == 0);
        }
        assert(fake.wait_count == 1u);
        stack_diag_transport_end(&transport, &ops);
    }
}

static void test_thousand_run_pressure(void)
{
    struct stack_diag_transport transport = {0};
    struct fake_rtt fake = {.available = 1023u};
    struct stack_diag_transport_ops ops = fake_ops(&fake);

    establish_draining_host(&transport, &fake, &ops);
    for (unsigned run = 0u; run < 1000u; run++) {
        assert(stack_diag_transport_write(&transport, &ops,
                                          "run-begin\n") == 0);
        assert(stack_diag_transport_write(&transport, &ops,
                                          "sample-commit\n") == 0);
        assert(stack_diag_transport_write(&transport, &ops,
                                          "run-end\n") == 0);
        stack_diag_transport_end(&transport, &ops);
        if (run + 1u < 1000u) {
            fake.available = 1023u;
            assert(stack_diag_transport_begin(&transport, &ops) == 0);
        }
    }
    assert(fake.write_count == 3001u);
}

int main(void)
{
    test_host_absent_probe_is_tiny_and_bounded();
    test_initial_zero_write_can_confirm_attached_host();
    test_drain_before_end_is_monotonic();
    test_confirmed_host_retries_complete_record();
    test_partial_write_fails_closed();
    test_retry_deadline_is_bounded_and_wrap_safe();
    test_every_record_position_recovers_from_one_zero_write();
    test_thousand_run_pressure();
    return 0;
}
