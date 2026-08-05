#include "gateway_membership.h"

#include <string.h>

static uint16_t gateway_membership_snapshot_checksum(
    const struct gateway_membership_snapshot *snapshot)
{
    static const uint8_t zero_checksum[sizeof(snapshot->checksum)];
    const uint8_t *bytes = (const uint8_t *)snapshot;
    const size_t checksum_offset =
        offsetof(struct gateway_membership_snapshot, checksum);
    uint16_t crc;

    if (snapshot == NULL) {
        return 0u;
    }
    crc = proto_crc16_ccitt_false_update(UINT16_C(0xFFFF),
                                         bytes,
                                         checksum_offset);
    crc = proto_crc16_ccitt_false_update(crc,
                                         zero_checksum,
                                         sizeof(zero_checksum));
    return proto_crc16_ccitt_false_update(
        crc,
        bytes + checksum_offset + sizeof(snapshot->checksum),
        sizeof(*snapshot) - checksum_offset - sizeof(snapshot->checksum));
}

static void gateway_membership_snapshot_finalize(
    struct gateway_membership_snapshot *snapshot)
{
    snapshot->version = GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION;
    snapshot->magic = GATEWAY_MEMBERSHIP_SNAPSHOT_MAGIC;
    snapshot->checksum = 0u;
    snapshot->checksum = gateway_membership_snapshot_checksum(snapshot);
}

static bool gateway_membership_commitment_is_zero(
    const struct discovery_assignment_table_commitment *commitment)
{
    uint8_t combined = 0u;

    if (commitment == NULL) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(commitment->bytes); i++) {
        combined |= commitment->bytes[i];
    }
    return combined == 0u;
}

static int validate_dense_node_ids(const uint64_t *node_ids,
                                   size_t node_count)
{
    if (node_ids == NULL || node_count == 0u) {
        return PROTO_ERR_ARG;
    }
    if (node_count > GATEWAY_MEMBERSHIP_MAX_NODES) {
        return PROTO_ERR_NO_SPACE;
    }

    for (size_t i = 0u; i < node_count; i++) {
        if (node_ids[i] == 0u) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t j = 0u; j < i; j++) {
            if (node_ids[j] == node_ids[i]) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }

    return PROTO_OK;
}

