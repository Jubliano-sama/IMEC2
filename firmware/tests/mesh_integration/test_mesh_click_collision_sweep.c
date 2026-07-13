#include "mesh_sim.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLICK_SWEEP_MAX_SOURCES 9u
#define CLICK_SWEEP_MAX_ROUNDS 6u
#define CLICK_SWEEP_ROUND_US UINT64_C(100000)
#define CLICK_SWEEP_GATEWAY_ID UINT64_C(0x9100000000000001)
#define CLICK_SWEEP_SOURCE_BASE UINT64_C(0x9200000000000000)
#define CLICK_SWEEP_SEED_COUNT 64u

enum offset_pattern {
    OFFSET_SIMULTANEOUS = 0,
    OFFSET_DESTRUCTIVE_HALF_FRAME = 1,
    OFFSET_STAGGERED = 2,
};

enum sweep_outcome {
    SWEEP_OUTCOME_RECOVERED = 0,
    SWEEP_OUTCOME_RETRY_EXHAUSTED = 1,
    SWEEP_OUTCOME_CAPACITY_EXHAUSTED = 2,
    SWEEP_OUTCOME_SIMULATOR_ERROR = 3,
};

struct click_state {
    uint64_t source_id;
    uint64_t arrival_us;
    uint64_t delivered_us;
    uint8_t attempts;
    bool rf_decoded;
    bool delivered;
    bool dropped;
};

struct sweep_config {
    uint32_t seed;
    uint8_t source_count;
    enum offset_pattern pattern;
    bool route_loss_recovery;
};

struct sweep_metrics {
    uint32_t seed;
    uint32_t attempted;
    uint32_t deferred;
    uint32_t delivered;
    uint32_t dropped;
    uint32_t retries;
    uint32_t collisions;
    uint32_t partials;
    uint64_t latency_p50_us;
    uint64_t latency_p95_us;
    uint64_t latency_max_us;
    enum sweep_outcome outcome;
};

static int failures;

#define CHECK(expression, ...) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line=%d ", __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

static const char *pattern_name(enum offset_pattern pattern)
{
    switch (pattern) {
    case OFFSET_SIMULTANEOUS:
        return "simultaneous";
    case OFFSET_DESTRUCTIVE_HALF_FRAME:
        return "half-frame-overlap";
    case OFFSET_STAGGERED:
        return "staggered";
    default:
        return "unknown";
    }
}

static uint32_t sweep_seed(uint32_t index)
{
    uint32_t value = UINT32_C(0xc11c0001) +
                     index * UINT32_C(0x9e3779b9);

    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    return value ^ (value >> 16);
}

static uint64_t percentile(const uint64_t *values, size_t count, uint8_t percent)
{
    uint64_t sorted[CLICK_SWEEP_MAX_SOURCES];
    size_t index;

    if (count == 0u || count > CLICK_SWEEP_MAX_SOURCES) {
        return 0u;
    }
    memcpy(sorted, values, count * sizeof(sorted[0]));
    for (size_t i = 1u; i < count; i++) {
        uint64_t value = sorted[i];
        size_t j = i;

        while (j > 0u && sorted[j - 1u] > value) {
            sorted[j] = sorted[j - 1u];
            j--;
        }
        sorted[j] = value;
    }
    index = ((count - 1u) * percent + 99u) / 100u;
    return sorted[index];
}

static uint64_t initial_offset_us(enum offset_pattern pattern,
                                  uint8_t source,
                                  uint32_t frame_us)
{
    switch (pattern) {
    case OFFSET_SIMULTANEOUS:
        return 0u;
    case OFFSET_DESTRUCTIVE_HALF_FRAME:
        return (uint64_t)(source % 3u) * (frame_us / 2u);
    case OFFSET_STAGGERED:
        return (uint64_t)source * (frame_us + 200u);
    default:
        return 0u;
    }
}

static uint8_t retry_slot(uint32_t seed, uint8_t source_count, uint8_t source)
{
    uint8_t rotation = (uint8_t)(seed % source_count);

    return (uint8_t)((source + rotation) % source_count);
}

static bool route_available(const struct sweep_config *config,
                            uint8_t source,
                            uint8_t round)
{
    if (!config->route_loss_recovery) {
        return true;
    }
    return round != 1u || (source & 1u) != 0u;
}

