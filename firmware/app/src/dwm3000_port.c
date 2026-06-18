#include "dwm3000_port.h"

#include <hal/nrf_gpio.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>

#include <errno.h>

#if defined(CONFIG_IMEC_HIGH_DEBUG)
#define DWM3000_PORT_LOG_LEVEL LOG_LEVEL_DBG
#else
#define DWM3000_PORT_LOG_LEVEL LOG_LEVEL_INF
#endif

LOG_MODULE_REGISTER(dwm3000_port, DWM3000_PORT_LOG_LEVEL);

#define DWM3000_NODE DT_ALIAS(dwm3000)

#if !DT_NODE_HAS_STATUS(DWM3000_NODE, okay)
#error "The DWM3000 devicetree alias must point at an enabled SPI device"
#endif

#define DWM3000_SLOW_SPI_HZ DT_PROP(DWM3000_NODE, slow_spi_frequency)
#define DWM3000_FAST_SPI_HZ DT_PROP(DWM3000_NODE, fast_spi_frequency)
#define DWM3000_SPI_MAX_HZ DT_PROP(DWM3000_NODE, spi_max_frequency)
#define DWM3000_BUS_MAX_HZ DT_PROP(DT_BUS(DWM3000_NODE), max_frequency)
#define DWM3000_RUNTIME_MIN_HZ 32000000u
#define DWM3000_RESET_ASSERT_MS 2u
#define DWM3000_RESET_RELEASE_MS 5u
#define DWM3000_WAKE_ASSERT_US 500u
#define DWM3000_WAKE_SETTLE_MS 2u
#define DWM3000_DEV_ID_READ_HEADER 0x00u
#define DWM3000_DEV_ID_LEN 4u
#define DWM3000_SPI_BUS_NODE DT_BUS(DWM3000_NODE)
#define DWM3000_SPI_DEFAULT_PINCTRL DT_PHANDLE_BY_IDX(DWM3000_SPI_BUS_NODE, pinctrl_0, 0)
#define DWM3000_SPI_DEFAULT_GROUP DT_CHILD(DWM3000_SPI_DEFAULT_PINCTRL, group1)
#define DWM3000_SPI_PIN(idx) \
    (((DT_PROP_BY_IDX(DWM3000_SPI_DEFAULT_GROUP, psels, idx)) >> NRF_PIN_POS) & NRF_PIN_MSK)

BUILD_ASSERT(DWM3000_SLOW_SPI_HZ <= 7000000u,
             "DW3000 reset and soft-reset SPI rate must be <= 7 MHz");
BUILD_ASSERT(DWM3000_FAST_SPI_HZ >= DWM3000_RUNTIME_MIN_HZ,
             "DWM3000 runtime SPI must be configured for at least 32 MHz");
BUILD_ASSERT(DWM3000_FAST_SPI_HZ <= DWM3000_SPI_MAX_HZ,
             "fast-spi-frequency must not exceed spi-max-frequency");
BUILD_ASSERT(DWM3000_FAST_SPI_HZ <= DWM3000_BUS_MAX_HZ,
             "DWM3000 fast SPI exceeds the selected SPI controller limit");
BUILD_ASSERT(DWM3000_SLOW_SPI_HZ < DWM3000_FAST_SPI_HZ,
             "DWM3000 slow SPI must be slower than runtime SPI");
BUILD_ASSERT(DT_PROP_LEN(DWM3000_SPI_DEFAULT_GROUP, psels) == 3,
             "DWM3000 SPI pinctrl must expose SCK, MISO, and MOSI pins");