static int validate_sparse_node_ids(
    const uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES],
    uint8_t node_count,
    uint8_t slot_span)
{
    size_t observed_count = 0u;

    if (node_ids == NULL ||
        node_count == 0u ||
        node_count > GATEWAY_MEMBERSHIP_MAX_NODES ||
        slot_span == 0u ||
        slot_span > GATEWAY_MEMBERSHIP_MAX_NODES ||
        node_count > slot_span) {
        return PROTO_ERR_MALFORMED;
    }
    if (node_ids[slot_span - 1u] == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t slot = 0u; slot < GATEWAY_MEMBERSHIP_MAX_NODES; slot++) {
        uint64_t node_id = node_ids[slot];

        if (slot >= slot_span) {
            if (node_id != 0u) {
                return PROTO_ERR_MALFORMED;
            }
            continue;
        }
        if (node_id == 0u) {
            continue;
        }
        observed_count++;
        for (size_t previous = 0u; previous < slot; previous++) {
            if (node_ids[previous] == node_id) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }

    return observed_count == node_count ? PROTO_OK :
                                          PROTO_ERR_MALFORMED;
}

static int validate_roster(const struct gateway_membership_roster *roster)
{
    if (roster == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!roster->valid || roster->membership_epoch == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    return validate_sparse_node_ids(roster->node_ids,
                                    roster->node_count,
                                    roster->slot_span);
}

static uint64_t valid_claim_mask(uint8_t claimed_count)
{
    if (claimed_count >= 64u) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << claimed_count) - UINT64_C(1);
}

static bool publication_is_zero(
    const struct gateway_membership_publication *publication)
{
    if (publication == NULL) {
        return false;
    }
    for (size_t slot = 0u;
         slot < GATEWAY_MEMBERSHIP_MAX_NODES;
         slot++) {
        if (publication->claimed_node_ids[slot] != 0u) {
            return false;
        }
    }

    return publication->host_command.msg_type == 0u &&
           publication->host_command.flags == 0u &&
           publication->host_command.src_id == 0u &&
           publication->host_command.dst_id == 0u &&
           publication->host_command.session_id == 0u &&
           publication->host_command.seq == 0u &&
           publication->host_command.ttl == 0u &&
           publication->host_command.payload_len == 0u &&
           publication->host_command.message_age_ms == 0u &&
           publication->committed_mask == 0u &&
           publication->acknowledged_mask == 0u &&
           publication->command_id == 0u &&
           publication->event_gateway_epoch == 0u &&
           publication->duplicate_count == 0u &&
           publication->claimed_count == 0u &&
           publication->claimed_slot_span == 0u &&
           publication->table_round == 0u &&
           publication->publish_pending == 0u;
}

static bool host_command_identity_valid(
    const struct gateway_membership_publication *publication)
{
    return publication->command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
           publication->host_command.msg_type == MSG_COMMAND &&
           publication->host_command.payload_len >=
               PROTO_TLV_U16_ENCODED_LEN &&
           publication->host_command.payload_len <=
               PACKET_MAX_PAYLOAD_LEN;
}

static bool snapshot_publication_fields_zero(
    const struct gateway_membership_snapshot *snapshot)
{
    if (snapshot->publication_host_src_id != 0u ||
        snapshot->publication_host_dst_id != 0u ||
        snapshot->publication_acknowledged_mask != 0u ||
        snapshot->publication_host_session_id != 0u ||
        snapshot->publication_host_message_age_ms != 0u ||
        snapshot->publication_host_seq != 0u ||
        snapshot->publication_host_payload_len != 0u ||
        snapshot->publication_event_gateway_epoch != 0u ||
        snapshot->publication_duplicate_count != 0u ||
        snapshot->publication_host_flags != 0u ||
        snapshot->publication_host_ttl != 0u) {
        return false;
    }
    for (size_t slot = 0u;
         slot < GATEWAY_MEMBERSHIP_MAX_NODES;
         slot++) {
        if (snapshot->publication_claimed_node_ids[slot] != 0u) {
            return false;
        }
    }
    return true;
}

static int snapshot_restore_publication(
    const struct gateway_membership_snapshot *snapshot,
    struct gateway_membership_publication *publication)
{
    uint8_t claimed_count = 0u;
    uint8_t claimed_slot_span = 0u;

    if (snapshot == NULL || publication == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(publication, 0, sizeof(*publication));
    if (snapshot->publication_table_round == 0u) {
        return snapshot_publication_fields_zero(snapshot) ?
               PROTO_ERR_NOT_FOUND : PROTO_ERR_MALFORMED;
    }

    memcpy(publication->claimed_node_ids,
           snapshot->publication_claimed_node_ids,
           sizeof(publication->claimed_node_ids));
    for (size_t slot = 0u;
         slot < GATEWAY_MEMBERSHIP_MAX_NODES;
         slot++) {
        if (publication->claimed_node_ids[slot] != 0u) {
            claimed_count++;
            claimed_slot_span = (uint8_t)(slot + 1u);
        }
        if (snapshot->node_ids[slot] != 0u) {
            publication->committed_mask |= UINT64_C(1) << slot;
        }
    }
    publication->host_command.msg_type = MSG_COMMAND;
    publication->host_command.flags = snapshot->publication_host_flags;
    publication->host_command.src_id = snapshot->publication_host_src_id;
    publication->host_command.dst_id = snapshot->publication_host_dst_id;
    publication->host_command.session_id =
        snapshot->publication_host_session_id;
    publication->host_command.seq = snapshot->publication_host_seq;
    publication->host_command.ttl = snapshot->publication_host_ttl;
    publication->host_command.payload_len =
        snapshot->publication_host_payload_len;
    publication->host_command.message_age_ms =
        snapshot->publication_host_message_age_ms;
    publication->acknowledged_mask =
        snapshot->publication_acknowledged_mask;
    publication->command_id = CMD_ASSIGN_DISCOVERY_SLOTS;
    publication->event_gateway_epoch =
        snapshot->publication_event_gateway_epoch;
    publication->duplicate_count = snapshot->publication_duplicate_count;
    publication->claimed_count = claimed_count;
    publication->claimed_slot_span = claimed_slot_span;
    publication->table_round = snapshot->publication_table_round;
    publication->publish_pending = 1u;
    return PROTO_OK;
}

static void snapshot_store_publication(
    struct gateway_membership_snapshot *snapshot,
    const struct gateway_membership_publication *publication)
{
    memcpy(snapshot->publication_claimed_node_ids,
           publication->claimed_node_ids,
           sizeof(snapshot->publication_claimed_node_ids));
    snapshot->publication_host_src_id = publication->host_command.src_id;
    snapshot->publication_host_dst_id = publication->host_command.dst_id;
    snapshot->publication_acknowledged_mask =
        publication->acknowledged_mask;
    snapshot->publication_host_session_id =
        publication->host_command.session_id;
    snapshot->publication_host_message_age_ms =
        publication->host_command.message_age_ms;
    snapshot->publication_host_seq = publication->host_command.seq;
    snapshot->publication_host_payload_len =
        publication->host_command.payload_len;
    snapshot->publication_event_gateway_epoch =
        publication->event_gateway_epoch;
    snapshot->publication_duplicate_count = publication->duplicate_count;
    snapshot->publication_host_flags = publication->host_command.flags;
    snapshot->publication_host_ttl = publication->host_command.ttl;
    snapshot->publication_table_round = publication->table_round;
}

static int validate_pending_publication(
    const uint64_t roster_node_ids[GATEWAY_MEMBERSHIP_MAX_NODES],
    uint8_t roster_node_count,
    uint8_t roster_slot_span,
    bool assignment_proof_valid,
    const struct gateway_membership_publication *publication)
{
    uint64_t allowed_mask;
    size_t committed_count;
    int ret;

    if (roster_node_ids == NULL || publication == NULL) {
        return PROTO_ERR_ARG;
    }
    if (publication->publish_pending > 1u) {
        return PROTO_ERR_MALFORMED;
    }
    if (!publication->publish_pending) {
        return publication_is_zero(publication) ? PROTO_OK :
                                                  PROTO_ERR_MALFORMED;
    }
    if (!assignment_proof_valid ||
        !host_command_identity_valid(publication) ||
        publication->claimed_count == 0u ||
        publication->claimed_count > GATEWAY_MEMBERSHIP_MAX_NODES ||
        publication->claimed_slot_span == 0u ||
        publication->claimed_slot_span > GATEWAY_MEMBERSHIP_MAX_NODES ||
        publication->table_round == 0u ||
        publication->claimed_slot_span < roster_slot_span) {
        return PROTO_ERR_MALFORMED;
    }

    ret = validate_sparse_node_ids(publication->claimed_node_ids,
                                   publication->claimed_count,
                                   publication->claimed_slot_span);
    if (ret != PROTO_OK) {
        return PROTO_ERR_MALFORMED;
    }

    allowed_mask = valid_claim_mask(publication->claimed_slot_span);
    if (publication->committed_mask == 0u ||
        (publication->committed_mask & ~allowed_mask) != 0u ||
        publication->acknowledged_mask == 0u ||
        (publication->acknowledged_mask & ~allowed_mask) != 0u ||
        (publication->acknowledged_mask &
         ~publication->committed_mask) != 0u) {
        return PROTO_ERR_MALFORMED;
    }
    for (size_t slot = 0u; slot < publication->claimed_slot_span; slot++) {
        if ((publication->committed_mask & (UINT64_C(1) << slot)) != 0u &&
            publication->claimed_node_ids[slot] == 0u) {
            return PROTO_ERR_MALFORMED;
        }
    }
    committed_count =
        (size_t)__builtin_popcountll(publication->committed_mask);
    if (committed_count != roster_node_count) {
        return PROTO_ERR_MALFORMED;
    }

    for (size_t slot = 0u; slot < GATEWAY_MEMBERSHIP_MAX_NODES; slot++) {
        bool committed =
            slot < publication->claimed_slot_span &&
            (publication->committed_mask & (UINT64_C(1) << slot)) != 0u;
        uint64_t expected =
            committed ? publication->claimed_node_ids[slot] : 0u;

        if (roster_node_ids[slot] != expected) {
            return PROTO_ERR_MALFORMED;
        }
    }

    return PROTO_OK;
}

static int validate_snapshot(const struct gateway_membership_snapshot *snapshot,
                             struct gateway_membership_roster *restored)
{
    struct gateway_membership_publication publication;
    int publication_ret;
    int ret;

    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (snapshot->version != GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION ||
        snapshot->magic != GATEWAY_MEMBERSHIP_SNAPSHOT_MAGIC) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (snapshot->valid > 1u ||
        snapshot->assignment_proof_valid > 1u ||
        snapshot->checksum !=
            gateway_membership_snapshot_checksum(snapshot)) {
        return PROTO_ERR_MALFORMED;
    }
    if (snapshot->valid == 0u || snapshot->membership_epoch == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if ((snapshot->assignment_proof_valid != 0u &&
         (snapshot->assignment_epoch == 0u ||
          snapshot->assignment_table_seq == 0u ||
          gateway_membership_commitment_is_zero(
              &snapshot->assignment_table_commitment))) ||
        (snapshot->assignment_proof_valid == 0u &&
         (snapshot->assignment_epoch != 0u ||
          snapshot->assignment_table_seq != 0u ||
          !gateway_membership_commitment_is_zero(
              &snapshot->assignment_table_commitment)))) {
        return PROTO_ERR_MALFORMED;
    }

    ret = validate_sparse_node_ids(snapshot->node_ids,
                                   snapshot->node_count,
                                   snapshot->slot_span);
    if (ret != PROTO_OK) {
        return ret;
    }
    publication_ret =
        snapshot_restore_publication(snapshot, &publication);
    if (publication_ret != PROTO_OK &&
        publication_ret != PROTO_ERR_NOT_FOUND) {
        return publication_ret;
    }
    ret = validate_pending_publication(
        snapshot->node_ids,
        snapshot->node_count,
        snapshot->slot_span,
        snapshot->assignment_proof_valid != 0u,
        &publication);
    if (ret != PROTO_OK) {
        return ret;
    }

    if (restored != NULL) {
        memset(restored, 0, sizeof(*restored));
        memcpy(restored->node_ids,
               snapshot->node_ids,
               sizeof(restored->node_ids));
        restored->membership_epoch = snapshot->membership_epoch;
        restored->node_count = snapshot->node_count;
        restored->slot_span = snapshot->slot_span;
        restored->valid = true;
    }
    return PROTO_OK;
}

void gateway_membership_clear(struct gateway_membership_roster *roster)
{
    if (roster == NULL) {
        return;
    }

    memset(roster, 0, sizeof(*roster));
}

int gateway_membership_set_roster_preserve_order(
    struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    const uint64_t *node_ids,
    size_t node_count)
{
    struct gateway_membership_roster tmp;
    int ret;

    if (roster == NULL) {
        return PROTO_ERR_ARG;
    }
    if (membership_epoch == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    ret = validate_dense_node_ids(node_ids, node_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.node_ids, node_ids, node_count * sizeof(tmp.node_ids[0]));
    tmp.membership_epoch = membership_epoch;
    tmp.node_count = (uint8_t)node_count;
    tmp.slot_span = (uint8_t)node_count;
    tmp.valid = true;

    *roster = tmp;
    return PROTO_OK;
}

int gateway_membership_set_roster_explicit_slots(
    struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    const uint64_t *node_ids,
    const uint8_t *slots,
    size_t node_count)
{
    struct gateway_membership_roster tmp;

    if (roster == NULL || node_ids == NULL || slots == NULL ||
        node_count == 0u) {
        return PROTO_ERR_ARG;
    }
    if (membership_epoch == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    if (node_count > GATEWAY_MEMBERSHIP_MAX_NODES) {
        return PROTO_ERR_NO_SPACE;
    }

    memset(&tmp, 0, sizeof(tmp));
    for (size_t i = 0u; i < node_count; i++) {
        uint8_t slot = slots[i];

        if (node_ids[i] == 0u ||
            slot >= GATEWAY_MEMBERSHIP_MAX_NODES ||
            tmp.node_ids[slot] != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        for (size_t previous = 0u; previous < i; previous++) {
            if (node_ids[previous] == node_ids[i]) {
                return PROTO_ERR_MALFORMED;
            }
        }
        tmp.node_ids[slot] = node_ids[i];
        if ((uint8_t)(slot + 1u) > tmp.slot_span) {
            tmp.slot_span = (uint8_t)(slot + 1u);
        }
    }
    tmp.membership_epoch = membership_epoch;
    tmp.node_count = (uint8_t)node_count;
    tmp.valid = true;

    *roster = tmp;
    return PROTO_OK;
}

int gateway_membership_lookup_node_index(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t node_id,
    size_t *index)
{
    int ret;

    if (index == NULL || node_id == 0u || membership_epoch == 0u) {
        return PROTO_ERR_ARG;
    }

    ret = validate_roster(roster);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (roster->membership_epoch != membership_epoch) {
        return PROTO_ERR_STALE;
    }

    for (size_t slot = 0u; slot < roster->slot_span; slot++) {
        if (roster->node_ids[slot] == node_id) {
            *index = slot;
            return PROTO_OK;
        }
    }

    return PROTO_ERR_NOT_FOUND;
}

bool gateway_membership_contains_node_id(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t node_id)
{
    size_t index = 0u;

    return gateway_membership_lookup_node_index(roster,
                                                membership_epoch,
                                                node_id,
                                                &index) == PROTO_OK;
}

int gateway_membership_export_node_ids_preserve_order(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t *out_node_ids,
    size_t out_cap,
    size_t *out_count)
{
    size_t cursor = 0u;
    int ret;

    if (out_count == NULL || (out_node_ids == NULL && out_cap != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = validate_roster(roster);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (membership_epoch == 0u) {
        return PROTO_ERR_ARG;
    }
    if (roster->membership_epoch != membership_epoch) {
        return PROTO_ERR_STALE;
    }

    *out_count = roster->node_count;
    if (out_cap < roster->node_count) {
        return PROTO_ERR_NO_SPACE;
    }
    for (size_t slot = 0u; slot < roster->slot_span; slot++) {
        if (roster->node_ids[slot] != 0u) {
            out_node_ids[cursor++] = roster->node_ids[slot];
        }
    }

    return PROTO_OK;
}

int gateway_membership_export_node_ids_with_slots(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t *out_node_ids,
    uint8_t *out_slots,
    size_t out_cap,
    size_t *out_count)
{
    size_t cursor = 0u;
    int ret;

    if (out_count == NULL ||
        ((out_node_ids == NULL || out_slots == NULL) && out_cap != 0u)) {
        return PROTO_ERR_ARG;
    }

    ret = validate_roster(roster);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (membership_epoch == 0u) {
        return PROTO_ERR_ARG;
    }
    if (roster->membership_epoch != membership_epoch) {
        return PROTO_ERR_STALE;
    }

    *out_count = roster->node_count;
    if (out_cap < roster->node_count) {
        return PROTO_ERR_NO_SPACE;
    }
    for (size_t slot = 0u; slot < roster->slot_span; slot++) {
        if (roster->node_ids[slot] != 0u) {
            out_node_ids[cursor] = roster->node_ids[slot];
            out_slots[cursor] = (uint8_t)slot;
            cursor++;
        }
    }

    return PROTO_OK;
}

int gateway_membership_export_snapshot(
    const struct gateway_membership_roster *roster,
    struct gateway_membership_snapshot *snapshot)
{
    int ret;

    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_roster(roster);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    memcpy(snapshot->node_ids,
           roster->node_ids,
           sizeof(snapshot->node_ids));
    snapshot->membership_epoch = roster->membership_epoch;
    snapshot->node_count = roster->node_count;
    snapshot->slot_span = roster->slot_span;
    snapshot->valid = 1u;
    gateway_membership_snapshot_finalize(snapshot);
    return PROTO_OK;
}

int gateway_membership_export_assignment_snapshot(
    const struct gateway_membership_roster *roster,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const struct gateway_membership_publication *publication,
    struct gateway_membership_snapshot *snapshot)
{
    int ret;

    if (assignment_epoch == 0u || table_seq == 0u ||
        table_commitment == NULL ||
        gateway_membership_commitment_is_zero(table_commitment)) {
        return PROTO_ERR_ARG;
    }
    ret = gateway_membership_export_snapshot(roster, snapshot);
    if (ret != PROTO_OK) {
        return ret;
    }
    snapshot->assignment_epoch = assignment_epoch;
    snapshot->assignment_table_seq = table_seq;
    snapshot->assignment_table_commitment = *table_commitment;
    snapshot->assignment_proof_valid = 1u;
    if (publication != NULL) {
        ret = validate_pending_publication(roster->node_ids,
                                           roster->node_count,
                                           roster->slot_span,
                                           true,
                                           publication);
        if (ret != PROTO_OK) {
            memset(snapshot, 0, sizeof(*snapshot));
            return ret;
        }
        if (publication->publish_pending) {
            snapshot_store_publication(snapshot, publication);
        }
    }
    gateway_membership_snapshot_finalize(snapshot);
    return PROTO_OK;
}

int gateway_membership_restore_snapshot(
    struct gateway_membership_roster *roster,
    const struct gateway_membership_snapshot *snapshot)
{
    int ret;

    if (roster == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_snapshot(snapshot, roster);
    if (ret != PROTO_OK) {
        return ret;
    }

    return PROTO_OK;
}

int gateway_membership_snapshot_get_publication(
    const struct gateway_membership_snapshot *snapshot,
    struct gateway_membership_publication *publication)
{
    int ret;

    if (snapshot == NULL || publication == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = validate_snapshot(snapshot, NULL);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = snapshot_restore_publication(snapshot, publication);
    if (ret == PROTO_ERR_NOT_FOUND) {
        memset(publication, 0, sizeof(*publication));
        return PROTO_ERR_NOT_FOUND;
    }
    return ret;
}

bool gateway_membership_snapshot_proves_assignment(
    const struct gateway_membership_snapshot *snapshot,
    uint32_t assignment_epoch,
    uint32_t table_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    uint64_t node_id)
{
    if (node_id == 0u || table_commitment == NULL ||
        validate_snapshot(snapshot, NULL) != PROTO_OK ||
        snapshot->assignment_proof_valid == 0u ||
        snapshot->assignment_epoch != assignment_epoch ||
        snapshot->assignment_table_seq != table_seq ||
        !discovery_assignment_table_commitment_equal(
            &snapshot->assignment_table_commitment,
            table_commitment)) {
        return false;
    }
    for (size_t slot = 0u; slot < snapshot->slot_span; slot++) {
        if (snapshot->node_ids[slot] == node_id) {
            return true;
        }
    }
    return false;
}
