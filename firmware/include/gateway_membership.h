#ifndef GATEWAY_MEMBERSHIP_H
#define GATEWAY_MEMBERSHIP_H

#include "gateway_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_MEMBERSHIP_MAX_NODES GATEWAY_COMMAND_EXPECTED_NODE_ID_CAP
#define GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION 1u

struct gateway_membership_roster {
    uint16_t membership_epoch;
    uint16_t node_count;
    bool valid;
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
};

struct gateway_membership_snapshot {
    uint8_t version;
    bool valid;
    uint16_t membership_epoch;
    uint16_t node_count;
    uint64_t node_ids[GATEWAY_MEMBERSHIP_MAX_NODES];
};

void gateway_membership_clear(struct gateway_membership_roster *roster);
int gateway_membership_set_roster_preserve_order(struct gateway_membership_roster *roster,
                                                 uint16_t membership_epoch,
                                                 const uint64_t *node_ids,
                                                 size_t node_count);
bool gateway_membership_contains_node_id(const struct gateway_membership_roster *roster,
                                         uint16_t membership_epoch,
                                         uint64_t node_id);
int gateway_membership_lookup_node_index(const struct gateway_membership_roster *roster,
                                         uint16_t membership_epoch,
                                         uint64_t node_id,
                                         size_t *index);
int gateway_membership_export_node_ids_preserve_order(
    const struct gateway_membership_roster *roster,
    uint16_t membership_epoch,
    uint64_t *out_node_ids,
    size_t out_cap,
    size_t *out_count);
int gateway_membership_export_snapshot(
    const struct gateway_membership_roster *roster,
    struct gateway_membership_snapshot *snapshot);
int gateway_membership_restore_snapshot(
    struct gateway_membership_roster *roster,
    const struct gateway_membership_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
