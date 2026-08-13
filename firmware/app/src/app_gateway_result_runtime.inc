#line 566 __BASE_FILE__
#if DEVICE_ROLE == ROLE_GATEWAY
static struct gateway_command_pending gateway_command_pending_state;
static struct gateway_command_result_validation_leases
    gateway_command_result_validation_state;
static struct app_gateway_command_result_queue gateway_host_command_results;
static struct gateway_collection_state gateway_collection_state;
static struct app_gateway_collection_recovery gateway_collection_recovery;
static struct gateway_membership_roster gateway_membership_roster_state;
/* One immutable assignment proof lives with the gateway roster, not with the
 * collection ledger. Its opaque receipt is minted only by the NVS owner and
 * must gate every roster activation, replay, retirement, or decommission. */
static struct gateway_membership_snapshot gateway_membership_snapshot_state;
static struct app_durable_state_gateway_assignment_identity
    gateway_membership_assignment_identity;
static struct app_durable_receipt gateway_membership_durable_receipt;
static bool gateway_membership_durable_receipt_valid;
/* A failed save may have committed before its readback failed. Keep the exact
 * candidate in RAM and retry/adopt it before admitting any successor. */
static bool gateway_membership_adoption_pending;
static bool gateway_membership_publisher_activation_pending;
/* A live assignment lease exists only in the pre-reset owner.  A replay has
 * no old lease to release after it retires the durable publication debt. */
static bool gateway_membership_publication_live_owner;
static bool gateway_membership_publication_owner_release_pending;
static struct k_work_delayable gateway_command_result_timeout_work;
static struct k_work_delayable gateway_command_timeout_side_effect_work;
static struct k_work_delayable gateway_command_timeout_side_effect_retry_work;
static struct k_work_delayable gateway_collection_eack_work;
static struct app_gateway_eack_retry_state gateway_collection_eack_retry_state;
static bool gateway_collection_eack_round_dirty;
static struct k_work_delayable gateway_persistence_retry_work;
static bool gateway_membership_clear_pending;
static bool gateway_membership_restore_pending;
static atomic_t gateway_assignment_publication_pending_state =
    ATOMIC_INIT(0);
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
/* The singleton result-wait/timeout-dispatch owner retains one full command
 * commitment until its route-owned terminal side effects finish. */
static uint8_t
    gateway_pending_command_semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
static bool gateway_pending_command_semantic_digest_valid;
static uint32_t gateway_command_wait_reservation_next;
static uint32_t gateway_command_wait_reservation_active;

BUILD_ASSERT(APP_GATEWAY_COMMAND_RESULT_BUFFERED_INGRESS_DEPTH ==
                 GATEWAY_BLE_RX_FRAME_QUEUE_DEPTH,
             "command result credits must cover every buffered GATT frame");

void gateway_command_timeout_side_effects(
    const struct proto_packet *command,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN],
    enum command_id command_id);
void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason);

static bool gateway_collection_tracking_active(void);
static void gateway_schedule_collection_eack_round(void);
static int gateway_restore_membership_runtime(void);
int gateway_replay_pending_assignment_publication(void);

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
    /* Durable membership writes and publisher completion share one route
     * owner, so a reset replay cannot race roster activation or retirement. */
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

static void gateway_finalize_collection_clear(void)
{
    gateway_collection_clear(&gateway_collection_state);
    gateway_collection_eack_round_dirty = false;
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    atomic_clear(&gateway_collection_eack_schedule_deferred);
    atomic_clear(&gateway_collection_eack_deferred_delay_ms);
    (void)k_work_cancel_delayable(&gateway_collection_eack_work);
}

static void gateway_membership_clear_durable_evidence(void)
{
    memset(&gateway_membership_snapshot_state,
           0,
           sizeof(gateway_membership_snapshot_state));
    memset(&gateway_membership_assignment_identity,
           0,
           sizeof(gateway_membership_assignment_identity));
    memset(&gateway_membership_durable_receipt,
           0,
           sizeof(gateway_membership_durable_receipt));
    gateway_membership_durable_receipt_valid = false;
    gateway_membership_adoption_pending = false;
    gateway_membership_publisher_activation_pending = false;
}

static int gateway_membership_identity_from_publication(
    uint32_t assignment_epoch,
    const struct gateway_membership_publication *publication,
    struct app_durable_state_gateway_assignment_identity *identity)
{
    if (assignment_epoch == 0u || publication == NULL || identity == NULL ||
        publication->host_command.session_id == 0u ||
        publication->host_command.seq == 0u ||
        publication->event_gateway_epoch == 0u) {
        return -EINVAL;
    }

    *identity =
        (struct app_durable_state_gateway_assignment_identity) {
            .correlation_id = publication->host_command.session_id,
            .gateway_sequence = assignment_epoch,
            .host_session_id = publication->host_command.session_id,
            .gateway_epoch = publication->event_gateway_epoch,
            .host_seq = publication->host_command.seq,
        };
    return 0;
}

static bool gateway_membership_identity_matches_event(
    const struct app_durable_state_gateway_assignment_identity *identity,
    const struct gateway_command_event *event)
{
    return identity != NULL && event != NULL &&
           event->kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION &&
           event->command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
           event->correlation_id == identity->correlation_id &&
           event->gateway_sequence == identity->gateway_sequence &&
           event->host_session_id == identity->host_session_id &&
           event->gateway_epoch == identity->gateway_epoch &&
           event->host_seq == identity->host_seq;
}

static int gateway_membership_receipt_validate(void)
{
    /* A receipt is minted only after the durable owner has written and read
     * back this exact record.  Re-reading it here adds an NVS operation to
     * every replay without strengthening the sole-owner capability: retire()
     * and delete() revalidate their irreversible transitions themselves. */
    return gateway_membership_durable_receipt_valid ? 0 : -EACCES;
}