static int schedule_rf_round(const struct sweep_config *config,
                             struct click_state *clicks,
                             uint8_t round,
                             struct sweep_metrics *metrics)
{
    static struct mesh_sim_world rf_world;
    uint8_t source_nodes[CLICK_SWEEP_MAX_SOURCES];
    uint8_t gateway;
    uint64_t round_base_us = UINT64_C(1000);
    uint32_t frame_us = mesh_sim_frame_duration_us(
        MESH_SIM_PHY_CHANNEL9_MESH, PACKET_HEADER_LEN);
    uint64_t rx_end_us = round_base_us +
        (uint64_t)CLICK_SWEEP_MAX_SOURCES * (frame_us + 200u) + frame_us;
    uint16_t rx_window;
    bool attempted[CLICK_SWEEP_MAX_SOURCES] = {false};

    mesh_sim_init(&rf_world, config->seed ^ ((uint32_t)round << 24));
    if (mesh_sim_add_role(&rf_world,
                          MESH_SIM_ROLE_GATEWAY,
                          CLICK_SWEEP_GATEWAY_ID,
                          CLICK_SWEEP_GATEWAY_ID,
                          1u,
                          &gateway) != MESH_SIM_OK) {
        return MESH_SIM_ERR_CAPACITY;
    }
    for (uint8_t source = 0u; source < config->source_count; source++) {
        if (mesh_sim_add_role(&rf_world,
                              MESH_SIM_ROLE_TRANSMITTER,
                              clicks[source].source_id,
                              CLICK_SWEEP_GATEWAY_ID,
                              1u,
                              &source_nodes[source]) != MESH_SIM_OK ||
            mesh_sim_set_link(&rf_world,
                              source_nodes[source],
                              gateway,
                              96u,
                              1u) != MESH_SIM_OK) {
            return MESH_SIM_ERR_CAPACITY;
        }
        if (!route_available(config, source, round)) {
            rf_world.reachable[source_nodes[source]][gateway] = false;
            rf_world.reachable[gateway][source_nodes[source]] = false;
        }
    }
    if (mesh_sim_schedule_rx(&rf_world,
                             gateway,
                             0u,
                             rx_end_us,
                             UWB_CHANNEL_MESH_PAYLOAD,
                             MESH_SIM_PHY_CHANNEL9_MESH,
                             &rx_window) != MESH_SIM_OK) {
        return MESH_SIM_ERR_CAPACITY;
    }

    for (uint8_t source = 0u; source < config->source_count; source++) {
        struct proto_packet packet = {
            .msg_type = MSG_MESH_DATA,
            .src_id = clicks[source].source_id,
            .dst_id = CLICK_SWEEP_GATEWAY_ID,
            .session_id = config->seed,
            .seq = (uint16_t)(source + 1u),
            .ttl = MESH_DEFAULT_TTL,
            .payload_len = 0u,
        };
        uint64_t offset_us;
        uint16_t transmission;

        if (clicks[source].rf_decoded || clicks[source].dropped) {
            continue;
        }
        if (!rf_world.reachable[source_nodes[source]][gateway]) {
            metrics->deferred++;
            continue;
        }
        if (round == 0u) {
            offset_us = initial_offset_us(config->pattern, source, frame_us);
        } else {
            offset_us = (uint64_t)retry_slot(config->seed,
                                              config->source_count,
                                              source) *
                        (frame_us + 200u);
        }
        if (mesh_sim_schedule_packet_tx(&rf_world,
                                        source_nodes[source],
                                        round_base_us + offset_us,
                                        UWB_CHANNEL_MESH_PAYLOAD,
                                        MESH_SIM_PHY_CHANNEL9_MESH,
                                        &packet,
                                        NULL,
                                        0u,
                                        &transmission) != MESH_SIM_OK) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        attempted[source] = true;
        clicks[source].attempts++;
        metrics->attempted++;
        if (clicks[source].attempts > 1u) {
            metrics->retries++;
        }
    }
    if (mesh_sim_run(&rf_world) != MESH_SIM_OK) {
        return rf_world.last_error;
    }

    for (uint8_t source = 0u; source < config->source_count; source++) {
        enum mesh_sim_rx_outcome outcome = MESH_SIM_RX_DECODE_ERROR;
        bool found = false;

        if (!attempted[source]) {
            continue;
        }
        for (size_t i = 0u; i < rf_world.reception_count; i++) {
            const struct mesh_sim_reception *reception = &rf_world.receptions[i];

            if (reception->source_id == clicks[source].source_id &&
                reception->receiver_id == CLICK_SWEEP_GATEWAY_ID) {
                outcome = reception->outcome;
                found = true;
                break;
            }
        }
        if (!found) {
            return MESH_SIM_ERR_PROTOCOL;
        }
        if (outcome == MESH_SIM_RX_DECODED) {
            clicks[source].rf_decoded = true;
        } else if (outcome == MESH_SIM_RX_COLLISION) {
            metrics->collisions++;
        } else {
            metrics->partials++;
        }
    }
    return MESH_SIM_OK;
}

