#include "stack_budget.h"

#include "protocol.h"

#include <limits.h>

struct stack_budget_policy_entry {
    const char *preset_name;
    struct stack_budget_role_config config;
};

#define STACK_BUDGET_POLICY_ENTRY(                                           \
    role_name, preset, main_size, workqueue_size, isr_size, idle_size,        \
    log_size, hci_tx_size, bt_rx_size, ram_headroom, init_enabled,           \
    hw_protection,                                                            \
    mpu_guard, stack_info, sentinel)                                         \
    [STACK_BUDGET_ROLE_##role_name] = {                                      \
        .preset_name = preset,                                               \
        .config = {                                                          \
            .main_bytes = main_size,                                         \
            .system_workqueue_bytes = workqueue_size,                        \
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

static const struct stack_budget_policy_entry
    stack_budget_policy[STACK_BUDGET_ROLE_COUNT] = {
        STACK_BUDGET_PRESET_POLICY(STACK_BUDGET_POLICY_ENTRY)
    };

#undef STACK_BUDGET_POLICY_ENTRY

static bool stack_budget_role_valid(enum stack_budget_role role)
{
    return (unsigned int)role < (unsigned int)STACK_BUDGET_ROLE_COUNT;
}

int stack_budget_role_baseline(enum stack_budget_role role,
                               struct stack_budget_role_config *config)
{
    if (!stack_budget_role_valid(role) || config == NULL) {
        return PROTO_ERR_ARG;
    }

    *config = stack_budget_policy[role].config;
    return PROTO_OK;
}

const char *stack_budget_role_preset_name(enum stack_budget_role role)
{
    if (!stack_budget_role_valid(role)) {
        return NULL;
    }
    return stack_budget_policy[role].preset_name;
}

int stack_budget_validate_role_config(
    enum stack_budget_role role,
    const struct stack_budget_role_config *config,
    uint32_t static_ram_headroom_bytes)
{
    struct stack_budget_role_config expected;

    if (config == NULL ||
        stack_budget_role_baseline(role, &expected) != PROTO_OK) {
        return PROTO_ERR_ARG;
    }
    if (config->main_bytes != expected.main_bytes ||
        config->system_workqueue_bytes != expected.system_workqueue_bytes ||
        config->isr_bytes != expected.isr_bytes ||
        config->idle_bytes != expected.idle_bytes ||
        config->log_processor_bytes != expected.log_processor_bytes ||
        config->bt_hci_tx_bytes != expected.bt_hci_tx_bytes ||
        config->bt_rx_bytes != expected.bt_rx_bytes ||
        config->hw_stack_protection != expected.hw_stack_protection ||
        config->mpu_stack_guard != expected.mpu_stack_guard ||
        config->thread_stack_info != expected.thread_stack_info ||
        config->init_stacks != expected.init_stacks ||
        config->stack_sentinel != expected.stack_sentinel ||
        config->minimum_static_ram_headroom_bytes !=
            expected.minimum_static_ram_headroom_bytes ||
        static_ram_headroom_bytes <
            expected.minimum_static_ram_headroom_bytes) {
        return PROTO_ERR_STALE;
    }
    return PROTO_OK;
}

bool stack_budget_large_local_allowed(size_t frame_bytes)
{
    return frame_bytes <= STACK_BUDGET_LARGE_LOCAL_FRAME_MAX_BYTES;
}

static int add_checked(uint32_t left, uint32_t right, uint32_t *sum)
{
    if (sum == NULL || left > UINT32_MAX - right) {
        return PROTO_ERR_BAD_LENGTH;
    }
    *sum = left + right;
    return PROTO_OK;
}

int stack_budget_calculate_combined(
    const struct stack_budget_combined_scenario *scenario,
    struct stack_budget_combined_usage *usage)
{
    uint32_t click_branch;
    uint32_t gateway_branch;
    uint32_t largest_branch;
    uint32_t measured;
    int ret;

    if (scenario == NULL || usage == NULL ||
        scenario->sequential_click_count == 0u) {
        return PROTO_ERR_ARG;
    }
    ret = add_checked(scenario->ds_twr_bytes,
                      scenario->cir_pack_bytes,
                      &click_branch);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = add_checked(scenario->gateway_command_bytes,
                      scenario->ble_fragment_bytes,
                      &gateway_branch);
    if (ret != PROTO_OK) {
        return ret;
    }
    largest_branch = click_branch;
    if (scenario->transit_retry_bytes > largest_branch) {
        largest_branch = scenario->transit_retry_bytes;
    }
    if (gateway_branch > largest_branch) {
        largest_branch = gateway_branch;
    }
    ret = add_checked(scenario->root_frame_bytes,
                      scenario->shared_mesh_bytes,
                      &measured);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = add_checked(measured, largest_branch, &measured);
    if (ret != PROTO_OK) {
        return ret;
    }
    usage->measured_chain_bytes = measured;
    usage->indirect_reserve_bytes = scenario->indirect_reserve_bytes;
    return add_checked(measured,
                       scenario->indirect_reserve_bytes,
                       &usage->total_bytes);
}

uint32_t stack_budget_configured_for_owner(
    const struct stack_budget_role_config *config,
    enum stack_budget_owner owner)
{
    if (config == NULL) {
        return 0u;
    }
    switch (owner) {
    case STACK_BUDGET_OWNER_MAIN:
        return config->main_bytes;
    case STACK_BUDGET_OWNER_SYSTEM_WORKQUEUE:
        return config->system_workqueue_bytes;
    case STACK_BUDGET_OWNER_ISR:
        return config->isr_bytes;
    case STACK_BUDGET_OWNER_IDLE:
        return config->idle_bytes;
    case STACK_BUDGET_OWNER_LOG_PROCESSOR:
        return config->log_processor_bytes;
    case STACK_BUDGET_OWNER_BT_HCI_TX:
        return config->bt_hci_tx_bytes;
    case STACK_BUDGET_OWNER_BT_RX:
        return config->bt_rx_bytes;
    case STACK_BUDGET_OWNER_DEDICATED_APP:
    default:
        return 0u;
    }
}

uint32_t stack_budget_required_free(uint32_t configured_bytes,
                                    enum stack_budget_owner owner)
{
    uint32_t percentage = (uint32_t)(
        ((uint64_t)configured_bytes * STACK_BUDGET_MIN_FREE_PERCENT + 99u) /
        100u);
    uint32_t absolute =
        owner == STACK_BUDGET_OWNER_LOG_PROCESSOR ||
        owner == STACK_BUDGET_OWNER_BT_HCI_TX ||
        owner == STACK_BUDGET_OWNER_BT_RX ?
        STACK_BUDGET_SERVICE_MIN_FREE_BYTES :
        STACK_BUDGET_APP_MIN_FREE_BYTES;

    return percentage > absolute ? percentage : absolute;
}

int stack_budget_evaluate(uint32_t configured_bytes,
                          uint32_t measured_bytes,
                          uint32_t indirect_reserve_bytes,
                          enum stack_budget_owner owner,
                          struct stack_budget_result *result)
{
    uint32_t total_used;

    if (configured_bytes == 0u || result == NULL ||
        owner > STACK_BUDGET_OWNER_DEDICATED_APP) {
        return PROTO_ERR_ARG;
    }
    if (measured_bytes > UINT32_MAX - indirect_reserve_bytes) {
        return PROTO_ERR_BAD_LENGTH;
    }
    total_used = measured_bytes + indirect_reserve_bytes;
    result->configured_bytes = configured_bytes;
    result->measured_bytes = measured_bytes;
    result->indirect_reserve_bytes = indirect_reserve_bytes;
    result->required_free_bytes = stack_budget_required_free(configured_bytes,
                                                              owner);
    result->remaining_bytes = total_used < configured_bytes ?
                              configured_bytes - total_used : 0u;
    result->passes = total_used <= configured_bytes &&
                     result->remaining_bytes >= result->required_free_bytes;
    return PROTO_OK;
}
