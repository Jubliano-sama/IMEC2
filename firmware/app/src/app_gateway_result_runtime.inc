#line 566 __BASE_FILE__
#if DEVICE_ROLE == ROLE_GATEWAY
static struct gateway_command_pending gateway_command_pending_state;
static struct gateway_command_result_validation_leases
    gateway_command_result_validation_state;
static struct app_gateway_command_result_queue gateway_host_command_results;
static struct gateway_collection_state gateway_collection_state;
static struct gateway_membership_roster gateway_membership_roster_state;
static struct k_work_delayable gateway_command_result_timeout_work;
static struct k_work_delayable gateway_command_timeout_side_effect_work;
static struct k_work_delayable gateway_command_timeout_side_effect_retry_work;
static struct k_work_delayable gateway_collection_eack_work;
static struct app_gateway_eack_retry_state gateway_collection_eack_retry_state;
static bool gateway_collection_eack_round_dirty;
static bool gateway_collection_recovery_eack_active;
static struct k_work_delayable gateway_persistence_retry_work;
static bool gateway_collection_persistence_dirty;
static bool gateway_collection_clear_pending;
static bool gateway_collection_restore_pending;
static bool gateway_eack_custody_clear_pending;
static bool gateway_membership_persistence_dirty;
static bool gateway_membership_clear_pending;
static bool gateway_membership_restore_pending;
static atomic_t gateway_assignment_publication_pending_state =
    ATOMIC_INIT(0);
static atomic_t gateway_host_journal_restore_pending = ATOMIC_INIT(0);
static atomic_t gateway_host_journal_restored = ATOMIC_INIT(0);
static bool gateway_ble_stream_initialized;
static atomic_t gateway_persistence_retry_schedule_deferred =
    ATOMIC_INIT(0);
static atomic_t gateway_persistence_retry_deferred_delay_ms =
    ATOMIC_INIT(0);
static atomic_t gateway_collection_eack_schedule_deferred =
    ATOMIC_INIT(0);
static atomic_t gateway_collection_eack_deferred_delay_ms =
    ATOMIC_INIT(0);
static struct k_spinlock gateway_host_command_result_lock;
K_MUTEX_DEFINE(gateway_command_pending_lock);
static uint8_t gateway_persistence_retry_round;
static uint32_t gateway_command_result_dispatch_token;
static uint32_t gateway_pending_command_result_token;

BUILD_ASSERT(APP_GATEWAY_COMMAND_RESULT_BUFFERED_INGRESS_DEPTH ==
                 GATEWAY_BLE_RX_FRAME_QUEUE_DEPTH,
             "command result credits must cover every buffered GATT frame");

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id);
void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason);

static bool gateway_collection_tracking_active(void);
static void gateway_schedule_collection_eack_round(void);
static int gateway_restore_membership_runtime(void);
static int gateway_restore_collection_runtime(void);
static int gateway_restore_host_journal_runtime(void);
static int gateway_retire_notified_host_journal_runtime(void);

struct gateway_decoded_command_result {
    enum command_status status;
    uint8_t reason;
};

struct gateway_command_timeout_dispatch {
    struct proto_packet command;
    uint32_t result_reservation_token;
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool pending;
    bool running;
};

static struct gateway_command_timeout_dispatch
    gateway_command_timeout_dispatch_state;

#define GATEWAY_COMMAND_TIMEOUT_SIDE_EFFECT_RETRY_MS 10u
#define GATEWAY_PROTOCOL_VALIDATION_RECHECK_MS 10u
#define GATEWAY_PROTOCOL_VALIDATION_PROCESSING_GUARD_MS 100u

static int gateway_pending_command_lock(void)
{
    if (k_is_in_isr()) {
        LOG_ERR("gateway pending command lock requested from ISR");
        return -EWOULDBLOCK;
    }
    return k_mutex_lock(&gateway_command_pending_lock, K_FOREVER);
}

static bool gateway_pending_snapshot_current_locked(
    const struct gateway_command_pending *snapshot)
{
    return snapshot != NULL &&
           snapshot->active &&
           gateway_command_pending_state.active &&
           gateway_command_pending_state.command_id == snapshot->command_id &&
           gateway_command_pending_state.started_at_ms ==
               snapshot->started_at_ms &&
           gateway_command_pending_state.deadline_ms == snapshot->deadline_ms &&
           gateway_command_pending_state.command.msg_type ==
               snapshot->command.msg_type &&
           gateway_command_pending_state.command.src_id ==
               snapshot->command.src_id &&
           gateway_command_pending_state.command.dst_id ==
               snapshot->command.dst_id &&
           gateway_command_pending_state.command.session_id ==
               snapshot->command.session_id &&
           gateway_command_pending_state.command.seq ==
               snapshot->command.seq;
}

static bool gateway_command_result_validation_rearm_locked(
    uint32_t *delay_ms)
{
    enum gateway_command_result_validation_check validation_check;
    uint32_t now_ms;

    if (delay_ms == NULL ||
        !gateway_command_pending_state.active ||
        gateway_command_timeout_dispatch_state.pending) {
        return false;
    }
    now_ms = k_uptime_get_32();
    validation_check = gateway_command_result_validation_check_interval(
        &gateway_command_result_validation_state,
        gateway_command_pending_state.started_at_ms,
        gateway_command_pending_state.deadline_ms,
        now_ms);
    if (validation_check ==
        GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
        /*
         * Releasing one lease does not mean that another physical receive
         * has finished semantic validation.  Keep polling the immutable
         * command boundary until every relevant lease is clear or one
         * expires; otherwise the timeout worker can remain unscheduled
         * forever after the first release.
         */
        *delay_ms = GATEWAY_PROTOCOL_VALIDATION_RECHECK_MS;
        return true;
    }
    *delay_ms =
        (int32_t)(now_ms - gateway_command_pending_state.deadline_ms) >= 0 ?
        0u : gateway_command_pending_state.deadline_ms - now_ms;
    return true;
}

static int gateway_command_result_timeout_arm(uint32_t delay_ms,
                                              const char *source)
{
    int ret = k_work_reschedule(
        &gateway_command_result_timeout_work, K_MSEC(delay_ms));

    if (ret < 0) {
        LOG_ERR("gateway command timeout owner scheduling failed closed: source=%s ret=%d delay_ms=%u",
                source == NULL ? "unknown" : source,
                ret,
                delay_ms);
        app_watchdog_stop_feeding();
    }
    return ret;
}

int gateway_command_result_validation_reserve(
    const struct proto_packet *result,
    uint64_t received_at_ms,
    uint32_t *token)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || token == NULL) {
        return -EINVAL;
    }
    *token = 0u;
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    ret = gateway_command_result_validation_acquire(
        &gateway_command_result_validation_state,
        &gateway_command_pending_state,
        result,
        received_at_ms,
        token);
    k_mutex_unlock(&gateway_command_pending_lock);
    if (ret == PROTO_OK) {
        return 0;
    }
    return ret == PROTO_ERR_NOT_FOUND ? -ENOENT :
           ret == PROTO_ERR_NO_SPACE ? -ENOSPC :
           mesh_errno_from_proto(ret);
}

int gateway_protocol_validation_arm(uint32_t receive_timeout_ms,
                                    uint32_t *token)
{
    uint32_t hold_ms;
    uint32_t now_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || token == NULL ||
        receive_timeout_ms == 0u ||
        receive_timeout_ms >
            GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS -
            GATEWAY_PROTOCOL_VALIDATION_PROCESSING_GUARD_MS) {
        return -EINVAL;
    }
    *token = 0u;
    hold_ms = receive_timeout_ms +
              GATEWAY_PROTOCOL_VALIDATION_PROCESSING_GUARD_MS;
    now_ms = k_uptime_get_32();
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    ret = gateway_command_result_validation_arm(
        &gateway_command_result_validation_state,
        now_ms,
        now_ms + hold_ms,
        token);
    k_mutex_unlock(&gateway_command_pending_lock);
    return ret == PROTO_OK ? 0 :
           ret == PROTO_ERR_NO_SPACE ? -ENOSPC :
           mesh_errno_from_proto(ret);
}

int gateway_protocol_validation_complete(uint32_t token,
                                         uint64_t received_at_ms)
{
    bool completed;

    if (DEVICE_ROLE != ROLE_GATEWAY || token == 0u ||
        gateway_pending_command_lock() < 0) {
        return -EINVAL;
    }
    completed = gateway_command_result_validation_complete(
        &gateway_command_result_validation_state,
        token,
        (uint32_t)received_at_ms);
    k_mutex_unlock(&gateway_command_pending_lock);
    return completed ? 0 : -ESTALE;
}

enum gateway_command_result_validation_check
gateway_protocol_validation_check_interval(uint32_t started_at_ms,
                                           uint32_t deadline_ms)
{
    enum gateway_command_result_validation_check check =
        GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR;

    if (DEVICE_ROLE != ROLE_GATEWAY ||
        gateway_pending_command_lock() < 0) {
        return GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED;
    }
    check = gateway_command_result_validation_check_interval(
        &gateway_command_result_validation_state,
        started_at_ms,
        deadline_ms,
        k_uptime_get_32());
    k_mutex_unlock(&gateway_command_pending_lock);
    return check;
}

void gateway_command_result_validation_release_reserved(uint32_t token)
{
    uint32_t delay_ms = 0u;
    bool rearm = false;

    if (DEVICE_ROLE != ROLE_GATEWAY || token == 0u ||
        gateway_pending_command_lock() < 0) {
        return;
    }
    if (gateway_command_result_validation_release(
            &gateway_command_result_validation_state, token)) {
        rearm = gateway_command_result_validation_rearm_locked(&delay_ms);
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    if (rearm) {
        (void)gateway_command_result_timeout_arm(
            delay_ms, "validation-release");
    }
}

static uint32_t gateway_command_result_validation_adopt(
    const struct proto_packet *result,
    uint64_t received_at_ms,
    uint32_t token)
{
    uint32_t adopted_token = token;
    int ret;

    if (gateway_pending_command_lock() < 0) {
        return 0u;
    }
    if (!gateway_command_result_validation_contains(
            &gateway_command_result_validation_state,
            &gateway_command_pending_state,
            token,
            result,
            received_at_ms)) {
        adopted_token = 0u;
        ret = gateway_command_result_validation_acquire(
            &gateway_command_result_validation_state,
            &gateway_command_pending_state,
            result,
            received_at_ms,
            &adopted_token);
        if (ret != PROTO_OK) {
            adopted_token = 0u;
        }
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    return adopted_token;
}

static int gateway_capture_decoded_command_result(
    void *ctx,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    struct gateway_decoded_command_result *decoded = ctx;

    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    if (decoded == NULL) {
        return -EINVAL;
    }
    decoded->status = status;
    decoded->reason = reason;
    return 0;
}

static void gateway_schedule_persistence_retry(const char *reason)
{
    uint32_t delay_ms = discovery_assignment_retry_backoff_ms(
        gateway_persistence_retry_round,
        sys_rand32_get());
    int schedule_ret;

    if (gateway_persistence_retry_round < UINT8_MAX) {
        gateway_persistence_retry_round++;
    }
    /*
     * Collection persistence reads the same mutable snapshot as EACK
     * generation.  Keep it on the mesh-route owner instead of allowing the
     * system workqueue to race result ingestion or round advancement.
     */
    schedule_ret = mesh_route_work_reschedule(
        &gateway_persistence_retry_work, delay_ms);
    if (schedule_ret < 0) {
        atomic_set(&gateway_persistence_retry_deferred_delay_ms,
                   (atomic_val_t)delay_ms);
        atomic_set(&gateway_persistence_retry_schedule_deferred, 1);
        if (schedule_ret != -ESHUTDOWN) {
            LOG_ERR("gateway persistence retry lost its route owner: ret=%d",
                    schedule_ret);
            app_watchdog_stop_feeding();
        }
    } else {
        atomic_clear(&gateway_persistence_retry_schedule_deferred);
    }
    LOG_WRN("gateway persistence retry scheduled: reason=%s round=%u delay_ms=%u schedule_ret=%d",
            reason == NULL ? "unknown" : reason,
            gateway_persistence_retry_round,
            delay_ms,
            schedule_ret);
}

static int gateway_clear_eack_custody_or_defer(const char *reason)
{
    int ret = app_mesh_persistence_clear_gateway_eack_custody();

    if (ret < 0) {
        /*
         * The old key may describe an EACK that reached RF. Keep one explicit
         * runtime owner until its tombstone is durable, and stop every path
         * that could freeze or transmit a replacement identity.
         */
        gateway_eack_custody_clear_pending = true;
        atomic_clear(&gateway_collection_eack_schedule_deferred);
        atomic_clear(&gateway_collection_eack_deferred_delay_ms);
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
        gateway_schedule_persistence_retry(reason);
        LOG_WRN("gateway EACK custody clear deferred until NVS delete succeeds: ret=%d reason=%s",
                ret,
                reason == NULL ? "" : reason);
        return ret;
    }

    gateway_eack_custody_clear_pending = false;
    return 0;
}

static void gateway_finalize_collection_clear(void)
{
    gateway_collection_clear(&gateway_collection_state);
    gateway_collection_eack_round_dirty = false;
    gateway_collection_persistence_dirty = false;
    gateway_collection_clear_pending = false;
    if (!gateway_collection_recovery_eack_active) {
        app_gateway_eack_retry_reset(
            &gateway_collection_eack_retry_state);
        (void)gateway_clear_eack_custody_or_defer(
            "collection-clear-eack-custody");
        atomic_clear(&gateway_collection_eack_schedule_deferred);
        atomic_clear(&gateway_collection_eack_deferred_delay_ms);
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
    }
}

static void gateway_finalize_membership_clear(void)
{
    gateway_membership_clear(&gateway_membership_roster_state);
    gateway_membership_persistence_dirty = false;
    gateway_membership_clear_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state, 0);
}

/*
 * Bluetooth completion owns only the volatile ATT-credit transition.  Flash
 * reads and writes run here on the route owner so the bt_rx stack cannot enter
 * the NVS metadata/digest chain.  The stream retains the exact journal head
 * until the durable NOTIFIED transition either succeeds or returns a result
 * that restore can classify conservatively.
 */
static int gateway_retire_notified_host_journal_runtime(void)
{
    struct proto_packet packet;
    uint8_t payload_digest[SEMANTIC_DIGEST_SHA256_LEN];
    k_spinlock_key_t key;
    int marker_ret;
    int ret;

    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.head_send_phase !=
        GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED) {
        k_spin_unlock(&gateway_ble_stream_lock, key);
        return 0;
    }
    ret = gateway_ble_stream_head_journal_identity(
        &gateway_ble_stream_state, &packet, payload_digest);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret < 0) {
        return ret;
    }

    marker_ret =
        app_mesh_persistence_mark_gateway_host_journal_notified_if_matches_digest(
            &packet, payload_digest);
    if (marker_ret == -EAGAIN || marker_ret == -EBUSY) {
        return marker_ret;
    }

    /*
     * A non-transient marker error can mean either that the write never
     * happened or that NOTIFIED became durable but its verification failed.
     * Remove only the exact retained RAM owner, then let durable restore
     * distinguish COMMITTED (replay) from NOTIFIED (receipt/clear).
     */
    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.head_send_phase !=
        GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED) {
        ret = -ESTALE;
    } else {
        gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                     k_uptime_get_32());
        ret = 0;
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret < 0) {
        LOG_ERR("gateway journal phase race: %d", ret);
        app_watchdog_stop_feeding();
        return ret;
    }

    atomic_clear(&gateway_host_journal_restored);
    atomic_set(&gateway_host_journal_restore_pending, 1);
    if (marker_ret < 0) {
        LOG_WRN("gateway journal marker ret=%d; reconciling", marker_ret);
    }
    return 1;
}