static const struct spi_dt_spec dwm_spi =
    SPI_DT_SPEC_GET(DWM3000_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec dwm_reset =
    GPIO_DT_SPEC_GET(DWM3000_NODE, reset_gpios);
static const struct gpio_dt_spec dwm_wakeup =
    GPIO_DT_SPEC_GET(DWM3000_NODE, wakeup_gpios);

static struct spi_config dwm_spi_cfg;
static uint32_t current_spi_hz;
static bool port_ready;

static uint32_t u32_from_le(const uint8_t data[4])
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int ensure_ready(void)
{
    if (port_ready) {
        return 0;
    }

    return dwm3000_port_init();
}

int dwm3000_port_set_slow_spi(void)
{
    dwm_spi_cfg = dwm_spi.config;
    dwm_spi_cfg.frequency = DWM3000_SLOW_SPI_HZ;
    current_spi_hz = DWM3000_SLOW_SPI_HZ;
    return 0;
}

int dwm3000_port_set_fast_spi(void)
{
    dwm_spi_cfg = dwm_spi.config;
    dwm_spi_cfg.frequency = DWM3000_FAST_SPI_HZ;
    current_spi_hz = DWM3000_FAST_SPI_HZ;
    return 0;
}

uint32_t dwm3000_port_current_spi_hz(void)
{
    return current_spi_hz;
}

int dwm3000_port_init(void)
{
    int ret;

    if (!spi_is_ready_dt(&dwm_spi)) {
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&dwm_reset) || !gpio_is_ready_dt(&dwm_wakeup)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&dwm_wakeup, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&dwm_reset, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    (void)dwm3000_port_set_slow_spi();
    port_ready = true;

    LOG_INF("DWM3000 port ready on %s: slow=%u Hz fast=%u Hz status polling",
            dwm_spi.bus->name,
            (unsigned int)DWM3000_SLOW_SPI_HZ,
            (unsigned int)DWM3000_FAST_SPI_HZ);
    return 0;
}

int dwm3000_port_hw_reset(void)
{
    int ret;

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    (void)dwm3000_port_set_slow_spi();

    ret = gpio_pin_configure_dt(&dwm_reset, GPIO_OUTPUT_INACTIVE | GPIO_OPEN_DRAIN);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_set_dt(&dwm_reset, 1);
    if (ret < 0) {
        return ret;
    }
    k_msleep(DWM3000_RESET_ASSERT_MS);

    ret = gpio_pin_set_dt(&dwm_reset, 0);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&dwm_reset, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    k_msleep(DWM3000_RESET_RELEASE_MS);
    return 0;
}

int dwm3000_port_wakeup(void)
{
    int ret;

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_set_dt(&dwm_wakeup, 1);
    if (ret < 0) {
        return ret;
    }

    k_busy_wait(DWM3000_WAKE_ASSERT_US);

    ret = gpio_pin_set_dt(&dwm_wakeup, 0);
    if (ret < 0) {
        return ret;
    }

    k_msleep(DWM3000_WAKE_SETTLE_MS);
    return 0;
}

int dwm3000_port_prepare_systemoff(void)
{
    int ret = 0;

    if (dwm_spi.config.cs.gpio.port != NULL &&
        gpio_is_ready_dt(&dwm_spi.config.cs.gpio)) {
        ret = gpio_pin_configure_dt(&dwm_spi.config.cs.gpio,
                                    GPIO_DISCONNECTED);
    }
    if (gpio_is_ready_dt(&dwm_wakeup)) {
        int wake_ret = gpio_pin_configure_dt(&dwm_wakeup,
                                             GPIO_DISCONNECTED);

        if (ret == 0 && wake_ret < 0) {
            ret = wake_ret;
        }
    }
    if (gpio_is_ready_dt(&dwm_reset)) {
        int reset_ret = gpio_pin_configure_dt(&dwm_reset,
                                              GPIO_DISCONNECTED);

        if (ret == 0 && reset_ret < 0) {
            ret = reset_ret;
        }
    }
    nrf_gpio_cfg_default(DWM3000_SPI_PIN(0));
    nrf_gpio_cfg_default(DWM3000_SPI_PIN(1));
    nrf_gpio_cfg_default(DWM3000_SPI_PIN(2));

    port_ready = false;
    return ret;
}

bool dwm3000_port_dev_id_supported(uint32_t dev_id)
{
    switch (dev_id) {
    case DWM3000_DEV_ID_A0:
    case DWM3000_DEV_ID_A0_PDOA:
    case DWM3000_DEV_ID_B0:
    case DWM3000_DEV_ID_B0_PDOA:
    case DWM3000_DEV_ID_C0:
    case DWM3000_DEV_ID_C0_PDOA:
        return true;
    default:
        return false;
    }
}

int dwm3000_port_read_dev_id(uint32_t *dev_id)
{
    const uint8_t header = DWM3000_DEV_ID_READ_HEADER;
    uint8_t data[DWM3000_DEV_ID_LEN];
    int ret;

    if (dev_id == NULL) {
        return -EINVAL;
    }

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    (void)dwm3000_port_set_slow_spi();

    ret = dwm3000_port_read(&header, sizeof(header), data, sizeof(data));
    if (ret < 0) {
        return ret;
    }

    *dev_id = u32_from_le(data);
    if (!dwm3000_port_dev_id_supported(*dev_id)) {
        return -ENODEV;
    }

    return 0;
}

int dwm3000_port_transceive(const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct spi_buf tx_buf = {
        .buf = (void *)tx,
        .len = len,
    };
    struct spi_buf rx_buf = {
        .buf = rx,
        .len = len,
    };
    const struct spi_buf_set tx_set = {
        .buffers = &tx_buf,
        .count = 1u,
    };
    const struct spi_buf_set rx_set = {
        .buffers = &rx_buf,
        .count = 1u,
    };
    int ret;

    if (len > 0u && tx == NULL && rx == NULL) {
        return -EINVAL;
    }

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    return spi_transceive(dwm_spi.bus, &dwm_spi_cfg,
                          tx != NULL ? &tx_set : NULL,
                          rx != NULL ? &rx_set : NULL);
}

int dwm3000_port_write(const uint8_t *header, size_t header_len,
                            const uint8_t *body, size_t body_len)
{
    const struct spi_buf tx_bufs[] = {
        {
            .buf = (void *)header,
            .len = header_len,
        },
        {
            .buf = (void *)body,
            .len = body_len,
        },
    };
    const struct spi_buf_set tx_set = {
        .buffers = tx_bufs,
        .count = 2u,
    };
    int ret;

    if ((header_len > 0u && header == NULL) || (body_len > 0u && body == NULL)) {
        return -EINVAL;
    }

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    return spi_write(dwm_spi.bus, &dwm_spi_cfg, &tx_set);
}

int dwm3000_port_write_with_crc(const uint8_t *header, size_t header_len,
                                     const uint8_t *body, size_t body_len,
                                     uint8_t crc8)
{
    const struct spi_buf tx_bufs[] = {
        {
            .buf = (void *)header,
            .len = header_len,
        },
        {
            .buf = (void *)body,
            .len = body_len,
        },
        {
            .buf = &crc8,
            .len = sizeof(crc8),
        },
    };
    const struct spi_buf_set tx_set = {
        .buffers = tx_bufs,
        .count = 3u,
    };
    int ret;

    if ((header_len > 0u && header == NULL) || (body_len > 0u && body == NULL)) {
        return -EINVAL;
    }

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    return spi_write(dwm_spi.bus, &dwm_spi_cfg, &tx_set);
}

int dwm3000_port_read(const uint8_t *header, size_t header_len,
                           uint8_t *body, size_t body_len)
{
    const struct spi_buf tx_bufs[] = {
        {
            .buf = (void *)header,
            .len = header_len,
        },
        {
            .buf = NULL,
            .len = body_len,
        },
    };
    const struct spi_buf rx_bufs[] = {
        {
            .buf = NULL,
            .len = header_len,
        },
        {
            .buf = body,
            .len = body_len,
        },
    };
    const struct spi_buf_set tx_set = {
        .buffers = tx_bufs,
        .count = 2u,
    };
    const struct spi_buf_set rx_set = {
        .buffers = rx_bufs,
        .count = 2u,
    };
    int ret;

    if ((header_len > 0u && header == NULL) || (body_len > 0u && body == NULL)) {
        return -EINVAL;
    }

    ret = ensure_ready();
    if (ret < 0) {
        return ret;
    }

    return spi_transceive(dwm_spi.bus, &dwm_spi_cfg, &tx_set, &rx_set);
}
