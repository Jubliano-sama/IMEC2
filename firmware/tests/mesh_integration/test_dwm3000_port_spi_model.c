#include "dwm3000_port_spi_model.h"
#include "dwm3000_port.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

const struct device model_spi_bus = { "SPIM3" };
const struct device model_gpio_port = { "GPIO0" };
NRF_SPIM_Type model_spim;
static const struct spi_config *cached_config;
static bool initialized;
static bool suspended;
static unsigned int transfers;
static unsigned int reconfigurations;
static unsigned int parked_pins;
static uint32_t gpio_flags[32];
static int gpio_values[32];
static uint64_t elapsed_us;
static int next_spi_error;
static int next_pm_error;

int pm_device_action_run(const struct device *device, enum pm_device_action action)
{
    REQUIRE(device == &model_spi_bus);
    int ret = next_pm_error;
    next_pm_error = 0;
    if (ret < 0) { return ret; }
    suspended = action == PM_DEVICE_ACTION_SUSPEND;
    if (suspended) { initialized = false; }
    return 0;
}

bool spi_is_ready_dt(const struct spi_dt_spec *spec)
{
    return spec->bus == &model_spi_bus;
}

bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
    return spec->port == &model_gpio_port;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, uint32_t flags)
{
    REQUIRE(spec->pin < 32u);
    gpio_flags[spec->pin] = flags;
    return 0;
}

int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value)
{
    REQUIRE(spec->pin < 32u);
    REQUIRE(gpio_flags[spec->pin] != GPIO_DISCONNECTED);
    gpio_values[spec->pin] = value;
    return 0;
}

void nrf_spim_disable(NRF_SPIM_Type *reg) { REQUIRE(reg == &model_spim); }

void nrf_spim_pins_set(NRF_SPIM_Type *reg, uint32_t sck, uint32_t mosi, uint32_t miso)
{
    REQUIRE(reg == &model_spim);
    REQUIRE(sck == NRF_SPIM_PIN_NOT_CONNECTED);
    REQUIRE(mosi == NRF_SPIM_PIN_NOT_CONNECTED);
    REQUIRE(miso == NRF_SPIM_PIN_NOT_CONNECTED);
}

void nrf_gpio_cfg_default(uint32_t pin)
{
    REQUIRE(pin >= 20u && pin <= 22u);
    parked_pins |= 1u << (pin - 20u);
}

void k_msleep(uint32_t milliseconds) { elapsed_us += milliseconds * 1000u; }
void k_busy_wait(uint32_t microseconds) { elapsed_us += microseconds; }

int spi_transceive(const struct device *device, const struct spi_config *config,
                   const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
    REQUIRE(device == &model_spi_bus && !suspended);
    REQUIRE(config != NULL);
    REQUIRE(config->operation == (SPI_WORD_SET(8) | SPI_TRANSFER_MSB));
    REQUIRE(config->slave == 0u);
    REQUIRE(config->cs.gpio.port == &model_gpio_port);
    REQUIRE(config->cs.gpio.pin == MODEL_CS_PIN && config->cs.gpio.dt_flags == 1u);
    REQUIRE(gpio_flags[MODEL_CS_PIN] == GPIO_OUTPUT_INACTIVE);
    REQUIRE(tx != NULL || rx != NULL);

    /* Match spi_nrfx_spim.c configure() / spi_context_configured(): the
     * controller caches the ADDRESS, not a snapshot or value comparison.
     * Updating frequency inside the same config must leave hardware stale. */
    if (!initialized || cached_config != config) {
        cached_config = config;
        model_spim.FREQUENCY = config->frequency;
        initialized = true;
        reconfigurations++;
    }
    size_t tx_bytes = 0u;
    size_t rx_bytes = 0u;
    for (size_t index = 0u; tx != NULL && index < tx->count; index++) {
        tx_bytes += tx->buffers[index].len;
    }
    for (size_t index = 0u; rx != NULL && index < rx->count; index++) {
        rx_bytes += rx->buffers[index].len;
    }
    size_t wire_bytes = tx_bytes > rx_bytes ? tx_bytes : rx_bytes;
    elapsed_us += 1u + ((uint64_t)wire_bytes * 8000000u +
                       model_spim.FREQUENCY - 1u) / model_spim.FREQUENCY;
    transfers++;
    int ret = next_spi_error;
    next_spi_error = 0;
    if (ret < 0) { return ret; }
    if (rx != NULL) {
        for (size_t index = 0u; index < rx->count; index++) {
            if (rx->buffers[index].buf != NULL) {
                memset(rx->buffers[index].buf, 0xa5, rx->buffers[index].len);
            }
        }
    }
    return 0;
}

int spi_write(const struct device *device, const struct spi_config *config,
              const struct spi_buf_set *tx)
{
    return spi_transceive(device, config, tx, NULL);
}