static void gateway_persistence_retry_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);
    atomic_clear(&gateway_persistence_retry_schedule_deferred);
    atomic_clear(&gateway_persistence_retry_deferred_delay_ms);

    ret = gateway_retire_notified_host_journal_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("host-output-restore");
        return;
    }
    if (gateway_membership_restore_pending) {
        ret = gateway_restore_membership_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-restore");
            return;
        }
    }
    if (gateway_collection_restore_pending) {
        ret = gateway_restore_collection_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("collection-restore");
            return;
        }
    }
    /*
     * PREPARED host-output classification may consult the restored durable
     * collection/membership proof. Never retry it ahead of those owners or a
     * transient restore failure can self-sustain forever.
     */
    if (atomic_get(&gateway_host_journal_restore_pending) != 0) {
        ret = gateway_restore_host_journal_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("host-output-restore");
            return;
        }
        /*
         * The host-complete head remains a stream barrier until NOTIFIED has
         * been classified, receipts have landed, and the durable owner has
         * cleared. Only then may a later queued record consume ATT credit.
         */
        gateway_ble_schedule_stream_drain();
    }
    if (gateway_eack_custody_clear_pending) {
        ret = gateway_clear_eack_custody_or_defer(
            "eack-custody-clear");
        if (ret < 0) {
            return;
        }
        if (!gateway_collection_clear_pending &&
            !gateway_collection_restore_pending &&
            gateway_collection_tracking_active() &&
            gateway_collection_state.eack_pending) {
            gateway_schedule_collection_eack_round();
        }
    }
    if (gateway_membership_clear_pending) {
        ret = app_mesh_persistence_clear_gateway_membership();
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-clear");
            return;
        }
        gateway_finalize_membership_clear();
    }
    if (gateway_collection_clear_pending) {
        ret = app_mesh_persistence_clear_gateway_collection();
        if (ret < 0) {
            gateway_schedule_persistence_retry("collection-clear");
            return;
        }
        gateway_finalize_collection_clear();
    }
    if (gateway_collection_persistence_dirty) {
        if (!gateway_collection_tracking_active()) {
            gateway_collection_persistence_dirty = false;
        } else {
            ret = app_mesh_persistence_save_gateway_collection(
                &gateway_collection_state);
            if (ret < 0) {
                gateway_schedule_persistence_retry("collection");
                return;
            }
            gateway_collection_persistence_dirty = false;
        }
    }
    if (gateway_membership_persistence_dirty) {
        ret = app_mesh_persistence_save_gateway_membership(
            &gateway_membership_roster_state);
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership");
            return;
        }
        gateway_membership_persistence_dirty = false;
    }
    if (gateway_ble_stream_initialized &&
        atomic_get(&gateway_assignment_publication_pending_state) != 0) {
        struct app_gateway_assignment_publisher_diagnostics diagnostics;

        app_gateway_assignment_publisher_get_diagnostics(&diagnostics);
        if (diagnostics.active) {
            ret = app_gateway_assignment_publisher_complete_pending();
            if (ret < 0) {
                gateway_schedule_persistence_retry(
                    "assignment-publication-complete");
                return;
            }
            if (ret == 0) {
                app_gateway_assignment_publisher_pump();
            }
        } else {
            ret = gateway_resume_pending_assignment_publication();
            if (ret < 0) {
                gateway_schedule_persistence_retry(
                    "assignment-publication-restore");
                return;
            }
        }
    }
    gateway_persistence_retry_round = 0u;
}

void gateway_command_result_tracking_resume(void)
{
    uint32_t delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    /*
     * Only inspect the atomic scheduling latches here. The durable state
     * itself belongs to the mesh-route queue and must not be sampled from the
     * lifecycle thread that resumes transport.
     */
    if (atomic_cas(&gateway_persistence_retry_schedule_deferred, 1, 0)) {
        delay_ms = (uint32_t)atomic_get(
            &gateway_persistence_retry_deferred_delay_ms);
        ret = mesh_route_work_reschedule(
            &gateway_persistence_retry_work, delay_ms);
        if (ret < 0) {
            atomic_set(&gateway_persistence_retry_deferred_delay_ms,
                       (atomic_val_t)delay_ms);
            atomic_set(&gateway_persistence_retry_schedule_deferred, 1);
            LOG_WRN("gateway persistence retry resume deferred: ret=%d", ret);
            if (ret != -ESHUTDOWN) {
                app_watchdog_stop_feeding();
            }
        }
    }

    if (atomic_cas(&gateway_collection_eack_schedule_deferred, 1, 0)) {
        delay_ms = (uint32_t)atomic_get(
            &gateway_collection_eack_deferred_delay_ms);
        ret = mesh_route_work_reschedule(
            &gateway_collection_eack_work, delay_ms);
        if (ret < 0) {
            atomic_set(&gateway_collection_eack_deferred_delay_ms,
                       (atomic_val_t)delay_ms);
            atomic_set(&gateway_collection_eack_schedule_deferred, 1);
            LOG_WRN("gateway collection EACK resume deferred: ret=%d", ret);
            if (ret != -ESHUTDOWN) {
                app_watchdog_stop_feeding();
            }
        }
    }
}
#endif

int gateway_ble_stream_packet(const struct proto_packet *packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint32_t received_at_ms)
{
    enum gateway_ble_stream_class packet_class;
    k_spinlock_key_t key;
    bool retain_until_sent;
    bool ready;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

    packet_class = packet == NULL ?
        GATEWAY_BLE_STREAM_CLASS_UNKNOWN :
        gateway_ble_stream_classify_packet(packet->msg_type, packet->flags);
    /*
     * A durable host record from the previous boot owns recovery progress.
     * Best-effort telemetry must not continuously refill the only queue from
     * which that owner is waiting to evict or drain records.
     */
#if DEVICE_ROLE == ROLE_GATEWAY
    if (atomic_get(&gateway_host_journal_restore_pending) != 0 &&
        (packet_class == GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC ||
         packet_class == GATEWAY_BLE_STREAM_CLASS_STATUS)) {
        return -EAGAIN;
    }
#endif

    ready = gateway_ble_stream_ready();
    retain_until_sent = false;
    if (packet != NULL &&
        packet->msg_type == MSG_GATEWAY_COMMAND_EVENT &&
        payload != NULL) {
        struct gateway_command_event event;

        retain_until_sent =
            packet->flags == 0u &&
            packet->src_id != 0u &&
            packet->src_id == packet->dst_id &&
            gateway_command_event_decode(payload,
                                         payload_len,
                                         &event) == 0 &&
            event.event_seq != 0u &&
            packet->session_id == event.event_seq &&
            packet->seq == (uint16_t)event.event_seq &&
            (event.flags & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u;
    }
    key = k_spin_lock(&gateway_ble_stream_lock);
    if (retain_until_sent) {
        ret = gateway_ble_stream_enqueue_retained_packet(
            &gateway_ble_stream_state,
            packet,
            payload,
            payload_len,
            received_at_ms,
            k_uptime_get_32(),
            ready);
    } else {
        ret = gateway_ble_stream_enqueue_packet(&gateway_ble_stream_state,
                                                packet,
                                                payload,
                                                payload_len,
                                                received_at_ms,
                                                k_uptime_get_32(),
                                                ready);
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret < 0 ? ret : 0;
}

#if DEVICE_ROLE == ROLE_GATEWAY
static unsigned int gateway_flush_host_command_results(void)
{
    struct app_gateway_command_result_item item;
    k_spinlock_key_t key;
    unsigned int moved = 0u;
    bool ready;

    if (!gateway_ble_stream_initialized) {
        return 0u;
    }

    key = k_spin_lock(&gateway_host_command_result_lock);
    if (!app_gateway_command_result_flush_begin(
            &gateway_host_command_results)) {
        k_spin_unlock(&gateway_host_command_result_lock, key);
        return 0u;
    }
    k_spin_unlock(&gateway_host_command_result_lock, key);

    for (;;) {
        int ret;

        key = k_spin_lock(&gateway_host_command_result_lock);
        ret = app_gateway_command_result_queue_peek(
            &gateway_host_command_results, &item);
        k_spin_unlock(&gateway_host_command_result_lock, key);
        if (ret < 0) {
            break;
        }

        ready = gateway_ble_stream_ready();
        key = k_spin_lock(&gateway_ble_stream_lock);
        ret = gateway_ble_stream_enqueue_retained_packet(
            &gateway_ble_stream_state,
            &item.packet,
            item.payload,
            item.payload_len,
            k_uptime_get_32(),
            k_uptime_get_32(),
            ready);
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (ret <= 0) {
            break;
        }

        key = k_spin_lock(&gateway_host_command_result_lock);
        ret = app_gateway_command_result_queue_pop(
            &gateway_host_command_results);
        k_spin_unlock(&gateway_host_command_result_lock, key);
        if (ret < 0) {
            LOG_ERR("gateway command result custody desynchronized: %d", ret);
            break;
        }
        moved++;
    }

    key = k_spin_lock(&gateway_host_command_result_lock);
    app_gateway_command_result_flush_end(&gateway_host_command_results);
    k_spin_unlock(&gateway_host_command_result_lock, key);
    if (moved > 0u) {
        gateway_ble_schedule_stream_drain();
        gateway_ble_resume_rx();
    }
    return moved;
}

int gateway_command_result_reserve_ingress(uint32_t *token)
{
    k_spinlock_key_t key;
    int ret;

    if (token == NULL) {
        return -EINVAL;
    }
    (void)gateway_flush_host_command_results();
    key = k_spin_lock(&gateway_host_command_result_lock);
    ret = app_gateway_command_result_reserve(
        &gateway_host_command_results, token);
    k_spin_unlock(&gateway_host_command_result_lock, key);
    return ret;
}

int gateway_command_result_bind_ingress(uint32_t token,
                                        const struct proto_packet *command,
                                        enum command_id command_id)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_result_lock);
    int ret = app_gateway_command_result_bind(
        &gateway_host_command_results, token, command, command_id);

    k_spin_unlock(&gateway_host_command_result_lock, key);
    return ret;
}

int gateway_command_result_rebind_command(
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result_command)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_result_lock);
    int ret = app_gateway_command_result_rebind(
        &gateway_host_command_results,
        gateway_command_result_dispatch_token,
        command,
        command_id,
        result_command);

    k_spin_unlock(&gateway_host_command_result_lock, key);
    return ret;
}

void gateway_command_result_release_ingress(uint32_t token)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_result_lock);
    int ret = app_gateway_command_result_release(
        &gateway_host_command_results, token);

    k_spin_unlock(&gateway_host_command_result_lock, key);
    if (ret == 0) {
        gateway_ble_resume_rx();
    }
}

void gateway_command_result_release_command(
    const struct proto_packet *command,
    enum command_id command_id)
{
    gateway_command_result_release_reserved(
        gateway_command_result_dispatch_token, command, command_id);
}

void gateway_command_result_release_reserved(
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_result_lock);
    int ret = app_gateway_command_result_release_command(
        &gateway_host_command_results, token, command, command_id);

    k_spin_unlock(&gateway_host_command_result_lock, key);
    if (ret == 0) {
        gateway_ble_resume_rx();
    }
}

