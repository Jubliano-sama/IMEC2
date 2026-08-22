/*
 * Flood-parent learning contract for mesh_relay_note_flood_parent_candidate:
 * a locally accepted gateway-originated discovery-assignment command flood
 * proves a live upstream edge, so the anchor stores the physical ingress hop
 * as an upstream candidate toward the gateway before answering the flood.
 * Cost-based selection keeps any existing better parent, stale epochs never
 * clobber newer route bookkeeping, and same-parent refreshes preserve
 * failure and hold-down state.
 */
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "route.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GATEWAY UINT64_C(0xa001000000000001)
#define ANCHOR UINT64_C(0xa002000000010000)
#define PARENT UINT64_C(0xa002000000010100)
#define OTHER_PARENT UINT64_C(0xa002000000010200)
#define ROUTE_EPOCH 7u
#define FLOOD_ORIGIN_TTL FLOOD_EPOCH_GLOBAL_TTL

static unsigned int failures;

#define CHECK(expression)                                         \
	do {                                                      \
		if (!(expression)) {                              \
			fprintf(stderr, "FAIL line=%d: %s\n",     \
				__LINE__, #expression);           \
			failures++;                               \
		}                                                 \
	} while (0)

static struct proto_packet flood_command(uint8_t ttl)
{
	return (struct proto_packet) {
		.src_id = GATEWAY,
		.dst_id = MESH_BROADCAST_ID,
		.msg_type = MSG_COMMAND,
		.seq = 41u,
		.ttl = ttl,
	};
}

static uint8_t upstream_candidate_count(const struct mesh_relay *relay)
{
	uint8_t count = 0u;

	for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
		if (relay->upstream.candidates[i].valid) {
			count++;
		}
	}
	return count;
}

static void test_empty_upstream_learns_claim_parent(void)
{
	struct mesh_relay anchor;
	struct proto_packet flood = flood_command(FLOOD_ORIGIN_TTL - 1u);
	const struct route_candidate *selected;

	mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR, GATEWAY,
			ROUTE_EPOCH);
	CHECK(route_selected(&anchor.upstream) == NULL);
	CHECK(!anchor.route_discovery.active);

	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1000u) == PROTO_OK);
	selected = route_selected(&anchor.upstream);
	CHECK(selected != NULL);
	CHECK(selected->next_hop_id == PARENT);
	CHECK(selected->gateway_id == GATEWAY);
	CHECK(selected->route_epoch == ROUTE_EPOCH);
	CHECK(selected->hop_count == 1u);
	CHECK(selected->last_seen_ms == 1000u);
	CHECK(upstream_candidate_count(&anchor) == 1u);
	/* The proven parent answers the flood; no discovery wake train. */
	CHECK(!anchor.route_discovery.active);
}

static void test_existing_better_route_not_downgraded(void)
{
	struct mesh_relay anchor;
	struct proto_packet flood = flood_command(FLOOD_ORIGIN_TTL - 2u);
	struct route_candidate direct = {
		.next_hop_id = GATEWAY,
		.gateway_id = GATEWAY,
		.route_epoch = ROUTE_EPOCH,
		.last_seen_ms = 900u,
		.hop_count = 1u,
		.link_quality = 50u,
		.relay_capacity_state = RELAY_CAP_UNKNOWN,
		.valid = true,
	};
	const struct route_candidate *selected;

	mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR, GATEWAY,
			ROUTE_EPOCH);
	CHECK(route_upsert_candidate(&anchor.upstream, &direct) == PROTO_OK);
	route_set_channel9_timing_valid(&anchor.upstream, GATEWAY, GATEWAY,
					true, 950u);
	selected = route_selected(&anchor.upstream);
	CHECK(selected != NULL && selected->next_hop_id == GATEWAY);

	/* A two-hop flood parent must never replace the cheaper direct edge,
	 * and the selected parent keeps its Channel-9 timing validity. */
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1000u) == PROTO_OK);
	selected = route_selected(&anchor.upstream);
	CHECK(selected->next_hop_id == GATEWAY);
	CHECK(selected->channel9_timing_valid);
	CHECK(upstream_candidate_count(&anchor) == 2u);

	/* A same-depth refresh of the selected parent stays selected. */
	flood.ttl = FLOOD_ORIGIN_TTL;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, GATEWAY, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1010u) == PROTO_OK);
	selected = route_selected(&anchor.upstream);
	CHECK(selected != NULL && selected->next_hop_id == GATEWAY);
	CHECK(selected->hop_count == 0u);
	CHECK(selected->channel9_timing_valid);
}