static void gateway_membership_hold_adoption(
    const struct gateway_membership_snapshot *snapshot,
    const struct app_durable_state_gateway_assignment_identity *identity,
    const char *reason)
{
    if (snapshot == NULL || identity == NULL) {
        return;
    }
    gateway_membership_snapshot_state = *snapshot;
    gateway_membership_assignment_identity = *identity;
    memset(&gateway_membership_durable_receipt,
           0,
           sizeof(gateway_membership_durable_receipt));
    gateway_membership_durable_receipt_valid = false;
    gateway_membership_adoption_pending = true;
    gateway_membership_publisher_activation_pending = false;
    /* The prepared publisher and operation lease remain owned by the original
     * assignment until this exact candidate is adopted and host-receipted. */
    gateway_membership_publication_live_owner = true;
    gateway_membership_publication_owner_release_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state, 0);
    gateway_schedule_persistence_retry(reason);
}

static int gateway_membership_adopt_pending_commit(void)
{
    struct gateway_membership_roster restored = {0};
    struct app_durable_receipt adopted_receipt;
    uint16_t acknowledged_count;
    int ret;

    if (!gateway_membership_adoption_pending) {
        return 0;
    }
    if (gateway_membership_snapshot_state.publication_table_round == 0u) {
        return -EPROTO;
    }

    /* save() first rereads/adopts an exact committed candidate. If the first
     * write did not land, this is the same semantic commit retry, never a
     * new successor allocation. */
    ret = app_durable_state_save_gateway_assignment_commit(
        DEVICE_ID,
        &gateway_membership_snapshot_state,
        &gateway_membership_assignment_identity,
        &adopted_receipt);
    if (ret == APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_SAVE_ADOPT_REQUIRED) {
        return -EAGAIN;
    }
    if (ret < 0) {
        return ret;
    }
    ret = gateway_membership_restore_snapshot(
        &restored, &gateway_membership_snapshot_state);
    if (ret != PROTO_OK) {
        return -EPROTO;
    }
    acknowledged_count = (uint16_t)__builtin_popcountll(
        gateway_membership_snapshot_state.publication_acknowledged_mask);
    ret = app_anchor_gateway_assignment_adopted_result_commit(
        &gateway_membership_assignment_identity, acknowledged_count);
    if (ret < 0) {
        /* The same RAM reservation stays with the inactive assignment owner
         * until its exact local result has queue custody.  Do not activate
         * the publisher or emit a durable-success terminal before then. */
        return ret;
    }

    gateway_membership_roster_state = restored;
    gateway_membership_durable_receipt = adopted_receipt;
    gateway_membership_durable_receipt_valid = true;
    gateway_membership_adoption_pending = false;
    gateway_membership_publisher_activation_pending = true;
    /* The original assignment deliberately retained its prepared publisher
     * and lease. It is not a reboot replay, so terminal host receipt still
     * has to release that live lease. */
    gateway_membership_publication_owner_release_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state, 1);
    return 0;
}

static void gateway_finalize_membership_clear(void)
{
    gateway_membership_clear(&gateway_membership_roster_state);
    gateway_membership_clear_durable_evidence();
    gateway_membership_clear_pending = false;
    gateway_membership_publication_live_owner = false;
    gateway_membership_publication_owner_release_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state, 0);
}