void gateway_command_result_release_terminal_reserved(
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_result_lock);
    int ret = app_gateway_command_result_release_terminal(
        &gateway_host_command_results, token, command, command_id);

    k_spin_unlock(&gateway_host_command_result_lock, key);
    if (ret == 0) {
        gateway_ble_resume_rx();
    }
}
#else
int gateway_command_result_reserve_ingress(uint32_t *token)
{
    ARG_UNUSED(token);
    return -ENOTSUP;
}

int gateway_command_result_bind_ingress(uint32_t token,
                                        const struct proto_packet *command,
                                        enum command_id command_id)
{
    ARG_UNUSED(token);
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    return -ENOTSUP;
}

int gateway_command_result_rebind_command(
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result_command)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(result_command);
    return -ENOTSUP;
}

void gateway_command_result_release_ingress(uint32_t token)
{
    ARG_UNUSED(token);
}

void gateway_command_result_release_command(
    const struct proto_packet *command,
    enum command_id command_id)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
}

void gateway_command_result_release_reserved(
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    ARG_UNUSED(token);
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
}

void gateway_command_result_release_terminal_reserved(
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    ARG_UNUSED(token);
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
}
#endif

int gateway_ble_reserve_stream_packet(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms)
{
    k_spinlock_key_t key;
    bool ready;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

#if DEVICE_ROLE == ROLE_GATEWAY
    if (app_mesh_persistence_gateway_host_journal_supports(packet)) {
        int journal_ret;

        /* A journal left by a previous boot owns admission until it has been
         * restored into the host queue.  This prevents a new accepted host
         * record from overwriting the only durable copy. */
        if (atomic_get(&gateway_host_journal_restore_pending) != 0) {
            return -EAGAIN;
        }
        journal_ret = app_mesh_persistence_gateway_host_journal_matches(
            packet, payload, payload_len);
        if (journal_ret ==
                APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED ||
            journal_ret ==
                APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW) {
            return GATEWAY_BLE_STREAM_RESERVATION_JOURNAL_TERMINAL;
        }
        if (journal_ret ==
                APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED) {
            gateway_ble_require_host_journal_restore(
                "host-output-notified-retirement");
            /*
             * Host delivery is already terminal. Restore will transfer this
             * raw identity into the compact receipt ledger, but the retained
             * NOTIFIED journal is itself sufficient exact proof to re-ACK a
             * lost raw ACK now without exposing the record to BLE again.
             */
            return GATEWAY_BLE_STREAM_RESERVATION_JOURNAL_TERMINAL;
        }
        if (journal_ret == -EBADMSG) {
            /*
             * The marker may be the only evidence for a record already
             * gateway-ACKed to its sender.  Put the existing restore owner into
             * service so every later admission remains closed until explicit
             * repair, without reserving another host queue slot.
             */
            gateway_ble_require_host_journal_restore(
                "host-output-corrupt-custody");
        }
        if (journal_ret < 0) {
            return journal_ret;
        }
        if (app_gateway_terminal_receipts_supports(
                packet, payload, payload_len)) {
            journal_ret = app_gateway_terminal_receipts_classify(
                packet, payload, payload_len, k_uptime_get_32());
            if (journal_ret == 1) {
                return GATEWAY_BLE_STREAM_RESERVATION_RECEIPT_TERMINAL;
            }
            if (journal_ret < 0) {
                return journal_ret;
            }
        }
        /*
         * An exact PREPARED retry still needs bounded BLE capacity while it
         * re-runs semantic preflight/commit and promotes the durable marker.
         */
    }
#endif

    ready = gateway_ble_stream_ready();
    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_reserve_packet(&gateway_ble_stream_state,
                                            packet,
                                            payload,
                                            payload_len,
                                            received_at_ms,
                                            k_uptime_get_32(),
                                            ready);
    k_spin_unlock(&gateway_ble_stream_lock, key);
    return ret;
}

int gateway_ble_commit_stream_reservation(const struct proto_packet *packet,
                                          const uint8_t *payload,
                                          size_t payload_len)
{
    k_spinlock_key_t key;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_commit_reservation(&gateway_ble_stream_state,
                                                packet,
                                                payload,
                                                payload_len);
    k_spin_unlock(&gateway_ble_stream_lock, key);
#if DEVICE_ROLE == ROLE_GATEWAY
    if (ret < 0 &&
        app_mesh_persistence_gateway_host_journal_supports(packet)) {
        gateway_ble_require_host_journal_restore(
            "host-output-stream-commit");
    }
#endif
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret;
}

int gateway_ble_commit_stream_reservation_projection(
    const struct proto_packet *packet,
    const uint8_t *raw_payload,
    size_t raw_payload_len,
    uint8_t accepted_record_mask)
{
    k_spinlock_key_t key;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }
    if (accepted_record_mask == 0u) {
        return gateway_ble_commit_stream_reservation(
            packet, raw_payload, raw_payload_len);
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_commit_bundle_projection_reservation(
        &gateway_ble_stream_state,
        packet,
        raw_payload,
        raw_payload_len,
        accepted_record_mask);
    k_spin_unlock(&gateway_ble_stream_lock, key);
#if DEVICE_ROLE == ROLE_GATEWAY
    if (ret < 0 &&
        app_mesh_persistence_gateway_host_journal_supports(packet)) {
        gateway_ble_require_host_journal_restore(
            "host-output-projection-stream-commit");
    }
#endif
    if (ret > 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret;
}

void gateway_ble_cancel_stream_reservation(void)
{
    k_spinlock_key_t key;

    if (DEVICE_ROLE != ROLE_GATEWAY || !gateway_ble_transport_enabled()) {
        return;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    gateway_ble_stream_cancel_reservation(&gateway_ble_stream_state);
    k_spin_unlock(&gateway_ble_stream_lock, key);
}

void gateway_ble_require_host_journal_restore(const char *reason)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    atomic_set(&gateway_host_journal_restore_pending, 1);
    atomic_clear(&gateway_host_journal_restored);
    gateway_schedule_persistence_retry(reason);
#else
    ARG_UNUSED(reason);
#endif
}

void gateway_ble_stream_get_status(struct gateway_ble_stream_diagnostics *diagnostics)
{
    k_spinlock_key_t key;

    if (diagnostics == NULL) {
        return;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    gateway_ble_stream_get_diagnostics(&gateway_ble_stream_state,
                                       k_uptime_get_32(),
                                       diagnostics);
    k_spin_unlock(&gateway_ble_stream_lock, key);
}

#if DEVICE_ROLE == ROLE_GATEWAY
static int gateway_recover_prepared_host_journal(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t received_at_ms)
{
    const uint8_t *collection_epoch_raw = NULL;
    uint8_t collection_epoch_len = 0u;
    bool collection_record = false;
    int ret;

    if (packet == NULL || payload == NULL ||
        packet->payload_len != payload_len) {
        return -EINVAL;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
        ret = app_mesh_persistence_commit_gateway_host_journal(
            packet, payload, payload_len);
        return ret < 0 ? ret :
               APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED;
    case MSG_RESULT_BUNDLE:
        collection_record = true;
        break;
    case MSG_COMMAND_RESULT: {
        struct discovery_assignment_result assignment_result;
        int assignment_ret = discovery_assignment_parse_result_tlvs(
            payload, payload_len, &assignment_result);

        if (assignment_ret == PROTO_OK) {
            /*
             * Assignment control is not host output and current firmware
             * never journals it. Retire a legacy/torn record so the sender
             * can retry against the restored durable membership proof.
             */
            ret =
                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                    packet, payload, payload_len);
            return ret < 0 ? ret : 0;
        }
        if (assignment_ret != PROTO_ERR_NOT_FOUND) {
            ret =
                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                    packet, payload, payload_len);
            return ret < 0 ? ret : 0;
        }

        ret = tlv_find_unique(payload,
                              payload_len,
                              TLV_COLLECTION_EPOCH_ID,
                              &collection_epoch_raw,
                              &collection_epoch_len);
        if (ret == PROTO_OK) {
            if (collection_epoch_len != sizeof(uint32_t)) {
                ret =
                    app_mesh_persistence_clear_gateway_host_journal_if_matches(
                        packet, payload, payload_len);
                return ret < 0 ? ret : 0;
            }
            collection_record = true;
        } else if (ret != PROTO_ERR_NOT_FOUND) {
            ret =
                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                    packet, payload, payload_len);
            return ret < 0 ? ret : 0;
        }
        break;
    }
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        break;
    default:
        return -EBADMSG;
    }

    if (collection_record) {
        if (gateway_collection_restore_pending ||
            gateway_membership_restore_pending ||
            gateway_eack_custody_clear_pending) {
            return -EAGAIN;
        }
        ret = gateway_preflight_result_semantic_delivery(
            packet,
            payload,
            payload_len,
            received_at_ms,
            0u,
            0u);
        if (ret == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE ||
            ret == APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE) {
            ret = app_mesh_persistence_commit_gateway_host_journal(
                packet, payload, payload_len);
            return ret < 0 ? ret :
                   APP_MESH_GATEWAY_HOST_JOURNAL_COMMITTED;
        }
        if (ret == APP_GATEWAY_SEMANTIC_ACCEPT_NEW) {
            /*
             * PREPARED landed before the durable collection mutation. Drop
             * only this unaccepted journal owner; relay custody will retry
             * the exact record with its previous-hop context intact.
             */
            ret =
                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                    packet, payload, payload_len);
            return ret < 0 ? ret : 0;
        }
        if (ret == -EAGAIN || ret == -EBUSY) {
            return ret;
        }
        if (ret == -EBADMSG || ret == -EINVAL) {
            ret =
                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                    packet, payload, payload_len);
            return ret < 0 ? ret : 0;
        }
        /*
         * The durable collection has already become terminal or was cleared
         * during reset recovery. Its volatile orchestration cannot be
         * resumed, but retaining the exact raw record avoids losing host
         * evidence and lets a later exact relay retry retire its custody.
         */
    }

    ret = app_mesh_persistence_recover_raw_gateway_host_journal(
        packet, payload, payload_len);
    return ret < 0 ? ret :
           APP_MESH_GATEWAY_HOST_JOURNAL_RECOVERED_RAW;
}

