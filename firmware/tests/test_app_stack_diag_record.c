#include "app_stack_diag_record.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_long_typed_records_are_complete(void)
{
    static const char build_identity[] =
        "imec-stack-v1:mesh_gateway:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char record[APP_STACK_DIAG_RECORD_CAPACITY];
    int length;

    length = app_stack_diag_record_format(
        record, sizeof(record),
        "DBG_STACK_BOOT preset=%s build=%s epoch=%llu uptime=%u\n",
        "mesh_gateway", build_identity,
        (unsigned long long)UINT64_MAX, UINT32_MAX);
    assert(length > 128);
    assert((size_t)length == strlen(record));
    assert(record[length - 1] == '\n');

    length = app_stack_diag_record_format(
        record, sizeof(record),
        "DBG_STACK_RUN_END epoch=%llu run=%u kind=%s owner=%s outcome=%s queue=%u custody=%u credit=%u retry=%u drain=%u src=%llu dst=%llu session=%u seq=%u type=%u samples=%u sequence=%u previous=%u uptime=%u\n",
        (unsigned long long)UINT64_MAX, UINT32_MAX,
        "gateway_priority_control", "system_workqueue",
        "direct_ack_failure", UINT16_MAX, UINT16_MAX, UINT16_MAX,
        UINT16_MAX, UINT16_MAX,
        (unsigned long long)UINT64_MAX, (unsigned long long)UINT64_MAX,
        UINT32_MAX, UINT16_MAX, UINT8_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX);
    assert(length == APP_STACK_DIAG_MAX_FORMATTED_LENGTH);
    assert((size_t)length + 1u <= sizeof(record));
    assert((size_t)length == strlen(record));
    assert(record[length - 1] == '\n');
}

static void test_truncation_and_missing_delimiter_fail_closed(void)
{
    char small[32];

    assert(app_stack_diag_record_format(
               small, sizeof(small),
               "DBG_STACK_BOOT preset=%s build=%s\n",
               "mesh_gateway", "far-too-long-for-this-buffer") == -EMSGSIZE);
    assert(small[0] == '\0');
    assert(app_stack_diag_record_format(
               small, sizeof(small), "DBG_STACK name=test") == -EBADMSG);
    assert(small[0] == '\0');
}

int main(void)
{
    test_long_typed_records_are_complete();
    test_truncation_and_missing_delimiter_fail_closed();
    puts("app stack diagnostic record tests passed");
    return 0;
}
