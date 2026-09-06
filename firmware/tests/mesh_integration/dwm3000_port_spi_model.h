#ifndef DWM3000_PORT_SPI_MODEL_H
#define DWM3000_PORT_SPI_MODEL_H

/* Only the platform surface used by the production DWM3000 port. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct device { const char *name; };
struct gpio_dt_spec {
    const struct device *port;
    uint32_t pin;
    uint32_t dt_flags;
};
struct spi_cs_control { struct gpio_dt_spec gpio; uint32_t delay; };
struct spi_config {
    uint32_t frequency;
    uint16_t operation;
    uint16_t slave;
    struct spi_cs_control cs;
};
struct spi_dt_spec { const struct device *bus; struct spi_config config; };
struct spi_buf { void *buf; size_t len; };
struct spi_buf_set { const struct spi_buf *buffers; size_t count; };
typedef struct { uint32_t FREQUENCY; } NRF_SPIM_Type;
extern const struct device model_spi_bus;
extern const struct device model_gpio_port;
extern NRF_SPIM_Type model_spim;

#define MODEL_SLOW_HZ 2000000u
#define MODEL_FAST_HZ 32000000u
#define MODEL_RESET_PIN 10u
#define MODEL_WAKE_PIN 11u
#define MODEL_CS_PIN 12u
#define DT_ALIAS(alias) 1
#define DT_BUS(node) 2
#define DT_NODE_HAS_STATUS(node, status) 1
#define DT_NODE_HAS_COMPAT(node, compat) 1
#define DT_PHANDLE_BY_IDX(node, prop, index) 3
#define DT_CHILD(node, child) 4
#define DT_PROP(node, prop) MODEL_DT_PROP_##prop
#define MODEL_DT_PROP_slow_spi_frequency MODEL_SLOW_HZ
#define MODEL_DT_PROP_fast_spi_frequency MODEL_FAST_HZ
#define MODEL_DT_PROP_spi_max_frequency MODEL_FAST_HZ
#define MODEL_DT_PROP_max_frequency MODEL_FAST_HZ
#define DT_PROP_LEN(node, prop) 3
#define DT_PROP_BY_IDX(node, prop, index) (20u + (index))
#define MODEL_CAT_(left, right) left##right
#define MODEL_CAT(left, right) MODEL_CAT_(left, right)
#define DT_REG_ADDR(node) MODEL_CAT(MODEL_DT_REG_ADDR_, node)
#define MODEL_DT_REG_ADDR_1 0u
#define MODEL_DT_REG_ADDR_2 ((uintptr_t)&model_spim)
#define GPIO_DT_SPEC_GET(node, prop) \
    { .port = &model_gpio_port, .pin = MODEL_GPIO_##prop }
#define MODEL_GPIO_reset_gpios MODEL_RESET_PIN
#define MODEL_GPIO_wakeup_gpios MODEL_WAKE_PIN
#define SPI_CS_CONTROL_INIT(node, delay_) \
    { .gpio = { .port = &model_gpio_port, .pin = MODEL_CS_PIN, \
                .dt_flags = 1u }, .delay = (delay_) }
#define SPI_WORD_SET(bits) ((bits) << 5)
#define SPI_TRANSFER_MSB 0u
#define SPI_CONFIG_DT(node, operation_, delay_) \
    { .frequency = MODEL_FAST_HZ, .operation = (operation_), .slave = 0u, \
      .cs = SPI_CS_CONTROL_INIT(node, delay_) }
#define SPI_DT_SPEC_GET(node, operation_, delay_) \
    { .bus = &model_spi_bus, .config = SPI_CONFIG_DT(node, operation_, delay_) }
#define NRF_PIN_POS 0u
#define NRF_PIN_MSK 0xffu
#define NRF_SPIM_PIN_NOT_CONNECTED UINT32_MAX
#define GPIO_OUTPUT_INACTIVE 1u
#define GPIO_OPEN_DRAIN 2u
#define GPIO_INPUT 4u
#define GPIO_DISCONNECTED 8u
#define BUILD_ASSERT(condition, message) _Static_assert(condition, message)
#define LOG_LEVEL_INF 1
#define LOG_MODULE_REGISTER(name, level) _Static_assert(1, "logging stub")
#define LOG_INF(...) ((void)0)

enum pm_device_action { PM_DEVICE_ACTION_RESUME, PM_DEVICE_ACTION_SUSPEND };
int pm_device_action_run(const struct device *device, enum pm_device_action action);
bool spi_is_ready_dt(const struct spi_dt_spec *spec);
bool gpio_is_ready_dt(const struct gpio_dt_spec *spec);
int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, uint32_t flags);
int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value);
void nrf_spim_disable(NRF_SPIM_Type *reg);
void nrf_spim_pins_set(NRF_SPIM_Type *reg, uint32_t sck, uint32_t mosi, uint32_t miso);
void nrf_gpio_cfg_default(uint32_t pin);
void k_msleep(uint32_t milliseconds);
void k_busy_wait(uint32_t microseconds);
int spi_transceive(const struct device *device, const struct spi_config *config,
                   const struct spi_buf_set *tx, const struct spi_buf_set *rx);
int spi_write(const struct device *device, const struct spi_config *config,
              const struct spi_buf_set *tx);

typedef int atomic_t;
typedef int atomic_val_t;
static inline bool atomic_cas(atomic_t *value, int before, int after)
{
    if (*value != before) { return false; }
    *value = after;
    return true;
}
static inline void atomic_clear(atomic_t *value) { *value = 0; }
static inline int atomic_set(atomic_t *value, int next)
{
    int previous = *value;
    *value = next;
    return previous;
}

#endif