static int gateway_restore_host_journal_runtime(void)
{
    struct proto_packet staging_packet = {0};
    struct proto_packet *packet = &staging_packet;
    uint8_t *payload;
    size_t payload_len = 0u;
    uint32_t received_at_ms = 0u;
    uint8_t host_projection_mask = 0u;
    uint32_t staging_offset =
        GATEWAY_BLE_STREAM_RECORD_POOL_BYTES -
        PACKET_EXT_MAX_PAYLOAD_LEN;
    k_spinlock_key_t key;
    int ret;

    if (!gateway_ble_stream_initialized) {
        atomic_set(&gateway_host_journal_restore_pending, 1);
        return -EAGAIN;
    }
    if (atomic_get(&gateway_host_journal_restored) != 0) {
        atomic_clear(&gateway_host_journal_restore_pending);
        mesh_gateway_semantic_commit_owner_clear();
        return 0;
    }

    /* Do not let a retry overwrite an active semantic reservation or a
     * record already occupying the payload staging tail. */
    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.reservation_active ||
        gateway_ble_stream_state.head_send_phase !=
            GATEWAY_BLE_STREAM_HEAD_IDLE ||
        gateway_ble_stream_state.restore_staging_active ||
        gateway_ble_stream_state.pool_used > staging_offset) {
        k_spin_unlock(&gateway_ble_stream_lock, key);
        atomic_set(&gateway_host_journal_restore_pending, 1);
        return -EAGAIN;
    }
    payload = &gateway_ble_stream_state.record_pool[staging_offset];
    gateway_ble_stream_state.restore_staging_active = true;
    gateway_ble_stream_state.restore_staging_offset = staging_offset;
    k_spin_unlock(&gateway_ble_stream_lock, key);

    ret = app_mesh_persistence_restore_gateway_host_journal_projection(
        packet,
        payload,
        PACKET_EXT_MAX_PAYLOAD_LEN,
        &payload_len,
        &received_at_ms,
        &host_projection_mask);
    if (ret == APP_MESH_GATEWAY_HOST_JOURNAL_PREPARED) {
        ret = gateway_recover_prepared_host_journal(
            packet,
            payload,
            payload_len,
            received_at_ms);
    }
    if (ret == APP_MESH_GATEWAY_HOST_JOURNAL_NOTIFIED) {
        bool journal_cleared = false;

        /*
         * Host notification is already terminal.  Materialize per-node
         * collection receipts from the retained immutable raw journal before
         * deleting it; partial bundle progress is idempotent, and any failure
         * keeps NOTIFIED durable so neither reset nor retry can expose the
         * record to the host again.
         */
        ret = app_gateway_collection_receipts_record_host_notification(
            packet,
            payload,
            payload_len,
            host_projection_mask);
        if (ret == 0 &&
            app_gateway_terminal_receipts_supports(
                packet, payload, payload_len)) {
            int finalize_ret =
                app_mesh_persistence_finalize_gateway_host_journal_if_complete(
                    packet, payload, payload_len);

            if (finalize_ret < 0) {
                ret = finalize_ret;
            } else if (finalize_ret > 0) {
                /*
                 * ACK_CONFIRM was already durable before NOTIFIED. The raw
                 * source has advanced, so no duplicate-suppression receipt is
                 * needed and retaining one would consume a source slot until
                 * expiry.
                 */
                journal_cleared = true;
            } else {
                ret = app_gateway_terminal_receipts_record(
                    packet,
                    payload,
                    payload_len,
                    k_uptime_get_32());
                if (ret > 0) {
                    ret = 0;
                }
            }
        }
        if (ret == 0 && !journal_cleared) {
            ret =
                app_mesh_persistence_clear_gateway_host_journal_if_matches(
                    packet, payload, payload_len);
        }
        if (ret < 0) {
            key = k_spin_lock(&gateway_ble_stream_lock);
            gateway_ble_stream_state.restore_staging_active = false;
            gateway_ble_stream_state.restore_staging_offset = 0u;
            k_spin_unlock(&gateway_ble_stream_lock, key);
            atomic_set(&gateway_host_journal_restore_pending, 1);
            return ret;
        }

        key = k_spin_lock(&gateway_ble_stream_lock);
        gateway_ble_stream_state.restore_staging_active = false;
        gateway_ble_stream_state.restore_staging_offset = 0u;
        k_spin_unlock(&gateway_ble_stream_lock, key);
        atomic_clear(&gateway_host_journal_restore_pending);
        atomic_clear(&gateway_host_journal_restored);
        mesh_gateway_semantic_commit_owner_clear();
        return 0;
    }
    if (ret == -EBADMSG) {
        struct proto_packet terminal_packet;
        uint8_t terminal_digest[SEMANTIC_DIGEST_SHA256_LEN];
        bool source_confirmed = false;
        int terminal_ret =
            app_mesh_persistence_restore_gateway_host_terminal_marker(
                &terminal_packet,
                terminal_digest,
                &source_confirmed);

        if (terminal_ret == 0) {
            /* A concurrent exact terminal clear completed after the failed
             * payload read, so there is no longer an admission owner. */
            ret = 0;
        } else if (terminal_ret > 0) {
            int proof_ret = source_confirmed ? 1 :
                app_gateway_terminal_receipts_classify_identity(
                    &terminal_packet,
                    terminal_digest,
                    k_uptime_get_32());

            if (proof_ret > 0) {
                terminal_ret =
                    app_mesh_persistence_retire_notified_gateway_host_journal_if_matches(
                        &terminal_packet,
                        terminal_digest);
                ret = terminal_ret < 0 ? terminal_ret : 0;
            } else if (proof_ret < 0) {
                ret = proof_ret;
            }
        } else {
            ret = terminal_ret;
        }
    }
    if (ret == 0) {
        key = k_spin_lock(&gateway_ble_stream_lock);
        gateway_ble_stream_state.restore_staging_active = false;
        gateway_ble_stream_state.restore_staging_offset = 0u;
        k_spin_unlock(&gateway_ble_stream_lock, key);
        atomic_clear(&gateway_host_journal_restore_pending);
        atomic_clear(&gateway_host_journal_restored);
        mesh_gateway_semantic_commit_owner_clear();
        return 0;
    }
    if (ret < 0) {
        key = k_spin_lock(&gateway_ble_stream_lock);
        gateway_ble_stream_state.restore_staging_active = false;
        gateway_ble_stream_state.restore_staging_offset = 0u;
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (ret == -EBADMSG) {
            LOG_ERR("gateway host-output journal is corrupt and retained; host-output admission remains closed");
        }
        atomic_set(&gateway_host_journal_restore_pending, 1);
        return ret;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = host_projection_mask == 0u ?
        gateway_ble_stream_enqueue_staged_packet(
            &gateway_ble_stream_state,
            packet,
            payload_len,
            received_at_ms,
            k_uptime_get_32(),
            true) :
        gateway_ble_stream_enqueue_staged_bundle_projection(
            &gateway_ble_stream_state,
            packet,
            payload_len,
            host_projection_mask,
            received_at_ms,
            k_uptime_get_32(),
            true);
    gateway_ble_stream_state.restore_staging_active = false;
    gateway_ble_stream_state.restore_staging_offset = 0u;
    if (ret > 0) {
        gateway_ble_stream_state.items[
            gateway_ble_stream_state.count - 1u].retain_until_sent = true;
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret <= 0) {
        atomic_set(&gateway_host_journal_restore_pending, 1);
        return ret == 0 ? -EAGAIN : ret;
    }

    atomic_set(&gateway_host_journal_restored, 1);
    atomic_clear(&gateway_host_journal_restore_pending);
    mesh_gateway_semantic_commit_owner_clear();
    gateway_ble_schedule_stream_drain();
    return 0;
}

#include "app_gateway_collection_runtime.inc"
#line 1874 __BASE_FILE__
#endif

#if DEVICE_ROLE != ROLE_GATEWAY
int gateway_accept_collection_receipt_redrive(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    return -ENOTSUP;
}
#endif

static int gateway_validate_raw_command_result(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    bool collection_identity_required)
{
    struct command_result_id result_id;
    enum command_id command_id;
    enum command_status status;
    const uint8_t *collection_epoch_raw = NULL;
    uint8_t collection_epoch_len = 0u;
    uint8_t reason;
    int epoch_ret;
    int ret;

    if (packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->payload_len != payload_len) {
        return -EINVAL;
    }
    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    ret = app_mesh_gateway_command_flow_decode_result(command_id,
                                                       payload,
                                                       payload_len,
                                                       &status,
                                                       &reason);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    ARG_UNUSED(status);
    ARG_UNUSED(reason);

    epoch_ret = tlv_find_unique(payload,
                                payload_len,
                                TLV_COLLECTION_EPOCH_ID,
                                &collection_epoch_raw,
                                &collection_epoch_len);
    if (!collection_identity_required) {
        return epoch_ret == PROTO_ERR_NOT_FOUND ? 0 : -EBADMSG;
    }
    if (epoch_ret != PROTO_OK ||
        collection_epoch_len != sizeof(uint32_t) ||
        proto_get_u32_le(collection_epoch_raw) == 0u) {
        return -EBADMSG;
    }
    ret = command_result_id_from_tlvs(payload, payload_len, &result_id);
    if (ret != PROTO_OK ||
        result_id.gateway_id != packet->dst_id ||
        result_id.gateway_epoch == 0u ||
        result_id.command_seq != packet->session_id ||
        result_id.node_id != packet->src_id ||
        result_id.node_boot_counter == 0u ||
        result_id.result_seq == 0u) {
        return -EBADMSG;
    }
    return 0;
}

static int gateway_validate_raw_result_bundle(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct result_bundle_header bundle;
    size_t cursor = 0u;
    uint8_t parsed_count = 0u;
    int ret;

    if (packet == NULL || payload == NULL ||
        packet->msg_type != MSG_RESULT_BUNDLE ||
        packet->payload_len != payload_len) {
        return -EINVAL;
    }
    ret = result_bundle_header_from_tlvs(payload, payload_len, &bundle);
    if (ret != PROTO_OK ||
        bundle.gateway_id != packet->dst_id ||
        bundle.gateway_id == 0u ||
        bundle.gateway_epoch == 0u ||
        bundle.command_seq != packet->session_id ||
        bundle.command_seq == 0u ||
        bundle.collection_epoch_id == 0u ||
        bundle.record_count == 0u ||
        bundle.record_count > COLLECTION_BUNDLE_MAX_RECORDS) {
        return -EBADMSG;
    }

    while (cursor < payload_len) {
        uint8_t type;
        uint8_t len;

        if (payload_len - cursor < PROTO_TLV_HEADER_LEN) {
            return -EBADMSG;
        }
        type = payload[cursor];
        len = payload[cursor + 1u];
        if (payload_len - cursor - PROTO_TLV_HEADER_LEN < len) {
            return -EBADMSG;
        }
        if (type == TLV_RESULT_RECORD) {
            break;
        }
        cursor += PROTO_TLV_HEADER_LEN + len;
    }
    if (cursor >= payload_len ||
        proto_crc16_ccitt_false(&payload[cursor], payload_len - cursor) !=
            bundle.bundle_crc) {
        return -EBADMSG;
    }

    while (cursor < payload_len) {
        struct command_result_id payload_id;
        struct result_bundle_record record;
        const uint8_t *collection_epoch_raw = NULL;
        uint8_t collection_epoch_len = 0u;
        size_t before = cursor;

        if (payload[cursor] != TLV_RESULT_RECORD ||
            parsed_count >= bundle.record_count) {
            return -EBADMSG;
        }
        ret = result_bundle_record_next_from_tlvs(payload,
                                                  payload_len,
                                                  &cursor,
                                                  &record);
        if (ret != PROTO_OK || cursor == before ||
            record.result_id.gateway_id != bundle.gateway_id ||
            record.result_id.gateway_epoch != bundle.gateway_epoch ||
            record.result_id.command_seq != bundle.command_seq ||
            record.result_id.node_id == 0u ||
            record.result_id.node_boot_counter == 0u ||
            record.result_id.result_seq == 0u ||
            command_result_id_from_tlvs(record.payload,
                                        record.payload_len,
                                        &payload_id) != PROTO_OK ||
            payload_id.gateway_id != record.result_id.gateway_id ||
            payload_id.gateway_epoch != record.result_id.gateway_epoch ||
            payload_id.command_seq != record.result_id.command_seq ||
            payload_id.node_id != record.result_id.node_id ||
            payload_id.node_boot_counter !=
                record.result_id.node_boot_counter ||
            payload_id.result_seq != record.result_id.result_seq ||
            tlv_find_unique(record.payload,
                            record.payload_len,
                            TLV_COLLECTION_EPOCH_ID,
                            &collection_epoch_raw,
                            &collection_epoch_len) != PROTO_OK ||
            collection_epoch_len != sizeof(uint32_t) ||
            proto_get_u32_le(collection_epoch_raw) !=
                bundle.collection_epoch_id) {
            return -EBADMSG;
        }
        if (gateway_validate_raw_command_result(
                &(struct proto_packet) {
                    .msg_type = MSG_COMMAND_RESULT,
                    .src_id = record.result_id.node_id,
                    .dst_id = bundle.gateway_id,
                    .session_id = bundle.command_seq,
                    .payload_len = record.payload_len,
                },
                record.payload,
                record.payload_len,
                true) < 0) {
            return -EBADMSG;
        }
        parsed_count++;
    }
    return parsed_count == bundle.record_count ? 0 : -EBADMSG;
}

int gateway_preflight_result_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t first_received_at_ms,
    uint32_t result_validation_token,
    uint64_t previous_hop_id)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct gateway_decoded_command_result decoded = {0};
    struct gateway_command_pending pending_snapshot = {0};
    enum gateway_command_result_admission admission;
    enum gateway_survey_result_preflight_outcome survey_preflight =
        GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    bool pending_matches;
    bool token_valid = false;
    bool auto_transaction_owned;
    int ret;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        (packet->msg_type != MSG_COMMAND_RESULT &&
         packet->msg_type != MSG_RESULT_BUNDLE)) {
        return -EINVAL;
    }
    if (gateway_collection_restore_pending ||
        gateway_collection_clear_pending ||
        gateway_eack_custody_clear_pending) {
        return -EAGAIN;
    }

    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        uint16_t accepted_count = 0u;
        uint16_t duplicate_count = 0u;
        int receipt_ret;

        ret = gateway_validate_raw_result_bundle(
            packet, payload, payload_len);
        if (ret < 0) {
            return ret;
        }
        if (!gateway_collection_tracking_active()) {
            receipt_ret =
                app_gateway_collection_receipts_classify_retry(
                    packet, payload, payload_len);
            return receipt_ret == 1 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE :
                   receipt_ret == 0 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW :
                   receipt_ret;
        }
        ret = gateway_collection_preflight_bundle_from_hop(
            &gateway_collection_state,
            packet,
            payload,
            payload_len,
            previous_hop_id,
            &accepted_count,
            &duplicate_count);
        if (ret != PROTO_OK) {
            if (ret != PROTO_ERR_STALE &&
                ret != PROTO_ERR_NOT_FOUND &&
                ret != PROTO_ERR_NO_SPACE) {
                return mesh_errno_from_proto(ret);
            }
            receipt_ret =
                app_gateway_collection_receipts_classify_retry(
                    packet, payload, payload_len);
            return receipt_ret == 1 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE :
                   receipt_ret == 0 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW :
                   receipt_ret;
        }
        if (accepted_count == 0u && duplicate_count == 0u) {
            return -EBADMSG;
        }
        return accepted_count == 0u ?
               APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE :
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    }

    {
        const uint8_t *collection_epoch_raw = NULL;
        uint8_t collection_epoch_len = 0u;
        bool duplicate = false;
        int receipt_ret;
        int epoch_ret = tlv_find_unique(payload,
                                        payload_len,
                                        TLV_COLLECTION_EPOCH_ID,
                                        &collection_epoch_raw,
                                        &collection_epoch_len);

        if (epoch_ret != PROTO_OK &&
            epoch_ret != PROTO_ERR_NOT_FOUND) {
            return -EBADMSG;
        }
        if (epoch_ret == PROTO_OK &&
            collection_epoch_len != sizeof(uint32_t)) {
            return -EBADMSG;
        }
        if (epoch_ret == PROTO_ERR_NOT_FOUND) {
            ret = gateway_validate_raw_command_result(
                packet, payload, payload_len, false);
            if (ret < 0) {
                return ret;
            }
            goto ordinary_command_result;
        }
        ret = gateway_validate_raw_command_result(
            packet, payload, payload_len, true);
        if (ret < 0) {
            return ret;
        }
        if (!gateway_collection_tracking_active()) {
            receipt_ret =
                app_gateway_collection_receipts_classify_retry(
                    packet, payload, payload_len);
            return receipt_ret == 1 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE :
                   receipt_ret == 0 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW :
                   receipt_ret;
        }
        ret = gateway_collection_preflight_result_from_hop(
            &gateway_collection_state,
            packet,
            payload,
            payload_len,
            previous_hop_id,
            &duplicate);
        if (ret != PROTO_OK) {
            if (ret != PROTO_ERR_STALE &&
                ret != PROTO_ERR_NOT_FOUND &&
                ret != PROTO_ERR_NO_SPACE) {
                return mesh_errno_from_proto(ret);
            }
            receipt_ret =
                app_gateway_collection_receipts_classify_retry(
                    packet, payload, payload_len);
            return receipt_ret == 1 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECEIPT_REDRIVE :
                   receipt_ret == 0 ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW :
                   receipt_ret;
        }
        return duplicate ?
               APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE :
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    }