static void gateway_persistence_retry_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);
    atomic_clear(&gateway_persistence_retry_schedule_deferred);
    atomic_clear(&gateway_persistence_retry_deferred_delay_ms);

    if (gateway_membership_restore_pending) {
        ret = gateway_restore_membership_runtime();
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-restore");
            return;
        }
    }
    if (gateway_membership_adoption_pending) {
        ret = gateway_membership_adopt_pending_commit();
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-save-adopt");
            return;
        }
    }
    if (gateway_membership_publisher_activation_pending &&
        gateway_ble_stream_initialized) {
        ret = gateway_replay_pending_assignment_publication();
        if (ret < 0) {
            gateway_schedule_persistence_retry(
                "membership-adopt-publisher-activate");
            return;
        }
        gateway_membership_publisher_activation_pending = false;
    }
    if (gateway_membership_clear_pending) {
        ret = app_durable_state_delete_gateway_assignment(
            DEVICE_ID,
            &gateway_membership_assignment_identity,
            &gateway_membership_durable_receipt);
        if (ret < 0) {
            gateway_schedule_persistence_retry("membership-clear-delete");
            return;
        }
        gateway_finalize_membership_clear();
    }
    if (gateway_membership_publication_owner_release_pending) {
        ret = app_anchor_gateway_assignment_publication_complete();
        if (ret < 0) {
            gateway_schedule_persistence_retry(
                "assignment-publication-owner-release");
            return;
        }
        gateway_membership_publication_owner_release_pending = false;
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
            if (ret > 0 &&
                gateway_membership_publication_owner_release_pending) {
                ret = app_anchor_gateway_assignment_publication_complete();
                if (ret < 0) {
                    gateway_schedule_persistence_retry(
                        "assignment-publication-owner-release");
                    return;
                }
                gateway_membership_publication_owner_release_pending = false;
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
    ready = gateway_ble_stream_ready();
    retain_until_sent = false;
    if (packet != NULL && packet->msg_type == MSG_GATEWAY_COMMAND_EVENT) {
        struct gateway_command_event event;

        if (packet->flags == 0u) {
            /* Ordinary observability has no host-receipt owner.  It remains
             * a best-effort stream item and must never reach publisher
             * receipt advancement. */
        } else if (packet->flags == FLAG_GATEWAY_ACK_REQUIRED &&
                   payload != NULL && packet->src_id != 0u &&
                   packet->src_id == packet->dst_id &&
                   gateway_command_event_decode(payload,
                                                payload_len,
                                                &event) == 0 &&
                   event.event_seq != 0u &&
                   packet->session_id == event.event_seq &&
                   packet->seq == (uint16_t)event.event_seq &&
                   app_gateway_assignment_publisher_event_is_reliable(
                       &event)) {
            retain_until_sent = true;
        } else {
            /* ACK_REQUIRED is an explicit publisher-custody marker.  Do not
             * silently downgrade an invalid marked event to best effort,
             * because its future receipt could otherwise name a different
             * mesh owner behind it. */
            return -EBADMSG;
        }
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

static bool gateway_ble_packet_identity_matches(
    const struct proto_packet *left,
    const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq;
}

static bool gateway_host_receipt_identity_matches(
    const struct gateway_host_receipt_identity *identity,
    const struct proto_packet *packet,
    const uint8_t record_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return identity != NULL && packet != NULL && record_digest != NULL &&
           identity->original_msg_type == packet->msg_type &&
           identity->original_flags == packet->flags &&
           identity->src_id == packet->src_id &&
           identity->dst_id == packet->dst_id &&
           identity->session_id == packet->session_id &&
           identity->seq == packet->seq &&
           semantic_digest_equal(identity->stream_record_digest,
                                 record_digest,
                                 SEMANTIC_DIGEST_SHA256_LEN);
}

/* Gateway command results are synthesized locally from a host command and
 * retained only by the BLE stream.  They share MSG_COMMAND_RESULT with mesh
 * collection records, so require the exact local envelope before bypassing
 * the mesh custody completion path. */
static bool gateway_local_command_result_packet(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return packet != NULL && packet->msg_type == MSG_COMMAND_RESULT &&
           packet->src_id == DEVICE_ID && packet->dst_id == DEVICE_ID &&
           gateway_local_command_result_valid(packet, payload, payload_len);
}

static int gateway_ble_accept_host_receipt(
    const struct proto_packet *receipt_packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct gateway_host_receipt_identity identity;
    struct proto_packet head_packet;
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    uint8_t record_digest[SEMANTIC_DIGEST_SHA256_LEN];
    k_spinlock_key_t key;
    bool local_command_result = false;
    bool self_addressed_command_result = false;
    int ret;

    if (receipt_packet == NULL || payload == NULL) {
        return -EINVAL;
    }
    ret = gateway_host_receipt_packet_validate(receipt_packet,
                                               payload,
                                               payload_len,
                                               &identity);
    if (ret != PROTO_OK) {
        return -EBADMSG;
    }
    if (receipt_packet->dst_id != DEVICE_ID ||
        receipt_packet->src_id == DEVICE_ID ||
        receipt_packet->session_id != identity.session_id ||
        receipt_packet->seq != identity.seq) {
        return -EBADMSG;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    ret = gateway_ble_stream_peek(&gateway_ble_stream_state,
                                  &record,
                                  &record_len);
    if (ret < 0) {
        k_spin_unlock(&gateway_ble_stream_lock, key);
        if (ret == -ENOENT &&
            identity.original_msg_type == MSG_GATEWAY_COMMAND_EVENT &&
            gateway_command_event_duplicate_host_receipt_valid(
                &identity) == 0) {
            /* The command-event owner already consumed this exact receipt.
             * Keep a retransmitted GUI receipt idempotent without pretending
             * there is a generic mesh custody handoff to rearm. */
            return 0;
        }
        return ret;
    }
    ret = gateway_ble_stream_head_packet(&gateway_ble_stream_state,
                                         &head_packet);
    if (ret < 0 ||
        !semantic_digest_sha256(record, record_len, record_digest) ||
        !gateway_host_receipt_identity_matches(&identity,
                                               &head_packet,
                                               record_digest)) {
        k_spin_unlock(&gateway_ble_stream_lock, key);
        return -ESTALE;
    }
    if (record_len >= GATEWAY_BLE_STREAM_RECORD_HEADER_LEN) {
        self_addressed_command_result =
            head_packet.msg_type == MSG_COMMAND_RESULT &&
            head_packet.src_id == head_packet.dst_id;
        local_command_result = gateway_local_command_result_packet(
            &head_packet,
            &record[GATEWAY_BLE_STREAM_RECORD_HEADER_LEN],
            record_len - GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    }
    if (self_addressed_command_result && !local_command_result) {
        /* Self-addressed results are gateway-local BLE custody only.  A bad
         * local envelope may not fall through to the generic mesh callback,
         * which would complete whichever unrelated mesh item is pending. */
        k_spin_unlock(&gateway_ble_stream_lock, key);
        return -EBADMSG;
    }
    if (gateway_ble_stream_state.head_send_phase ==
            GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED) {
        ret = gateway_ble_stream_accept_host_receipt(
            &gateway_ble_stream_state);
    } else if (gateway_ble_stream_state.head_send_phase ==
               GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED) {
        /* A duplicate GUI receipt is harmless and re-arms mesh completion. */
        ret = -EALREADY;
    } else {
        ret = -EAGAIN;
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret != 0 && ret != -EALREADY) {
        return ret;
    }
    if (head_packet.msg_type == MSG_GATEWAY_COMMAND_EVENT) {
        /* Command telemetry is BLE-only. Retiring this head can expose an
         * unrelated mesh-custody item behind it, so it must never also signal
         * the generic mesh receipt owner. */
        return gateway_command_event_finish_host_receipt(&head_packet);
    }
    if (local_command_result) {
        /* A local host-command result was moved out of its admission queue
         * into this exact retained BLE head.  It has no mesh RX owner, and
         * signalling the generic callback could complete a different mesh
         * item that happens to be waiting behind it. */
        ret = gateway_ble_finish_host_delivery(&head_packet);
        if (ret == 0) {
#if DEVICE_ROLE == ROLE_GATEWAY
            (void)gateway_flush_host_command_results();
#endif
        }
        return ret;
    }
    mesh_gateway_host_receipt_ready();
    return 0;
}

int gateway_ble_accept_host_receipt_frame(const uint8_t *frame,
                                          size_t frame_len)
{
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    if (frame == NULL || frame_len == 0u) {
        return -EINVAL;
    }
    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK || packet.msg_type != MSG_GATEWAY_HOST_RECEIPT) {
        /* Let the ordinary gateway ingress report malformed/non-host frames. */
        return 0;
    }
    ret = gateway_ble_accept_host_receipt(&packet, payload, payload_len);
    return ret == 0 || ret == -EALREADY ? 1 : ret;
}

int gateway_ble_finish_host_delivery(const struct proto_packet *packet)
{
    k_spinlock_key_t key;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL) {
        return packet == NULL ? -EINVAL : -ENOTSUP;
    }

    key = k_spin_lock(&gateway_ble_stream_lock);
    if (gateway_ble_stream_state.count == 0u ||
        gateway_ble_stream_state.head_send_phase !=
            GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED) {
        ret = -EAGAIN;
    } else if (!gateway_ble_stream_state.items[0].host_custody_owner ||
               !gateway_ble_packet_identity_matches(
                   packet, &gateway_ble_stream_state.items[0].packet)) {
        ret = -ESTALE;
    } else {
        gateway_ble_stream_mark_sent(&gateway_ble_stream_state,
                                     k_uptime_get_32());
        ret = 0;
    }
    k_spin_unlock(&gateway_ble_stream_lock, key);
    if (ret == 0) {
        gateway_ble_schedule_stream_drain();
    }
    return ret;
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
#include "app_gateway_collection_runtime.inc"
#line 1874 __BASE_FILE__
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
    bool survey_transaction_owned;
    int ret;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len ||
        (packet->msg_type != MSG_COMMAND_RESULT &&
         packet->msg_type != MSG_RESULT_BUNDLE)) {
        return -EINVAL;
    }
    if (packet->msg_type == MSG_RESULT_BUNDLE) {
        uint16_t accepted_count = 0u;
        uint16_t duplicate_count = 0u;

        ret = gateway_validate_raw_result_bundle(
            packet, payload, payload_len);
        if (ret < 0) {
            return ret;
        }
        if (!gateway_collection_tracking_active()) {
            /* The raw packet is still held by its source.  A reboot removed
             * the RAM collection ledger, so retain one exact host record and
             * let only its later GUI receipt create the strict recovery EACK. */
            ret = app_gateway_collection_recovery_preflight(
                packet,
                payload,
                payload_len,
                DEVICE_ID,
                (uint16_t)mesh_runtime.upstream.current_epoch);
            return ret < 0 ? ret :
                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY;
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
            /* Stale/expired collection state has no volatile receipt ledger;
             * leave the sender's packet retryable instead of ACKing raw data. */
            return -EAGAIN;
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
            ret = app_gateway_collection_recovery_preflight(
                packet,
                payload,
                payload_len,
                DEVICE_ID,
                (uint16_t)mesh_runtime.upstream.current_epoch);
            return ret < 0 ? ret :
                   APP_GATEWAY_SEMANTIC_ACCEPT_COLLECTION_RECOVERY;
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
            return -EAGAIN;
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

    survey_transaction_owned = gateway_survey_owns_pending_control(
        &pending_snapshot.command,
        pending_snapshot.command_id);
    if (survey_transaction_owned || !pending_matches) {
        survey_preflight = gateway_survey_preflight_result(
            packet, payload, payload_len, first_received_at_ms);
    }
    admission = gateway_command_result_admit(
        &pending_snapshot,
        packet,
        survey_transaction_owned,
        survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED);
    gateway_survey_discard_preflight_result();

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

int gateway_collection_recovery_reserve_host_custody(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_collection_tracking_active()) {
        return -EBUSY;
    }
    return app_gateway_collection_recovery_reserve_host_custody(
        &gateway_collection_recovery,
        packet,
        payload,
        payload_len,
        DEVICE_ID,
        (uint16_t)mesh_runtime.upstream.current_epoch);
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
#endif
}

int gateway_collection_recovery_cancel_host_custody(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    return app_gateway_collection_recovery_cancel_host_custody(
        &gateway_collection_recovery, packet, payload, payload_len);
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
#endif
}

int gateway_collection_recovery_finish_host_delivery(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    return app_gateway_collection_recovery_finish_host_delivery(
        &gateway_collection_recovery, packet, payload, payload_len);
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
#endif
}

bool gateway_collection_recovery_active(void)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    return gateway_collection_recovery.active;
#else
    return false;
#endif
}

bool gateway_collection_recovery_flood_matches(
    const struct mesh_outbound *outbound)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    return app_gateway_collection_recovery_outbound_matches(
        &gateway_collection_recovery, outbound);
#else
    ARG_UNUSED(outbound);
    return false;
#endif
}

int gateway_collection_recovery_after_host_receipt(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct mesh_outbound outbound;
    struct app_mesh_flood_result flood_result = {0};
    uint32_t recovery_attempt_id = 0u;
    bool rf_started = false;
    int ret;

    if (packet == NULL || (payload == NULL && payload_len != 0u) ||
        packet->payload_len != payload_len) {
        return -EINVAL;
    }

    if (!gateway_collection_recovery.active) {
        /* The pre-receipt reservation is the ownership proof that this GUI
         * receipt names the one stale packet that blocked new collections. */
        if (!gateway_collection_recovery.host_custody_pending ||
            !app_gateway_collection_recovery_matches(
                &gateway_collection_recovery,
                packet,
                payload,
                payload_len)) {
            return -ESTALE;
        }

        /* packet_sequence is a 16-bit wire projection. Never transmit its
         * zero value; consume the rare skipped shared control ID instead. */
        do {
            ret = app_gateway_control_sequence_next(&recovery_attempt_id);
            if (ret < 0) {
                return ret;
            }
        } while ((uint16_t)recovery_attempt_id == 0u);

        ret = app_gateway_collection_recovery_begin(
            &gateway_collection_recovery,
            packet,
            payload,
            payload_len,
            DEVICE_ID,
            (uint16_t)mesh_runtime.upstream.current_epoch,
            recovery_attempt_id);
        if (ret < 0) {
            return ret;
        }
    } else if (!app_gateway_collection_recovery_matches(
                   &gateway_collection_recovery,
                   packet,
                   payload,
                   payload_len)) {
        return -ESTALE;
    }

    /* A failed BLE retirement may redrive this semantic completion. The
     * frozen flood already happened, so preserve its owner and do not emit a
     * second recovery attempt. */
    if (gateway_collection_recovery.flood_progress.complete) {
        return 0;
    }

    ret = app_gateway_collection_recovery_outbound(
        &gateway_collection_recovery, &outbound);
    if (ret < 0) {
        return ret;
    }
    ret = mesh_try_send_c5_flood_resume(
        &outbound,
        C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD,
        "collection-recovery-eack-c5",
        &gateway_collection_recovery.flood_progress,
        &flood_result,
        &rf_started);
    if (ret < 0) {
        return ret;
    }
    if (!gateway_collection_recovery.flood_progress.complete ||
        gateway_collection_recovery.flood_progress.next_opportunity !=
            app_mesh_flood_repeat_limit() ||
        flood_result.sent_count != app_mesh_flood_repeat_limit()) {
        return -EAGAIN;
    }

    recovery_attempt_id = gateway_collection_recovery.identity.recovery_attempt_id;
    LOG_INF("gateway collection recovery EACK sent: src=0x%016llx session=%u seq=%u attempt=%u",
            (unsigned long long)packet->src_id,
            packet->session_id,
            packet->seq,
            recovery_attempt_id);
    return 0;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
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
    if (!gateway_collection_tracking_active()) {
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
int gateway_commit_host_command_result_reserved(
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
        return -EINVAL;
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
        return mesh_errno_from_proto(ret);
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
        return ret;
    }
    (void)gateway_flush_host_command_results();
    return 0;
}

void gateway_emit_host_command_result_reserved(
    uint32_t result_reservation_token,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    int ret = gateway_commit_host_command_result_reserved(
        result_reservation_token, command, command_id, status, reason);

    if (ret < 0) {
        LOG_ERR("gateway command result custody unavailable: cmd=0x%04x ret=%d",
                (unsigned int)command_id,
                ret);
    }
}

void gateway_emit_host_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason)
{
    /*
     * The serialized host-command worker owns retryable admission pressure.
     * A local handler reports that pressure as COMMAND_BUSY while the exact
     * dispatch token remains published.  Do not convert the sole reserved
     * result slot at that intermediate boundary: the worker emits BACKOFF and
     * either retries the same command or converts the reservation once when
     * its bounded retry budget is exhausted.
     */
    if (status == COMMAND_BUSY &&
        gateway_command_result_dispatch_token != 0u) {
        return;
    }
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
        memset(gateway_pending_command_semantic_digest,
               0,
               sizeof(gateway_pending_command_semantic_digest));
        gateway_pending_command_semantic_digest_valid = false;
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
    bool wait_reserved =
        gateway_command_wait_reservation_active != 0u;

    k_mutex_unlock(&gateway_command_pending_lock);
    if (pending_active || timeout_side_effect_pending || wait_reserved ||
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
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    gateway_collection_eack_round_dirty = false;
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
    struct gateway_membership_snapshot candidate_snapshot;
    struct app_durable_state_gateway_assignment_identity candidate_identity;
    struct app_durable_receipt candidate_receipt;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || node_ids == NULL || slots == NULL ||
        table_commitment == NULL ||
        publication == NULL || publication->publish_pending == 0u) {
        return -EINVAL;
    }
    if (gateway_membership_restore_pending ||
        gateway_membership_clear_pending ||
        gateway_membership_adoption_pending ||
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

    ret = gateway_membership_export_assignment_snapshot(
        &candidate,
        assignment_epoch,
        table_seq,
        table_commitment,
        publication,
        &candidate_snapshot);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = gateway_membership_identity_from_publication(
        assignment_epoch, publication, &candidate_identity);
    if (ret < 0) {
        return ret;
    }
    ret = app_durable_state_save_gateway_assignment_commit(
        DEVICE_ID,
        &candidate_snapshot,
        &candidate_identity,
        &candidate_receipt);
    if (ret == APP_DURABLE_STATE_GATEWAY_ASSIGNMENT_SAVE_ADOPT_REQUIRED) {
        /* The write may be durable but its mandatory readback did not mint a
         * receipt. Retain this prepared batch and original lease; ordinary
         * failure/abort would emit a false terminal for a committed roster. */
        gateway_membership_hold_adoption(&candidate_snapshot,
                                         &candidate_identity,
                                         "membership-save-adopt");
        return GATEWAY_MEMBERSHIP_COMMIT_ADOPTION_PENDING;
    }
    if (ret < 0) {
        return ret;
    }
    gateway_membership_roster_state = candidate;
    gateway_membership_snapshot_state = candidate_snapshot;
    gateway_membership_assignment_identity = candidate_identity;
    gateway_membership_durable_receipt = candidate_receipt;
    gateway_membership_durable_receipt_valid = true;
    gateway_membership_publication_live_owner = true;
    gateway_membership_publication_owner_release_pending = false;
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
        gateway_membership_clear_pending ||
        gateway_membership_adoption_pending) {
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
            gateway_membership_clear_pending ||
            gateway_membership_adoption_pending ||
            gateway_membership_publisher_activation_pending ||
            gateway_membership_publication_owner_release_pending ||
            atomic_get(&gateway_assignment_publication_pending_state) != 0);
}

int gateway_complete_assignment_publication(
    const struct gateway_command_event *base_event,
    void *ctx)
{
    struct gateway_membership_snapshot retired_snapshot;
    struct app_durable_receipt retired_receipt;
    int ret;

    ARG_UNUSED(ctx);
    if (DEVICE_ROLE != ROLE_GATEWAY || base_event == NULL ||
        base_event->kind !=
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION ||
        base_event->command_id != CMD_ASSIGN_DISCOVERY_SLOTS) {
        return -EINVAL;
    }
    if (atomic_get(&gateway_assignment_publication_pending_state) == 0 ||
        !gateway_membership_identity_matches_event(
            &gateway_membership_assignment_identity, base_event)) {
        return -ESTALE;
    }

    /* Do not pre-validate the old receipt here. A prior durable write may
     * have committed before its readback failed; retire() deliberately
     * recognizes that exact already-retired terminal and returns its new
     * receipt. Pre-validating would turn that safe retry into a wedge. */
    ret = app_durable_state_retire_gateway_assignment_commit(
        DEVICE_ID,
        &gateway_membership_durable_receipt,
        base_event,
        &retired_receipt);
    if (ret < 0) {
        return ret;
    }
    ret = gateway_membership_snapshot_retire_publication(
        &gateway_membership_snapshot_state, &retired_snapshot);
    if (ret != PROTO_OK) {
        return -EPROTO;
    }
    gateway_membership_snapshot_state = retired_snapshot;
    gateway_membership_durable_receipt = retired_receipt;
    gateway_membership_durable_receipt_valid = true;
    gateway_membership_publication_owner_release_pending =
        gateway_membership_publication_live_owner;
    gateway_membership_publication_live_owner = false;
    atomic_set(&gateway_assignment_publication_pending_state, 0);
    return 0;
}

void gateway_clear_registered_membership_roster(void)
{
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || gateway_membership_restore_pending ||
        gateway_membership_clear_pending) {
        return;
    }
    if (gateway_assignment_publication_pending()) {
        LOG_ERR("gateway membership decommission blocked by publication debt");
        return;
    }
    if (!gateway_membership_roster_state.valid) {
        return;
    }

    ret = app_durable_state_delete_gateway_assignment(
        DEVICE_ID,
        &gateway_membership_assignment_identity,
        &gateway_membership_durable_receipt);
    if (ret < 0) {
        LOG_ERR("gateway membership decommission retained: delete ret=%d", ret);
        gateway_membership_clear_pending = true;
        gateway_schedule_persistence_retry("membership-clear-delete");
        return;
    }
    gateway_finalize_membership_clear();
}

int gateway_replay_pending_assignment_publication(void)
{
    struct gateway_command_event base_event = {
        .schema_version = GATEWAY_COMMAND_EVENT_SCHEMA_VERSION,
        .record_len = GATEWAY_COMMAND_EVENT_WIRE_LEN,
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
    struct gateway_command_event table_event;
    struct gateway_command_event terminal_event;
    uint64_t claimed_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
    uint8_t claimed_slots[GATEWAY_MEMBERSHIP_MAX_NODES];
    uint16_t acknowledged_count;
    size_t claimed_count = 0u;
    bool cold_boot_replay;
    bool activated_prepared = false;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }
    if (gateway_membership_restore_pending ||
        gateway_membership_clear_pending) {
        return -EAGAIN;
    }
    if (atomic_get(&gateway_assignment_publication_pending_state) == 0) {
        return 0;
    }
    /* An ambiguous same-boot save keeps the original assignment lease and
     * prepared publisher alive.  It must resume as the live operation, while
     * a restored durable record has no predecessor lease and must identify
     * every reconstructed event as REPLAY. */
    cold_boot_replay = !gateway_membership_publication_live_owner;
    ret = gateway_membership_receipt_validate();
    if (ret < 0) {
        return ret;
    }
    if (gateway_membership_snapshot_state.publication_table_round == 0u ||
        gateway_membership_snapshot_state.publication_acknowledged_mask ==
            0u) {
        return -EPROTO;
    }

    for (uint8_t slot = 0u; slot < GATEWAY_MEMBERSHIP_MAX_NODES; slot++) {
        uint64_t node_id =
            gateway_membership_snapshot_state.publication_claimed_node_ids[
                slot];

        if (node_id == 0u) {
            continue;
        }
        claimed_ids[claimed_count] = node_id;
        claimed_slots[claimed_count] = slot;
        claimed_count++;
    }
    if (claimed_count == 0u) {
        return -EPROTO;
    }

    base_event.gateway_epoch =
        gateway_membership_assignment_identity.gateway_epoch;
    base_event.correlation_id =
        gateway_membership_assignment_identity.correlation_id;
    base_event.gateway_sequence =
        gateway_membership_assignment_identity.gateway_sequence;
    base_event.host_session_id =
        gateway_membership_assignment_identity.host_session_id;
    base_event.host_seq = gateway_membership_assignment_identity.host_seq;

    acknowledged_count = (uint16_t)__builtin_popcountll(
        gateway_membership_snapshot_state.publication_acknowledged_mask);
    if (acknowledged_count > claimed_count) {
        return -EPROTO;
    }

    ret = app_gateway_assignment_publisher_prepare_table(
        &base_event,
        claimed_ids,
        claimed_slots,
        NULL,
        claimed_count,
        gateway_membership_snapshot_state.publication_acknowledged_mask,
        gateway_membership_snapshot_state.publication_duplicate_count);
    if (ret < 0) {
        return ret;
    }
    if (ret > 0) {
        ret = app_gateway_assignment_publisher_commit_prepared_batch(
            &base_event);
        if (ret == -ESTALE) {
            /* A non-prepared matching publisher already owns the replay. */
            app_gateway_assignment_publisher_pump();
            return 1;
        }
        if (ret < 0) {
            return ret;
        }
        activated_prepared = true;
    } else {
        if (cold_boot_replay) {
            /* This is a fresh publisher reconstructed from durable
             * publication debt after reset. Tag it before any event can
             * leave the prepared batch so every mapping/table/terminal tells
             * the GUI that it is a replay. A same-boot adoption keeps its
             * original operation semantics even if it needs activation. */
            ret = app_gateway_assignment_publisher_mark_prepared_replay(
                &base_event);
            if (ret < 0) {
                (void)app_gateway_assignment_publisher_abort_prepared_batch(
                    &base_event);
                return ret;
            }
        }
        ret = app_gateway_assignment_publisher_commit_prepared_batch(
            &base_event);
    }
    if (ret < 0) {
        if (!activated_prepared) {
            (void)app_gateway_assignment_publisher_abort_prepared_batch(
                &base_event);
        }
        return ret;
    }

    table_event = base_event;
    table_event.stage = GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY;
    table_event.attempt =
        gateway_membership_snapshot_state.publication_table_round;
    table_event.progress_count = acknowledged_count;
    table_event.total_count = (uint16_t)claimed_count;
    table_event.success_count = acknowledged_count;
    table_event.failure_count =
        (uint16_t)(claimed_count - acknowledged_count);
    table_event.duplicate_count =
        gateway_membership_snapshot_state.publication_duplicate_count;
    app_gateway_assignment_publisher_stage_table_ready(&table_event);

    terminal_event = table_event;
    terminal_event.stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
    terminal_event.flags = GATEWAY_COMMAND_EVENT_FLAG_TERMINAL;
    if (!app_gateway_assignment_publisher_capture_terminal(&terminal_event)) {
        return -ESTALE;
    }
    return 1;
}

void gateway_clear_command_collection(const struct gateway_command_options *options)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || options == NULL ||
        !gateway_collection_tracking_active()) {
        return;
    }
    if (gateway_collection_state.command_seq != options->command_seq ||
        gateway_collection_state.collection_epoch_id != options->collection_epoch_id) {
        return;
    }

    /* RAM-only build: clearing the collection is immediate, no NVS tombstone. */
    gateway_finalize_collection_clear();
}

