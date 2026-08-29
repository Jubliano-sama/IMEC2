#include "dwm3000_runtime.h"
#include "dwm3000_timing.h"
#include "protocol.h"
#include "uwb.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static unsigned int failures;

#define CHECK_TRUE(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,            \
                    #expression);                                                \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_INT(actual_expression, expected_expression)                        \
    do {                                                                         \
        int actual_value = (actual_expression);                                  \
        int expected_value = (expected_expression);                              \
        if (actual_value != expected_value) {                                    \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s=%d expected %d\n",                         \
                    __FILE__, __LINE__, #actual_expression,                      \
                    actual_value, expected_value);                               \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_U64(actual_expression, expected_expression)                        \
    do {                                                                         \
        uint64_t actual_value = (uint64_t)(actual_expression);                   \
        uint64_t expected_value = (uint64_t)(expected_expression);               \
        if (actual_value != expected_value) {                                    \
            fprintf(stderr,                                                       \
                    "FAIL %s:%d: %s=%" PRIu64 " expected %" PRIu64 "\n",       \
                    __FILE__, __LINE__, #actual_expression,                      \
                    actual_value, expected_value);                               \
            failures++;                                                          \
        }                                                                        \
    } while (0)

struct airtime_vector {
    const char *name;
    enum dwm3000_timing_phy phy;
    size_t frame_bytes;
    uint64_t airtime_rctu;
    uint64_t airtime_us_ceil;
};

static void test_airtime_golden_vectors(void)
{
    static const struct airtime_vector vectors[] = {
        {"channel-5-control", DWM3000_TIMING_PHY_CH5_MESH_CONTROL,
         34u, UINT64_C(291299328), 4559u},
        {"channel-9-ack", DWM3000_TIMING_PHY_CH9_MESH,
         49u, UINT64_C(102035456), 1597u},
        {"channel-9-81", DWM3000_TIMING_PHY_CH9_MESH,
         81u, UINT64_C(121958400), 1909u},
        {"channel-9-965", DWM3000_TIMING_PHY_CH9_MESH,
         965u, UINT64_C(651489280), 10196u},
        {"channel-9-974", DWM3000_TIMING_PHY_CH9_MESH,
         974u, UINT64_C(656207872), 10270u},
        {"channel-5-standard-max", DWM3000_TIMING_PHY_CH5_WAKE,
         124u, UINT64_C(347922432), 5445u},
        {"channel-9-extended-max", DWM3000_TIMING_PHY_CH9_MESH,
         1020u, UINT64_C(683470848), 10697u},
    };

    for (size_t i = 0u; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint64_t rctu = dwm3000_timing_airtime_rctu(vectors[i].phy,
                                                    vectors[i].frame_bytes);
        uint64_t us = dwm3000_timing_airtime_us_ceil(vectors[i].phy,
                                                     vectors[i].frame_bytes);

        if (rctu != vectors[i].airtime_rctu ||
            us != vectors[i].airtime_us_ceil) {
            fprintf(stderr,
                    "FAIL airtime vector %s: rctu=%" PRIu64
                    " expected=%" PRIu64 " us=%" PRIu64
                    " expected=%" PRIu64 "\n",
                    vectors[i].name,
                    rctu,
                    vectors[i].airtime_rctu,
                    us,
                    vectors[i].airtime_us_ceil);
            failures++;
        }
    }

    CHECK_U64(dwm3000_timing_shr_rctu(DWM3000_TIMING_PHY_CH5_WAKE),
              UINT64_C(267378688));
    CHECK_U64(dwm3000_timing_shr_rctu(DWM3000_TIMING_PHY_CH9_MESH),
              UINT64_C(67104768));
    CHECK_U64(dwm3000_timing_airtime_rctu(DWM3000_TIMING_PHY_CH5_WAKE, 0u),
              0u);
    CHECK_U64(dwm3000_timing_airtime_rctu(DWM3000_TIMING_PHY_CH5_WAKE, 125u),
              0u);
    CHECK_U64(dwm3000_timing_airtime_rctu(DWM3000_TIMING_PHY_CH9_MESH, 1021u),
              0u);
    CHECK_U64(dwm3000_timing_airtime_rctu((enum dwm3000_timing_phy)99, 34u),
              0u);
}

static void test_delayed_tx_and_rounding(void)
{
    const uint64_t requested = UINT64_C(1000000123);
    const uint64_t propagation = dwm3000_timing_us_to_rctu_ceil(3u);
    struct dwm3000_air_interval interval;

    CHECK_U64(dwm3000_timing_quantize_delayed_raw_rmarker(requested),
              UINT64_C(1000000000));
    CHECK_U64(propagation, UINT64_C(191693));
    CHECK_INT(dwm3000_timing_delayed_tx_interval(
                  DWM3000_TIMING_PHY_CH9_MESH,
                  81u,
                  requested,
                  propagation,
                  &interval),
              PROTO_OK);
    CHECK_U64(interval.rmarker_rctu, UINT64_C(1000208038));
    CHECK_U64(interval.start_rctu, UINT64_C(933103270));
    CHECK_U64(interval.end_rctu, UINT64_C(1055061670));
    CHECK_U64(interval.end_rctu - interval.start_rctu,
              UINT64_C(121958400));

    CHECK_U64(dwm3000_timing_us_to_rctu_floor(1u), 63897u);
    CHECK_U64(dwm3000_timing_us_to_rctu_ceil(1u), 63898u);
    CHECK_U64(dwm3000_timing_rctu_to_us_floor(63897u), 0u);
    CHECK_U64(dwm3000_timing_rctu_to_us_ceil(63897u), 1u);
    CHECK_U64(dwm3000_timing_rctu_to_us_floor(63898u), 1u);
    CHECK_U64(dwm3000_timing_rctu_to_us_ceil(63898u), 2u);
    CHECK_U64(dwm3000_timing_rctu_to_us_floor(
                  DWM3000_TIMING_RCTU_PER_SECOND),
              1000000u);
    CHECK_U64(dwm3000_timing_rctu_to_us_ceil(
                  DWM3000_TIMING_RCTU_PER_SECOND),
              1000000u);

    CHECK_INT(dwm3000_timing_delayed_tx_interval(
                  DWM3000_TIMING_PHY_CH9_MESH,
                  0u,
                  requested,
                  0u,
                  &interval),
              PROTO_ERR_BAD_LENGTH);
    CHECK_INT(dwm3000_timing_delayed_tx_interval(
                  DWM3000_TIMING_PHY_CH9_MESH,
                  81u,
                  0u,
                  0u,
                  &interval),
              PROTO_ERR_ARG);
    CHECK_INT(dwm3000_timing_delayed_tx_interval(
                  DWM3000_TIMING_PHY_CH9_MESH,
                  81u,
                  UINT64_MAX,
                  0u,
                  &interval),
              PROTO_ERR_ARG);
}

static void test_runtime_legal_sequence_and_delays(void)
{
    struct dwm3000_runtime runtime;
    struct dwm3000_runtime_interval interval;
    uint32_t operation_count;

    dwm3000_runtime_init(&runtime);
    CHECK_U64(dwm3000_runtime_spi_transfer_us(DWM3000_RUNTIME_SPI_SLOW, 8u),
              34u);
    CHECK_U64(dwm3000_runtime_spi_transfer_us(DWM3000_RUNTIME_SPI_FAST, 8u),
              4u);
    CHECK_U64(dwm3000_runtime_spi_transfer_us(DWM3000_RUNTIME_SPI_FAST, 96u),
              26u);
    CHECK_U64(DWM3000_RUNTIME_SOFT_RESET_US, 2000u);

    CHECK_INT(dwm3000_runtime_set_spi_rate(&runtime,
                                           DWM3000_RUNTIME_SPI_SLOW,
                                           0u,
                                           &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 1u);
    CHECK_INT(dwm3000_runtime_reset(&runtime, 1u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 7001u);
    CHECK_INT(dwm3000_runtime_read_device_id(&runtime, 7001u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 7035u);
    CHECK_INT(dwm3000_runtime_set_spi_rate(&runtime,
                                           DWM3000_RUNTIME_SPI_FAST,
                                           7035u,
                                           &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 7036u);
    CHECK_INT(dwm3000_runtime_configure(&runtime,
                                        DWM3000_TIMING_PHY_CH9_MESH,
                                        7036u,
                                        &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 7062u);
    CHECK_INT(dwm3000_runtime_lock_pll(&runtime, 7062u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 7232u);

    CHECK_INT(dwm3000_runtime_arm_rx(&runtime, 7232u, 10236u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.start_us, 7236u);
    CHECK_U64(interval.end_us, 10236u);
    CHECK_INT(dwm3000_runtime_status_poll(&runtime, 7300u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 7350u);
    CHECK_INT(dwm3000_runtime_status_poll(&runtime, 7301u, &interval),
              DWM3000_RUNTIME_ERR_BUSY);
    CHECK_INT(dwm3000_runtime_finish_rx(&runtime, 10236u),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_read_frame(&runtime, 81u, 10236u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 10264u);
    CHECK_INT(dwm3000_runtime_read_cir(&runtime, 1152u, 10264u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 10556u);
    CHECK_INT(dwm3000_runtime_write_frame(&runtime, 81u, 10556u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 10584u);
    CHECK_INT(dwm3000_runtime_start_tx(&runtime, 10584u, 12000u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.start_us, 10588u);
    CHECK_U64(interval.end_us, 12000u);
    CHECK_INT(dwm3000_runtime_status_poll(&runtime, 10600u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 10650u);
    CHECK_INT(dwm3000_runtime_finish_tx(&runtime, 11999u),
              DWM3000_RUNTIME_ERR_RADIO_STATE);
    CHECK_INT(dwm3000_runtime_finish_tx(&runtime, 12000u),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_enter_retained_sleep(&runtime,
                                                   12000u,
                                                   &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us, 12001u);
    CHECK_TRUE(runtime.retained_common && runtime.retained_txrx);

    operation_count = runtime.operation_count;
    CHECK_INT(dwm3000_runtime_prepare_phy(&runtime,
                                          DWM3000_TIMING_PHY_CH9_MESH,
                                          13000u,
                                          &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.start_us, 13000u);
    CHECK_U64(interval.end_us, 15934u);
    CHECK_U64(runtime.operation_count - operation_count, 6u);
    CHECK_TRUE(runtime.configured && runtime.pll_locked && runtime.awake);
    CHECK_INT(runtime.configured_phy, DWM3000_TIMING_PHY_CH9_MESH);

    CHECK_INT(dwm3000_runtime_enter_retained_sleep(&runtime,
                                                   16000u,
                                                   &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_prepare_phy(&runtime,
                                          DWM3000_TIMING_PHY_CH5_WAKE,
                                          17000u,
                                          &interval),
              DWM3000_RUNTIME_OK);
    CHECK_U64(interval.end_us - interval.start_us, 7236u);
    CHECK_INT(runtime.configured_phy, DWM3000_TIMING_PHY_CH5_WAKE);
    CHECK_TRUE(!runtime.retained_common && !runtime.retained_txrx);
}

static void test_runtime_rejects_illegal_order_and_overlap(void)
{
    struct dwm3000_runtime runtime;
    struct dwm3000_runtime_interval interval;

    dwm3000_runtime_init(&runtime);
    CHECK_INT(dwm3000_runtime_reset(&runtime, 0u, &interval),
              DWM3000_RUNTIME_ERR_SPI_ORDER);
    CHECK_INT(dwm3000_runtime_set_spi_rate(&runtime,
                                           DWM3000_RUNTIME_SPI_SLOW,
                                           0u,
                                           &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_reset(&runtime, 0u, &interval),
              DWM3000_RUNTIME_ERR_BUSY);
    CHECK_INT(dwm3000_runtime_reset(&runtime, 1u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_configure(&runtime,
                                        DWM3000_TIMING_PHY_CH9_MESH,
                                        7001u,
                                        &interval),
              DWM3000_RUNTIME_ERR_SPI_ORDER);
    CHECK_INT(dwm3000_runtime_set_spi_rate(&runtime,
                                           DWM3000_RUNTIME_SPI_FAST,
                                           7001u,
                                           &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_read_device_id(&runtime, 7002u, &interval),
              DWM3000_RUNTIME_ERR_SPI_ORDER);

    dwm3000_runtime_init(&runtime);
    CHECK_INT(dwm3000_runtime_prepare_phy(&runtime,
                                          DWM3000_TIMING_PHY_CH9_MESH,
                                          0u,
                                          &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_start_tx(&runtime,
                                       interval.end_us,
                                       interval.end_us + 100u,
                                       &interval),
              DWM3000_RUNTIME_ERR_NOT_READY);
    CHECK_INT(dwm3000_runtime_arm_rx(&runtime, 7236u, 10240u, &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_set_spi_rate(&runtime,
                                           DWM3000_RUNTIME_SPI_SLOW,
                                           7240u,
                                           &interval),
              DWM3000_RUNTIME_ERR_RADIO_STATE);
    CHECK_INT(dwm3000_runtime_write_frame(&runtime, 81u, 7240u, &interval),
              DWM3000_RUNTIME_ERR_RADIO_STATE);
    CHECK_INT(dwm3000_runtime_read_frame(&runtime, 81u, 7240u, &interval),
              DWM3000_RUNTIME_ERR_RADIO_STATE);
    CHECK_INT(dwm3000_runtime_extend_rx(&runtime, 11240u),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_extend_rx(&runtime, 11240u),
              DWM3000_RUNTIME_ERR_RADIO_STATE);
    CHECK_INT(dwm3000_runtime_finish_rx(&runtime, 11240u),
              DWM3000_RUNTIME_OK);
    CHECK_TRUE(runtime.illegal_operation_count >= 5u);

    dwm3000_runtime_init(&runtime);
    CHECK_INT(dwm3000_runtime_set_spi_rate(&runtime,
                                           DWM3000_RUNTIME_SPI_SLOW,
                                           0u,
                                           &interval),
              DWM3000_RUNTIME_OK);
    CHECK_INT(dwm3000_runtime_wake(&runtime, 1u, &interval),
              DWM3000_RUNTIME_ERR_RADIO_STATE);
}

static int run_delayed_final_deadline_scenario(
    bool insert_rx_diagnostics,
    bool prestage_final,
    uint64_t critical_path_stall_us,
    uint64_t *scheduled_final_start_us,
    uint64_t *start_command_end_us)
{
    const enum dwm3000_timing_phy phy = DWM3000_TIMING_PHY_CH5_RANGE;
    const uint64_t response_rmarker_rctu =
        dwm3000_timing_us_to_rctu_floor(20000u);
    const uint64_t response_airtime_rctu =
        dwm3000_timing_airtime_rctu(phy, UWB_RESP_LEN);
    const uint64_t shr_rctu = dwm3000_timing_shr_rctu(phy);
    const uint64_t final_raw_rmarker_rctu =
        response_rmarker_rctu +
        dwm3000_timing_uus_to_rctu(UWB_RANGE_REPLY_DELAY_UUS);
    struct dwm3000_air_interval final_air;
    struct dwm3000_runtime runtime;
    struct dwm3000_runtime_interval interval;
    uint64_t response_end_us;
    uint64_t final_start_us;
    uint64_t final_end_us;
    uint64_t cursor;
    int ret;

    if (response_airtime_rctu <= shr_rctu ||
        dwm3000_timing_delayed_tx_interval(phy,
                                           UWB_FINAL_LEN,
                                           final_raw_rmarker_rctu,
                                           0u,
                                           &final_air) != PROTO_OK) {
        return DWM3000_RUNTIME_ERR_ARG;
    }
    response_end_us = dwm3000_timing_rctu_to_us_ceil(
        response_rmarker_rctu + response_airtime_rctu - shr_rctu);
    final_start_us = dwm3000_timing_rctu_to_us_floor(final_air.start_rctu);
    final_end_us = dwm3000_timing_rctu_to_us_ceil(final_air.end_rctu);

    dwm3000_runtime_init(&runtime);
    ret = dwm3000_runtime_prepare_phy(&runtime, phy, 0u, &interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = interval.end_us;
    if (prestage_final) {
        ret = dwm3000_runtime_write_frame(&runtime,
                                          UWB_FINAL_LEN,
                                          cursor,
                                          &interval);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = interval.end_us;
    }
    ret = dwm3000_runtime_arm_rx(&runtime,
                                  cursor,
                                  response_end_us +
                                      DWM3000_RUNTIME_STATUS_POLL_PERIOD_US +
                                      1u,
                                  &interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }

    ret = dwm3000_runtime_status_poll(&runtime, response_end_us, &interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = interval.end_us;
    ret = dwm3000_runtime_finish_rx(&runtime, cursor);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    /* Stress any unbudgeted work inserted after RX and before delayed TX. */
    cursor += critical_path_stall_us;
    if (insert_rx_diagnostics) {
        ret = dwm3000_runtime_read_rx_diagnostics(&runtime,
                                                   cursor,
                                                   &interval);
        if (ret != DWM3000_RUNTIME_OK) {
            return ret;
        }
        cursor = interval.end_us;
    }
    ret = dwm3000_runtime_read_frame(&runtime,
                                     UWB_RESP_LEN,
                                     cursor,
                                     &interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = interval.end_us;
    ret = dwm3000_runtime_process_range_frame(&runtime, cursor, &interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = interval.end_us;
    ret = dwm3000_runtime_write_frame(&runtime,
                                      prestage_final ?
                                          3u * sizeof(uint32_t) :
                                          UWB_FINAL_LEN,
                                      cursor,
                                      &interval);
    if (ret != DWM3000_RUNTIME_OK) {
        return ret;
    }
    cursor = interval.end_us;
    ret = dwm3000_runtime_start_delayed_tx(&runtime,
                                            cursor,
                                            final_start_us,
                                            final_end_us,
                                            &interval);
    if (scheduled_final_start_us != NULL) {
        *scheduled_final_start_us = final_start_us;
    }
    if (start_command_end_us != NULL) {
        *start_command_end_us = runtime.cpu_busy_until_us;
    }
    return ret;
}

static void test_ds_twr_delayed_final_deadline(void)
{
    uint64_t lean_deadline_us = 0u;
    uint64_t lean_command_end_us = 0u;
    uint64_t prepared_deadline_us = 0u;
    uint64_t prepared_command_end_us = 0u;
    uint64_t diagnostic_deadline_us = 0u;
    uint64_t diagnostic_command_end_us = 0u;
    uint64_t stalled_deadline_us = 0u;
    uint64_t stalled_command_end_us = 0u;

    CHECK_INT(run_delayed_final_deadline_scenario(false,
                                                   false,
                                                   0u,
                                                   &lean_deadline_us,
                                                   &lean_command_end_us),
              DWM3000_RUNTIME_OK);
    CHECK_TRUE(lean_command_end_us < lean_deadline_us);
    CHECK_TRUE(lean_deadline_us - lean_command_end_us >= 1000u);

    CHECK_INT(run_delayed_final_deadline_scenario(false,
                                                   true,
                                                   0u,
                                                   &prepared_deadline_us,
                                                   &prepared_command_end_us),
              DWM3000_RUNTIME_OK);
    CHECK_U64(prepared_deadline_us, lean_deadline_us);
    CHECK_TRUE(prepared_command_end_us < lean_command_end_us);

    CHECK_INT(run_delayed_final_deadline_scenario(true,
                                                   true,
                                                   0u,
                                                   &diagnostic_deadline_us,
                                                   &diagnostic_command_end_us),
              DWM3000_RUNTIME_ERR_DEADLINE_MISSED);
    CHECK_U64(diagnostic_deadline_us, lean_deadline_us);
    CHECK_TRUE(diagnostic_command_end_us >= diagnostic_deadline_us);

    CHECK_INT(run_delayed_final_deadline_scenario(
                  false,
                  true,
                  UWB_RANGE_REPLY_DELAY_UUS,
                  &stalled_deadline_us,
                  &stalled_command_end_us),
              DWM3000_RUNTIME_ERR_DEADLINE_MISSED);
    CHECK_U64(stalled_deadline_us, lean_deadline_us);
    CHECK_TRUE(stalled_command_end_us >= stalled_deadline_us);
}

int main(void)
{
    test_airtime_golden_vectors();
    test_delayed_tx_and_rounding();
    test_runtime_legal_sequence_and_delays();
    test_runtime_rejects_illegal_order_and_overlap();
    test_ds_twr_delayed_final_deadline();

    if (failures != 0u) {
        fprintf(stderr, "dwm3000 model tests failed: %u\n", failures);
        return 1;
    }
    puts("dwm3000 model tests passed");
    return 0;
}
