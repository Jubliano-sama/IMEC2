#include "uwb_rf_scope.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static struct uwb_rf_scope build_scope(enum uwb_rf_scope_role role,
                                       uint8_t forced_relay_hops)
{
    struct uwb_rf_scope scope = {0};

    assert(uwb_rf_scope_build(role, forced_relay_hops, &scope) == 0);
    return scope;
}

static void assert_visibility(const struct uwb_rf_scope *left,
                              const struct uwb_rf_scope *right,
                              bool expected)
{
    assert(uwb_rf_scope_visible(left, right) == expected);
    assert(uwb_rf_scope_visible(right, left) == expected);
}

static void test_roles_map_to_physical_layers(void)
{
    const struct uwb_rf_scope gateway =
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u);
    const struct uwb_rf_scope anchor =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u);
    const struct uwb_rf_scope forced_one =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u);
    const struct uwb_rf_scope forced_two =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u);
    const struct uwb_rf_scope clicker =
        build_scope(UWB_RF_SCOPE_ROLE_CLICKER, 0u);
    struct uwb_rf_scope scope = {0};

    assert(gateway.layer == 0u);
    assert(!gateway.clicker);
    assert(anchor.layer == 1u);
    assert(!anchor.clicker);
    assert(forced_one.layer == 2u);
    assert(!forced_one.clicker);
    assert(forced_two.layer == 3u);
    assert(!forced_two.clicker);
    assert(clicker.clicker);

    assert(uwb_rf_scope_build(UWB_RF_SCOPE_ROLE_ANCHOR,
                              UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS,
                              &scope) == 0);
    assert(scope.layer == UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS + 1u);
    assert(uwb_rf_scope_build(
               UWB_RF_SCOPE_ROLE_ANCHOR,
               (uint8_t)(UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS + 1u),
               &scope) != 0);
}

static void test_visibility_is_same_or_adjacent_layer_only(void)
{
    const struct uwb_rf_scope layers[] = {
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 3u),
    };
    size_t local;
    size_t remote;

    for (local = 0u; local < sizeof(layers) / sizeof(layers[0]); local++) {
        for (remote = 0u; remote < sizeof(layers) / sizeof(layers[0]);
             remote++) {
            const size_t gap = local > remote ? local - remote : remote - local;

            assert(uwb_rf_scope_visible(&layers[local], &layers[remote]) ==
                   (gap <= 1u));
        }
    }
}

static void test_gateway_and_anchor_packets_cannot_skip_layers(void)
{
    const struct uwb_rf_scope gateway =
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u);
    const struct uwb_rf_scope direct_anchor =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u);
    const struct uwb_rf_scope forced_one =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u);
    const struct uwb_rf_scope forced_two =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u);

    /* A direct gateway wake or payload reaches only the direct RF layer. */
    assert_visibility(&gateway, &direct_anchor, true);
    assert_visibility(&gateway, &forced_one, false);
    assert_visibility(&gateway, &forced_two, false);

    /* A normal anchor cannot bypass forced-one on the way to forced-two. */
    assert_visibility(&direct_anchor, &forced_one, true);
    assert_visibility(&direct_anchor, &forced_two, false);
    assert_visibility(&forced_one, &forced_two, true);
}

static void test_gateway_origin_uses_each_physical_transmitter_scope(void)
{
    const struct uwb_rf_scope gateway_tx =
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u);
    const struct uwb_rf_scope direct_anchor_tx =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u);
    const struct uwb_rf_scope forced_one_tx =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u);
    const struct uwb_rf_scope forced_two_rx =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u);

    /* The logical packet can remain gateway-originated, but its RF scope is
     * replaced at each transmission. Forced-two therefore rejects both the
     * gateway and direct-anchor copies and accepts the forced-one copy. */
    assert(!uwb_rf_scope_visible(&forced_two_rx, &gateway_tx));
    assert(!uwb_rf_scope_visible(&forced_two_rx, &direct_anchor_tx));
    assert(uwb_rf_scope_visible(&forced_two_rx, &forced_one_tx));
}

static void test_clicker_is_visible_across_every_anchor_layer(void)
{
    const struct uwb_rf_scope clicker =
        build_scope(UWB_RF_SCOPE_ROLE_CLICKER, 0u);
    const struct uwb_rf_scope nodes[] = {
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 3u),
    };
    size_t index;

    for (index = 0u; index < sizeof(nodes) / sizeof(nodes[0]); index++) {
        assert_visibility(&clicker, &nodes[index], true);
    }
}

