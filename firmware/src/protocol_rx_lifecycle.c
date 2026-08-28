#include "protocol_rx_lifecycle.h"

#include <stddef.h>

static bool operation_valid(enum protocol_rx_operation operation)
{
    return operation >= PROTOCOL_RX_OPERATION_HERE_I_AM &&
           operation <= PROTOCOL_RX_OPERATION_SURVEY;
}

static bool identity_matches(const struct protocol_rx_lifecycle *lifecycle,
                             enum protocol_rx_operation operation,
                             uint64_t generation)
{
    return lifecycle != NULL && operation_valid(operation) &&
           generation != 0u && lifecycle->operation == operation &&
           lifecycle->generation == generation;
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool deadline_is_bounded_future(uint32_t now_ms,
                                       uint32_t deadline_ms)
{
    return (int32_t)(deadline_ms - now_ms) > 0;
}

void protocol_rx_lifecycle_init(struct protocol_rx_lifecycle *lifecycle)
{
    if (lifecycle != NULL) {
        *lifecycle = (struct protocol_rx_lifecycle) {0};
    }
}

enum protocol_rx_begin_result protocol_rx_lifecycle_begin(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t deadline_ms)
{
    if (lifecycle == NULL || !operation_valid(operation) || generation == 0u ||
        !deadline_is_bounded_future(now_ms, deadline_ms)) {
        return PROTOCOL_RX_BEGIN_INVALID;
    }
    if (lifecycle->operation != PROTOCOL_RX_OPERATION_NONE) {
        if (!identity_matches(lifecycle, operation, generation)) {
            return PROTOCOL_RX_BEGIN_BUSY;
        }
        /* Later phases and exact RF replays cannot extend the operation cap. */
        return PROTOCOL_RX_BEGIN_DUPLICATE;
    }
    lifecycle->operation = operation;
    lifecycle->mode = PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5;
    lifecycle->generation = generation;
    lifecycle->deadline_ms = deadline_ms;
    return PROTOCOL_RX_BEGIN_ACCEPTED;
}

bool protocol_rx_lifecycle_set_deadline(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t deadline_ms)
{
    if (!identity_matches(lifecycle, operation, generation) ||
        !deadline_is_bounded_future(now_ms, deadline_ms)) {
        return false;
    }
    lifecycle->deadline_ms = deadline_ms;
    return true;
}

bool protocol_rx_lifecycle_rf_begin(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation)
{
    if (!identity_matches(lifecycle, operation, generation) ||
        lifecycle->mode != PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5) {
        return false;
    }
    lifecycle->mode = PROTOCOL_RX_MODE_OWNED_RF_WORK;
    return true;
}

bool protocol_rx_lifecycle_rf_end(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation)
{
    if (!identity_matches(lifecycle, operation, generation) ||
        lifecycle->mode != PROTOCOL_RX_MODE_OWNED_RF_WORK) {
        return false;
    }
    lifecycle->mode = PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5;
    return true;
}

bool protocol_rx_lifecycle_terminate(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation)
{
    if (!identity_matches(lifecycle, operation, generation)) {
        return false;
    }
    protocol_rx_lifecycle_init(lifecycle);
    return true;
}

enum protocol_rx_recovery_result protocol_rx_lifecycle_note_rx_recovery(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation,
    bool recovered)
{
    if (!identity_matches(lifecycle, operation, generation) ||
        lifecycle->mode != PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5) {
        return PROTOCOL_RX_RECOVERY_INVALID;
    }
    if (recovered) {
        return PROTOCOL_RX_RECOVERY_REARMED;
    }
    protocol_rx_lifecycle_init(lifecycle);
    return PROTOCOL_RX_RECOVERY_TERMINATED;
}

bool protocol_rx_lifecycle_expire(struct protocol_rx_lifecycle *lifecycle,
                                  uint32_t now_ms)
{
    if (lifecycle == NULL ||
        lifecycle->operation == PROTOCOL_RX_OPERATION_NONE ||
        !deadline_reached(now_ms, lifecycle->deadline_ms)) {
        return false;
    }
    protocol_rx_lifecycle_init(lifecycle);
    return true;
}

void protocol_rx_downstream_activation_init(
    struct protocol_rx_downstream_activation *activation)
{
    if (activation != NULL) {
        *activation = (struct protocol_rx_downstream_activation) {0};
    }
}

bool protocol_rx_downstream_activation_needs_wake(
    const struct protocol_rx_downstream_activation *activation,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms)
{
    if (activation == NULL || !operation_valid(operation) || generation == 0u) {
        return false;
    }
    return !activation->activated ||
           deadline_reached(now_ms, activation->deadline_ms) ||
           activation->operation != operation ||
           activation->generation != generation;
}

bool protocol_rx_downstream_activation_mark(
    struct protocol_rx_downstream_activation *activation,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t deadline_ms)
{
    if (activation == NULL || !operation_valid(operation) || generation == 0u ||
        !deadline_is_bounded_future(now_ms, deadline_ms)) {
        return false;
    }
    if (activation->activated &&
        deadline_reached(now_ms, activation->deadline_ms)) {
        protocol_rx_downstream_activation_init(activation);
    }
    if (activation->activated &&
        (activation->operation != operation ||
         activation->generation != generation)) {
        return false;
    }
    activation->operation = operation;
    activation->generation = generation;
    activation->deadline_ms = deadline_ms;
    activation->activated = true;
    return true;
}

bool protocol_rx_downstream_activation_clear(
    struct protocol_rx_downstream_activation *activation,
    enum protocol_rx_operation operation,
    uint64_t generation)
{
    if (activation == NULL || !activation->activated ||
        activation->operation != operation ||
        activation->generation != generation) {
        return false;
    }
    protocol_rx_downstream_activation_init(activation);
    return true;
}

bool protocol_rx_downstream_activation_expire(
    struct protocol_rx_downstream_activation *activation,
    uint32_t now_ms)
{
    if (activation == NULL || !activation->activated ||
        !deadline_reached(now_ms, activation->deadline_ms)) {
        return false;
    }
    protocol_rx_downstream_activation_init(activation);
    return true;
}
