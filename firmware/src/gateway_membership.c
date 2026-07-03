#include "gateway_membership.h"

#include <string.h>

static int validate_node_ids(const uint64_t *node_ids, size_t node_count)
{
    if (node_ids == NULL || node_count == 0u) {
        return PROTO_ERR_ARG;
    }
    if (node_count > GATEWAY_MEMBERSHIP_MAX_NODES || node_count > UINT16_MAX) {
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

static int validate_roster(const struct gateway_membership_roster *roster)
{
    if (roster == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!roster->valid ||
        roster->membership_epoch == 0u ||
        roster->node_count == 0u ||
        roster->node_count > GATEWAY_MEMBERSHIP_MAX_NODES) {
        return PROTO_ERR_MALFORMED;
    }

    return validate_node_ids(roster->node_ids, roster->node_count);
}

static int validate_snapshot(const struct gateway_membership_snapshot *snapshot,
                             struct gateway_membership_roster *restored)
{
    struct gateway_membership_roster tmp;
    int ret;

    if (snapshot == NULL || restored == NULL) {
        return PROTO_ERR_ARG;
    }
    if (snapshot->version != GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (!snapshot->valid ||
        snapshot->membership_epoch == 0u ||
        snapshot->node_count == 0u ||
        snapshot->node_count > GATEWAY_MEMBERSHIP_MAX_NODES) {
        return PROTO_ERR_MALFORMED;
    }

    ret = validate_node_ids(snapshot->node_ids, snapshot->node_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.valid = true;
    tmp.membership_epoch = snapshot->membership_epoch;
    tmp.node_count = snapshot->node_count;
    memcpy(tmp.node_ids, snapshot->node_ids, tmp.node_count * sizeof(tmp.node_ids[0]));

    *restored = tmp;
    return PROTO_OK;
}

void gateway_membership_clear(struct gateway_membership_roster *roster)
{
    if (roster == NULL) {
        return;
    }

    memset(roster, 0, sizeof(*roster));
}

int gateway_membership_set_roster_preserve_order(struct gateway_membership_roster *roster,
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

    ret = validate_node_ids(node_ids, node_count);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.valid = true;
    tmp.membership_epoch = membership_epoch;
    tmp.node_count = (uint16_t)node_count;
    memcpy(tmp.node_ids, node_ids, node_count * sizeof(tmp.node_ids[0]));

    *roster = tmp;
    return PROTO_OK;
}

int gateway_membership_lookup_node_index(const struct gateway_membership_roster *roster,
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

    for (size_t i = 0u; i < roster->node_count; i++) {
        if (roster->node_ids[i] == node_id) {
            *index = i;
            return PROTO_OK;
        }
    }

    return PROTO_ERR_NOT_FOUND;
}

bool gateway_membership_contains_node_id(const struct gateway_membership_roster *roster,
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
    if (roster->node_count != 0u) {
        memcpy(out_node_ids, roster->node_ids, roster->node_count * sizeof(roster->node_ids[0]));
    }

    return PROTO_OK;
}

int gateway_membership_export_snapshot(
    const struct gateway_membership_roster *roster,
    struct gateway_membership_snapshot *snapshot)
{
    struct gateway_membership_snapshot tmp;
    int ret;

    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_roster(roster);
    if (ret != PROTO_OK) {
        return ret;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.version = GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION;
    tmp.valid = true;
    tmp.membership_epoch = roster->membership_epoch;
    tmp.node_count = roster->node_count;
    memcpy(tmp.node_ids, roster->node_ids, roster->node_count * sizeof(roster->node_ids[0]));

    *snapshot = tmp;
    return PROTO_OK;
}

int gateway_membership_restore_snapshot(
    struct gateway_membership_roster *roster,
    const struct gateway_membership_snapshot *snapshot)
{
    struct gateway_membership_roster restored;
    int ret;

    if (roster == NULL || snapshot == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = validate_snapshot(snapshot, &restored);
    if (ret != PROTO_OK) {
        return ret;
    }

    *roster = restored;
    return PROTO_OK;
}