ordinary_command_result:
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    pending_snapshot = gateway_command_pending_state;
    pending_matches = gateway_command_pending_matches_result(
        &pending_snapshot, packet);
    if (pending_matches && result_validation_token != 0u) {
        token_valid = gateway_command_result_validation_contains(
            &gateway_command_result_validation_state,
            &gateway_command_pending_state,
            result_validation_token,
            packet,
            first_received_at_ms);
    }
    k_mutex_unlock(&gateway_command_pending_lock);

    if (pending_matches &&
        uptime_deadline_reached((uint32_t)first_received_at_ms,
                                pending_snapshot.started_at_ms) &&
        !uptime_deadline_reached((uint32_t)first_received_at_ms,
                                 pending_snapshot.deadline_ms) &&
        !token_valid) {
        return -EIO;
    }
    if (pending_matches &&
        uptime_deadline_reached((uint32_t)first_received_at_ms,
                                pending_snapshot.deadline_ms)) {
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }

    auto_transaction_owned = gateway_survey_auto_owns_pending_command(
        &pending_snapshot.command,
        pending_snapshot.command_id);
    if (auto_transaction_owned || !pending_matches) {
        survey_preflight = gateway_survey_auto_preflight_result(
            packet, payload, payload_len, first_received_at_ms);
    }
    admission = gateway_command_result_admit(
        &pending_snapshot,
        packet,
        auto_transaction_owned,
        survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED);
    gateway_survey_auto_discard_preflight_result();

    if (admission == GATEWAY_COMMAND_RESULT_IGNORE) {
        return survey_preflight ==
                   GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED ?
               APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE :
               APP_GATEWAY_SEMANTIC_ACCEPT_RECOVERED_RAW;
    }
    if (admission == GATEWAY_COMMAND_RESULT_WAIT) {
        if (survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED) {
            return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
        }
        return -EAGAIN;
    }

    ret = app_mesh_command_orchestrator_gateway_deliver(
        &pending_snapshot.command,
        pending_snapshot.command_id,
        packet,
        payload,
        payload_len,
        gateway_capture_decoded_command_result,
        &decoded);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    return APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(first_received_at_ms);
    ARG_UNUSED(result_validation_token);
    ARG_UNUSED(previous_hop_id);
    return -ENOTSUP;
#endif
}

int gateway_result_bundle_host_projection_mask(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t *accepted_record_mask)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct gateway_collection_bundle_projection projection = {0};
    int ret;

    if (packet == NULL || payload == NULL ||
        accepted_record_mask == NULL ||
        packet->msg_type != MSG_RESULT_BUNDLE ||
        packet->payload_len != payload_len) {
        return -EINVAL;
    }
    *accepted_record_mask = 0u;
    if (!gateway_collection_tracking_active() ||
        gateway_collection_restore_pending ||
        gateway_collection_clear_pending ||
        gateway_eack_custody_clear_pending) {
        return -EAGAIN;
    }
    ret = gateway_collection_preflight_bundle_projection_from_hop(
        &gateway_collection_state,
        packet,
        payload,
        payload_len,
        previous_hop_id,
        &projection);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (projection.accepted_count == 0u ||
        projection.accepted_record_mask == 0u) {
        return -ESTALE;
    }
    *accepted_record_mask = projection.accepted_record_mask;
    return 0;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    if (accepted_record_mask != NULL) {
        *accepted_record_mask = 0u;
    }
    return -ENOTSUP;
#endif
}

int gateway_finalize_semantic_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t received_radio_channel,
    const struct mesh_event_plan *current_channel9_plan,
    int semantic_acceptance)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    const uint8_t *collection_epoch_raw = NULL;
    uint8_t collection_epoch_len = 0u;
    uint32_t collection_epoch_id = 0u;
    bool collection_packet = false;
    int epoch_ret;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        (semantic_acceptance != APP_GATEWAY_SEMANTIC_ACCEPT_NEW &&
         semantic_acceptance != APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE &&
         semantic_acceptance !=
             APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE)) {
        return -EINVAL;
    }
    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        collection_packet = true;
    } else if (packet->msg_type == MSG_COMMAND_RESULT) {
        epoch_ret = tlv_find_unique(payload,
                                    payload_len,
                                    TLV_COLLECTION_EPOCH_ID,
                                    &collection_epoch_raw,
                                    &collection_epoch_len);
        if (epoch_ret == PROTO_ERR_NOT_FOUND) {
            return 0;
        }
        if (epoch_ret != PROTO_OK ||
            collection_epoch_len != sizeof(uint32_t)) {
            return -EBADMSG;
        }
        collection_epoch_id = proto_get_u32_le(collection_epoch_raw);
        collection_packet = true;
    }
    if (!collection_packet) {
        return 0;
    }
    if (!gateway_collection_tracking_active() ||
        packet->session_id != gateway_collection_state.command_seq ||
        (packet->msg_type == MSG_COMMAND_RESULT &&
         collection_epoch_id !=
            gateway_collection_state.collection_epoch_id)) {
        return -ESTALE;
    }

    return gateway_drive_collection_eack_after_record(
        (semantic_acceptance == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE ||
         semantic_acceptance ==
             APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_REDRIVE) ?
            "collection-eack-semantic-duplicate" :
            "collection-eack-semantic-new",
        previous_hop_id,
        received_radio_channel,
        current_channel9_plan);
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    ARG_UNUSED(semantic_acceptance);
    return -ENOTSUP;
#endif
}

int gateway_encode_host_packet_frame(const struct proto_packet *packet,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     uint8_t *frame,
                                     size_t frame_cap,
                                     size_t *frame_len,
                                     struct proto_packet *frame_packet)
{
    int ret;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        payload_len > UINT8_MAX || frame == NULL || frame_len == NULL ||
        frame_packet == NULL) {
        return -EINVAL;
    }

    *frame_packet = *packet;
    frame_packet->payload_len = (uint8_t)payload_len;
    ret = serial_frame_encode_packet(frame_packet,
                                     payload,
                                     frame,
                                     frame_cap,
                                     frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    return 0;
}

int gateway_emit_host_packet(const struct proto_packet *packet,
                             const uint8_t *payload,
                             size_t payload_len)
{
    struct proto_packet frame_packet;
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY &&
        !(DEVICE_ROLE == ROLE_CLICKER && IS_ENABLED(CONFIG_IMEC_ML_CLICKER)) &&
        !(DEVICE_ROLE == ROLE_ANCHOR && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST))) {
        return 0;
    }
    if (!gateway_ble_transport_enabled()) {
        return -ENOTSUP;
    }

    ret = gateway_encode_host_packet_frame(packet,
                                           payload,
                                           payload_len,
                                           frame,
                                           sizeof(frame),
                                           &frame_len,
                                           &frame_packet);
    if (ret < 0) {
        return ret;
    }

    ret = gateway_ble_send_packet_frame(frame, frame_len);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("gateway BLE COBS frame emitted: msg=0x%02x src=0x%016llx seq=%u payload_len=%u frame_len=%u",
            frame_packet.msg_type,
            (unsigned long long)frame_packet.src_id,
            frame_packet.seq,
            (unsigned int)payload_len,
            (unsigned int)frame_len);
    return 0;
}

#if DEVICE_ROLE == ROLE_GATEWAY
void gateway_emit_host_command_result_reserved(
    uint32_t result_reservation_token,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    struct proto_packet result = {0};
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    if (command == NULL || DEVICE_ROLE != ROLE_GATEWAY ||
        result_reservation_token == 0u) {
        return;
    }

    ret = gateway_command_build_result(command,
                                       DEVICE_ID,
                                       command_id,
                                       status,
                                       reason,
                                       k_uptime_get_32(),
                                       &result,
                                       payload,
                                       sizeof(payload),
                                       &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway command failure result build failed: %d", ret);
        return;
    }

    k_spinlock_key_t key =
        k_spin_lock(&gateway_host_command_result_lock);

    ret = app_gateway_command_result_commit(
        &gateway_host_command_results,
        result_reservation_token,
        command,
        command_id,
        &result,
        payload,
        payload_len);
    k_spin_unlock(&gateway_host_command_result_lock, key);
    if (ret < 0) {
        LOG_ERR("gateway command result custody unavailable: cmd=0x%04x ret=%d",
                (unsigned int)command_id,
                ret);
    } else {
        (void)gateway_flush_host_command_results();
    }
}

void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    gateway_emit_host_command_result_reserved(
        gateway_command_result_dispatch_token,
        command,
        command_id,
        status,
        reason);
}

void gateway_command_result_set_dispatch_token(uint32_t token)
{
    gateway_command_result_dispatch_token = token;
}

uint32_t gateway_command_result_get_dispatch_token(void)
{
    return gateway_command_result_dispatch_token;
}

static bool gateway_pending_command_matches(const struct proto_packet *command)
{
    if (command == NULL || !gateway_command_pending_state.active) {
        return false;
    }

    return gateway_command_pending_state.command.dst_id == command->dst_id &&
           gateway_command_pending_state.command.session_id == command->session_id &&
           gateway_command_pending_state.command.seq == command->seq;
}

bool gateway_clear_pending_command_result(const struct proto_packet *command)
{
    bool cleared = false;

    if (DEVICE_ROLE != ROLE_GATEWAY ||
        gateway_pending_command_lock() < 0) {
        return false;
    }
    if (gateway_pending_command_matches(command)) {
        gateway_command_pending_clear(&gateway_command_pending_state);
        gateway_pending_command_result_token = 0u;
        cleared = true;
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    if (cleared) {
        (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
    }
    return cleared;
}

int gateway_begin_command_collection(const struct gateway_command_options *options)
{
    enum gateway_command_collection_roster_source roster_source =
        GATEWAY_COMMAND_COLLECTION_ROSTER_NONE;
    uint64_t resolved_node_ids[GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP] = {0};
    size_t resolved_node_count = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (gateway_pending_command_lock() < 0) {
        return -EWOULDBLOCK;
    }
    bool pending_active = gateway_command_pending_state.active;
    bool timeout_side_effect_pending =
        gateway_command_timeout_dispatch_state.pending;

    k_mutex_unlock(&gateway_command_pending_lock);
    if (pending_active || timeout_side_effect_pending ||
        gateway_collection_work_pending()) {
        return -EBUSY;
    }

    ret = gateway_command_resolve_collection_roster(options,
                                                    &gateway_membership_roster_state,
                                                    resolved_node_ids,
                                                    ARRAY_SIZE(resolved_node_ids),
                                                    &resolved_node_count,
                                                    &roster_source);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway collection roster unavailable: ret=%d command_seq=%u epoch=%u expected=%u explicit=%u",
                ret,
                options->command_seq,
                options->membership_epoch,
                options->expected_node_count,
                options->expected_node_id_count);
        return mesh_errno_from_proto(ret);
    }

    ret = gateway_collection_start(&gateway_collection_state,
                                   DEVICE_ID,
                                   (uint16_t)mesh_runtime.upstream.current_epoch,
                                   options->command_seq,
                                   options->collection_epoch_id,
                                   options->membership_epoch,
                                   options->expected_node_count,
                                   0u,
                                   COLLECTION_RETRY_ROUND_0_MS);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = gateway_collection_set_expected_roster(&gateway_collection_state,
                                                 resolved_node_ids,
                                                 resolved_node_count);
    if (ret != PROTO_OK) {
        gateway_collection_rollback_failed_start();
        return mesh_errno_from_proto(ret);
    }

    LOG_INF("gateway collection tracking started: command_seq=%u collection=%u epoch=%u expected=%u roster_count=%u roster_source=%u",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.gateway_epoch,
            gateway_collection_state.expected_count,
            gateway_collection_state.expected_node_id_count,
            (unsigned int)roster_source);
    ret = gateway_persist_collection_state("collection-start");
    if (ret < 0) {
        /*
         * The previous closed collection remains the durable snapshot.  Do
         * not expose or schedule a new collection that exists only in RAM.
         */
        gateway_collection_rollback_failed_start();
        return ret;
    }
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    gateway_collection_eack_round_dirty = false;
    if (gateway_clear_eack_custody_or_defer(
            "collection-start-eack-custody-clear") < 0) {
        /*
         * The new collection is already durable. Keep it blocked behind the
         * old tombstone owner; the persistence worker will start its first
         * EACK only after the prior key is positively absent.
         */
        return 0;
    }
    gateway_schedule_collection_eack_round();
    return 0;
}

