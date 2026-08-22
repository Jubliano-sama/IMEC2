#include "stack_budget.h"

#include "app_discovery_assignment_stack.h"
#include "protocol.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK_TRUE(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,            \
                    #expression);                                                \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_INT(actual_expression, expected_expression)                        \
    do {                                                                         \
        int actual_value = (actual_expression);                                  \
        int expected_value = (expected_expression);                              \
        if (actual_value != expected_value) {                                    \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s=%d expected %d\n",                         \
                    __FILE__, __LINE__, #actual_expression,                      \
                    actual_value, expected_value);                               \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_U32(actual_expression, expected_expression)                        \
    do {                                                                         \
        uint32_t actual_value = (uint32_t)(actual_expression);                   \
        uint32_t expected_value = (uint32_t)(expected_expression);               \
        if (actual_value != expected_value) {                                    \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s=%" PRIu32 " expected %" PRIu32 "\n",       \
                    __FILE__, __LINE__, #actual_expression,                      \
                    actual_value, expected_value);                               \
            failures++;                                                          \
        }                                                                        \
    } while (0)

struct expected_role {
    enum stack_budget_role role;
    const char *preset_name;
    struct stack_budget_role_config config;
};

#define EXPECTED_ROLE_ENTRY(                                                  \
    role_name, preset, main_size, workqueue_size, mesh_route_size, isr_size, \
    idle_size,                                                               \
    log_size, hci_tx_size, bt_rx_size, ram_headroom, init_enabled,           \
    hw_protection,                                                            \
    mpu_guard, stack_info, sentinel)                                         \
    {                                                                        \
        .role = STACK_BUDGET_ROLE_##role_name,                               \
        .preset_name = preset,                                               \
        .config = {                                                          \
            .main_bytes = main_size,                                         \
            .system_workqueue_bytes = workqueue_size,                        \
            .mesh_route_bytes = mesh_route_size,                             \
            .isr_bytes = isr_size,                                           \
            .idle_bytes = idle_size,                                         \
            .log_processor_bytes = log_size,                                 \
            .bt_hci_tx_bytes = hci_tx_size,                                  \
            .bt_rx_bytes = bt_rx_size,                                       \
            .hw_stack_protection = hw_protection,                            \
            .mpu_stack_guard = mpu_guard,                                    \
            .thread_stack_info = stack_info,                                 \
            .init_stacks = init_enabled,                                     \
            .stack_sentinel = sentinel,                                      \
            .minimum_static_ram_headroom_bytes = ram_headroom,                \
        },                                                                   \
    },

static const struct expected_role expected_roles[] = {
    STACK_BUDGET_PRESET_POLICY(EXPECTED_ROLE_ENTRY)
};

#undef EXPECTED_ROLE_ENTRY

static void test_exact_role_baselines(void)
{
    CHECK_U32(sizeof(expected_roles) / sizeof(expected_roles[0]),
              STACK_BUDGET_ROLE_COUNT);
    for (size_t i = 0u;
         i < sizeof(expected_roles) / sizeof(expected_roles[0]);
         i++) {
        const struct expected_role *expected = &expected_roles[i];
        struct stack_budget_role_config config;

        CHECK_INT(stack_budget_role_baseline(expected->role, &config),
                  PROTO_OK);
        CHECK_TRUE(strcmp(stack_budget_role_preset_name(expected->role),
                          expected->preset_name) == 0);
        CHECK_U32(config.main_bytes, expected->config.main_bytes);
        CHECK_U32(config.system_workqueue_bytes,
                  expected->config.system_workqueue_bytes);
        CHECK_U32(config.mesh_route_bytes,
                  expected->config.mesh_route_bytes);
        CHECK_U32(config.isr_bytes, expected->config.isr_bytes);
        CHECK_U32(config.idle_bytes, expected->config.idle_bytes);
        CHECK_U32(config.log_processor_bytes,
                  expected->config.log_processor_bytes);
        CHECK_U32(config.bt_hci_tx_bytes,
                  expected->config.bt_hci_tx_bytes);
        CHECK_U32(config.bt_rx_bytes, expected->config.bt_rx_bytes);
        CHECK_TRUE(config.hw_stack_protection ==
                   expected->config.hw_stack_protection);
        CHECK_TRUE(config.mpu_stack_guard ==
                   expected->config.mpu_stack_guard);
        CHECK_TRUE(config.thread_stack_info ==
                   expected->config.thread_stack_info);
        CHECK_TRUE(config.init_stacks == expected->config.init_stacks);
        CHECK_TRUE(config.stack_sentinel ==
                   expected->config.stack_sentinel);
        CHECK_U32(config.minimum_static_ram_headroom_bytes,
                  expected->config.minimum_static_ram_headroom_bytes);
        CHECK_U32(stack_budget_configured_for_owner(
                      &config, STACK_BUDGET_OWNER_MAIN),
                  expected->config.main_bytes);
        CHECK_U32(stack_budget_configured_for_owner(
                      &config, STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE),
                  expected->config.system_workqueue_bytes);
        CHECK_U32(stack_budget_configured_for_owner(
                      &config, STACK_BUDGET_OWNER_ISR),
                  expected->config.isr_bytes);
        CHECK_INT(stack_budget_validate_role_config(
                      expected->role,
                      &config,
                      config.minimum_static_ram_headroom_bytes),
                  PROTO_OK);
    }
    CHECK_TRUE(stack_budget_role_preset_name(STACK_BUDGET_ROLE_COUNT) == NULL);
    CHECK_INT(stack_budget_role_baseline(STACK_BUDGET_ROLE_COUNT, NULL),
              PROTO_ERR_ARG);
}