static struct sweep_metrics run_sweep(const struct sweep_config *config)
{
    struct click_state clicks[CLICK_SWEEP_MAX_SOURCES] = {0};
    struct sweep_metrics metrics = {
        .seed = config->seed,
        .outcome = SWEEP_OUTCOME_RECOVERED,
    };
    uint64_t latencies[CLICK_SWEEP_MAX_SOURCES];
    size_t latency_count = 0u;

    for (uint8_t source = 0u; source < config->source_count; source++) {
        clicks[source].source_id = CLICK_SWEEP_SOURCE_BASE + source + 1u;
        clicks[source].arrival_us = initial_offset_us(
            config->pattern, source, 1000u);
    }

    for (uint8_t round = 0u; round < CLICK_SWEEP_MAX_ROUNDS; round++) {
        int ret = schedule_rf_round(config, clicks, round, &metrics);
        uint64_t now_us = (uint64_t)(round + 1u) * CLICK_SWEEP_ROUND_US;

        if (ret != MESH_SIM_OK) {
            metrics.outcome = SWEEP_OUTCOME_SIMULATOR_ERROR;
            break;
        }
        for (uint8_t source = 0u; source < config->source_count; source++) {
            if (!clicks[source].rf_decoded || clicks[source].delivered ||
                clicks[source].dropped) {
                continue;
            }
            clicks[source].delivered = true;
            clicks[source].delivered_us = now_us;
            metrics.delivered++;
        }
        if (metrics.delivered + metrics.dropped == config->source_count) {
            break;
        }
    }

    for (uint8_t source = 0u; source < config->source_count; source++) {
        if (!clicks[source].delivered && !clicks[source].dropped) {
            clicks[source].dropped = true;
            metrics.dropped++;
            if (metrics.outcome == SWEEP_OUTCOME_RECOVERED) {
                metrics.outcome = SWEEP_OUTCOME_RETRY_EXHAUSTED;
            }
        }
        if (clicks[source].delivered) {
            uint64_t latency = clicks[source].delivered_us -
                               clicks[source].arrival_us;

            latencies[latency_count++] = latency;
            if (latency > metrics.latency_max_us) {
                metrics.latency_max_us = latency;
            }
        }
    }
    metrics.latency_p50_us = percentile(latencies, latency_count, 50u);
    metrics.latency_p95_us = percentile(latencies, latency_count, 95u);
    return metrics;
}

static void print_metrics(const struct sweep_config *config,
                          const struct sweep_metrics *metrics)
{
    printf("click-sweep seed=0x%08" PRIx32
           " n=%u offsets=%s route_recovery=%u layer=phy-only"
           " attempted=%u deferred=%u delivered=%u dropped=%u retries=%u"
           " collisions=%u partials=%u"
           " latency_p50_us=%" PRIu64 " latency_p95_us=%" PRIu64
           " latency_max_us=%" PRIu64 " outcome=%u\n",
           config->seed,
           config->source_count,
           pattern_name(config->pattern),
           config->route_loss_recovery,
           metrics->attempted,
           metrics->deferred,
           metrics->delivered,
           metrics->dropped,
           metrics->retries,
           metrics->collisions,
           metrics->partials,
           metrics->latency_p50_us,
           metrics->latency_p95_us,
           metrics->latency_max_us,
           metrics->outcome);
}

