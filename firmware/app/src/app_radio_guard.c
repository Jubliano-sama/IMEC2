#include "app_radio_guard.h"
#include "app_watchdog.h"

#if defined(CONFIG_IMEC_GATEWAY_BLE)
#include "app_gateway_ble.h"
#endif

#include <zephyr/kernel.h>

#if defined(__ZEPHYR__)
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_radio_guard, LOG_LEVEL_DBG);
#define RADIO_GUARD_LOG_ERR(...) LOG_ERR(__VA_ARGS__)
#else
#define RADIO_GUARD_LOG_ERR(...) ((void)0)
#endif

#include <errno.h>
#include <string.h>

static struct k_spinlock uwb_rf_lock;

static enum radio_guard_uwb_phase uwb_rf_phase;
static struct radio_guard_uwb_lease uwb_rf_owner;
static struct radio_guard_uwb_lease legacy_lease;
static const char *uwb_rf_owner_reason;
static uint32_t uwb_rf_owner_since_ms;
static uint32_t uwb_rf_next_generation;
static int uwb_rf_poison_error;
static bool uwb_rf_admission_paused;

static bool radio_guard_uwb_client_valid(enum radio_guard_uwb_client client)
{
    return client > RADIO_GUARD_UWB_CLIENT_NONE &&
           client < RADIO_GUARD_UWB_CLIENT_COUNT;
}

static bool radio_guard_uwb_lease_matches(
    const struct radio_guard_uwb_lease *lease)
{
    return lease != NULL && lease->generation != 0u &&
           uwb_rf_owner.generation == lease->generation &&
           uwb_rf_owner.client == lease->client;
}

static uint32_t radio_guard_uwb_next_generation(void)
{
    uwb_rf_next_generation++;
    if (uwb_rf_next_generation == 0u) {
        uwb_rf_next_generation++;
    }
    return uwb_rf_next_generation;
}

int radio_guard_uwb_claim(enum radio_guard_uwb_client client,
                          const char *reason,
                          struct radio_guard_uwb_lease *lease_out)
{
    k_spinlock_key_t key;
    enum radio_guard_uwb_phase phase;
    const char *owner_reason = NULL;
    enum radio_guard_uwb_client owner_client = RADIO_GUARD_UWB_CLIENT_NONE;
    uint32_t owner_since_ms = 0u;
    int ret;

    if (!radio_guard_uwb_client_valid(client) || reason == NULL ||
        lease_out == NULL) {
        return -EINVAL;
    }

    key = k_spin_lock(&uwb_rf_lock);
    phase = uwb_rf_phase;
    if (uwb_rf_admission_paused) {
        ret = -ESHUTDOWN;
    } else if (phase == RADIO_GUARD_UWB_POISONED) {
        ret = uwb_rf_poison_error < 0 ? uwb_rf_poison_error : -EIO;
    } else if (phase != RADIO_GUARD_UWB_IDLE) {
        ret = -EBUSY;
    } else {
        uwb_rf_owner = (struct radio_guard_uwb_lease) {
            .generation = radio_guard_uwb_next_generation(),
            .client = client,
        };
        uwb_rf_phase = RADIO_GUARD_UWB_ACTIVE;
        uwb_rf_owner_reason = reason;
        uwb_rf_owner_since_ms = k_uptime_get_32();
        *lease_out = uwb_rf_owner;
        ret = 0;
    }
    if (ret == -EBUSY) {
        owner_reason = uwb_rf_owner_reason;
        owner_client = uwb_rf_owner.client;
        owner_since_ms = uwb_rf_owner_since_ms;
    }
    k_spin_unlock(&uwb_rf_lock, key);

    if (ret < 0) {
        if (ret == -EBUSY) {
            RADIO_GUARD_LOG_ERR(
                "blocked UWB operation: %s owner=%s client=%u age_ms=%u",
                reason,
                owner_reason == NULL ? "unknown" : owner_reason,
                (unsigned int)owner_client,
                k_uptime_get_32() - owner_since_ms);
        }
        return ret;
    }

#if defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_enter_uwb_quiet(reason);
#endif
    return 0;
}

int radio_guard_uwb_release_begin(const struct radio_guard_uwb_lease *lease)
{
    k_spinlock_key_t key;
    int ret;

    if (lease == NULL || !radio_guard_uwb_client_valid(lease->client) ||
        lease->generation == 0u) {
        return -EINVAL;
    }

    key = k_spin_lock(&uwb_rf_lock);
    if (!radio_guard_uwb_lease_matches(lease)) {
        ret = -ESTALE;
    } else if (uwb_rf_phase == RADIO_GUARD_UWB_ACTIVE) {
        uwb_rf_phase = RADIO_GUARD_UWB_RELEASING;
        ret = 0;
    } else if (uwb_rf_phase == RADIO_GUARD_UWB_RELEASING) {
        ret = -EALREADY;
    } else if (uwb_rf_phase == RADIO_GUARD_UWB_POISONED) {
        ret = uwb_rf_poison_error < 0 ? uwb_rf_poison_error : -EIO;
    } else {
        ret = -ESTALE;
    }
    k_spin_unlock(&uwb_rf_lock, key);
    return ret;
}