static void test_measured_click_data_must_climb_each_forced_layer(void)
{
    const struct uwb_rf_scope gateway =
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u);
    const struct uwb_rf_scope direct_anchor =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u);
    const struct uwb_rf_scope forced_one =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u);
    const struct uwb_rf_scope forced_two =
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u);

    /* A report produced by forced-two cannot physically shortcut its two
     * relay layers, irrespective of the report's semantic source/destination. */
    assert(!uwb_rf_scope_visible(&forced_two, &gateway));
    assert(!uwb_rf_scope_visible(&forced_two, &direct_anchor));
    assert(uwb_rf_scope_visible(&forced_two, &forced_one));
    assert(uwb_rf_scope_visible(&forced_one, &direct_anchor));
    assert(uwb_rf_scope_visible(&direct_anchor, &gateway));
}

static void test_wire_scope_round_trips_without_aliasing(void)
{
    const struct uwb_rf_scope scopes[] = {
        build_scope(UWB_RF_SCOPE_ROLE_GATEWAY, 0u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 0u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 1u),
        build_scope(UWB_RF_SCOPE_ROLE_ANCHOR, 2u),
        build_scope(UWB_RF_SCOPE_ROLE_CLICKER, 0u),
    };
    uint8_t wires[sizeof(scopes) / sizeof(scopes[0])] = {0};
    size_t index;
    size_t other;

    for (index = 0u; index < sizeof(scopes) / sizeof(scopes[0]); index++) {
        struct uwb_rf_scope decoded = {0};

        assert(uwb_rf_scope_encode(&scopes[index], &wires[index]) == 0);
        assert(uwb_rf_scope_decode(wires[index], &decoded) == 0);
        assert(decoded.layer == scopes[index].layer);
        assert(decoded.clicker == scopes[index].clicker);
    }

    for (index = 0u; index < sizeof(wires) / sizeof(wires[0]); index++) {
        for (other = index + 1u; other < sizeof(wires) / sizeof(wires[0]);
             other++) {
            assert(wires[index] != wires[other]);
        }
    }
}

static void test_invalid_inputs_fail_closed(void)
{
    struct uwb_rf_scope scope = {0};
    uint8_t wire = 0u;

    assert(uwb_rf_scope_build(UWB_RF_SCOPE_ROLE_GATEWAY, 0u, NULL) != 0);
    assert(uwb_rf_scope_build((enum uwb_rf_scope_role)UINT8_MAX,
                              0u,
                              &scope) != 0);
    assert(uwb_rf_scope_build(UWB_RF_SCOPE_ROLE_GATEWAY, 1u, &scope) != 0);
    assert(uwb_rf_scope_build(UWB_RF_SCOPE_ROLE_CLICKER, 1u, &scope) != 0);
    assert(uwb_rf_scope_encode(NULL, &wire) != 0);
    assert(uwb_rf_scope_encode(&scope, NULL) != 0);
    assert(uwb_rf_scope_decode(wire, NULL) != 0);

    scope.layer = (uint8_t)(UWB_RF_SCOPE_MAX_FORCED_RELAY_HOPS + 2u);
    assert(uwb_rf_scope_encode(&scope, &wire) != 0);
    scope.layer = 1u;
    scope.clicker = true;
    assert(uwb_rf_scope_encode(&scope, &wire) != 0);

    /* Plain packet bytes and noncanonical layer/clicker combinations cannot
     * silently opt a received frame into the local RF scope. */
    assert(uwb_rf_scope_decode(0u, &scope) != 0);
    assert(uwb_rf_scope_decode(0xffu, &scope) != 0);
    assert(!uwb_rf_scope_visible(NULL, &scope));
    assert(!uwb_rf_scope_visible(&scope, NULL));
}

int main(void)
{
    test_roles_map_to_physical_layers();
    test_visibility_is_same_or_adjacent_layer_only();
    test_gateway_and_anchor_packets_cannot_skip_layers();
    test_gateway_origin_uses_each_physical_transmitter_scope();
    test_clicker_is_visible_across_every_anchor_layer();
    test_measured_click_data_must_climb_each_forced_layer();
    test_wire_scope_round_trips_without_aliasing();
    test_invalid_inputs_fail_closed();
    return 0;
}