int gateway_set_registered_membership_roster(uint16_t membership_epoch,
                                             const uint64_t *node_ids,
                                             const uint8_t *slots,
                                             size_t node_count,
                                             uint32_t assignment_epoch,
                                             uint32_t table_seq,
                                             const struct discovery_assignment_table_commitment *table_commitment,
                                             const struct gateway_membership_publication *publication)
{
    struct gateway_membership_roster candidate;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || node_ids == NULL || slots == NULL ||
        table_commitment == NULL ||
        publication == NULL || publication->publish_pending == 0u) {
        return -EINVAL;
    }
    if (gateway_membership_restore_pending ||
        gateway_collection_restore_pending ||
        gateway_membership_persistence_dirty ||
        gateway_membership_clear_pending ||
        atomic_get(&gateway_assignment_publication_pending_state) != 0) {
        return -EAGAIN;
    }

    ret = gateway_membership_set_roster_explicit_slots(
        &candidate,
        membership_epoch,
        node_ids,
        slots,
        node_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    ret = app_mesh_persistence_save_gateway_assignment_membership(
        &candidate,
        assignment_epoch,
        table_seq,
        table_commitment,
        publication);
    if (ret < 0) {
        LOG_WRN("gateway membership snapshot save failed: ret=%d", ret);
        return ret;
    }
    gateway_membership_roster_state = candidate;
    gateway_membership_persistence_dirty = false;
    atomic_set(&gateway_assignment_publication_pending_state, 1);
    return 0;
}

int gateway_get_registered_membership_roster_with_slots(
    uint64_t *node_ids,
    uint8_t *slots,
    size_t node_cap,
    size_t *node_count,
    uint16_t *membership_epoch)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || node_ids == NULL || slots == NULL ||
        node_count == NULL || membership_epoch == NULL) {
        return -EINVAL;
    }
    if (gateway_membership_restore_pending ||
        gateway_membership_persistence_dirty ||
        gateway_membership_clear_pending) {
        return -EAGAIN;
    }
    if (!gateway_membership_roster_state.valid) {
        *node_count = 0u;
        *membership_epoch = 0u;
        return -ENOENT;
    }
    *membership_epoch =
        gateway_membership_roster_state.membership_epoch;
    ret = gateway_membership_export_node_ids_with_slots(
        &gateway_membership_roster_state,
        *membership_epoch,
        node_ids,
        slots,
        node_cap,
        node_count);
    return ret == PROTO_OK ? 0 : mesh_errno_from_proto(ret);
}

bool gateway_assignment_publication_pending(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY &&
           (gateway_membership_restore_pending ||
            atomic_get(&gateway_assignment_publication_pending_state) != 0);
}

int gateway_complete_assignment_publication(
    const struct gateway_command_event *base_event,
    void *ctx)
{
    int ret;

    ARG_UNUSED(ctx);
    if (DEVICE_ROLE != ROLE_GATEWAY || base_event == NULL ||
        base_event->kind !=
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION ||
        base_event->command_id != CMD_ASSIGN_DISCOVERY_SLOTS) {
        return -EINVAL;
    }
    ret = app_mesh_persistence_complete_gateway_assignment_publication(
        base_event->gateway_sequence,
        base_event->gateway_epoch,
        base_event->host_session_id,
        base_event->host_seq);
    if (ret < 0) {
        LOG_WRN("gateway assignment publication completion persist failed: assignment_epoch=%u ret=%d",
                base_event->gateway_sequence,
                ret);
        return ret;
    }
    atomic_set(&gateway_assignment_publication_pending_state, 0);
    return 0;
}

void gateway_clear_registered_membership_roster(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || gateway_membership_restore_pending ||
        gateway_collection_restore_pending) {
        return;
    }

    ret = app_mesh_persistence_clear_gateway_membership();
    if (ret < 0) {
        gateway_membership_clear_pending = true;
        gateway_membership_persistence_dirty = false;
        gateway_schedule_persistence_retry("membership-clear");
        LOG_WRN("gateway membership clear deferred until NVS delete succeeds: ret=%d",
                ret);
        return;
    }
    gateway_finalize_membership_clear();
}

void gateway_clear_command_collection(const struct gateway_command_options *options)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL ||
        !gateway_collection_tracking_active()) {
        return;
    }
    if (gateway_collection_state.command_seq != options->command_seq ||
        gateway_collection_state.collection_epoch_id != options->collection_epoch_id) {
        return;
    }

    ret = app_mesh_persistence_clear_gateway_collection();
    if (ret < 0) {
        gateway_collection_clear_pending = true;
        gateway_collection_persistence_dirty = false;
        (void)k_work_cancel_delayable(&gateway_collection_eack_work);
        gateway_schedule_persistence_retry("collection-clear");
        LOG_WRN("gateway collection clear deferred until journal tombstone persists: ret=%d",
                ret);
        return;
    }
    gateway_finalize_collection_clear();
}

