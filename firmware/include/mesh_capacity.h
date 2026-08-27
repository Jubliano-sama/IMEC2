#ifndef MESH_CAPACITY_H
#define MESH_CAPACITY_H

/*
 * Connected-routing product limits shared by the Zephyr application and the
 * native hardware model. Keep storage sizing separate from these policy
 * limits so simulations can exercise capacity failures explicitly.
 */
#define MESH_CONNECTED_MAX_ANCHORS 50u
/*
 * The product requirement includes a fleet of at least eighteen clickers.
 * Gateway source-custody structures must cover that complete fleet at the
 * same time as the maximum anchor installation; anchor-only cardinalities
 * are insufficient for gateway-bound reports.
 */
#define MESH_CONNECTED_REQUIRED_CLICKERS 18u
#define MESH_CONNECTED_REQUIRED_SOURCES \
    (MESH_CONNECTED_MAX_ANCHORS + MESH_CONNECTED_REQUIRED_CLICKERS)

/* One relay may serve its own click plus three child anchors. */
#define MESH_CONNECTED_SHARED_RELAY_CLICK_ANCHORS 4u

/*
 * The Zephyr report queue owns the nominal entries.  While its immutable head
 * is owned by a sender, one additional report can be held in the explicit
 * custody-recovery reserve without mutating that head.
 */
#define MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH 16u
#define MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY 1u
#define MESH_CONNECTED_ANCHOR_REPORT_STORAGE_CAPACITY \
    (MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH + \
     MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY)

/* Compatibility name: this is the nominal queue depth, not total storage. */
#define MESH_CONNECTED_ANCHOR_REPORT_QUEUE_CAPACITY \
    MESH_CONNECTED_ANCHOR_REPORT_QUEUE_DEPTH

#endif