static void test_phy_collision_sweep_without_delivery_claim(void)
{
    uint32_t total_clicks = 0u;
    uint32_t total_delivered = 0u;

    for (uint32_t seed_index = 0u;
         seed_index < CLICK_SWEEP_SEED_COUNT;
         seed_index++) {
        for (uint8_t source_count = 1u; source_count <= 8u; source_count++) {
            for (enum offset_pattern pattern = OFFSET_SIMULTANEOUS;
                 pattern <= OFFSET_STAGGERED;
                 pattern++) {
                struct sweep_config config = {
                    .seed = sweep_seed(seed_index),
                    .source_count = source_count,
                    .pattern = pattern,
                    .route_loss_recovery = pattern != OFFSET_STAGGERED,
                };
                struct sweep_metrics metrics = run_sweep(&config);

                print_metrics(&config, &metrics);
                CHECK(metrics.outcome == SWEEP_OUTCOME_RECOVERED,
                      "PHY recovery scenario failed seed=0x%08" PRIx32
                      " n=%u pattern=%s outcome=%u",
                      config.seed, source_count, pattern_name(pattern),
                      metrics.outcome);
                CHECK(metrics.delivered == source_count && metrics.dropped == 0u,
                      "PHY harness recovery mismatch seed=0x%08" PRIx32
                      " n=%u delivered=%u dropped=%u",
                      config.seed, source_count,
                      metrics.delivered, metrics.dropped);
                CHECK(metrics.attempted + metrics.deferred >= source_count,
                      "progress accounting missing seed=0x%08" PRIx32
                      " n=%u attempted=%u deferred=%u",
                      config.seed, source_count,
                      metrics.attempted, metrics.deferred);
                CHECK(metrics.latency_max_us <=
                          CLICK_SWEEP_MAX_ROUNDS * CLICK_SWEEP_ROUND_US,
                      "latency bound exceeded seed=0x%08" PRIx32
                      " n=%u max=%" PRIu64,
                      config.seed, source_count, metrics.latency_max_us);
                if (source_count > 1u && pattern != OFFSET_STAGGERED) {
                    CHECK(metrics.collisions > 0u && metrics.retries > 0u,
                          "destructive overlap did not force retry seed=0x%08"
                          PRIx32 " n=%u pattern=%s",
                          config.seed, source_count, pattern_name(pattern));
                }
                total_clicks += source_count;
                total_delivered += metrics.delivered;
            }
        }
    }
    printf("phy-only aggregate recovered=%u total=%u seeds=%u cases=%u"
           " delivery_rate_claim=none\n",
           total_delivered,
           total_clicks,
           CLICK_SWEEP_SEED_COUNT,
           CLICK_SWEEP_SEED_COUNT * 8u * 3u);
}

static enum mesh_sim_rx_outcome containment_outcome(int32_t rx_end_adjust_us)
{
    static struct mesh_sim_world containment_world;
    uint8_t source;
    uint8_t gateway;
    uint16_t window;
    uint16_t transmission;
    const uint64_t tx_start_us = UINT64_C(1000);
    uint64_t rx_end_us;
    struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .src_id = CLICK_SWEEP_SOURCE_BASE,
        .dst_id = CLICK_SWEEP_GATEWAY_ID,
        .session_id = 1u,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };

    mesh_sim_init(&containment_world, UINT32_C(0xc11cb00d));
    if (mesh_sim_add_role(&containment_world,
                          MESH_SIM_ROLE_TRANSMITTER,
                          packet.src_id,
                          packet.dst_id,
                          1u,
                          &source) != MESH_SIM_OK ||
        mesh_sim_add_role(&containment_world,
                          MESH_SIM_ROLE_GATEWAY,
                          packet.dst_id,
                          packet.dst_id,
                          1u,
                          &gateway) != MESH_SIM_OK ||
        mesh_sim_set_link(&containment_world, source, gateway, 96u, 1u) !=
            MESH_SIM_OK ||
        mesh_sim_schedule_packet_tx(&containment_world,
                                    source,
                                    tx_start_us,
                                    UWB_CHANNEL_MESH_PAYLOAD,
                                    MESH_SIM_PHY_CHANNEL9_MESH,
                                    &packet,
                                    NULL,
                                    0u,
                                    &transmission) != MESH_SIM_OK) {
        return MESH_SIM_RX_DECODE_ERROR;
    }
    rx_end_us = containment_world.transmissions[transmission].end_us +
                containment_world.propagation_us[source][gateway] +
                rx_end_adjust_us;
    if (mesh_sim_schedule_rx(&containment_world,
                             gateway,
                             0u,
                             rx_end_us,
                             UWB_CHANNEL_MESH_PAYLOAD,
                             MESH_SIM_PHY_CHANNEL9_MESH,
                             &window) != MESH_SIM_OK ||
        mesh_sim_run(&containment_world) != MESH_SIM_OK ||
        containment_world.reception_count != 1u) {
        return MESH_SIM_RX_DECODE_ERROR;
    }
    return containment_world.receptions[0].outcome;
}

static void test_complete_airtime_boundaries(void)
{
    CHECK(containment_outcome(0) == MESH_SIM_RX_DECODED,
          "frame ending exactly at RX close was not decoded");
    CHECK(containment_outcome(1) == MESH_SIM_RX_DECODED,
          "frame ending 1 us before RX close was not decoded");
    CHECK(containment_outcome(-1) != MESH_SIM_RX_DECODED,
          "frame extending 1 us past RX close decoded incorrectly");
}

int main(void)
{
    test_complete_airtime_boundaries();
    test_phy_collision_sweep_without_delivery_claim();
    if (failures != 0) {
        fprintf(stderr, "mesh click collision sweep failures=%d\n", failures);
        return 1;
    }
    puts("mesh click collision sweeps passed");
    return 0;
}