static void transfer(unsigned int kind, uint32_t expected_hz)
{
    const uint8_t header = 0x12;
    uint8_t data[4] = { 1u, 2u, 3u, 4u };
    int ret;
    switch (kind) {
    case 0:
        ret = dwm3000_port_transceive(&header, data, 1u);
        break;
    case 1:
        ret = dwm3000_port_write(&header, 1u, data, sizeof(data));
        break;
    case 2:
        ret = dwm3000_port_write_with_crc(&header, 1u, data, sizeof(data), 0x5a);
        break;
    default:
        ret = dwm3000_port_read(&header, 1u, data, sizeof(data));
        break;
    }
    REQUIRE(ret == 0);
    REQUIRE(dwm3000_port_current_spi_hz() == expected_hz);
    REQUIRE(model_spim.FREQUENCY == expected_hz);
}

static void repeated_rate_changes(void)
{
    REQUIRE(dwm3000_port_init() == 0);
    for (unsigned int repeat = 0; repeat < 32u; repeat++) {
        for (unsigned int kind = 0; kind < 4u; kind++) {
            REQUIRE(dwm3000_port_set_slow_spi() == 0);
            uint64_t started = elapsed_us;
            transfer(kind, MODEL_SLOW_HZ);
            uint64_t slow_us = elapsed_us - started;
            REQUIRE(dwm3000_port_set_fast_spi() == 0);
            started = elapsed_us;
            transfer(kind, MODEL_FAST_HZ);
            REQUIRE(elapsed_us > started && elapsed_us - started < slow_us);
            unsigned int configured = reconfigurations;
            transfer(kind, MODEL_FAST_HZ);
            REQUIRE(reconfigurations == configured);
            REQUIRE(dwm3000_port_set_slow_spi() == 0);
            transfer(kind, MODEL_SLOW_HZ);
        }
    }
}

static void low_power_cycles(void)
{
    for (unsigned int repeat = 0; repeat < 32u; repeat++) {
        REQUIRE(dwm3000_port_set_fast_spi() == 0);
        transfer(1u, MODEL_FAST_HZ);
        uint64_t before = elapsed_us;
        REQUIRE(dwm3000_port_hw_reset() == 0);
        REQUIRE(elapsed_us - before == 7000u);
        REQUIRE(gpio_values[MODEL_RESET_PIN] == 0);
        REQUIRE(gpio_flags[MODEL_RESET_PIN] == GPIO_INPUT);
        transfer(0u, MODEL_SLOW_HZ);
        before = elapsed_us;
        REQUIRE(dwm3000_port_wakeup() == 0);
        REQUIRE(elapsed_us - before == 2500u);
        REQUIRE(gpio_values[MODEL_WAKE_PIN] == 0);
        REQUIRE(dwm3000_port_set_fast_spi() == 0);
        transfer(2u, MODEL_FAST_HZ);
        REQUIRE(dwm3000_port_float_pins() == 0);
        REQUIRE(suspended && parked_pins == 7u);
        REQUIRE(gpio_flags[MODEL_CS_PIN] == GPIO_DISCONNECTED);
        REQUIRE(gpio_flags[MODEL_RESET_PIN] == GPIO_DISCONNECTED);
        REQUIRE(gpio_flags[MODEL_WAKE_PIN] == GPIO_DISCONNECTED);
        REQUIRE(dwm3000_port_wakeup() == 0);
        transfer(3u, MODEL_SLOW_HZ);
    }
}

static void failures_preserve_recovery(void)
{
    uint8_t byte = 0x55;
    next_spi_error = -EIO;
    REQUIRE(dwm3000_port_set_fast_spi() == 0);
    REQUIRE(dwm3000_port_transceive(&byte, &byte, 1u) == -EIO);
    REQUIRE(dwm3000_port_write(NULL, 1u, NULL, 0u) == -EINVAL);
    REQUIRE(dwm3000_port_take_error() == -EIO);
    REQUIRE(dwm3000_port_take_error() == 0);
    REQUIRE(dwm3000_port_set_slow_spi() == 0);
    transfer(1u, MODEL_SLOW_HZ);

    REQUIRE(dwm3000_port_prepare_systemoff() == 0);
    unsigned int before = transfers;
    next_pm_error = -EIO;
    REQUIRE(dwm3000_port_transceive(&byte, &byte, 1u) == -EIO);
    REQUIRE(transfers == before);
    REQUIRE(dwm3000_port_take_error() == -EIO);
    transfer(0u, MODEL_SLOW_HZ);
    REQUIRE(dwm3000_port_set_fast_spi() == 0);
    transfer(3u, MODEL_FAST_HZ);
}

int main(void)
{
    repeated_rate_changes();
    low_power_cycles();
    failures_preserve_recovery();
    printf("DWM3000 production port SPI model passed (%u transfers)\n", transfers);
    return 0;
}
