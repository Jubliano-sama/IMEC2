#include "debug_log.h"

#include <assert.h>
#include <string.h>

static void test_debug_prefix_contains_stable_fields(void)
{
    char prefix[96];
    int ret;

    ret = debug_log_format_prefix(prefix,
                                  sizeof(prefix),
                                  1234u,
                                  "tag",
                                  0x1111222233334444ull,
                                  2,
                                  "RANGE_OK");
    assert(ret > 0);
    assert(strcmp(prefix, "[1234][tag][0x1111222233334444][stage2][RANGE_OK]") == 0);
}

static void test_debug_prefix_rejects_truncation(void)
{
    char prefix[16];

    assert(debug_log_format_prefix(prefix,
                                   sizeof(prefix),
                                   1u,
                                   "gateway",
                                   0x9999888877776666ull,
                                   3,
                                   "BOOT_CONFIG") < 0);
}

int main(void)
{
    test_debug_prefix_contains_stable_fields();
    test_debug_prefix_rejects_truncation();
    return 0;
}
