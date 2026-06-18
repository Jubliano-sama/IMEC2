#include "dwm3000_driver.h"
#include "dwm3000_port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(uwb_smoke_test, LOG_LEVEL_INF);

#ifndef DEVICE_ID
#define DEVICE_ID 0x1111222233334444ull
#endif

#define STATUS_GREEN_NODE DT_ALIAS(status0_green)
#define STATUS_BLUE_NODE DT_ALIAS(status0_blue)
#define STATUS_RED_NODE DT_ALIAS(status0_red)

#define TX_PERIOD_MS 5000u
#define TX_TIMEOUT_MS 100u
#define RX_WINDOW_MS 6000u
#define SMOKE_FRAME_LEN 28u
#define SMOKE_VERSION 1u

static const uint8_t smoke_magic[8] = { 'I', 'M', 'E', 'C', 'U', 'W', 'B', '0' };

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(STATUS_GREEN_NODE, gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(STATUS_BLUE_NODE, gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(STATUS_RED_NODE, gpios);

static void led_set(const struct gpio_dt_spec *led, bool on)
{
	if (gpio_is_ready_dt(led)) {
		(void)gpio_pin_set_dt(led, on ? 1 : 0);
	}
}

static void flash_led(const struct gpio_dt_spec *led, uint32_t ms)
{
	led_set(led, true);
	k_msleep(ms);
	led_set(led, false);
}

static void configure_leds(void)
{
	if (gpio_is_ready_dt(&led_green)) {
		(void)gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	}
	if (gpio_is_ready_dt(&led_blue)) {
		(void)gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
	}
	if (gpio_is_ready_dt(&led_red)) {
		(void)gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	}
}

#if defined(UWB_SMOKE_TX)
static void encode_smoke_frame(uint32_t seq, uint8_t frame[SMOKE_FRAME_LEN])
{
	memset(frame, 0, SMOKE_FRAME_LEN);
	memcpy(frame, smoke_magic, sizeof(smoke_magic));
	frame[8] = SMOKE_VERSION;
	frame[9] = 'T';
	sys_put_le16(0u, &frame[10]);
	sys_put_le32(seq, &frame[12]);
	sys_put_le32(k_uptime_get_32(), &frame[16]);
	sys_put_le64(DEVICE_ID, &frame[20]);
}
#endif

#if defined(UWB_SMOKE_RX)
static bool decode_smoke_frame(const uint8_t *frame, size_t len,
			       uint32_t *seq, uint32_t *uptime_ms,
			       uint64_t *sender_id)
{
	if (frame == NULL || len < SMOKE_FRAME_LEN) {
		return false;
	}
	if (memcmp(frame, smoke_magic, sizeof(smoke_magic)) != 0 ||
	    frame[8] != SMOKE_VERSION) {
		return false;
	}

	if (seq != NULL) {
		*seq = sys_get_le32(&frame[12]);
	}
	if (uptime_ms != NULL) {
		*uptime_ms = sys_get_le32(&frame[16]);
	}
	if (sender_id != NULL) {
		*sender_id = sys_get_le64(&frame[20]);
	}
	return true;
}
#endif

static int configure_radio(void)
{
	uint32_t dev_id = 0u;
	int ret;

	ret = dwm3000_port_init();
	if (ret < 0) {
		LOG_ERR("DWM3000 port init failed: %d", ret);
		return ret;
	}

	ret = dwm3000_port_wakeup();
	if (ret < 0) {
		LOG_ERR("DWM3000 wake failed: %d", ret);
		return ret;
	}

	ret = dwm3000_port_hw_reset();
	if (ret < 0) {
		LOG_ERR("DWM3000 reset failed: %d", ret);
		return ret;
	}

	ret = dwm3000_driver_probe(&dev_id);
	if (ret < 0) {
		LOG_ERR("DWM3000 DEV_ID probe failed: %d", ret);
		return ret;
	}
	LOG_INF("DWM3000 DEV_ID OK: 0x%08" PRIx32, dev_id);

	ret = dwm3000_driver_configure_default();
	if (ret < 0) {
		LOG_ERR("DWM3000 default config failed: %d", ret);
		return ret;
	}

	return 0;
}

#if defined(UWB_SMOKE_TX)
static void run_tx(void)
{
	uint8_t frame[SMOKE_FRAME_LEN];
	uint32_t seq = 0u;

	LOG_INF("UWB smoke TX active: device=0x%016" PRIx64 " period_ms=%u",
		(uint64_t)DEVICE_ID, TX_PERIOD_MS);

	while (true) {
		int ret;

		encode_smoke_frame(seq, frame);
		ret = dwm3000_driver_send_frame(frame, sizeof(frame), TX_TIMEOUT_MS);
		if (ret == 0) {
			LOG_INF("TX_OK seq=%" PRIu32 " len=%u", seq, (unsigned int)sizeof(frame));
			flash_led(&led_green, 60u);
		} else {
			LOG_ERR("TX_FAIL seq=%" PRIu32 " ret=%d", seq, ret);
			flash_led(&led_red, 120u);
		}
		seq++;
		k_msleep(TX_PERIOD_MS);
	}
}
#endif

#if defined(UWB_SMOKE_RX)
static const char *rx_failure_name(enum dwm3000_rx_failure failure)
{
	switch (failure) {
	case DWM3000_RX_FAILURE_NONE:
		return "none";
	case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
		return "no_preamble";
	case DWM3000_RX_FAILURE_SFD_TIMEOUT:
		return "sfd_timeout";
	case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
		return "frame_timeout";
	case DWM3000_RX_FAILURE_CRC_OR_PHY:
		return "crc_or_phy";
	case DWM3000_RX_FAILURE_BAD_FRAME:
		return "bad_frame";
	default:
		return "unknown";
	}
}

static void run_rx(void)
{
	uint8_t frame[64];
	size_t frame_len = 0u;
	uint8_t quality = 0u;
	int8_t rsl_dbm = 0;
	uint32_t rx_count = 0u;

	LOG_INF("UWB smoke RX active: device=0x%016" PRIx64 " window_ms=%u",
		(uint64_t)DEVICE_ID, RX_WINDOW_MS);

	while (true) {
		enum dwm3000_rx_failure failure = DWM3000_RX_FAILURE_NONE;
		uint32_t seq = 0u;
		uint32_t sender_uptime_ms = 0u;
		uint64_t sender_id = 0u;
		bool decoded;
		int ret;

		frame_len = 0u;
		quality = 0u;
		rsl_dbm = 0;
		ret = dwm3000_driver_receive_frame_detailed(RX_WINDOW_MS,
							    frame,
							    sizeof(frame),
							    &frame_len,
							    &quality,
							    &rsl_dbm,
							    &failure);
		if (ret == -ETIMEDOUT) {
			LOG_INF("RX_TIMEOUT window_ms=%u failure=%s",
				RX_WINDOW_MS, rx_failure_name(failure));
			continue;
		}
		if (ret < 0) {
			LOG_WRN("RX_FAIL ret=%d failure=%s", ret, rx_failure_name(failure));
			flash_led(&led_red, 80u);
			continue;
		}

		decoded = decode_smoke_frame(frame, frame_len, &seq, &sender_uptime_ms,
					     &sender_id);
		rx_count++;
		LOG_INF("RX_OK count=%" PRIu32 " decoded=%u seq=%" PRIu32
			" sender=0x%016" PRIx64 " sender_uptime_ms=%" PRIu32
			" len=%u quality=%u rsl_dbm=%d",
			rx_count, decoded ? 1u : 0u, seq, sender_id, sender_uptime_ms,
			(unsigned int)frame_len, quality, (int)rsl_dbm);
		flash_led(decoded ? &led_blue : &led_red, decoded ? 60u : 120u);
	}
}
#endif

int main(void)
{
	int ret;

	configure_leds();
	LOG_INF("IMEC UWB smoke test boot");

	ret = configure_radio();
	if (ret < 0) {
		LOG_ERR("radio setup failed: %d", ret);
		led_set(&led_red, true);
		return 0;
	}

#if defined(UWB_SMOKE_TX)
	run_tx();
#elif defined(UWB_SMOKE_RX)
	run_rx();
#else
#error "Build must define UWB_SMOKE_TX or UWB_SMOKE_RX"
#endif

	return 0;
}
