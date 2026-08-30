#ifndef PROTOCOL_RX_LIFECYCLE_H
#define PROTOCOL_RX_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

enum protocol_rx_operation {
    PROTOCOL_RX_OPERATION_NONE = 0,
    PROTOCOL_RX_OPERATION_HERE_I_AM,
    PROTOCOL_RX_OPERATION_ENUMERATION,
    PROTOCOL_RX_OPERATION_SURVEY,
};

enum protocol_rx_mode {
    PROTOCOL_RX_MODE_LOW_DUTY = 0,
    PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5,
    PROTOCOL_RX_MODE_OWNED_RF_WORK,
};

enum protocol_rx_begin_result {
    PROTOCOL_RX_BEGIN_INVALID = 0,
    PROTOCOL_RX_BEGIN_ACCEPTED,
    PROTOCOL_RX_BEGIN_DUPLICATE,
    PROTOCOL_RX_BEGIN_BUSY,
};

enum protocol_rx_recovery_result {
    PROTOCOL_RX_RECOVERY_INVALID = 0,
    PROTOCOL_RX_RECOVERY_REARMED,
    PROTOCOL_RX_RECOVERY_TERMINATED,
};

/*
 * RAM-only ownership for one gateway-originated Channel-5 protocol.  The
 * caller maps LOW_DUTY and CONTINUOUS_CHANNEL5 onto the radio guard and scan
 * worker; this object deliberately owns no Zephyr work or persistence.
 */
struct protocol_rx_lifecycle {
    enum protocol_rx_operation operation;
    enum protocol_rx_mode mode;
    uint64_t generation;
    uint32_t deadline_ms;
};

struct protocol_rx_downstream_activation {
    uint64_t generation;
    uint32_t deadline_ms;
    enum protocol_rx_operation operation;
    bool activated;
};

void protocol_rx_lifecycle_init(struct protocol_rx_lifecycle *lifecycle);

enum protocol_rx_begin_result protocol_rx_lifecycle_begin(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t deadline_ms);

/* Replaces the immutable deadline only when a validated later phase of the
 * same operation supplies its own absolute stop. Duplicate begin calls still
 * cannot extend an operation accidentally. */
bool protocol_rx_lifecycle_set_deadline(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t deadline_ms);

bool protocol_rx_lifecycle_rf_begin(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation);

bool protocol_rx_lifecycle_rf_end(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation);

bool protocol_rx_lifecycle_terminate(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation);

/*
 * Records the bounded adapter-level recovery result after an unexpected RX
 * error. Successful recovery preserves the exact continuous-RX owner; failed
 * recovery clears it so the adapter can park fail-closed.
 */
enum protocol_rx_recovery_result protocol_rx_lifecycle_note_rx_recovery(
    struct protocol_rx_lifecycle *lifecycle,
    enum protocol_rx_operation operation,
    uint64_t generation,
    bool recovered);

/* Returns true only when an active owner timed out and was cleared. */
bool protocol_rx_lifecycle_expire(struct protocol_rx_lifecycle *lifecycle,
                                  uint32_t now_ms);

void protocol_rx_downstream_activation_init(
    struct protocol_rx_downstream_activation *activation);
bool protocol_rx_downstream_activation_needs_wake(
    const struct protocol_rx_downstream_activation *activation,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms);
bool protocol_rx_downstream_activation_mark(
    struct protocol_rx_downstream_activation *activation,
    enum protocol_rx_operation operation,
    uint64_t generation,
    uint32_t now_ms,
    uint32_t deadline_ms);
bool protocol_rx_downstream_activation_clear(
    struct protocol_rx_downstream_activation *activation,
    enum protocol_rx_operation operation,
    uint64_t generation);
bool protocol_rx_downstream_activation_expire(
    struct protocol_rx_downstream_activation *activation,
    uint32_t now_ms);

#endif
