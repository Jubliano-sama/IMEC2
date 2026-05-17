#include "dwm3000_port.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(dwm3000_sdk_port, LOG_LEVEL_INF);

typedef void (*port_deca_isr_t)(void);

static port_deca_isr_t dwm3000_isr;

int openspi(void)
{
    return dwm3000_port_init() == 0 ? 0 : -1;
}

int closespi(void)
{
    return 0;
}

void set_spi_speed_slow(void)
{
    (void)dwm3000_port_set_slow_spi();
}

void set_spi_speed_fast(void)
{
    (void)dwm3000_port_set_fast_spi();
}

int writetospiwithcrc(uint16_t headerLength,
                      const uint8_t *headerBuffer,
                      uint16_t bodyLength,
                      const uint8_t *bodyBuffer,
                      uint8_t crc8)
{
    return dwm3000_port_write_with_crc(headerBuffer, headerLength,
                                       bodyBuffer, bodyLength,
                                       crc8) == 0 ? 0 : -1;
}

int writetospi(uint16_t headerLength,
               const uint8_t *headerBuffer,
               uint16_t bodyLength,
               const uint8_t *bodyBuffer)
{
    return dwm3000_port_write(headerBuffer, headerLength, bodyBuffer, bodyLength) == 0 ? 0 : -1;
}

int readfromspi(uint16_t headerLength,
                const uint8_t *headerBuffer,
                uint16_t readLength,
                uint8_t *readBuffer)
{
    return dwm3000_port_read(headerBuffer, headerLength, readBuffer, readLength) == 0 ? 0 : -1;
}

void Sleep(uint32_t delay_ms)
{
    k_msleep(delay_ms);
}

void deca_sleep(unsigned int time_ms)
{
    k_msleep(time_ms);
}

void deca_usleep(unsigned long time_us)
{
    k_busy_wait((uint32_t)time_us);
}

uint32_t HAL_GetTick(void)
{
    return k_uptime_get_32();
}

unsigned long portGetTickCnt(void)
{
    return (unsigned long)k_uptime_get_32();
}

int port_is_switch_on(uint16_t gpio_pin)
{
    ARG_UNUSED(gpio_pin);
    return 0;
}

int port_is_boot1_low(void)
{
    return 0;
}

void wakeup_device_with_io(void)
{
    (void)dwm3000_port_wakeup();
}

void port_wakeup_dw3000(void)
{
    (void)dwm3000_port_wakeup();
}

void port_wakeup_dw3000_fast(void)
{
    (void)dwm3000_port_wakeup();
}

void port_set_dw_ic_spi_slowrate(void)
{
    (void)dwm3000_port_set_slow_spi();
}

void port_set_dw_ic_spi_fastrate(void)
{
    (void)dwm3000_port_set_fast_spi();
}

void reset_DWIC(void)
{
    (void)dwm3000_port_set_slow_spi();
    (void)dwm3000_port_hw_reset();
    (void)dwm3000_port_set_fast_spi();
}

void setup_DW3000RSTnIRQ(int enable)
{
    ARG_UNUSED(enable);
}

void process_dwRSTn_irq(void)
{
}

void process_deca_irq(void)
{
    if (dwm3000_isr != NULL) {
        dwm3000_isr();
    }
}

void led_on(uint32_t led)
{
    ARG_UNUSED(led);
}

void led_off(uint32_t led)
{
    ARG_UNUSED(led);
}

int peripherals_init(void)
{
    return dwm3000_port_init() == 0 ? 0 : -1;
}

void spi_peripheral_init(void)
{
    (void)openspi();
}

void port_set_dwic_isr(port_deca_isr_t deca_isr)
{
    int ret;

    dwm3000_isr = deca_isr;

    ret = dwm3000_port_set_irq_callback(deca_isr);
    if (ret < 0) {
        LOG_ERR("DWM3000 IRQ callback setup failed: %d", ret);
    }
}
