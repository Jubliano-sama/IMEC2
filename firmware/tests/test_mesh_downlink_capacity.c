#include "mesh_relay.h"

#include "mesh.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define RELAY_ID UINT64_C(0x5555666677778888)
#define GATEWAY_ID UINT64_C(0x9999888877776666)
#define TARGET_BASE UINT64_C(0xb700000000001000)
#define ROUTE_EPOCH 93u

static bool has_action(const struct mesh_relay_result *result,
                       enum mesh_relay_action action)
{
    return (result->actions & action) != 0u;
}

static struct route_candidate direct_gateway_route(void)
{
    return (struct route_candidate) {
        .next_hop_id = GATEWAY_ID,
        .gateway_id = GATEWAY_ID,
        .route_epoch = ROUTE_EPOCH,
        .last_seen_ms = 900u,
        .last_success_ms = 900u,
        .hop_count = 0u,
        .link_quality = 95u,
        .valid = true,
    };
}

static void test_shared_relay_routes_controls_to_fifty_descendants(void)
{
    static struct mesh_anchor_downlink_store overflow_store;
    struct mesh_relay relay;
    struct route_candidate gateway_route = direct_gateway_route();
    uint8_t command_payload[8];
    size_t command_payload_len = 0u;

    mesh_relay_init(&relay,
                    MESH_RELAY_ROLE_ANCHOR,
                    RELAY_ID,
                    GATEWAY_ID,
                    ROUTE_EPOCH);
    assert(mesh_relay_downlink_capacity(&relay) ==
           MESH_RELAY_DOWNLINK_ROUTES);
    assert(mesh_relay_attach_anchor_downlink_store(
               &relay, &overflow_store) == PROTO_OK);
    assert(mesh_relay_downlink_capacity(&relay) ==
           MESH_RELAY_ANCHOR_DOWNLINK_ROUTES);
    assert(route_upsert_candidate(&relay.upstream, &gateway_route) == PROTO_OK);
    assert(mesh_append_command_id(command_payload,
                                  sizeof(command_payload),
                                  &command_payload_len,
                                  CMD_GET_STATUS) == PROTO_OK);

    for (size_t i = 0u; i < MESH_RELAY_ANCHOR_DOWNLINK_ROUTES; i++) {
        struct mesh_relay origin;
        struct mesh_outbound route_request;
        struct mesh_relay_result result;
        uint64_t target_id = TARGET_BASE + i;

        mesh_relay_init(&origin,
                        MESH_RELAY_ROLE_ANCHOR,
                        target_id,
                        GATEWAY_ID,
                        ROUTE_EPOCH);
        assert(mesh_relay_build_route_request(&origin,
                                              GATEWAY_ID,
                                              &route_request,
                                              (uint32_t)(1000u + i)) ==
               PROTO_OK);
        assert(mesh_relay_handle_rx(&relay,
                                    &route_request.packet,
                                    route_request.payload,
                                    route_request.payload_len,
                                    target_id,
                                    90u,
                                    (uint32_t)(1100u + i),
                                    &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_SEND_ROUTE_REPLY));
        assert(mesh_relay_find_downlink(&relay, target_id) != NULL);
        assert(mesh_relay_find_downlink(&relay, target_id)->next_hop_id ==
               target_id);
    }

    for (size_t i = 0u; i < MESH_RELAY_ANCHOR_DOWNLINK_ROUTES; i++) {
        struct proto_packet command;
        struct mesh_relay_result result;
        uint64_t target_id = TARGET_BASE + i;

        assert(mesh_init_command(&command,
                                 GATEWAY_ID,
                                 target_id,
                                 UINT32_C(0xb7000000) + (uint32_t)i,
                                 (uint16_t)(i + 1u),
                                 (uint8_t)command_payload_len) == PROTO_OK);
        assert(mesh_relay_handle_rx(&relay,
                                    &command,
                                    command_payload,
                                    command_payload_len,
                                    GATEWAY_ID,
                                    95u,
                                    (uint32_t)(2000u + i),
                                    &result) == PROTO_OK);
        assert(has_action(&result, MESH_RELAY_ACTION_FORWARD));
        assert(!has_action(&result, MESH_RELAY_ACTION_DROP));
        if (result.forward.next_hop_id != target_id) {
            fprintf(stderr,
                    "target[%zu]=0x%llx forwarded via 0x%llx\n",
                    i,
                    (unsigned long long)target_id,
                    (unsigned long long)result.forward.next_hop_id);
            assert(result.forward.next_hop_id == target_id);
        }
        assert(result.forward.packet.dst_id == target_id);
    }
}

int main(void)
{
    test_shared_relay_routes_controls_to_fifty_descendants();
    return 0;
}
