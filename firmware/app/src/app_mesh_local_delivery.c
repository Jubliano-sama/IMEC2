#include "app_mesh_local_delivery.h"

#include "protocol.h"

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

static bool delivery_packet_valid(const struct mesh_outbound *outbound)
{
    return outbound != NULL &&
           outbound->packet.msg_type == MSG_SURVEY_DISCOVERY_REPORT &&
           outbound->packet.src_id != 0u && outbound->packet.dst_id != 0u &&
           outbound->packet.session_id != 0u && outbound->packet.seq != 0u &&
           outbound->payload_len == outbound->packet.payload_len &&
           outbound->payload_len <= sizeof(outbound->payload);
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
        identity->msg_type = outbound->packet.msg_type;
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
           identity->msg_type == packet->msg_type;
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
    if (app_mesh_local_delivery_active(delivery)) {
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
        int ret = delivery->ops.clear == NULL ? -ENOTSUP :
                  delivery->ops.clear(delivery->ops.ctx);

        if (ret == 0) {
            memset(&delivery->snapshot, 0, sizeof(delivery->snapshot));
        }
        return ret;
    }
    delivery->snapshot = *snapshot;
    return 0;
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

int app_mesh_local_delivery_note_ack(
    struct app_mesh_local_delivery *delivery,
    const struct proto_packet *packet)
{
    struct app_mesh_local_delivery_identity identity;
    struct app_mesh_local_delivery_snapshot candidate;
    int ret;

    if (!app_mesh_local_delivery_active(delivery)) {
        return -ENOENT;
    }
    if (packet == NULL) {
        return -EINVAL;
    }
    app_mesh_local_delivery_identity_from_outbound(&delivery->snapshot.outbound,
                                                   &identity);
    if (!app_mesh_local_delivery_identity_matches(&identity, packet)) {
        return -EKEYREJECTED;
    }
    candidate = delivery->snapshot;
    candidate.state = APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED;
    ret = delivery_commit(delivery, &candidate);
    if (ret < 0) {
        return ret;
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

int app_mesh_local_delivery_note_failed(
    struct app_mesh_local_delivery *delivery)
{
    return app_mesh_local_delivery_note_state(delivery,
                                              APP_MESH_LOCAL_DELIVERY_FAILED);
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

const struct mesh_outbound *app_mesh_local_delivery_outbound(
    const struct app_mesh_local_delivery *delivery)
{
    return app_mesh_local_delivery_active(delivery) &&
           delivery->snapshot.state != APP_MESH_LOCAL_DELIVERY_RECOVERY_WAIT ?
           &delivery->snapshot.outbound : NULL;
}
