#include "mesh_relay.h"

#include "mesh.h"
#include "route.h"

#include <stdint.h>
#include <stdio.h>

#define ORIGIN_ID UINT64_C(0x3333333333333301)
#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define ROUTE_EPOCH UINT32_C(17)
#define ROUTE_LISTEN_START_MS 9310u
#define ROUTE_LISTEN_TIMEOUT_MS 13410u
#define SECOND_DIRECT_PROBE_MS 13500u

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "line=%d assertion=%s\n", __LINE__, #expression); \
        return 1; \
    } \
} while (0)

static int test_route_listen_timeout_then_second_direct_probe_recovers(void)
{
    struct mesh_relay relay;
    struct mesh_outbound route_request;
    const struct route_candidate *selected;
    uint64_t next_hop_id = 0u;

    mesh_relay_init(&relay, MESH_RELAY_ROLE_ANCHOR, ORIGIN_ID,
                    GATEWAY_ID, ROUTE_EPOCH);

    CHECK(mesh_relay_prepare_route_request(&relay,
                                           GATEWAY_ID,
                                           ROUTE_LISTEN_START_MS,
                                           UINT32_C(0x13579bdf),
                                           &route_request) == PROTO_OK);
    CHECK(relay.route_discovery.active);
    CHECK(route_request.packet.msg_type == MSG_ROUTE_REQ);

    /* No route reply arrives in the bounded channel-5 listen window. */
    CHECK(ROUTE_LISTEN_TIMEOUT_MS > ROUTE_LISTEN_START_MS);
    CHECK(relay.route_discovery.active);
    CHECK(mesh_relay_select_next_hop(&relay, GATEWAY_ID, &next_hop_id) ==
          PROTO_ERR_NOT_FOUND);

    CHECK(SECOND_DIRECT_PROBE_MS > ROUTE_LISTEN_TIMEOUT_MS);
    CHECK(mesh_relay_note_direct_gateway_route(&relay,
                                               SECOND_DIRECT_PROBE_MS) == PROTO_OK);
    CHECK(!relay.route_discovery.active);
    CHECK(mesh_relay_select_next_hop(&relay, GATEWAY_ID, &next_hop_id) == PROTO_OK);
    CHECK(next_hop_id == GATEWAY_ID);

    selected = route_selected(&relay.upstream);
    CHECK(selected != NULL);
    CHECK(selected->next_hop_id == GATEWAY_ID);
    CHECK(selected->failure_count == 0u);
    CHECK(selected->last_success_ms == SECOND_DIRECT_PROBE_MS);
    return 0;
}

int main(void)
{
    if (test_route_listen_timeout_then_second_direct_probe_recovers() != 0) {
        return 1;
    }

    printf("mesh direct probe recovery scenario passed\n");
    return 0;
}