static void test_stale_epoch_does_not_clobber_newer_bookkeeping(void)
{
	struct mesh_relay anchor;
	struct proto_packet flood = flood_command(FLOOD_ORIGIN_TTL - 1u);
	struct route_candidate newer = {
		.next_hop_id = OTHER_PARENT,
		.gateway_id = GATEWAY,
		.route_epoch = ROUTE_EPOCH + 2u,
		.last_seen_ms = 900u,
		.hop_count = 1u,
		.link_quality = 90u,
		.relay_capacity_state = RELAY_CAP_UNKNOWN,
		.valid = true,
	};
	const struct route_candidate *selected;
	struct route_table unchanged;

	mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR, GATEWAY,
			ROUTE_EPOCH);
	CHECK(route_upsert_candidate(&anchor.upstream, &newer) == PROTO_OK);
	selected = route_selected(&anchor.upstream);
	CHECK(selected != NULL && selected->next_hop_id == OTHER_PARENT);
	unchanged = anchor.upstream;

	/* An older enumeration's flood matches route_upsert_candidate epoch
	 * semantics: rejected stale, zero mutation of the newer table. */
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1000u) == PROTO_ERR_STALE);
	CHECK(memcmp(&anchor.upstream, &unchanged,
		     sizeof(unchanged)) == 0);
	CHECK(upstream_candidate_count(&anchor) == 1u);

	/* A strictly newer enumeration legitimately advances the epoch and
	 * invalidates the previous generation's candidates. */
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH + 3u, 1010u) == PROTO_OK);
	CHECK(anchor.upstream.current_epoch == ROUTE_EPOCH + 3u);
	selected = route_selected(&anchor.upstream);
	CHECK(selected != NULL && selected->next_hop_id == PARENT);
	CHECK(upstream_candidate_count(&anchor) == 1u);
}

static void test_same_parent_refresh_preserves_failure_state(void)
{
	struct mesh_relay anchor;
	struct proto_packet flood = flood_command(FLOOD_ORIGIN_TTL - 1u);
	struct route_candidate *stored;

	mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR, GATEWAY,
			ROUTE_EPOCH);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1000u) == PROTO_OK);

	for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
		stored = &anchor.upstream.candidates[i];
		if (stored->valid && stored->next_hop_id == PARENT) {
			break;
		}
		stored = NULL;
	}
	CHECK(stored != NULL);
	stored->failure_count = 2u;
	stored->hold_down_until_ms = 5000u;
	stored->hold_down_valid = true;

	/* Re-learning the same parent must not reset its failure counters or
	 * clear a still-active hold-down. */
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 100u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1010u) == PROTO_OK);
	for (uint8_t i = 0u; i < ROUTE_MAX_CANDIDATES; i++) {
		stored = &anchor.upstream.candidates[i];
		if (stored->valid && stored->next_hop_id == PARENT) {
			break;
		}
		stored = NULL;
	}
	CHECK(stored != NULL);
	CHECK(stored->failure_count == 2u);
	CHECK(stored->hold_down_valid);
	CHECK(stored->hold_down_until_ms == 5000u);
	CHECK(stored->last_seen_ms == 1010u);
}

static void test_malformed_flood_parents_fail_closed(void)
{
	struct mesh_relay anchor;
	struct mesh_relay gateway;
	struct proto_packet flood = flood_command(FLOOD_ORIGIN_TTL - 1u);
	struct route_table unchanged;

	mesh_relay_init(&anchor, MESH_RELAY_ROLE_ANCHOR, ANCHOR, GATEWAY,
			ROUTE_EPOCH);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1000u) == PROTO_OK);
	unchanged = anchor.upstream;

	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, NULL, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1001u) == PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, 0u, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1002u) == PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, MESH_BROADCAST_ID, 90u,
		      FLOOD_ORIGIN_TTL, ROUTE_EPOCH, 1003u) ==
	      PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, ANCHOR, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1004u) == PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 101u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1005u) == PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, 0u,
		      ROUTE_EPOCH, 1006u) == PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u,
		      FLOOD_ORIGIN_TTL + 1u, ROUTE_EPOCH,
		      1007u) == PROTO_ERR_ARG);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      0u, 1008u) == PROTO_ERR_ARG);

	flood.ttl = 0u;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1009u) == PROTO_ERR_ARG);
	flood.ttl = FLOOD_ORIGIN_TTL + 1u;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1010u) == PROTO_ERR_ARG);
	flood.ttl = FLOOD_ORIGIN_TTL - 1u;

	flood.src_id = OTHER_PARENT;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1011u) == PROTO_ERR_ARG);
	flood.src_id = GATEWAY;

	flood.dst_id = OTHER_PARENT;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1012u) == PROTO_ERR_ARG);
	flood.dst_id = ANCHOR;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1013u) == PROTO_OK);
	flood.dst_id = MESH_BROADCAST_ID;
	unchanged = anchor.upstream;

	flood.msg_type = MSG_COMMAND_RESULT;
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &anchor, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1014u) == PROTO_ERR_ARG);
	flood.msg_type = MSG_COMMAND;

	CHECK(memcmp(&anchor.upstream, &unchanged,
		     sizeof(unchanged)) == 0);

	mesh_relay_init(&gateway, MESH_RELAY_ROLE_GATEWAY, GATEWAY, GATEWAY,
			ROUTE_EPOCH);
	CHECK(mesh_relay_note_flood_parent_candidate(
		      &gateway, &flood, PARENT, 90u, FLOOD_ORIGIN_TTL,
		      ROUTE_EPOCH, 1015u) == PROTO_ERR_ARG);
}

int main(void)
{
	test_empty_upstream_learns_claim_parent();
	test_existing_better_route_not_downgraded();
	test_stale_epoch_does_not_clobber_newer_bookkeeping();
	test_same_parent_refresh_preserves_failure_state();
	test_malformed_flood_parents_fail_closed();

	if (failures != 0u) {
		fprintf(stderr, "flood_parent_learning: %u failure(s)\n",
			failures);
		return EXIT_FAILURE;
	}
	printf("flood_parent_learning: ok\n");
	return EXIT_SUCCESS;
}