static void gateway_command_timeout_side_effect_handler(struct k_work *work)
{
    struct proto_packet command = {0};
    uint8_t command_digest[SEMANTIC_DIGEST_SHA256_LEN] = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_TIMEOUT;
    uint8_t reason = 0u;
    bool command_digest_valid = false;
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
        command_digest_valid =
            gateway_pending_command_semantic_digest_valid;
        if (command_digest_valid) {
            memcpy(command_digest,
                   gateway_pending_command_semantic_digest,
                   sizeof(command_digest));
        }
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
        gateway_command_timeout_side_effects(
            &command,
            command_digest_valid ? command_digest : NULL,
            command_id);
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
    memset(gateway_pending_command_semantic_digest,
           0,
           sizeof(gateway_pending_command_semantic_digest));
    gateway_pending_command_semantic_digest_valid = false;
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
    bool survey_transaction_owned;
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

    survey_transaction_owned = gateway_survey_owns_pending_control(
        &pending_snapshot.command,
        pending_snapshot.command_id);
    if (survey_transaction_owned || !pending_matches) {
        survey_preflight = gateway_survey_preflight_result(
            packet, payload, payload_len, result_received_at_ms);
    }

    admission = gateway_command_result_admit(&pending_snapshot,
                                              packet,
                                              survey_transaction_owned,
                                              survey_preflight ==
                                                  GATEWAY_SURVEY_RESULT_PREFLIGHT_ACCEPTED);

    if (admission == GATEWAY_COMMAND_RESULT_IGNORE) {
        if (survey_preflight ==
            GATEWAY_SURVEY_RESULT_PREFLIGHT_RECONCILED) {
            gateway_survey_commit_preflight_result();
            LOG_DBG("gateway survey duplicate/late result reconciled: src=0x%016llx session=%u seq=%u",
                    packet == NULL ? 0ull :
                    (unsigned long long)packet->src_id,
                    packet == NULL ? 0u : packet->session_id,
                    packet == NULL ? 0u : packet->seq);
        } else {
            gateway_survey_discard_preflight_result();
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
            gateway_survey_commit_preflight_result();
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
        gateway_survey_discard_preflight_result();
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
            gateway_survey_discard_preflight_result();
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
            } else if (result_claim ==
                       GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED) {
                memset(gateway_pending_command_semantic_digest,
                       0,
                       sizeof(gateway_pending_command_semantic_digest));
                gateway_pending_command_semantic_digest_valid = false;
            }
        }

        k_mutex_unlock(&gateway_command_pending_lock);
        if (result_claim ==
            GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED) {
            gateway_survey_discard_preflight_result();
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
            gateway_survey_discard_preflight_result();
            gateway_command_result_validation_release_reserved(
                validation_token);
            return collection_ret >= 0 ? collection_ret : -ENOENT;
        }
        gateway_survey_commit_preflight_result();
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
    gateway_survey_discard_preflight_result();
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

int gateway_begin_command_result_wait(const struct mesh_outbound *command,
                                      enum command_id command_id)
{
    return gateway_begin_command_result_wait_for(
        command, command_id, GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
}

int gateway_begin_command_result_wait_for(const struct mesh_outbound *command,
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
    const struct mesh_outbound *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms)
{
    uint32_t reservation_token = 0u;
    int ret = gateway_command_result_wait_reserve(&reservation_token);

    if (ret < 0) {
        return ret;
    }
    ret = gateway_command_result_wait_commit(
        reservation_token,
        command,
        command_id,
        absolute_deadline_ms);
    if (ret < 0) {
        (void)gateway_command_result_wait_cancel(reservation_token);
    }
    return ret;
}

int gateway_command_result_wait_reserve(uint32_t *reservation_token)
{
    int ret;

    if (reservation_token == NULL) {
        return -EINVAL;
    }
    *reservation_token = 0u;
    if (gateway_collection_work_pending()) {
        return -EBUSY;
    }
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    if (gateway_command_pending_state.active ||
        gateway_command_timeout_dispatch_state.pending ||
        gateway_command_timeout_dispatch_state.running ||
        gateway_command_wait_reservation_active != 0u) {
        ret = -EBUSY;
    } else {
        gateway_command_wait_reservation_next++;
        if (gateway_command_wait_reservation_next == 0u) {
            gateway_command_wait_reservation_next = 1u;
        }
        gateway_command_wait_reservation_active =
            gateway_command_wait_reservation_next;
        *reservation_token = gateway_command_wait_reservation_active;
        ret = 0;
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    return ret;
}

int gateway_command_result_wait_cancel(uint32_t reservation_token)
{
    int ret;

    if (reservation_token == 0u) {
        return -EINVAL;
    }
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    if (gateway_command_wait_reservation_active != reservation_token) {
        ret = -ESTALE;
    } else {
        gateway_command_wait_reservation_active = 0u;
        ret = 0;
    }
    k_mutex_unlock(&gateway_command_pending_lock);
    return ret;
}

int gateway_command_result_wait_commit(
    uint32_t reservation_token,
    const struct mesh_outbound *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms)
{
    struct gateway_command_pending started = {0};
    uint8_t command_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t now_ms;
    uint32_t remaining_ms;
    int ret;

    if (reservation_token == 0u || command == NULL ||
        command->packet.payload_len != command->payload_len ||
        !mesh_packet_semantic_digest(&command->packet,
                                     command->payload,
                                     command->payload_len,
                                     command_digest)) {
        return -EINVAL;
    }
    now_ms = k_uptime_get_32();
    remaining_ms = absolute_deadline_ms - now_ms;
    if (remaining_ms == 0u || remaining_ms > (uint32_t)INT32_MAX) {
        return -EINVAL;
    }
    ret = gateway_pending_command_lock();
    if (ret < 0) {
        return ret;
    }
    now_ms = k_uptime_get_32();
    remaining_ms = absolute_deadline_ms - now_ms;
    if (gateway_command_wait_reservation_active != reservation_token) {
        ret = -ESTALE;
    } else if (remaining_ms == 0u ||
               remaining_ms > (uint32_t)INT32_MAX) {
        ret = -ETIMEDOUT;
    } else if (gateway_command_pending_state.active ||
               gateway_command_timeout_dispatch_state.pending ||
               gateway_command_timeout_dispatch_state.running) {
        ret = -EBUSY;
    } else {
        ret = gateway_command_pending_start_until(
            &gateway_command_pending_state,
            &command->packet,
            command_id,
            now_ms,
            absolute_deadline_ms);
    }
    if (ret == PROTO_OK) {
        gateway_command_wait_reservation_active = 0u;
        started = gateway_command_pending_state;
        gateway_pending_command_result_token =
            gateway_command_result_dispatch_token;
        memcpy(gateway_pending_command_semantic_digest,
               command_digest,
               sizeof(gateway_pending_command_semantic_digest));
        gateway_pending_command_semantic_digest_valid = true;
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
                memset(gateway_pending_command_semantic_digest,
                       0,
                       sizeof(gateway_pending_command_semantic_digest));
                gateway_pending_command_semantic_digest_valid = false;
            }
            k_mutex_unlock(&gateway_command_pending_lock);
        }
        return ret;
    }
    return 0;
}

static int gateway_restore_membership_runtime(void)
{
    struct gateway_membership_roster restored = {0};
    bool replay_debt = false;
    int ret;

    gateway_membership_clear(&gateway_membership_roster_state);
    gateway_membership_clear_durable_evidence();
    gateway_membership_clear_pending = false;
    gateway_membership_publication_live_owner = false;
    gateway_membership_publication_owner_release_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state, 0);

    ret = app_durable_state_restore_gateway_assignment_commit(
        DEVICE_ID,
        &gateway_membership_snapshot_state,
        &gateway_membership_assignment_identity,
        &replay_debt,
        &gateway_membership_durable_receipt);
    if (ret < 0) {
        gateway_membership_clear_durable_evidence();
        return ret;
    }
    if (ret == 0) {
        gateway_membership_restore_pending = false;
        return 0;
    }

    ret = gateway_membership_restore_snapshot(
        &restored, &gateway_membership_snapshot_state);
    if (ret != PROTO_OK ||
        replay_debt !=
            (gateway_membership_snapshot_state.publication_table_round !=
             0u)) {
        gateway_membership_clear_durable_evidence();
        return -EPROTO;
    }
    gateway_membership_roster_state = restored;
    gateway_membership_durable_receipt_valid = true;
    gateway_membership_restore_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state,
               replay_debt ? 1 : 0);
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
    memset(gateway_pending_command_semantic_digest,
           0,
           sizeof(gateway_pending_command_semantic_digest));
    gateway_pending_command_semantic_digest_valid = false;
    gateway_command_wait_reservation_next = 0u;
    gateway_command_wait_reservation_active = 0u;
    app_gateway_command_result_queue_init(&gateway_host_command_results);
    gateway_collection_clear(&gateway_collection_state);
    app_gateway_collection_recovery_reset(&gateway_collection_recovery);
    app_gateway_eack_retry_reset(&gateway_collection_eack_retry_state);
    gateway_collection_eack_round_dirty = false;
    gateway_membership_clear(&gateway_membership_roster_state);
    gateway_membership_clear_durable_evidence();
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
    gateway_membership_clear_pending = false;
    gateway_membership_restore_pending = true;
    gateway_membership_publication_live_owner = false;
    gateway_membership_publication_owner_release_pending = false;
    atomic_set(&gateway_assignment_publication_pending_state, 0);
    gateway_ble_stream_initialized = false;
    atomic_clear(&gateway_persistence_retry_schedule_deferred);
    atomic_clear(&gateway_persistence_retry_deferred_delay_ms);
    atomic_clear(&gateway_collection_eack_schedule_deferred);
    atomic_clear(&gateway_collection_eack_deferred_delay_ms);
    gateway_persistence_retry_round = 0u;
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }

    ret = gateway_restore_membership_runtime();
    if (ret < 0) {
        gateway_schedule_persistence_retry("membership-restore");
        return;
    }
}
#else
int gateway_commit_host_command_result_reserved(
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
    return -ENOTSUP;
}

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

int gateway_begin_command_result_wait(const struct mesh_outbound *command,
                                      enum command_id command_id)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    return -ENOTSUP;
}

int gateway_begin_command_result_wait_until(
    const struct mesh_outbound *command,
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

int gateway_begin_command_result_wait_for(const struct mesh_outbound *command,
                                          enum command_id command_id,
                                          uint32_t timeout_ms)
{
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
}

int gateway_command_result_wait_reserve(uint32_t *reservation_token)
{
    ARG_UNUSED(reservation_token);
    return -ENOTSUP;
}

int gateway_command_result_wait_commit(
    uint32_t reservation_token,
    const struct mesh_outbound *command,
    enum command_id command_id,
    uint32_t absolute_deadline_ms)
{
    ARG_UNUSED(reservation_token);
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    ARG_UNUSED(absolute_deadline_ms);
    return -ENOTSUP;
}

int gateway_command_result_wait_cancel(uint32_t reservation_token)
{
    ARG_UNUSED(reservation_token);
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

int gateway_replay_pending_assignment_publication(void)
{
    return -ENOTSUP;
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
