#include "device_identity.h"

#include <assert.h>
#include <stdio.h>

static void test_ficr_word_order_and_stable_node_domain(void)
{
    const uint32_t word0 = UINT32_C(0x01234567);
    const uint32_t word1 = UINT32_C(0x89abcdef);
    const uint64_t hardware_id = UINT64_C(0x89abcdef01234567);
    uint64_t node_id = 0u;

    assert(device_identity_ficr_value(word0, word1) == hardware_id);
    assert(device_identity_node_from_ficr(word0, word1, &node_id));
    assert(node_id == (hardware_id ^ DEVICE_IDENTITY_NODE_DOMAIN));
    assert((node_id ^ DEVICE_IDENTITY_NODE_DOMAIN) == hardware_id);
    assert(DEVICE_IDENTITY_NODE_DOMAIN == DEVICE_IDENTITY_ANCHOR_DOMAIN);
    {
        uint64_t compatibility_id = 0u;

        assert(device_identity_anchor_from_ficr(
            word0, word1, &compatibility_id));
        assert(compatibility_id == node_id);
    }
}

static void test_identical_artifact_maps_two_anchors_to_distinct_ids(void)
{
    uint64_t first = 0u;
    uint64_t second = 0u;

    assert(device_identity_node_from_ficr(UINT32_C(0x10203040),
                                          UINT32_C(0x50607080),
                                          &first));
    assert(device_identity_node_from_ficr(UINT32_C(0x10203041),
                                          UINT32_C(0x50607080),
                                          &second));
    assert(first != second);
}

static void test_reboot_mapping_is_stable(void)
{
    uint64_t before_reboot = 0u;
    uint64_t after_reboot = 0u;

    assert(device_identity_node_from_ficr(UINT32_C(0x76543210),
                                          UINT32_C(0xfedcba98),
                                          &before_reboot));
    assert(device_identity_node_from_ficr(UINT32_C(0x76543210),
                                          UINT32_C(0xfedcba98),
                                          &after_reboot));
    assert(before_reboot == after_reboot);
}

static void test_invalid_or_reserved_values_fail_closed(void)
{
    uint64_t anchor_id = UINT64_MAX;
    uint64_t reserved_raw = DEVICE_IDENTITY_ANCHOR_DOMAIN;
    const uint64_t reserved_nodes[] = {
        DEVICE_IDENTITY_LEGACY_FIXED_CLICKER_ID,
        DEVICE_IDENTITY_DEFAULT_GATEWAY_ID,
    };

    assert(!device_identity_node_from_ficr(0u, 0u, &anchor_id));
    assert(anchor_id == 0u);
    assert(!device_identity_node_from_ficr(UINT32_MAX, UINT32_MAX,
                                           &anchor_id));
    assert(!device_identity_node_from_ficr((uint32_t)reserved_raw,
                                           (uint32_t)(reserved_raw >> 32),
                                           &anchor_id));
    for (size_t i = 0u; i < sizeof(reserved_nodes) / sizeof(reserved_nodes[0]);
         i++) {
        reserved_raw = reserved_nodes[i] ^ DEVICE_IDENTITY_NODE_DOMAIN;
        anchor_id = UINT64_MAX;
        assert(!device_identity_node_from_ficr(
            (uint32_t)reserved_raw,
            (uint32_t)(reserved_raw >> 32),
            &anchor_id));
        assert(anchor_id == 0u);
    }
    assert(!device_identity_node_from_ficr(1u, 2u, NULL));
}

static void test_sampled_mapping_has_no_width_loss_or_collisions(void)
{
    uint64_t ids[1024];

    for (uint32_t i = 0u; i < 1024u; i++) {
        uint32_t word0 = UINT32_C(0x13579bdf) + i;
        uint32_t word1 = UINT32_C(0x2468ace0) ^ (i * UINT32_C(0x9e3779b9));

        assert(device_identity_node_from_ficr(word0, word1, &ids[i]));
        assert((ids[i] ^ DEVICE_IDENTITY_NODE_DOMAIN) ==
               device_identity_ficr_value(word0, word1));
        for (uint32_t j = 0u; j < i; j++) {
            assert(ids[i] != ids[j]);
        }
    }
}

int main(void)
{
    test_ficr_word_order_and_stable_node_domain();
    test_identical_artifact_maps_two_anchors_to_distinct_ids();
    test_reboot_mapping_is_stable();
    test_invalid_or_reserved_values_fail_closed();
    test_sampled_mapping_has_no_width_loss_or_collisions();
    puts("device identity tests passed");
    return 0;
}