int radio_guard_uwb_release_finish(struct radio_guard_uwb_lease *lease,
                                   int parking_result)
{
    k_spinlock_key_t key;
    bool exit_quiet = false;
    bool poisoned = false;
    int ret;

    if (lease == NULL || !radio_guard_uwb_client_valid(lease->client) ||
        lease->generation == 0u) {
        return -EINVAL;
    }

    key = k_spin_lock(&uwb_rf_lock);
    if (!radio_guard_uwb_lease_matches(lease)) {
        ret = -ESTALE;
    } else if (uwb_rf_phase != RADIO_GUARD_UWB_RELEASING) {
        ret = uwb_rf_phase == RADIO_GUARD_UWB_POISONED &&
                      uwb_rf_poison_error < 0 ?
                  uwb_rf_poison_error :
                  -EALREADY;
    } else if (parking_result < 0) {
        uwb_rf_phase = RADIO_GUARD_UWB_POISONED;
        uwb_rf_poison_error = parking_result;
        poisoned = true;
        ret = parking_result;
    } else {
        uwb_rf_phase = RADIO_GUARD_UWB_IDLE;
        memset(&uwb_rf_owner, 0, sizeof(uwb_rf_owner));
        uwb_rf_owner_reason = NULL;
        uwb_rf_owner_since_ms = 0u;
        uwb_rf_poison_error = 0;
        memset(lease, 0, sizeof(*lease));
        exit_quiet = true;
        ret = 0;
    }
    k_spin_unlock(&uwb_rf_lock, key);

    if (poisoned) {
        status_debug_printf("DBG_RF_POISON client=%u ret=%d\n", (unsigned int)lease->client, ret);
        RADIO_GUARD_LOG_ERR(
            "UWB parking failed; retaining poisoned lease: generation=%u client=%u ret=%d",
            lease->generation,
            (unsigned int)lease->client,
            ret);
        /* No unrelated progress may keep an unknown RF state alive. */
        app_watchdog_stop_feeding();
    }
#if defined(CONFIG_IMEC_GATEWAY_BLE)
    if (exit_quiet) {
        gateway_ble_exit_uwb_quiet("radio_guard");
    }
#else
    (void)exit_quiet;
#endif
    return ret;
}

bool radio_guard_uwb_busy(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);
    bool busy = uwb_rf_phase != RADIO_GUARD_UWB_IDLE;

    k_spin_unlock(&uwb_rf_lock, key);
    return busy;
}

enum radio_guard_uwb_client radio_guard_uwb_owner_client(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);
    enum radio_guard_uwb_client client =
        uwb_rf_phase == RADIO_GUARD_UWB_IDLE ?
            RADIO_GUARD_UWB_CLIENT_NONE : uwb_rf_owner.client;

    k_spin_unlock(&uwb_rf_lock, key);
    return client;
}

bool radio_guard_uwb_rearm_allowed(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);
    bool allowed = uwb_rf_phase != RADIO_GUARD_UWB_POISONED;

    k_spin_unlock(&uwb_rf_lock, key);
    return allowed;
}

bool radio_guard_uwb_poisoned(void)
{
    return !radio_guard_uwb_rearm_allowed();
}

int radio_guard_uwb_poison_error(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);
    int error = uwb_rf_phase == RADIO_GUARD_UWB_POISONED ?
                    uwb_rf_poison_error :
                    0;

    k_spin_unlock(&uwb_rf_lock, key);
    return error;
}

enum radio_guard_uwb_phase radio_guard_uwb_phase(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);
    enum radio_guard_uwb_phase phase = uwb_rf_phase;

    k_spin_unlock(&uwb_rf_lock, key);
    return phase;
}

void radio_guard_uwb_admission_pause(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);

    uwb_rf_admission_paused = true;
    k_spin_unlock(&uwb_rf_lock, key);
}

void radio_guard_uwb_admission_resume(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);

    uwb_rf_admission_paused = false;
    k_spin_unlock(&uwb_rf_lock, key);
}

bool radio_guard_uwb_admission_paused(void)
{
    k_spinlock_key_t key = k_spin_lock(&uwb_rf_lock);
    bool paused = uwb_rf_admission_paused;

    k_spin_unlock(&uwb_rf_lock, key);
    return paused;
}

int radio_guard_uwb_start(const char *reason)
{
    return radio_guard_uwb_claim(RADIO_GUARD_UWB_CLIENT_LEGACY,
                                 reason,
                                 &legacy_lease);
}

void radio_guard_uwb_stop(void)
{
    int ret;

    if (legacy_lease.generation == 0u) {
        return;
    }

    ret = radio_guard_uwb_release_begin(&legacy_lease);
    if (ret < 0) {
        RADIO_GUARD_LOG_ERR(
            "rejected compatibility UWB release: generation=%u client=%u ret=%d",
            legacy_lease.generation,
            (unsigned int)legacy_lease.client,
            ret);
        return;
    }
    ret = radio_guard_uwb_release_finish(&legacy_lease, 0);
    if (ret < 0) {
        RADIO_GUARD_LOG_ERR("compatibility UWB release failed: %d", ret);
    }
}
