#include "app_mesh_local_delivery.h"

#include "mesh.h"
#include "protocol.h"
#include "survey.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

static uint32_t delivery_checksum(
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    const uint8_t *bytes = (const uint8_t *)snapshot;
    uint32_t hash = UINT32_C(2166136261);

    for (size_t i = 0u; i < sizeof(*snapshot); i++) {
        uint8_t value = i >= offsetof(struct app_mesh_local_delivery_snapshot,
                                     checksum) &&
                        i < offsetof(struct app_mesh_local_delivery_snapshot,
                                     checksum) + sizeof(snapshot->checksum) ?
                        0u : bytes[i];

        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool delivery_source_envelope_matches(
    const struct proto_packet *packet,
    const struct proto_packet *expected)
{
    return packet != NULL && expected != NULL &&
           packet->msg_type == expected->msg_type &&
           packet->flags == expected->flags &&
           packet->src_id == expected->src_id &&
           packet->dst_id == expected->dst_id &&
           packet->session_id == expected->session_id &&
           packet->seq == expected->seq &&
           packet->ttl == expected->ttl &&
           packet->payload_len == expected->payload_len &&
           packet->message_age_ms == 0u;
}

static bool delivery_pair_result_valid(const struct mesh_outbound *outbound)
{
    struct proto_packet expected = {0};
    struct survey_sample sample = {0};

    if (survey_pair_result_payload_validate(outbound->payload,
                                            outbound->payload_len,
                                            &sample) != PROTO_OK ||
        sample.pair.operation_generation == 0u ||
        survey_init_result_packet_from_reporter(
            &expected,
            &sample,
            outbound->packet.src_id,
            outbound->packet.dst_id,
            outbound->packet.seq,
            outbound->payload_len) != PROTO_OK) {
        return false;
    }
    return delivery_source_envelope_matches(&outbound->packet, &expected);
}

static bool delivery_discovery_report_valid(
    const struct mesh_outbound *outbound)
{
    struct survey_reachability_entry
        entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    struct proto_packet expected = {0};
    const uint8_t *boot_raw = NULL;
    const uint8_t *status_raw = NULL;
    uint8_t canonical[SURVEY_REACH_REPORT_MAX_PAYLOAD_LEN];
    uint64_t operation_generation = 0u;
    uint64_t anchor_id = 0u;
    uint32_t boot_incarnation = 0u;
    uint32_t survey_id = 0u;
    size_t canonical_len = 0u;
    size_t entry_count = 0u;
    uint8_t boot_len = 0u;
    uint8_t status_len = 0u;
    uint16_t status;

    if (survey_extract_reach_report_tlvs(
            outbound->payload,
            outbound->payload_len,
            &survey_id,
            &anchor_id,
            entries,
            sizeof(entries) / sizeof(entries[0]),
            &entry_count) != PROTO_OK ||
        survey_operation_generation_extract_tlv(
            outbound->payload,
            outbound->payload_len,
            &operation_generation) != PROTO_OK ||
        operation_generation == 0u ||
        tlv_find_unique(outbound->payload,
                        outbound->payload_len,
                        TLV_NODE_BOOT_COUNTER,
                        &boot_raw,
                        &boot_len) != PROTO_OK ||
        boot_len != sizeof(uint32_t) ||
        (boot_incarnation = proto_get_u32_le(boot_raw)) == 0u ||
        tlv_find_unique(outbound->payload,
                        outbound->payload_len,
                        TLV_COMMAND_STATUS,
                        &status_raw,
                        &status_len) != PROTO_OK ||
        status_len != sizeof(uint16_t)) {
        return false;
    }
    status = proto_get_u16_le(status_raw);
    if (status > COMMAND_INTERNAL_ERROR ||
        survey_reachability_report_endpoints_validate(
            anchor_id,
            outbound->packet.dst_id,
            entries,
            entry_count) != PROTO_OK ||
        survey_append_reach_report_tlvs(canonical,
                                        sizeof(canonical),
                                        &canonical_len,
                                        survey_id,
                                        anchor_id,
                                        entries,
                                        entry_count) != PROTO_OK ||
        survey_operation_generation_append_tlv(
            canonical,
            sizeof(canonical),
            &canonical_len,
            operation_generation) != PROTO_OK ||
        tlv_append_u32(canonical,
                       sizeof(canonical),
                       &canonical_len,
                       TLV_NODE_BOOT_COUNTER,
                       boot_incarnation) != PROTO_OK ||
        tlv_append_u16(canonical,
                       sizeof(canonical),
                       &canonical_len,
                       TLV_COMMAND_STATUS,
                       status) != PROTO_OK ||
        canonical_len != outbound->payload_len ||
        memcmp(canonical, outbound->payload, canonical_len) != 0 ||
        survey_init_discovery_report_packet(
            &expected,
            anchor_id,
            outbound->packet.dst_id,
            survey_id,
            operation_generation,
            boot_incarnation,
            outbound->packet.seq,
            (uint8_t)canonical_len) != PROTO_OK) {
        return false;
    }
    return delivery_source_envelope_matches(&outbound->packet, &expected);
}

static bool delivery_packet_valid(const struct mesh_outbound *outbound)
{
    bool payload_valid;

    if (outbound == NULL) {
        return false;
    }
    if (outbound->payload_len != outbound->packet.payload_len ||
        outbound->payload_len == 0u ||
        outbound->payload_len > sizeof(outbound->payload)) {
        return false;
    }
    if (outbound->packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT) {
        payload_valid = delivery_discovery_report_valid(outbound);
    } else if (outbound->packet.msg_type == MSG_SURVEY_PAIR_RESULT) {
        payload_valid = delivery_pair_result_valid(outbound);
    } else {
        return false;
    }
    return payload_valid;
}

static int delivery_commit(struct app_mesh_local_delivery *delivery,
                           struct app_mesh_local_delivery_snapshot *candidate)
{
    int ret;

    candidate->checksum = delivery_checksum(candidate);
    if (delivery->ops.save == NULL) {
        return -ENOTSUP;
    }
    ret = delivery->ops.save(delivery->ops.ctx, candidate);
    if (ret == 0) {
        delivery->snapshot = *candidate;
    }
    return ret;
}

void app_mesh_local_delivery_init(struct app_mesh_local_delivery *delivery,
                                  const struct app_mesh_local_delivery_ops *ops)
{
    if (delivery == NULL) {
        return;
    }
    memset(delivery, 0, sizeof(*delivery));
    if (ops != NULL) {
        delivery->ops = *ops;
    }
}

void app_mesh_local_delivery_identity_from_outbound(
    const struct mesh_outbound *outbound,
    struct app_mesh_local_delivery_identity *identity)
{
    if (identity == NULL) {
        return;
    }
    memset(identity, 0, sizeof(*identity));
    if (outbound != NULL) {
        identity->src_id = outbound->packet.src_id;
        identity->dst_id = outbound->packet.dst_id;
        identity->session_id = outbound->packet.session_id;
        identity->seq = outbound->packet.seq;
        identity->payload_len = outbound->packet.payload_len;
        identity->msg_type = outbound->packet.msg_type;
        identity->flags = outbound->packet.flags;
        identity->semantic_digest_valid = mesh_packet_semantic_digest(
            &outbound->packet,
            outbound->payload,
            outbound->payload_len,
            identity->semantic_digest);
    }
}

bool app_mesh_local_delivery_identity_matches(
    const struct app_mesh_local_delivery_identity *identity,
    const struct proto_packet *packet)
{
    return identity != NULL && packet != NULL &&
           identity->src_id == packet->src_id &&
           identity->dst_id == packet->dst_id &&
           identity->session_id == packet->session_id &&
           identity->seq == packet->seq &&
           identity->payload_len == packet->payload_len &&
           identity->msg_type == packet->msg_type &&
           identity->flags == packet->flags;
}

bool app_mesh_local_delivery_identity_matches_semantic(
    const struct app_mesh_local_delivery_identity *identity,
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    return app_mesh_local_delivery_identity_matches(identity, packet) &&
           semantic_digest != NULL &&
           identity->semantic_digest_valid &&
           semantic_digest_equal(identity->semantic_digest,
                                 semantic_digest,
                                 sizeof(identity->semantic_digest));
}

bool app_mesh_local_delivery_identity_matches_outbound(
    const struct app_mesh_local_delivery_identity *identity,
    const struct mesh_outbound *outbound)
{
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];

    return identity != NULL && outbound != NULL &&
           mesh_packet_semantic_digest(&outbound->packet,
                                       outbound->payload,
                                       outbound->payload_len,
                                       semantic_digest) &&
           app_mesh_local_delivery_identity_matches_semantic(
               identity, &outbound->packet, semantic_digest);
}

bool app_mesh_local_delivery_identity_equal(
    const struct app_mesh_local_delivery_identity *left,
    const struct app_mesh_local_delivery_identity *right)
{
    return left != NULL && right != NULL &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->payload_len == right->payload_len &&
           left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->semantic_digest_valid &&
           right->semantic_digest_valid &&
           semantic_digest_equal(left->semantic_digest,
                                 right->semantic_digest,
                                 sizeof(left->semantic_digest));
}

bool app_mesh_local_delivery_snapshot_valid(
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->version == APP_MESH_LOCAL_DELIVERY_SNAPSHOT_VERSION &&
           snapshot->size == sizeof(*snapshot) &&
           snapshot->generation != 0u &&
           (snapshot->attempts_remaining > 0u ||
            snapshot->state == APP_MESH_LOCAL_DELIVERY_STARTING ||
            snapshot->state == APP_MESH_LOCAL_DELIVERY_TRACKED ||
            snapshot->state == APP_MESH_LOCAL_DELIVERY_RETRY ||
            snapshot->state == APP_MESH_LOCAL_DELIVERY_PREEMPTED ||
            snapshot->state == APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED ||
            snapshot->state == APP_MESH_LOCAL_DELIVERY_FAILED) &&
           snapshot->attempts_remaining <= APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS &&
           snapshot->state >= APP_MESH_LOCAL_DELIVERY_STAGED &&
           snapshot->state <= APP_MESH_LOCAL_DELIVERY_FAILED &&
           delivery_packet_valid(&snapshot->outbound) &&
           snapshot->checksum == delivery_checksum(snapshot);
}

int app_mesh_local_delivery_stage(struct app_mesh_local_delivery *delivery,
                                  const struct mesh_outbound *outbound,
                                  uint32_t generation)
{
    struct app_mesh_local_delivery_snapshot candidate = {0};

    if (delivery == NULL || !delivery_packet_valid(outbound) || generation == 0u) {
        return -EINVAL;
    }
    if (app_mesh_local_delivery_occupied(delivery)) {
        return -EBUSY;
    }
    candidate.version = APP_MESH_LOCAL_DELIVERY_SNAPSHOT_VERSION;
    candidate.size = sizeof(candidate);
    candidate.generation = generation;
    candidate.attempts_remaining = APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS;
    candidate.state = APP_MESH_LOCAL_DELIVERY_STAGED;
    candidate.attempt_token = 0u;
    candidate.outbound = *outbound;
    return delivery_commit(delivery, &candidate);
}

int app_mesh_local_delivery_restore(
    struct app_mesh_local_delivery *delivery,
    const struct app_mesh_local_delivery_snapshot *snapshot)
{
    if (delivery == NULL || !app_mesh_local_delivery_snapshot_valid(snapshot)) {
        return -EINVAL;
    }
    if (snapshot->state == APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED) {
        delivery->snapshot = *snapshot;
        return app_mesh_local_delivery_cleanup_ack(delivery);
    }
    delivery->snapshot = *snapshot;
    return 0;
}

int app_mesh_local_delivery_rebase_after_boot(
    struct app_mesh_local_delivery *delivery,
    uint32_t now_ms)
{
    struct app_mesh_local_delivery_snapshot candidate;
    int ret;

    if (!app_mesh_local_delivery_active(delivery)) {
        return -ENOENT;
    }
    candidate = delivery->snapshot;
    /*
     * Both supported survey result types have durable, non-expiring source
     * custody. Time spent in NVS or behind another reliable owner is not mesh
     * transit age, and the stored uptime domain ended at reset. Clear both
     * timestamps so the transport stamps a fresh queue origin only when this
     * exact packet is next admitted.
     */
    (void)now_ms;
    candidate.outbound.queued_at_ms = 0u;
    candidate.outbound.queued_at_valid = false;
    candidate.outbound.earliest_tx_ms = 0u;
    candidate.outbound.earliest_tx_valid = false;
    candidate.checksum = delivery_checksum(&candidate);
    ret = delivery->ops.save == NULL ? -ENOTSUP :
          delivery->ops.save(delivery->ops.ctx, &candidate);
    /* Boot-relative timing is never valid after reset, even if NVS is busy. */
    delivery->snapshot = candidate;
    return ret;
}

int app_mesh_local_delivery_retire_elapsed_not_before(
    struct app_mesh_local_delivery *delivery,
    uint32_t now_ms)
{
    struct app_mesh_local_delivery_snapshot candidate;

    if (!app_mesh_local_delivery_active(delivery)) {
        return -ENOENT;
    }
    if (!delivery->snapshot.outbound.earliest_tx_valid) {
        return 0;
    }
    if ((int32_t)(now_ms -
                  delivery->snapshot.outbound.earliest_tx_ms) < 0) {
        return -EAGAIN;
    }

    candidate = delivery->snapshot;
    candidate.outbound.earliest_tx_ms = 0u;
    candidate.outbound.earliest_tx_valid = false;
    return delivery_commit(delivery, &candidate);
}

int app_mesh_local_delivery_postpone_not_before(
    struct app_mesh_local_delivery *delivery,
    uint32_t not_before_ms)
{
    struct app_mesh_local_delivery_snapshot candidate;
    int ret;

    if (!app_mesh_local_delivery_active(delivery) ||
        !delivery->snapshot.outbound.earliest_tx_valid) {
        return -ENOENT;
    }
    if (delivery->snapshot.attempts_remaining !=
            APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS ||
        delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        return -EALREADY;
    }
    if ((int32_t)(not_before_ms -
                  delivery->snapshot.outbound.earliest_tx_ms) <= 0) {
        return 0;
    }
    candidate = delivery->snapshot;
    candidate.outbound.earliest_tx_ms = not_before_ms;
    ret = delivery_commit(delivery, &candidate);
    return ret < 0 ? ret : 1;
}

int app_mesh_local_delivery_recover(
    struct app_mesh_local_delivery *delivery,
    const struct app_mesh_local_delivery_snapshot *snapshot,
    int persistence_result,
    struct app_mesh_local_delivery_recovery *recovery)
{
    int ret;

    if (delivery == NULL || recovery == NULL) {
        return -EINVAL;
    }
    memset(recovery, 0, sizeof(*recovery));
    if (persistence_result == 0 || persistence_result == -ENOENT ||
        persistence_result == -ENOTSUP) {
        if (delivery->snapshot.state ==
            APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT) {
            memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
        }
        return 0;
    }
    if (persistence_result > 0 &&
        app_mesh_local_delivery_snapshot_valid(snapshot)) {
        ret = app_mesh_local_delivery_restore(delivery, snapshot);
        if (ret == 0) {
            recovery->restored = app_mesh_local_delivery_active(delivery);
            return 0;
        }
        if (app_mesh_local_delivery_ack_committed(delivery)) {
            /*
             * Gateway acceptance was already durably committed.  A failed
             * tombstone is cleanup debt, not a reason to quarantine or replay
             * the report.  Keep the exact ACK_COMMITTED owner in RAM and ask
             * the runtime to retry only the delete.
             */
            recovery->source_error = ret;
            recovery->retry_required = true;
            return 0;
        }
        recovery->source_error = ret;
    } else if (persistence_result > 0 || persistence_result == -EBADMSG) {
        recovery->source_error = -EBADMSG;
    } else {
        recovery->source_error = persistence_result;
        recovery->retry_required = true;
        memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
        delivery->snapshot.version = APP_MESH_LOCAL_DELIVERY_SNAPSHOT_VERSION;
        delivery->snapshot.state = APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT;
        return 0;
    }

    recovery->quarantined = true;
    recovery->clear_error = delivery->ops.clear == NULL ? -ENOTSUP :
                            delivery->ops.clear(delivery->ops.ctx);
    memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
    return 0;
}

int app_mesh_local_delivery_note_state(
    struct app_mesh_local_delivery *delivery,
    enum app_mesh_local_delivery_state state)
{
    struct app_mesh_local_delivery_snapshot candidate;

    if (!app_mesh_local_delivery_active(delivery) ||
        state < APP_MESH_LOCAL_DELIVERY_STAGED ||
        state > APP_MESH_LOCAL_DELIVERY_FAILED) {
        return -EINVAL;
    }
    candidate = delivery->snapshot;
    candidate.state = (uint8_t)state;
    return delivery_commit(delivery, &candidate);
}

int app_mesh_local_delivery_note_tracked(
    struct app_mesh_local_delivery *delivery)
{
    uint8_t attempt_token;
    int ret;

    ret = app_mesh_local_delivery_begin_attempt(delivery, &attempt_token);
    if (ret < 0) {
        return ret;
    }
    return app_mesh_local_delivery_note_attempt_sent(delivery, attempt_token);
}

int app_mesh_local_delivery_begin_attempt(
    struct app_mesh_local_delivery *delivery,
    uint8_t *attempt_token)
{
    struct app_mesh_local_delivery_snapshot candidate;
    uint8_t next_token;

    if (!app_mesh_local_delivery_active(delivery) || attempt_token == NULL) {
        return -ENOENT;
    }
    candidate = delivery->snapshot;
    if (candidate.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
        candidate.state == APP_MESH_LOCAL_DELIVERY_TRACKED) {
        return -EINPROGRESS;
    }
    if (candidate.attempts_remaining == 0u) {
        return -ETIMEDOUT;
    }
    candidate.attempts_remaining--;
    next_token = (uint8_t)(candidate.attempt_token + 1u);
    if (next_token == 0u) {
        next_token = 1u;
    }
    candidate.attempt_token = next_token;
    candidate.state = APP_MESH_LOCAL_DELIVERY_STARTING;
    {
        int ret = delivery_commit(delivery, &candidate);

        if (ret < 0) {
            return ret;
        }
    }
    *attempt_token = next_token;
    return 0;
}

static int delivery_note_attempt_state(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token,
    enum app_mesh_local_delivery_state state,
    bool refund)
{
    struct app_mesh_local_delivery_snapshot candidate;

    if (!app_mesh_local_delivery_active(delivery)) {
        return -EALREADY;
    }
    if (attempt_token == 0u ||
        delivery->snapshot.attempt_token != attempt_token) {
        return -ESTALE;
    }
    candidate = delivery->snapshot;
    if (refund) {
        if (candidate.state != APP_MESH_LOCAL_DELIVERY_STARTING &&
            candidate.state != APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) {
            return -EALREADY;
        }
        if (candidate.attempts_remaining >=
            APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS) {
            return -EOVERFLOW;
        }
        candidate.attempts_remaining++;
    } else if (candidate.state != APP_MESH_LOCAL_DELIVERY_STARTING &&
               candidate.state != APP_MESH_LOCAL_DELIVERY_TRACKED) {
        return -EALREADY;
    }
    candidate.state = (uint8_t)state;
    return delivery_commit(delivery, &candidate);
}

int app_mesh_local_delivery_note_attempt_sent(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token)
{
    return delivery_note_attempt_state(delivery, attempt_token,
                                       APP_MESH_LOCAL_DELIVERY_TRACKED,
                                       false);
}

int app_mesh_local_delivery_note_attempt_not_sent(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token,
    enum app_mesh_local_delivery_state state)
{
    if (state != APP_MESH_LOCAL_DELIVERY_ROUTE_WAIT &&
        state != APP_MESH_LOCAL_DELIVERY_RETRY &&
        state != APP_MESH_LOCAL_DELIVERY_PREEMPTED) {
        return -EINVAL;
    }
    return delivery_note_attempt_state(delivery, attempt_token, state, true);
}

int app_mesh_local_delivery_note_attempt_released(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token,
    enum app_mesh_local_delivery_state state)
{
    if (state != APP_MESH_LOCAL_DELIVERY_RETRY &&
        state != APP_MESH_LOCAL_DELIVERY_PREEMPTED) {
        return -EINVAL;
    }
    return delivery_note_attempt_state(delivery, attempt_token, state, false);
}

int app_mesh_local_delivery_note_attempt_blocked(
    struct app_mesh_local_delivery *delivery,
    uint8_t attempt_token)
{
    if (!app_mesh_local_delivery_active(delivery)) {
        return -EALREADY;
    }
    if (attempt_token == 0u ||
        delivery->snapshot.attempt_token != attempt_token) {
        return -ESTALE;
    }
    if (delivery->snapshot.state != APP_MESH_LOCAL_DELIVERY_STARTING) {
        return -EALREADY;
    }
    delivery->snapshot.state = APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE;
    return 0;
}

int app_mesh_local_delivery_resume_blocked_attempt(
    struct app_mesh_local_delivery *delivery,
    uint8_t *attempt_token)
{
    if (!app_mesh_local_delivery_active(delivery) || attempt_token == NULL) {
        return -EINVAL;
    }
    if (delivery->snapshot.state != APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) {
        return -ENOENT;
    }
    delivery->snapshot.state = APP_MESH_LOCAL_DELIVERY_STARTING;
    *attempt_token = delivery->snapshot.attempt_token;
    return 0;
}

uint16_t app_mesh_local_delivery_attempts_available(
    const struct app_mesh_local_delivery *delivery)
{
    uint16_t available;

    if (!app_mesh_local_delivery_active(delivery)) {
        return 0u;
    }
    available = delivery->snapshot.attempts_remaining;
    if ((delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING ||
         delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) &&
        available < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS) {
        available++;
    }
    return available;
}

int app_mesh_local_delivery_commit_ack(
    struct app_mesh_local_delivery *delivery,
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    struct app_mesh_local_delivery_identity identity;
    struct app_mesh_local_delivery_snapshot candidate;
    int ret;

    if (app_mesh_local_delivery_ack_committed(delivery)) {
        app_mesh_local_delivery_identity_from_outbound(
            &delivery->snapshot.outbound, &identity);
        return packet != NULL && semantic_digest != NULL &&
               app_mesh_local_delivery_identity_matches_semantic(
                   &identity, packet, semantic_digest) ?
               0 : (packet == NULL || semantic_digest == NULL ?
                    -EINVAL : -EKEYREJECTED);
    }
    if (!app_mesh_local_delivery_active(delivery)) {
        return -ENOENT;
    }
    if (packet == NULL || semantic_digest == NULL) {
        return -EINVAL;
    }
    app_mesh_local_delivery_identity_from_outbound(&delivery->snapshot.outbound,
                                                   &identity);
    if (!app_mesh_local_delivery_identity_matches_semantic(
            &identity, packet, semantic_digest)) {
        return -EKEYREJECTED;
    }
    candidate = delivery->snapshot;
    candidate.state = APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED;
    ret = delivery_commit(delivery, &candidate);
    return ret;
}

int app_mesh_local_delivery_cleanup_ack(
    struct app_mesh_local_delivery *delivery)
{
    int ret;

    if (!app_mesh_local_delivery_ack_committed(delivery)) {
        return -EINVAL;
    }
    if (delivery->ops.clear == NULL) {
        return -ENOTSUP;
    }
    ret = delivery->ops.clear(delivery->ops.ctx);
    if (ret == 0) {
        memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
    }
    return ret;
}

int app_mesh_local_delivery_cancel(
    struct app_mesh_local_delivery *delivery)
{
    int ret;

    if (!app_mesh_local_delivery_occupied(delivery)) {
        return -ENOENT;
    }
    if (delivery->ops.clear == NULL) {
        return -ENOTSUP;
    }
    ret = delivery->ops.clear(delivery->ops.ctx);
    if (ret == 0) {
        memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
    }
    return ret;
}

int app_mesh_local_delivery_note_ack(
    struct app_mesh_local_delivery *delivery,
    const struct proto_packet *packet,
    const uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    int ret = app_mesh_local_delivery_commit_ack(
        delivery, packet, semantic_digest);

    return ret < 0 ? ret : app_mesh_local_delivery_cleanup_ack(delivery);
}

int app_mesh_local_delivery_note_failed(
    struct app_mesh_local_delivery *delivery)
{
    return app_mesh_local_delivery_note_state(delivery,
                                              APP_MESH_LOCAL_DELIVERY_FAILED);
}

int app_mesh_local_delivery_rearm_attempts(
    struct app_mesh_local_delivery *delivery)
{
    struct app_mesh_local_delivery_snapshot candidate;

    if (!app_mesh_local_delivery_active(delivery) ||
        delivery->snapshot.state ==
            APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED ||
        delivery->snapshot.state ==
            APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT ||
        delivery->snapshot.state ==
            APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE ||
        delivery->snapshot.attempts_remaining != 0u) {
        return -EINVAL;
    }
    candidate = delivery->snapshot;
    candidate.attempts_remaining =
        APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS;
    candidate.state = APP_MESH_LOCAL_DELIVERY_RETRY;
    return delivery_commit(delivery, &candidate);
}

int app_mesh_local_delivery_discard_failed(
    struct app_mesh_local_delivery *delivery)
{
    int ret;

    if (delivery == NULL ||
        delivery->snapshot.state != APP_MESH_LOCAL_DELIVERY_FAILED) {
        return -EINVAL;
    }
    if (delivery->ops.clear == NULL) {
        return -ENOTSUP;
    }
    ret = delivery->ops.clear(delivery->ops.ctx);
    if (ret == 0) {
        memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
    }
    return ret;
}

bool app_mesh_local_delivery_active(const struct app_mesh_local_delivery *delivery)
{
    return delivery != NULL &&
           delivery->snapshot.version ==
               APP_MESH_LOCAL_DELIVERY_SNAPSHOT_VERSION &&
           ((delivery->snapshot.state >= APP_MESH_LOCAL_DELIVERY_STAGED &&
             delivery->snapshot.state <= APP_MESH_LOCAL_DELIVERY_FAILED) ||
            delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT ||
            delivery->snapshot.state == APP_MESH_LOCAL_DELIVERY_BLOCKED_LIVE) &&
           delivery->snapshot.state != APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED;
}

bool app_mesh_local_delivery_occupied(
    const struct app_mesh_local_delivery *delivery)
{
    return app_mesh_local_delivery_active(delivery) ||
           app_mesh_local_delivery_ack_committed(delivery);
}

bool app_mesh_local_delivery_ack_committed(
    const struct app_mesh_local_delivery *delivery)
{
    return delivery != NULL &&
           delivery->snapshot.version ==
               APP_MESH_LOCAL_DELIVERY_SNAPSHOT_VERSION &&
           delivery->snapshot.state ==
               APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED;
}

const struct mesh_outbound *app_mesh_local_delivery_outbound(
    const struct app_mesh_local_delivery *delivery)
{
    return app_mesh_local_delivery_active(delivery) &&
           delivery->snapshot.state != APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT ?
           &delivery->snapshot.outbound : NULL;
}