static void gateway_command_timeout_side_effect_handler(struct k_work *work)
{
    struct proto_packet command = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_TIMEOUT;
    uint8_t reason = 0u;
    bool run_side_effects = false;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    if (gateway_pending_command_lock() < 0) {
        LOG_ERR("gateway timeout side-effect owner could not lock state");
        app_watchdog_stop_feeding();
        return;
    }
    if (gateway_command_timeout_dispatch_state.pending &&
        !gateway_command_timeout_dispatch_state.running) {
        command = gateway_command_timeout_dispatch_state.command;
        command_id = gateway_command_timeout_dispatch_state.command_id;
        status = gateway_command_timeout_dispatch_state.status;
        reason = gateway_command_timeout_dispatch_state.reason;
        gateway_command_timeout_dispatch_state.running = true;
        run_side_effects = true;
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    if (!run_side_effects) {
        return;
    }

    (void)k_work_cancel_delayable(
        &gateway_command_timeout_side_effect_retry_work);
    if (status == COMMAND_TIMEOUT) {
        mesh_relay_note_delivery_failure_at(&mesh_runtime,
                                            command.dst_id,
                                            k_uptime_get_32());
        gateway_command_timeout_side_effects(&command, command_id);
    } else {
        gateway_command_result_side_effects(
            &command, command_id, status, reason);
    }

    if (gateway_pending_command_lock() < 0) {
        LOG_ERR("gateway timeout side-effect custody could not be released");
        app_watchdog_stop_feeding();
        return;
    }
    gateway_command_timeout_dispatch_state.pending = false;
    gateway_command_timeout_dispatch_state.running = false;
    gateway_command_timeout_dispatch_state.result_reservation_token = 0u;
    k_mutex_unlock(&gateway_command_pending_lock);
}

static int gateway_schedule_command_timeout_side_effects(void)
{
    int ret = mesh_route_work_reschedule(
        &gateway_command_timeout_side_effect_work, 0u);

    if (ret >= 0) {
        return 0;
    }
    LOG_WRN("gateway timeout side-effect route submission deferred: ret=%d",
            ret);
    ret = k_work_reschedule(
        &gateway_command_timeout_side_effect_retry_work,
        K_MSEC(GATEWAY_COMMAND_TIMEOUT_SIDE_EFFECT_RETRY_MS));
    if (ret < 0) {
        LOG_ERR("gateway timeout side-effect retry submission failed: ret=%d",
                ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static void gateway_command_timeout_side_effect_retry_handler(
    struct k_work *work)
{
    bool retry_pending = false;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
    if (gateway_pending_command_lock() < 0) {
        LOG_ERR("gateway timeout side-effect retry owner could not lock state");
        app_watchdog_stop_feeding();
        return;
    }
    retry_pending = gateway_command_timeout_dispatch_state.pending &&
                    !gateway_command_timeout_dispatch_state.running;
    k_mutex_unlock(&gateway_command_pending_lock);
    if (retry_pending) {
        (void)gateway_schedule_command_timeout_side_effects();
    }
}

static void gateway_command_result_timeout_handler(struct k_work *work)
{
    struct proto_packet command = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    enum gateway_command_result_validation_check validation_check =
        GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR;
    enum command_status terminal_status = COMMAND_TIMEOUT;
    uint8_t terminal_reason = 0u;
    bool terminal = false;
    bool recheck = false;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    if (gateway_pending_command_lock() < 0) {
        LOG_ERR("gateway command timeout owner could not lock state");
        app_watchdog_stop_feeding();
        return;
    }
    if (!gateway_command_timeout_dispatch_state.pending &&
        gateway_command_pending_state.active) {
        validation_check =
            gateway_command_result_validation_check_interval(
            &gateway_command_result_validation_state,
            gateway_command_pending_state.started_at_ms,
            gateway_command_pending_state.deadline_ms,
            k_uptime_get_32());
        if (validation_check ==
            GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            command = gateway_command_pending_state.command;
            command_id = gateway_command_pending_state.command_id;
            gateway_command_pending_clear(
                &gateway_command_pending_state);
            terminal_status = COMMAND_INTERNAL_ERROR;
            terminal_reason = ETIMEDOUT;
            terminal = true;
        } else if (validation_check ==
                   GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED) {
            recheck = true;
        } else {
            terminal = gateway_command_pending_expired(
                &gateway_command_pending_state,
                k_uptime_get_32(),
                &command,
                &command_id);
        }
        if (terminal) {
            gateway_command_timeout_dispatch_state.command = command;
            gateway_command_timeout_dispatch_state.result_reservation_token =
                gateway_pending_command_result_token;
            gateway_command_timeout_dispatch_state.command_id = command_id;
            gateway_command_timeout_dispatch_state.status = terminal_status;
            gateway_command_timeout_dispatch_state.reason = terminal_reason;
            gateway_command_timeout_dispatch_state.pending = true;
            gateway_command_timeout_dispatch_state.running = false;
            gateway_pending_command_result_token = 0u;
        }
    }

    k_mutex_unlock(&gateway_command_pending_lock);
    if (recheck) {
        (void)gateway_command_result_timeout_arm(
            GATEWAY_PROTOCOL_VALIDATION_RECHECK_MS,
            "validation-recheck");
        return;
    }
    if (!terminal) {
        return;
    }

    LOG_WRN("gateway command result terminal: cmd=0x%04x status=%u reason=%u dst=0x%016llx session=%u seq=%u",
            (unsigned int)command_id,
            (unsigned int)terminal_status,
            terminal_reason,
            (unsigned long long)command.dst_id,
            command.session_id,
            command.seq);
    gateway_emit_host_command_result_reserved(
        gateway_command_timeout_dispatch_state.result_reservation_token,
        &command,
        command_id,
        terminal_status,
        terminal_reason);
    (void)gateway_schedule_command_timeout_side_effects();
}

int gateway_note_command_result(const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t first_received_at_ms,
                                uint32_t result_validation_token,
                                uint64_t previous_hop_id,
                                uint8_t received_radio_channel,
                                const struct mesh_event_plan *current_channel9_plan)
{
    struct gateway_decoded_command_result decoded = {0};
    struct gateway_command_pending pending_snapshot = {0};
    struct proto_packet command = {0};
    enum command_id pending_command_id = CMD_VENDOR_BASE;
    enum gateway_command_pending_result_claim result_claim =
        GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE;
    enum gateway_command_result_admission admission;
    enum gateway_survey_result_preflight_outcome survey_preflight =
        GATEWAY_SURVEY_RESULT_PREFLIGHT_UNRECOGNIZED;
    uint64_t result_received_at_ms = first_received_at_ms;
    uint32_t validation_token;
    uint32_t result_reservation_token = 0u;
    bool auto_transaction_owned;
    bool deadline_expired = false;
    bool validation_blocked = false;
    bool validation_failed = false;
    int claim_ret;
    bool pending_matches;
    int collection_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (received_radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EBADMSG;
    }
    ARG_UNUSED(current_channel9_plan);
    validation_token = gateway_command_result_validation_adopt(
        packet, result_received_at_ms, result_validation_token);

    claim_ret = gateway_discovery_assignment_note_claim(packet,
                                                        payload,
                                                        payload_len,
                                                        result_received_at_ms,
                                                        previous_hop_id);
    if (claim_ret != -ENOENT && claim_ret != -ENOTSUP) {
        gateway_command_result_validation_release_reserved(validation_token);
        return claim_ret;
    }

    collection_ret = gateway_note_collection_result(packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id);

    ret = gateway_pending_command_lock();
    if (ret < 0) {
        gateway_command_result_validation_release_reserved(validation_token);
        return ret;
    }
    pending_snapshot = gateway_command_pending_state;
    k_mutex_unlock(&gateway_command_pending_lock);

    pending_matches = gateway_command_pending_matches_result(
        &pending_snapshot, packet);
    if (pending_matches &&
        uptime_deadline_reached((uint32_t)result_received_at_ms,
                                pending_snapshot.started_at_ms) &&
        !uptime_deadline_reached((uint32_t)result_received_at_ms,
                                 pending_snapshot.deadline_ms) &&
        validation_token == 0u) {
        LOG_ERR("gateway predeadline result lacks live ingress validation custody: src=0x%016llx session=%u seq=%u",
                (unsigned long long)packet->src_id,
                packet->session_id,
                packet->seq);
        return -EIO;
    }

    /*
     * A delayed system-workqueue timeout must not let an at/after-deadline
     * result win.  Claim the timeout under the same mutex before semantic
     * admission; the timer will observe the staged owner and stay inert.
     */
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        gateway_command_result_validation_release_reserved(validation_token);
        return ret;
    }
    if (!gateway_command_timeout_dispatch_state.pending &&
        gateway_pending_snapshot_current_locked(&pending_snapshot) &&
        uptime_deadline_reached(
            (uint32_t)result_received_at_ms,
            gateway_command_pending_state.deadline_ms)) {
        enum gateway_command_result_validation_check validation_check =
            gateway_command_result_validation_check_interval(
                &gateway_command_result_validation_state,
                gateway_command_pending_state.started_at_ms,
                gateway_command_pending_state.deadline_ms,
                k_uptime_get_32());

        validation_blocked =
            validation_check == GATEWAY_COMMAND_RESULT_VALIDATION_BLOCKED;
        if (!validation_blocked &&
            validation_check !=
                GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            deadline_expired = gateway_command_pending_expired(
                &gateway_command_pending_state,
                (uint32_t)result_received_at_ms,
                &command,
                &pending_command_id);
        } else if (validation_check ==
                   GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED) {
            command = gateway_command_pending_state.command;
            pending_command_id =
                gateway_command_pending_state.command_id;
            gateway_command_pending_clear(
                &gateway_command_pending_state);
            validation_failed = true;
            deadline_expired = true;
        }
    }
    if (deadline_expired) {
        result_reservation_token = gateway_pending_command_result_token;
        gateway_pending_command_result_token = 0u;
        gateway_command_timeout_dispatch_state.command = command;
        gateway_command_timeout_dispatch_state.result_reservation_token =
            result_reservation_token;
        gateway_command_timeout_dispatch_state.command_id =
            pending_command_id;
        gateway_command_timeout_dispatch_state.status =
            validation_failed ? COMMAND_INTERNAL_ERROR :
                                COMMAND_TIMEOUT;
        gateway_command_timeout_dispatch_state.reason =
            validation_failed ? ETIMEDOUT : 0u;
        gateway_command_timeout_dispatch_state.pending = true;
        gateway_command_timeout_dispatch_state.running = false;
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    if (validation_blocked) {
        gateway_command_result_validation_release_reserved(validation_token);
        return -EAGAIN;
    }
    if (deadline_expired) {
        (void)k_work_cancel_delayable(
            &gateway_command_result_timeout_work);
        gateway_emit_host_command_result_reserved(
            result_reservation_token,
            &command,
            pending_command_id,
            validation_failed ? COMMAND_INTERNAL_ERROR : COMMAND_TIMEOUT,
            validation_failed ? ETIMEDOUT : 0u);
        (void)gateway_schedule_command_timeout_side_effects();
        gateway_command_result_validation_release_reserved(validation_token);
        return collection_ret >= 0 ? collection_ret :
               pending_matches ?
                   APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE : -ENOENT;
    }

    auto_transaction_owned = gateway_survey_auto_owns_pending_command(
        &pending_snapshot.command,
        pending_snapshot.command_id);
    if (auto_transaction_owned || !pending_matches) {
        survey_preflight = gateway_survey_auto_preflight_result(
            packet, payload, payload_len, result_received_at_ms);
    }

    admission = gateway_command_result_admit(&pending_snapshot,
                                              packet,
                                              auto_transaction_owned,
                                              survey_preflight ==
                                                  GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED);

    if (admission == GATEWAY_COMMAND_RESULT_IGNORE) {
        if (survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED) {
            gateway_survey_auto_commit_preflight_result();
            LOG_DBG("gateway survey duplicate/late result reconciled: src=0x%016llx session=%u seq=%u",
                    packet == NULL ? 0ull :
                    (unsigned long long)packet->src_id,
                    packet == NULL ? 0u : packet->session_id,
                    packet == NULL ? 0u : packet->seq);
        } else {
            gateway_survey_auto_discard_preflight_result();
        }
        if (collection_ret >= 0) {
            gateway_command_result_validation_release_reserved(
                validation_token);
            return collection_ret;
        }
        if (survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED) {
            gateway_command_result_validation_release_reserved(
                validation_token);
            return collection_ret >= 0 ? collection_ret :
                   APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
        }
        gateway_command_result_validation_release_reserved(validation_token);
        return survey_preflight ==
                       GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED ?
               -EAGAIN : -ENOENT;
    }
    if (admission == GATEWAY_COMMAND_RESULT_WAIT) {
        if (survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED) {
            gateway_survey_auto_commit_preflight_result();
            LOG_DBG("gateway survey late/duplicate/conflicting result retained no pending ownership: src=0x%016llx session=%u seq=%u",
                    packet == NULL ? 0ull :
                    (unsigned long long)packet->src_id,
                    packet == NULL ? 0u : packet->session_id,
                    packet == NULL ? 0u : packet->seq);
            gateway_command_result_validation_release_reserved(
                validation_token);
            return collection_ret >= 0 ? collection_ret :
                   APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
        }
        gateway_survey_auto_discard_preflight_result();
        LOG_WRN("gateway survey result identity rejected while wait remains active: src=0x%016llx session=%u seq=%u",
                packet == NULL ? 0ull :
                (unsigned long long)packet->src_id,
                packet == NULL ? 0u : packet->session_id,
                packet == NULL ? 0u : packet->seq);
        gateway_command_result_validation_release_reserved(validation_token);
        return collection_ret >= 0 ? collection_ret : -EAGAIN;
    }

    command = pending_snapshot.command;
    pending_command_id = pending_snapshot.command_id;
    ret = app_mesh_command_orchestrator_gateway_deliver(
        &command,
        pending_command_id,
        packet,
        payload,
        payload_len,
        gateway_capture_decoded_command_result,
        &decoded);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway command result payload mismatch: pending=0x%04x ret=%d",
                (unsigned int)pending_command_id,
                ret);
    }

    if (ret == PROTO_OK) {
        ret = gateway_pending_command_lock();
        if (ret < 0) {
            gateway_survey_auto_discard_preflight_result();
            gateway_command_result_validation_release_reserved(
                validation_token);
            return ret;
        }
        if (gateway_pending_snapshot_current_locked(&pending_snapshot)) {
            result_reservation_token =
                gateway_pending_command_result_token;
            result_claim = gateway_command_pending_claim_result(
                &gateway_command_pending_state,
                packet,
                (uint32_t)result_received_at_ms,
                &command,
                &pending_command_id);
            if (result_claim !=
                GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE) {
                gateway_pending_command_result_token = 0u;
            }
            if (result_claim ==
                GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED) {
                gateway_command_timeout_dispatch_state.command = command;
                gateway_command_timeout_dispatch_state
                    .result_reservation_token =
                        result_reservation_token;
                gateway_command_timeout_dispatch_state.command_id =
                    pending_command_id;
                gateway_command_timeout_dispatch_state.status =
                    COMMAND_TIMEOUT;
                gateway_command_timeout_dispatch_state.reason = 0u;
                gateway_command_timeout_dispatch_state.pending = true;
                gateway_command_timeout_dispatch_state.running = false;
            }
        }

        k_mutex_unlock(&gateway_command_pending_lock);
        if (result_claim ==
            GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED) {
            gateway_survey_auto_discard_preflight_result();
            (void)k_work_cancel_delayable(
                &gateway_command_result_timeout_work);
            gateway_emit_host_command_result_reserved(
                result_reservation_token,
                &command,
                pending_command_id,
                COMMAND_TIMEOUT,
                0u);
            (void)gateway_schedule_command_timeout_side_effects();
            gateway_command_result_validation_release_reserved(
                validation_token);
            return collection_ret >= 0 ? collection_ret :
                   APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
        }
        if (result_claim !=
            GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED) {
            gateway_survey_auto_discard_preflight_result();
            gateway_command_result_validation_release_reserved(
                validation_token);
            return collection_ret >= 0 ? collection_ret : -ENOENT;
        }
        gateway_survey_auto_commit_preflight_result();
        (void)k_work_cancel_delayable(&gateway_command_result_timeout_work);
        if (result_reservation_token != 0u) {
            gateway_command_result_release_terminal_reserved(
                result_reservation_token,
                &command,
                pending_command_id);
        }
        ret = gateway_command_delivery_boundary(NULL,
                                                &command,
                                                pending_command_id,
                                                decoded.status,
                                                decoded.reason);
        if (ret < 0) {
            LOG_ERR("gateway command result boundary failed after terminal claim: cmd=0x%04x ret=%d",
                    (unsigned int)pending_command_id,
                    ret);
            gateway_command_result_validation_release_reserved(
                validation_token);
            return ret;
        }
        LOG_INF("gateway command result received: src=0x%016llx session=%u seq=%u",
                (unsigned long long)packet->src_id,
                packet->session_id,
                packet->seq);
        gateway_command_result_validation_release_reserved(validation_token);
        return collection_ret >= 0 ? collection_ret :
               APP_GATEWAY_SEMANTIC_ACCEPT_NEW;
    }
    gateway_survey_auto_discard_preflight_result();
    gateway_command_result_validation_release_reserved(validation_token);
    return collection_ret >= 0 ? collection_ret : mesh_errno_from_proto(ret);
}

int gateway_note_command_result_bundle(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint8_t received_radio_channel,
                                       const struct mesh_event_plan *current_channel9_plan)
{
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (received_radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EBADMSG;
    }
    ARG_UNUSED(current_channel9_plan);

    return gateway_note_collection_bundle(packet,
                                          payload,
                                          payload_len,
                                          previous_hop_id);
}

int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id)
{
    return gateway_begin_command_result_wait_for(
        command, command_id, GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
}

int gateway_begin_command_result_wait_for(const struct proto_packet *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms)
{
    uint32_t now_ms = k_uptime_get_32();

    if (timeout_ms == 0u || timeout_ms > (uint32_t)INT32_MAX) {
        return -EINVAL;
    }
    return gateway_begin_command_result_wait_until(
        command, command_id, now_ms + timeout_ms);
}

int gateway_begin_command_result_wait_until(
    const struct proto_packet *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms)
{
    struct gateway_command_pending started = {0};
    uint32_t now_ms;
    uint32_t remaining_ms;
    int ret;

    now_ms = k_uptime_get_32();
    remaining_ms = absolute_deadline_ms - now_ms;
    if (remaining_ms == 0u || remaining_ms > (uint32_t)INT32_MAX) {
        return -EINVAL;
    }
    if (gateway_collection_work_pending()) {
        return -EBUSY;
    }

    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = k_uptime_get_32();
    remaining_ms = absolute_deadline_ms - now_ms;
    if (remaining_ms == 0u || remaining_ms > (uint32_t)INT32_MAX) {
        ret = -ETIMEDOUT;
    } else if (gateway_command_timeout_dispatch_state.pending) {
        ret = -EBUSY;
    } else {
        ret = gateway_command_pending_start_until(
            &gateway_command_pending_state,
            command,
            command_id,
            now_ms,
            absolute_deadline_ms);
    }
    if (ret == PROTO_OK) {
        started = gateway_command_pending_state;
        gateway_pending_command_result_token =
            gateway_command_result_dispatch_token;
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    if (ret != PROTO_OK) {
        if (ret == -EBUSY) {
            return ret;
        }
        return ret == PROTO_ERR_MALFORMED ? -EBUSY : mesh_errno_from_proto(ret);
    }

    now_ms = k_uptime_get_32();
    remaining_ms = uptime_deadline_reached(now_ms, absolute_deadline_ms) ?
                   0u : absolute_deadline_ms - now_ms;
    /*
     * The timeout remains on the system queue so a long radio receive window
     * cannot delay host termination.  The RF guard is armed before that
     * receive and is therefore the serialization boundary with this owner.
     */
    ret = gateway_command_result_timeout_arm(
        remaining_ms, "command-admission");
    if (ret < 0) {
        if (gateway_pending_command_lock() == 0) {
            if (gateway_pending_snapshot_current_locked(&started)) {
                gateway_command_pending_clear(
                    &gateway_command_pending_state);
                gateway_pending_command_result_token = 0u;
            }
            k_mutex_unlock(&gateway_command_pending_lock);
        }
        return ret;
    }
    return 0;
}

static int gateway_restore_membership_runtime(void)
{
    bool publication_pending = false;
    int ret = app_mesh_persistence_restore_gateway_membership(
        &gateway_membership_roster_state,
        &publication_pending);

    if (ret < 0) {
        gateway_membership_clear(&gateway_membership_roster_state);
        atomic_set(&gateway_assignment_publication_pending_state, 0);
        gateway_membership_restore_pending = true;
        LOG_WRN("gateway membership snapshot restore unavailable: ret=%d", ret);
        return ret;
    }
    atomic_set(&gateway_assignment_publication_pending_state,
               publication_pending ? 1 : 0);
    gateway_membership_restore_pending = false;
    return 0;
}

static bool gateway_restored_eack_is_committed_predecessor(
    const struct gateway_collection_eack *eack)
{
    uint16_t next_sequence;
    uint8_t next_round;

    if (eack == NULL || !gateway_collection_tracking_active()) {
        return false;
    }
    next_sequence = eack->packet_sequence == UINT16_MAX ?
                    1u : (uint16_t)(eack->packet_sequence + 1u);
    next_round = eack->retry_round == UINT8_MAX ?
                 UINT8_MAX : (uint8_t)(eack->retry_round + 1u);

    /*
     * A valid old key is safe to retire only when the durable collection is
     * exactly the next protocol round written before that tombstone. Results
     * can arrive while the frozen EACK is on RF, so received_count and
     * collection_open may legitimately advance in that same durable write;
     * they are not part of the predecessor proof. Any fixed-identity or
     * sequence mismatch is corruption and remains fail-closed.
     */
    return eack->gateway_id == gateway_collection_state.gateway_id &&
           eack->gateway_epoch == gateway_collection_state.gateway_epoch &&
           eack->command_seq == gateway_collection_state.command_seq &&
           eack->collection_epoch_id ==
               gateway_collection_state.collection_epoch_id &&
           eack->membership_epoch ==
               gateway_collection_state.membership_epoch &&
           eack->expected_count == gateway_collection_state.expected_count &&
           next_sequence == gateway_collection_state.eack_sequence &&
           next_round == gateway_collection_state.retry_round;
}

static int gateway_restore_collection_runtime(void)
{
    struct gateway_collection_eack decoded_eack = {0};
    int custody_ret;
    int ret;

    ret = app_mesh_persistence_restore_gateway_collection(&gateway_collection_state);
    if (ret < 0) {
        gateway_collection_clear(&gateway_collection_state);
        gateway_collection_restore_pending = true;
        LOG_WRN("gateway collection journal restore unavailable: ret=%d", ret);
        return ret;
    }
    gateway_collection_restore_pending = false;
    gateway_collection_recovery_eack_active = false;
    custody_ret = app_mesh_persistence_restore_gateway_eack_custody(
        &gateway_collection_eack_retry_state.snapshot);
    if (custody_ret < 0) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        /*
         * The custody key may describe an EACK that reached RF before reset.
         * Until that exact record can be restored or an operator explicitly
         * repairs the key, neither collection admission nor a replacement
         * EACK is safe.  Leave restore pending so every collection boundary
         * fails closed and the persistence retry owner keeps health visible.
         */
        gateway_collection_restore_pending = true;
        LOG_ERR("gateway collection EACK custody blocks collection recovery: ret=%d",
                custody_ret);
        return custody_ret;
    } else if (gateway_collection_eack_retry_state.snapshot.valid) {
        ret = gateway_import_collection_recovery_eack_custody();
        if (ret == 0) {
            LOG_INF("gateway collection receipt EACK custody restored: session=%u seq=%u",
                    gateway_collection_eack_retry_state.snapshot.packet.session_id,
                    gateway_collection_eack_retry_state.snapshot.packet.seq);
            gateway_schedule_collection_recovery_eack_retry(
                "collection-receipt-eack-reset-resume", -EAGAIN);
            return 0;
        }
        if (ret != -ENOENT) {
            LOG_ERR("gateway collection recovery EACK custody is malformed and retained: ret=%d",
                    ret);
            app_gateway_eack_retry_reset(
                &gateway_collection_eack_retry_state);
            gateway_collection_restore_pending = true;
            return -EBADMSG;
        }
    }

    if (!gateway_collection_tracking_active()) {
        bool custody_present =
            gateway_collection_eack_retry_state.snapshot.valid;

        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        if (custody_present) {
            (void)gateway_clear_eack_custody_or_defer(
                "orphaned-eack-custody-clear");
        }
        return 0;
    }

    LOG_INF("gateway collection tracking restored: command_seq=%u collection=%u received=%u expected=%u open=%u eack_pending=%u",
            gateway_collection_state.command_seq,
            gateway_collection_state.collection_epoch_id,
            gateway_collection_state.received_count,
            gateway_collection_state.expected_count,
            gateway_collection_state.collection_open ? 1u : 0u,
            gateway_collection_state.eack_pending ? 1u : 0u);
    if (gateway_collection_eack_retry_state.snapshot.valid) {
        ret = app_gateway_eack_retry_import_custody(
            &gateway_collection_eack_retry_state,
            &gateway_collection_state,
            &gateway_collection_eack_retry_state.snapshot);
        if (ret == PROTO_OK) {
            ret = gateway_collection_eack_packet_validate(
                &gateway_collection_eack_retry_state.snapshot.packet,
                gateway_collection_eack_retry_state.snapshot.payload,
                gateway_collection_eack_retry_state.snapshot.payload_len,
                &decoded_eack);
        }
        if (ret == PROTO_OK &&
            gateway_collection_state.eack_pending) {
            gateway_collection_eack_round_dirty =
                decoded_eack.received_count !=
                    gateway_collection_state.received_count ||
                decoded_eack.collection_open !=
                    gateway_collection_state.collection_open;
            gateway_schedule_collection_eack_retry(
                "collection-eack-reset-resume", -EAGAIN, 0u);
            return 0;
        }

        /*
         * A crash after the durable round advance but before key deletion
         * leaves exactly one provable predecessor. Delete only that identity.
         * Every other valid-but-inconsistent record remains fail-closed.
         */
        ret = gateway_collection_eack_packet_validate(
            &gateway_collection_eack_retry_state.snapshot.packet,
            gateway_collection_eack_retry_state.snapshot.payload,
            gateway_collection_eack_retry_state.snapshot.payload_len,
            &decoded_eack);
        if (ret != PROTO_OK ||
            !gateway_restored_eack_is_committed_predecessor(&decoded_eack)) {
            LOG_ERR("gateway collection EACK custody conflicts with durable collection and is retained: ret=%d",
                    ret);
            app_gateway_eack_retry_reset(
                &gateway_collection_eack_retry_state);
            gateway_collection_restore_pending = true;
            return -EBADMSG;
        }

        LOG_WRN("gateway collection EACK predecessor clear resumed after reset: command_seq=%u collection=%u seq=%u",
                decoded_eack.command_seq,
                decoded_eack.collection_epoch_id,
                decoded_eack.packet_sequence);
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        if (gateway_clear_eack_custody_or_defer(
                "restored-eack-predecessor-clear") < 0) {
            return 0;
        }
    }

    if (!gateway_collection_state.eack_pending) {
        app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
        return 0;
    }

    if (!gateway_collection_state.collection_open) {
        /* A reset landed after the final result but before its EACK custody. */
        gateway_schedule_collection_eack_retry(
            "collection-eack-final-reset-gap", -EAGAIN, 0u);
    } else {
        gateway_schedule_collection_eack_round();
    }
    return 0;
}

void gateway_command_result_tracking_init(void)
{
    int ret;

    gateway_command_pending_clear(&gateway_command_pending_state);
    gateway_command_result_validation_clear(
        &gateway_command_result_validation_state);
    gateway_command_result_dispatch_token = 0u;
    gateway_pending_command_result_token = 0u;
    app_gateway_command_result_queue_init(&gateway_host_command_results);
    gateway_collection_clear(&gateway_collection_state);
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    gateway_collection_eack_round_dirty = false;
    gateway_collection_recovery_eack_active = false;
    gateway_membership_clear(&gateway_membership_roster_state);
    memset(&gateway_command_timeout_dispatch_state,
           0,
           sizeof(gateway_command_timeout_dispatch_state));
    k_work_init_delayable(&gateway_command_result_timeout_work,
                          gateway_command_result_timeout_handler);
    k_work_init_delayable(&gateway_command_timeout_side_effect_work,
                          gateway_command_timeout_side_effect_handler);
    k_work_init_delayable(
        &gateway_command_timeout_side_effect_retry_work,
        gateway_command_timeout_side_effect_retry_handler);
    k_work_init_delayable(&gateway_collection_eack_work,
                          gateway_collection_eack_work_handler);
    k_work_init_delayable(&gateway_persistence_retry_work,
                          gateway_persistence_retry_work_handler);
    gateway_collection_persistence_dirty = false;
    gateway_collection_clear_pending = false;
    gateway_collection_restore_pending = true;
    gateway_eack_custody_clear_pending = false;
    gateway_membership_persistence_dirty = false;
    gateway_membership_clear_pending = false;
    gateway_membership_restore_pending = true;
    atomic_set(&gateway_assignment_publication_pending_state, 0);
    atomic_set(&gateway_host_journal_restore_pending, 1);
    atomic_clear(&gateway_host_journal_restored);
    gateway_ble_stream_initialized = false;
    atomic_clear(&gateway_persistence_retry_schedule_deferred);
    atomic_clear(&gateway_persistence_retry_deferred_delay_ms);
    atomic_clear(&gateway_collection_eack_schedule_deferred);
    atomic_clear(&gateway_collection_eack_deferred_delay_ms);
    gateway_persistence_retry_round = 0u;
    mesh_gateway_semantic_commit_owner_clear();
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    ret = gateway_restore_membership_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("membership-restore");
        return;
    }

    ret = gateway_restore_collection_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("collection-restore");
    }
}
#else
void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(status);
    ARG_UNUSED(reason);
}