static void test_role_config_drift_rejected(void)
{
    struct stack_budget_role_config config;

    CHECK_INT(stack_budget_role_baseline(STACK_BUDGET_ROLE_ANCHOR, &config),
              PROTO_OK);
    config.system_workqueue_bytes += 32u;
    CHECK_INT(stack_budget_validate_role_config(
                  STACK_BUDGET_ROLE_ANCHOR,
                  &config,
                  config.minimum_static_ram_headroom_bytes),
              PROTO_ERR_STALE);

    CHECK_INT(stack_budget_role_baseline(STACK_BUDGET_ROLE_GATEWAY, &config),
              PROTO_OK);
    CHECK_INT(stack_budget_validate_role_config(
                  STACK_BUDGET_ROLE_GATEWAY,
                  &config,
                  config.minimum_static_ram_headroom_bytes - 1u),
              PROTO_ERR_STALE);
}

static void test_margin_boundaries(void)
{
    struct stack_budget_result result;

    CHECK_U32(stack_budget_required_free(
                  6144u, STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE),
              1229u);
    CHECK_INT(stack_budget_evaluate(6144u,
                                    4000u,
                                    915u,
                                    STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_U32(result.remaining_bytes, 1229u);
    CHECK_INT(stack_budget_evaluate(6144u,
                                    4000u,
                                    916u,
                                    STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(!result.passes);
    CHECK_U32(result.remaining_bytes, 1228u);

    CHECK_U32(stack_budget_required_free(1024u, STACK_BUDGET_OWNER_BT_RX),
              256u);
    CHECK_INT(stack_budget_evaluate(1024u,
                                    512u,
                                    256u,
                                    STACK_BUDGET_OWNER_BT_RX,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_INT(stack_budget_evaluate(1024u,
                                    512u,
                                    257u,
                                    STACK_BUDGET_OWNER_BT_RX,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(!result.passes);
    CHECK_INT(stack_budget_evaluate(0u,
                                    1u,
                                    1u,
                                    STACK_BUDGET_OWNER_MAIN,
                                    &result),
              PROTO_ERR_ARG);
    CHECK_INT(stack_budget_evaluate(UINT32_MAX,
                                    UINT32_MAX,
                                    1u,
                                    STACK_BUDGET_OWNER_MAIN,
                                    &result),
              PROTO_ERR_BAD_LENGTH);
}

static void test_large_local_guard(void)
{
    CHECK_TRUE(stack_budget_large_local_allowed(0u));
    CHECK_TRUE(stack_budget_large_local_allowed(
        STACK_BUDGET_LARGE_LOCAL_FRAME_MAX_BYTES));
    CHECK_TRUE(!stack_budget_large_local_allowed(
        STACK_BUDGET_LARGE_LOCAL_FRAME_MAX_BYTES + 1u));
}

static void test_assignment_publish_large_local_budget(void)
{
    struct stack_budget_role_config gateway;
    struct stack_budget_result result;

    CHECK_TRUE(APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_BYTES <=
               APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_LIMIT_BYTES);
    CHECK_TRUE(stack_budget_large_local_allowed(
        APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_BYTES));
    CHECK_INT(stack_budget_role_baseline(STACK_BUDGET_ROLE_GATEWAY, &gateway),
              PROTO_OK);
    CHECK_INT(stack_budget_evaluate(
                  gateway.system_workqueue_bytes,
                  APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_BYTES,
                  0u,
                  STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
                  &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_TRUE(result.remaining_bytes >= result.required_free_bytes);
}

static void test_anchor_survey_restore_hardware_watermark(void)
{
    struct stack_budget_result result;

    /*
     * A restored 0x55 delivery exercises journal recovery, a direct gateway
     * route probe, SPI TX, and the ACK RX chain on this dedicated workqueue.
     * Hardware consumed 4080 of the former 4096 bytes and tripped the MPU stack
     * guard.  Preserve the complete chain on the shared anchor UWB owner queue
     * and retain the normal application margin.
     */
    CHECK_INT(stack_budget_evaluate(4096u,
                                    4080u,
                                    0u,
                                    STACK_BUDGET_OWNER_DEDICATED_APP,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(!result.passes);
    CHECK_INT(stack_budget_evaluate(12288u,
                                    4080u,
                                    0u,
                                    STACK_BUDGET_OWNER_DEDICATED_APP,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_U32(result.remaining_bytes, 8208u);
    CHECK_U32(result.required_free_bytes, 2458u);
}

static void test_anchor_scan_ddd_runtime_watermark(void)
{
    struct stack_budget_result result;

    /*
     * The fresh three-direct-anchor survey consumed 6472 bytes of the scan
     * owner. The former aligned 7232-byte runtime stack leaves only 760 bytes,
     * while the 8192-byte queue retains the common 20 percent policy reserve.
     */
    CHECK_INT(stack_budget_evaluate(7232u,
                                    6472u,
                                    0u,
                                    STACK_BUDGET_OWNER_DEDICATED_APP,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(!result.passes);
    CHECK_U32(result.remaining_bytes, 760u);
    CHECK_U32(result.required_free_bytes, 1447u);

    CHECK_INT(stack_budget_evaluate(8192u,
                                    6472u,
                                    0u,
                                    STACK_BUDGET_OWNER_DEDICATED_APP,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_U32(result.remaining_bytes, 1720u);
    CHECK_U32(result.required_free_bytes, 1639u);
}

static void test_anchor_route_hardware_watermark_rebalances_existing_ram(void)
{
    struct stack_budget_role_config anchor;
    struct stack_budget_role_config forcedhop;
    struct stack_budget_result result;

    CHECK_INT(stack_budget_role_baseline(STACK_BUDGET_ROLE_ANCHOR, &anchor),
              PROTO_OK);
    CHECK_INT(stack_budget_role_baseline(
                  STACK_BUDGET_ROLE_ANCHOR_FORCEDHOP, &forcedhop),
              PROTO_OK);
    CHECK_U32(anchor.system_workqueue_bytes, 5376u);
    CHECK_U32(anchor.mesh_route_bytes, 9472u);
    CHECK_U32(forcedhop.system_workqueue_bytes,
              anchor.system_workqueue_bytes);
    CHECK_U32(forcedhop.mesh_route_bytes, anchor.mesh_route_bytes);

    CHECK_INT(stack_budget_evaluate(anchor.mesh_route_bytes,
                                    7360u,
                                    0u,
                                    STACK_BUDGET_OWNER_MESH_ROUTE,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_U32(result.remaining_bytes, 2112u);
    CHECK_U32(result.required_free_bytes, 1895u);

    CHECK_INT(stack_budget_evaluate(8576u,
                                    7360u,
                                    0u,
                                    STACK_BUDGET_OWNER_MESH_ROUTE,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(!result.passes);

    CHECK_INT(stack_budget_evaluate(anchor.system_workqueue_bytes,
                                    4088u,
                                    0u,
                                    STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_U32(result.remaining_bytes, 1288u);
    CHECK_U32(result.required_free_bytes, 1076u);
}

static void test_worst_combined_scenario(void)
{
    const struct stack_budget_combined_scenario scenario = {
        .root_frame_bytes = 640u,
        .shared_mesh_bytes = 768u,
        .ds_twr_bytes = 2048u,
        .cir_pack_bytes = 1408u,
        .transit_retry_bytes = 2304u,
        .gateway_command_bytes = 1792u,
        .ble_fragment_bytes = 1024u,
        .indirect_reserve_bytes = 1536u,
        .sequential_click_count = 5u,
    };
    struct stack_budget_combined_usage usage;
    struct stack_budget_role_config forcedhop;
    struct stack_budget_result result;

    CHECK_INT(stack_budget_calculate_combined(&scenario, &usage), PROTO_OK);
    CHECK_U32(usage.measured_chain_bytes, 4864u);
    CHECK_U32(usage.indirect_reserve_bytes, 1536u);
    CHECK_U32(usage.total_bytes, 6400u);
    CHECK_INT(stack_budget_role_baseline(
                  STACK_BUDGET_ROLE_TRANSMITTER_FORCEDHOP,
                  &forcedhop),
              PROTO_OK);
    CHECK_U32(forcedhop.system_workqueue_bytes, 8192u);
    CHECK_INT(stack_budget_evaluate(
                  forcedhop.system_workqueue_bytes,
                  usage.measured_chain_bytes,
                  usage.indirect_reserve_bytes,
                  STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
                  &result),
              PROTO_OK);
    CHECK_TRUE(result.passes);
    CHECK_U32(result.remaining_bytes, 1792u);
    CHECK_U32(result.required_free_bytes, 1639u);

    CHECK_INT(stack_budget_evaluate(6144u,
                                    usage.measured_chain_bytes,
                                    usage.indirect_reserve_bytes,
                                    STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE,
                                    &result),
              PROTO_OK);
    CHECK_TRUE(!result.passes);
}

int main(void)
{
    test_exact_role_baselines();
    test_role_config_drift_rejected();
    test_margin_boundaries();
    test_large_local_guard();
    test_assignment_publish_large_local_budget();
    test_anchor_survey_restore_hardware_watermark();
    test_anchor_scan_ddd_runtime_watermark();
    test_anchor_route_hardware_watermark_rebalances_existing_ram();
    test_worst_combined_scenario();

    if (failures != 0u) {
        fprintf(stderr, "stack budget model tests failed: %u\n", failures);
        return 1;
    }
    puts("stack budget model tests passed");
    return 0;
}