void gateway_emit_host_command_result_reserved(
    uint32_t result_reservation_token,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    ARG_UNUSED(result_reservation_token);
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(status);
    ARG_UNUSED(reason);
}

void gateway_command_result_set_dispatch_token(uint32_t token)
{
    ARG_UNUSED(token);
}

uint32_t gateway_command_result_get_dispatch_token(void)
{
    return 0u;
}

int gateway_begin_command_result_wait(const struct proto_packet *command,
                                      enum command_id command_id)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    return -ENOTSUP;
}

int gateway_begin_command_result_wait_until(
    const struct proto_packet *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(absolute_deadline_ms);
    return -ENOTSUP;
}

int gateway_command_result_validation_reserve(
    const struct proto_packet *result,
    uint64_t received_at_ms,
    uint32_t *token)
{
    ARG_UNUSED(result);
    ARG_UNUSED(received_at_ms);
    ARG_UNUSED(token);
    return -ENOTSUP;
}

int gateway_protocol_validation_arm(uint32_t receive_timeout_ms,
                                    uint32_t *token)
{
    ARG_UNUSED(receive_timeout_ms);
    if (token != NULL) {
        *token = 0u;
    }
    return -ENOTSUP;
}

int gateway_protocol_validation_complete(uint32_t token,
                                         uint64_t received_at_ms)
{
    ARG_UNUSED(token);
    ARG_UNUSED(received_at_ms);
    return -ENOTSUP;
}

enum gateway_command_result_validation_check
gateway_protocol_validation_check_interval(uint32_t started_at_ms,
                                           uint32_t deadline_ms)
{
    ARG_UNUSED(started_at_ms);
    ARG_UNUSED(deadline_ms);
    return GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR;
}

void gateway_command_result_validation_release_reserved(uint32_t token)
{
    ARG_UNUSED(token);
}

int gateway_begin_command_result_wait_for(const struct proto_packet *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
}

bool gateway_clear_pending_command_result(const struct proto_packet *command)
{
    ARG_UNUSED(command);
    return false;
}

int gateway_begin_command_collection(const struct gateway_command_options *options)
{
    ARG_UNUSED(options);
    return -ENOTSUP;
}

void gateway_clear_command_collection(const struct gateway_command_options *options)
{
    ARG_UNUSED(options);
}

int gateway_set_registered_membership_roster(uint16_t membership_epoch,
                                             const uint64_t *node_ids,
                                             const uint8_t *slots,
                                             size_t node_count,
                                             uint32_t assignment_epoch,
                                             uint32_t table_seq,
                                             const struct discovery_assignment_table_commitment *table_commitment,
                                             const struct gateway_membership_publication *publication)
{
    ARG_UNUSED(membership_epoch);
    ARG_UNUSED(node_ids);
    ARG_UNUSED(slots);
    ARG_UNUSED(node_count);
    ARG_UNUSED(assignment_epoch);
    ARG_UNUSED(table_seq);
    ARG_UNUSED(table_commitment);
    ARG_UNUSED(publication);
    return -ENOTSUP;
}

int gateway_get_registered_membership_roster_with_slots(
    uint64_t *node_ids,
    uint8_t *slots,
    size_t node_cap,
    size_t *node_count,
    uint16_t *membership_epoch)
{
    ARG_UNUSED(node_ids);
    ARG_UNUSED(slots);
    ARG_UNUSED(node_cap);
    if (node_count != NULL) {
        *node_count = 0u;
    }
    if (membership_epoch != NULL) {
        *membership_epoch = 0u;
    }
    return -ENOTSUP;
}

bool gateway_assignment_publication_pending(void)
{
    return false;
}

int gateway_complete_assignment_publication(
    const struct gateway_command_event *base_event,
    void *ctx)
{
    ARG_UNUSED(base_event);
    ARG_UNUSED(ctx);
    return -ENOTSUP;
}

void gateway_clear_registered_membership_roster(void)
{
}

int gateway_note_command_result(const struct proto_packet *packet,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t first_received_at_ms,
                                uint32_t result_validation_token,
                                uint64_t previous_hop_id,
                                uint8_t received_radio_channel,
                                const struct mesh_event_plan *current_channel9_plan)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(first_received_at_ms);
    ARG_UNUSED(result_validation_token);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    return -ENOTSUP;
}

int gateway_note_command_result_bundle(const struct proto_packet *packet,
                                       const uint8_t *payload,
                                       size_t payload_len,
                                       uint64_t previous_hop_id,
                                       uint8_t received_radio_channel,
                                       const struct mesh_event_plan *current_channel9_plan)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(received_radio_channel);
    ARG_UNUSED(current_channel9_plan);
    return -ENOTSUP;
}

void gateway_command_result_tracking_init(void)
{
}

void gateway_command_result_tracking_resume(void)
{
}
#endif
