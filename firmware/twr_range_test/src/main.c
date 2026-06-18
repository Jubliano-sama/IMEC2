#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "uwb_session.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(twr_range_test, LOG_LEVEL_INF);

#define STATUS_GREEN_NODE DT_ALIAS(status0_green)
#define STATUS_BLUE_NODE DT_ALIAS(status0_blue)
#define STATUS_RED_NODE DT_ALIAS(status0_red)

#ifndef TWR_RANGE_PERIOD_MS
#define TWR_RANGE_PERIOD_MS 1000u
#endif
#ifndef TWR_LED_FLASH
#define TWR_LED_FLASH 1
#endif
#define INITIATOR_TIMEOUT_MS 80u
#define RESPONDER_SLICE_MS 250u
#define RESPONDER_REPORT_PERIOD_MS 5000u
#define RANGE_SEQ 1u
#define RANGE_FLAGS FLAG_DIAGNOSTIC
#ifndef TWR_DELAY_CALIBRATION
#define TWR_DELAY_CALIBRATION 0
#endif
#ifndef TWR_REPLY_DELAY_CALIBRATION_MAX_UUS
#define TWR_REPLY_DELAY_CALIBRATION_MAX_UUS DWM3000_DS_TWR_REPLY_DLY_UUS
#endif
#ifndef TWR_REPLY_DELAY_CALIBRATION_MIN_UUS
#define TWR_REPLY_DELAY_CALIBRATION_MIN_UUS DWM3000_DS_TWR_REPLY_DLY_UUS
#endif
#ifndef TWR_REPLY_DELAY_CALIBRATION_STEP_UUS
#define TWR_REPLY_DELAY_CALIBRATION_STEP_UUS 1u
#endif
#ifndef TWR_REPLY_DELAY_INITIAL_STEP_UUS
#define TWR_REPLY_DELAY_INITIAL_STEP_UUS TWR_REPLY_DELAY_CALIBRATION_STEP_UUS
#endif
#ifndef TWR_REPLY_DELAY_TESTS_PER_STEP
#define TWR_REPLY_DELAY_TESTS_PER_STEP 10u
#endif
#ifndef TWR_REPLY_DELAY_REQUIRED_SUCCESSES
#define TWR_REPLY_DELAY_REQUIRED_SUCCESSES 9u
#endif
#ifndef TWR_CAPTURE_RSL
#define TWR_CAPTURE_RSL 1
#endif
#ifndef TWR_CLICKER_DIAG
#define TWR_CLICKER_DIAG 1
#endif
#ifndef TWR_LOG_EACH_RANGE
#define TWR_LOG_EACH_RANGE 1
#endif

#if TWR_DELAY_CALIBRATION
BUILD_ASSERT(TWR_REPLY_DELAY_CALIBRATION_MAX_UUS >=
	     TWR_REPLY_DELAY_CALIBRATION_MIN_UUS,
	     "Calibration maximum delay must be >= minimum delay");
BUILD_ASSERT(TWR_REPLY_DELAY_CALIBRATION_STEP_UUS > 0u,
	     "Calibration delay step must be nonzero");
BUILD_ASSERT(TWR_REPLY_DELAY_INITIAL_STEP_UUS >= TWR_REPLY_DELAY_CALIBRATION_STEP_UUS,
	     "Initial calibration jump must be at least one calibration step");
BUILD_ASSERT((TWR_REPLY_DELAY_INITIAL_STEP_UUS %
	      TWR_REPLY_DELAY_CALIBRATION_STEP_UUS) == 0u,
	     "Initial calibration jump must align to the calibration step");
BUILD_ASSERT(TWR_REPLY_DELAY_TESTS_PER_STEP > 0u,
	     "Calibration attempts per step must be nonzero");
BUILD_ASSERT(TWR_REPLY_DELAY_REQUIRED_SUCCESSES <= TWR_REPLY_DELAY_TESTS_PER_STEP,
	     "Calibration required successes must fit in the test window");
BUILD_ASSERT(((TWR_REPLY_DELAY_CALIBRATION_MAX_UUS -
	       TWR_REPLY_DELAY_CALIBRATION_MIN_UUS) /
	      TWR_REPLY_DELAY_CALIBRATION_STEP_UUS) <= UINT8_MAX,
	     "Calibration range must fit in round_index");
#endif

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(STATUS_GREEN_NODE, gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(STATUS_BLUE_NODE, gpios);
static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(STATUS_RED_NODE, gpios);

static void led_set(const struct gpio_dt_spec *led, bool on)
{
	if (gpio_is_ready_dt(led)) {
		(void)gpio_pin_set_dt(led, on ? 1 : 0);
	}
}

static void led_set_rgb(bool red, bool green, bool blue)
{
	led_set(&led_red, red);
	led_set(&led_green, green);
	led_set(&led_blue, blue);
}

static void flash_rgb(bool red, bool green, bool blue, uint32_t ms)
{
#if TWR_LED_FLASH
	led_set_rgb(red, green, blue);
	k_msleep(ms);
	led_set_rgb(false, false, false);
#else
	(void)red;
	(void)green;
	(void)blue;
	(void)ms;
#endif
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

#if TWR_LOG_EACH_RANGE
static const char *range_status_name(enum range_status status)
{
	switch (status) {
	case RANGE_OK:
		return "ok";
	case RANGE_RX_TIMEOUT:
		return "rx_timeout";
	case RANGE_RX_ERROR:
		return "rx_error";
	case RANGE_BAD_FRAME:
		return "bad_frame";
	case RANGE_WRONG_TARGET:
		return "wrong_target";
	case RANGE_STS_QUALITY_FAIL:
		return "sts_quality_fail";
	case RANGE_DELAYED_TX_MISSED:
		return "delayed_tx_missed";
	case RANGE_INTERNAL_ERROR:
		return "internal_error";
	case RANGE_TIMING_INVALID:
		return "timing_invalid";
	default:
		return "unknown";
	}
}
#endif

static uint8_t calibration_max_delay_index(void)
{
#if TWR_DELAY_CALIBRATION
	uint32_t span_uus = TWR_REPLY_DELAY_CALIBRATION_MAX_UUS -
			    TWR_REPLY_DELAY_CALIBRATION_MIN_UUS;
	uint32_t max_index = span_uus / TWR_REPLY_DELAY_CALIBRATION_STEP_UUS;

	return max_index > UINT8_MAX ? UINT8_MAX : (uint8_t)max_index;
#else
	return 0u;
#endif
}

static uint16_t reply_delay_uus_from_index(uint8_t delay_index)
{
#if TWR_DELAY_CALIBRATION
	uint32_t max_index = calibration_max_delay_index();
	uint32_t index = MIN((uint32_t)delay_index, max_index);
	uint32_t delay_uus = TWR_REPLY_DELAY_CALIBRATION_MAX_UUS -
			     (index * TWR_REPLY_DELAY_CALIBRATION_STEP_UUS);

	if (delay_uus < TWR_REPLY_DELAY_CALIBRATION_MIN_UUS) {
		delay_uus = TWR_REPLY_DELAY_CALIBRATION_MIN_UUS;
	}
	return (uint16_t)delay_uus;
#else
	ARG_UNUSED(delay_index);
	return DWM3000_DS_TWR_REPLY_DLY_UUS;
#endif
}

#if TWR_DELAY_CALIBRATION && defined(TWR_RANGE_INITIATOR)
static uint16_t calibration_initial_probe_step(void)
{
	uint32_t initial_step = TWR_REPLY_DELAY_INITIAL_STEP_UUS /
				TWR_REPLY_DELAY_CALIBRATION_STEP_UUS;

	if (initial_step == 0u) {
		initial_step = 1u;
	}
	return initial_step > UINT16_MAX ? UINT16_MAX : (uint16_t)initial_step;
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

	ret = dwm3000_driver_configure_range_mode();
	if (ret < 0) {
		LOG_ERR("DWM3000 range config failed: %d", ret);
		return ret;
	}

	return 0;
}

static void fill_request(struct dwm3000_range_request *request, uint8_t seq)
{
	memset(request, 0, sizeof(*request));
	request->initiator_id = TWR_INITIATOR_ID;
	request->responder_id = TWR_RESPONDER_ID;
	request->network_id = TWR_NETWORK_ID;
	request->session_nonce = TWR_SESSION_NONCE;
	request->responder_short_addr = uwb_session_short_addr_from_id(TWR_RESPONDER_ID);
	request->session_id = TWR_SESSION_ID;
	request->seq = seq;
	request->round_index = 0u;
	request->flags = RANGE_FLAGS;
	request->timeout_ms = INITIATOR_TIMEOUT_MS;
	request->capture_rsl = TWR_CAPTURE_RSL != 0;
}

static void flash_range_failure(enum range_status status, bool exchange_started)
{
	if (status == RANGE_DELAYED_TX_MISSED) {
		flash_rgb(true, true, false, 90u); /* yellow: missed delayed TX slot */
	} else if (exchange_started && status == RANGE_RX_TIMEOUT) {
		flash_rgb(true, false, true, 90u); /* magenta: poll seen, later RX timeout */
	} else if (exchange_started) {
		flash_rgb(false, true, true, 90u); /* cyan: poll seen, non-timeout failure */
	} else {
		flash_rgb(true, false, false, 90u); /* red: no valid exchange started */
	}
}

#if defined(TWR_RANGE_INITIATOR)
#if TWR_DELAY_CALIBRATION
enum calibration_phase {
	CAL_PHASE_EXPONENTIAL = 0,
	CAL_PHASE_BINARY = 1,
	CAL_PHASE_HOLD = 2,
};

static const char *calibration_phase_name(enum calibration_phase phase)
{
	switch (phase) {
	case CAL_PHASE_EXPONENTIAL:
		return "exponential";
	case CAL_PHASE_BINARY:
		return "binary";
	case CAL_PHASE_HOLD:
		return "hold";
	default:
		return "unknown";
	}
}

static uint8_t calibration_midpoint_index(uint8_t reliable_index,
					  uint16_t failed_index)
{
	uint16_t delta = failed_index - reliable_index;
	uint16_t candidate = reliable_index + (delta / 2u);

	if (candidate <= reliable_index && failed_index > (uint16_t)reliable_index + 1u) {
		candidate = (uint16_t)reliable_index + 1u;
	}
	return candidate > UINT8_MAX ? UINT8_MAX : (uint8_t)candidate;
}
#endif

static void run_initiator(void)
{
	uint32_t attempt = 0u;
	uint8_t delay_index = 0u;
	uint32_t step_attempts = 0u;
	uint32_t step_successes = 0u;
	uint16_t best_reliable_delay_uus = 0u;
	uint8_t max_delay_index = calibration_max_delay_index();
#if TWR_DELAY_CALIBRATION
	enum calibration_phase phase = CAL_PHASE_EXPONENTIAL;
	bool have_reliable_delay = false;
	uint8_t best_reliable_index = 0u;
	uint16_t first_failed_index = UINT16_MAX;
	uint16_t probe_step = calibration_initial_probe_step();
	uint32_t step_status_counts[RANGE_TIMING_INVALID + 1u] = {0};
	uint32_t step_exchange_started = 0u;
#endif

	LOG_INF("TWR initiator active: local=0x%016" PRIx64
		" responder=0x%016" PRIx64 " period_ms=%u reply_delay_uus=%u"
		" calibration=%u min_delay_uus=%u step_uus=%u initial_step_uus=%u"
		" tests_per_step=%u required_successes=%u",
		(uint64_t)DEVICE_ID, (uint64_t)TWR_RESPONDER_ID, TWR_RANGE_PERIOD_MS,
		(unsigned int)DWM3000_DS_TWR_REPLY_DLY_UUS,
		(unsigned int)TWR_DELAY_CALIBRATION,
		(unsigned int)TWR_REPLY_DELAY_CALIBRATION_MIN_UUS,
		(unsigned int)TWR_REPLY_DELAY_CALIBRATION_STEP_UUS,
		(unsigned int)TWR_REPLY_DELAY_INITIAL_STEP_UUS,
		(unsigned int)TWR_REPLY_DELAY_TESTS_PER_STEP,
		(unsigned int)TWR_REPLY_DELAY_REQUIRED_SUCCESSES);

	while (true) {
		struct dwm3000_range_request request;
		struct dwm3000_range_result result;
#if TWR_LOG_EACH_RANGE
		int64_t started_ms = k_uptime_get();
#endif
		uint16_t delay_uus = reply_delay_uus_from_index(delay_index);
		bool range_ok;
		int ret;

		fill_request(&request, RANGE_SEQ);
		request.round_index = delay_index;
		memset(&result, 0, sizeof(result));
		result.status = RANGE_RX_TIMEOUT;

		flash_rgb(false, false, true, 20u); /* blue: initiator poll attempt */
		ret = dwm3000_driver_range_initiator(&request, &result);
		range_ok = ret == 0 && result.status == RANGE_OK;
#if TWR_LOG_EACH_RANGE
		if (range_ok) {
			LOG_INF("TWR_RANGE ok=1 attempt=%" PRIu32 " seq=%u distance_mm=%d"
				" quality=%u rsl_dbm=%d elapsed_ms=%lld responder=0x%016" PRIx64
				" delay_uus=%u delay_index=%u",
				attempt, request.seq, result.distance_mm, result.quality,
				(int)result.rsl_dbm, (long long)(k_uptime_get() - started_ms),
				(uint64_t)result.responder_id, (unsigned int)delay_uus,
				(unsigned int)delay_index);
			flash_rgb(false, true, false, 90u); /* green: complete range */
		} else {
			LOG_WRN("TWR_RANGE ok=0 attempt=%" PRIu32 " seq=%u ret=%d"
				" status=%s(%u) quality=%u rsl_dbm=%d"
				" exchange_started=%u responder=0x%016" PRIx64
				" distance_mm=%d elapsed_ms=%lld"
				" delay_uus=%u delay_index=%u",
				attempt, request.seq, ret, range_status_name(result.status),
				(unsigned int)result.status, result.quality,
				(int)result.rsl_dbm, result.exchange_started ? 1u : 0u,
				(uint64_t)result.responder_id, result.distance_mm,
				(long long)(k_uptime_get() - started_ms),
				(unsigned int)delay_uus, (unsigned int)delay_index);
			flash_range_failure(result.status, result.exchange_started);
		}
#else
		if (range_ok) {
			flash_rgb(false, true, false, 20u);
		} else {
			flash_range_failure(result.status, result.exchange_started);
		}
#endif

#if TWR_DELAY_CALIBRATION
		step_attempts++;
		if ((unsigned int)result.status <= RANGE_TIMING_INVALID) {
			step_status_counts[result.status]++;
		}
		if (result.exchange_started) {
			step_exchange_started++;
		}
		if (range_ok) {
			step_successes++;
		}
		if (step_attempts >= TWR_REPLY_DELAY_TESTS_PER_STEP) {
			const char *decision;
			const char *phase_name = calibration_phase_name(phase);
			uint8_t next_delay_index = delay_index;
			bool reliable = step_successes >= TWR_REPLY_DELAY_REQUIRED_SUCCESSES;

			if (reliable) {
				have_reliable_delay = true;
				best_reliable_index = delay_index;
				best_reliable_delay_uus = delay_uus;
				if (phase == CAL_PHASE_HOLD) {
					decision = "hold_verified";
				} else if (delay_index >= max_delay_index) {
					phase = CAL_PHASE_HOLD;
					decision = "hold_min";
				} else if (phase == CAL_PHASE_EXPONENTIAL) {
					uint16_t candidate = (uint16_t)delay_index + probe_step;

					if (candidate > max_delay_index) {
						candidate = max_delay_index;
					}
					next_delay_index = (uint8_t)candidate;
					if (probe_step <= UINT16_MAX / 2u) {
						probe_step *= 2u;
					}
					decision = "exp_lower";
				} else if (first_failed_index <=
					   (uint16_t)best_reliable_index + 1u) {
					phase = CAL_PHASE_HOLD;
					next_delay_index = best_reliable_index;
					decision = "hold_best";
				} else {
					next_delay_index = calibration_midpoint_index(
						best_reliable_index, first_failed_index);
					decision = "binary_lower";
				}
			} else if (phase == CAL_PHASE_EXPONENTIAL) {
				first_failed_index = delay_index;
				probe_step = calibration_initial_probe_step();
				if (!have_reliable_delay) {
					next_delay_index = 0u;
					decision = "retry_max";
				} else if (first_failed_index <=
					   (uint16_t)best_reliable_index + 1u) {
					phase = CAL_PHASE_HOLD;
					next_delay_index = best_reliable_index;
					decision = "hold_best";
				} else {
					phase = CAL_PHASE_BINARY;
					next_delay_index = calibration_midpoint_index(
						best_reliable_index, first_failed_index);
					decision = "binary_start";
				}
			} else if (phase == CAL_PHASE_BINARY) {
				first_failed_index = delay_index;
				if (first_failed_index <= (uint16_t)best_reliable_index + 1u) {
					phase = CAL_PHASE_HOLD;
					next_delay_index = best_reliable_index;
					decision = "hold_best";
				} else {
					next_delay_index = calibration_midpoint_index(
						best_reliable_index, first_failed_index);
					decision = "binary_raise";
				}
			} else if (delay_index > 0u) {
				next_delay_index = delay_index - 1u;
				best_reliable_index = next_delay_index;
				best_reliable_delay_uus =
					reply_delay_uus_from_index(next_delay_index);
				decision = "hold_backoff";
			} else {
				decision = "hold_max";
			}

			LOG_INF("TWR_CAL delay_uus=%u delay_index=%u ok=%u/%u"
				" required_successes=%u phase=%s decision=%s"
				" next_phase=%s next_delay_uus=%u next_delay_index=%u"
				" best_reliable_delay_uus=%u best_reliable_index=%u"
				" first_failed_index=%u probe_step=%u"
				" exchange_started=%u status_ok=%u status_rx_timeout=%u"
				" status_rx_error=%u status_bad_frame=%u"
				" status_wrong_target=%u status_sts_quality=%u"
				" status_delayed_tx_missed=%u status_internal_error=%u"
				" status_timing_invalid=%u",
				(unsigned int)delay_uus, (unsigned int)delay_index,
				(unsigned int)step_successes,
				(unsigned int)step_attempts,
				(unsigned int)TWR_REPLY_DELAY_REQUIRED_SUCCESSES,
				phase_name,
				decision,
				calibration_phase_name(phase),
				(unsigned int)reply_delay_uus_from_index(next_delay_index),
				(unsigned int)next_delay_index,
				(unsigned int)best_reliable_delay_uus,
				(unsigned int)best_reliable_index,
				(unsigned int)first_failed_index,
				(unsigned int)probe_step,
				(unsigned int)step_exchange_started,
				(unsigned int)step_status_counts[RANGE_OK],
				(unsigned int)step_status_counts[RANGE_RX_TIMEOUT],
				(unsigned int)step_status_counts[RANGE_RX_ERROR],
				(unsigned int)step_status_counts[RANGE_BAD_FRAME],
				(unsigned int)step_status_counts[RANGE_WRONG_TARGET],
				(unsigned int)step_status_counts[RANGE_STS_QUALITY_FAIL],
				(unsigned int)step_status_counts[RANGE_DELAYED_TX_MISSED],
				(unsigned int)step_status_counts[RANGE_INTERNAL_ERROR],
				(unsigned int)step_status_counts[RANGE_TIMING_INVALID]);
			delay_index = next_delay_index;
			step_attempts = 0u;
			step_successes = 0u;
			memset(step_status_counts, 0, sizeof(step_status_counts));
			step_exchange_started = 0u;
		}
#endif
		attempt++;
		if (TWR_RANGE_PERIOD_MS > 0u) {
			k_msleep(TWR_RANGE_PERIOD_MS);
		}
	}
}
#endif

#if defined(TWR_RANGE_RESPONDER)
static void run_responder(void)
{
	uint32_t ok_count = 0u;
	uint32_t fail_count = 0u;
	uint32_t timeout_count = 0u;
	uint32_t started_count = 0u;
	int64_t next_report_ms = k_uptime_get() + RESPONDER_REPORT_PERIOD_MS;

	LOG_INF("TWR responder active: local=0x%016" PRIx64
		" initiator=0x%016" PRIx64 " listen_slice_ms=%u reply_delay_uus=%u"
		" calibration=%u min_delay_uus=%u step_uus=%u capture_rsl=%u"
		" clicker_diag=%u log_each_range=%u",
		(uint64_t)DEVICE_ID, (uint64_t)TWR_INITIATOR_ID, RESPONDER_SLICE_MS,
		(unsigned int)DWM3000_DS_TWR_REPLY_DLY_UUS,
		(unsigned int)TWR_DELAY_CALIBRATION,
		(unsigned int)TWR_REPLY_DELAY_CALIBRATION_MIN_UUS,
		(unsigned int)TWR_REPLY_DELAY_CALIBRATION_STEP_UUS,
		(unsigned int)TWR_CAPTURE_RSL,
		(unsigned int)TWR_CLICKER_DIAG,
		(unsigned int)TWR_LOG_EACH_RANGE);

	while (true) {
		struct dwm3000_range_request expected;
		struct dwm3000_range_result result;
		uint16_t delay_uus;
		int ret;

		fill_request(&expected, RANGE_SEQ);
		expected.timeout_ms = RESPONDER_SLICE_MS;
		memset(&result, 0, sizeof(result));
		result.status = RANGE_RX_TIMEOUT;

		ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
							     &expected,
							     RESPONDER_SLICE_MS,
							     &result);
		delay_uus = reply_delay_uus_from_index(result.round_index);
		if (ret == 0 && result.status == RANGE_OK) {
			ok_count++;
			started_count++;
#if TWR_LOG_EACH_RANGE
			LOG_INF("TWR_RESPONDER ok=1 count=%" PRIu32 " seq=%u"
				" distance_mm=%d quality=%u rsl_dbm=%d"
				" exchange_started=%u initiator=0x%016" PRIx64
				" delay_uus=%u delay_index=%u",
				ok_count, result.seq, result.distance_mm, result.quality,
				(int)result.rsl_dbm, result.exchange_started ? 1u : 0u,
				(uint64_t)result.initiator_id, (unsigned int)delay_uus,
				(unsigned int)result.round_index);
#endif
			flash_rgb(false, true, false, 90u); /* green: complete range */
		} else if (result.exchange_started) {
			fail_count++;
			started_count++;
#if TWR_LOG_EACH_RANGE
			LOG_WRN("TWR_RESPONDER ok=0 count=%" PRIu32 " ret=%d status=%s(%u)"
				" exchange_started=%u seq=%u initiator=0x%016" PRIx64
				" responder=0x%016" PRIx64 " distance_mm=%d"
				" quality=%u rsl_dbm=%d delay_uus=%u delay_index=%u",
				fail_count, ret, range_status_name(result.status),
				(unsigned int)result.status,
				result.exchange_started ? 1u : 0u, result.seq,
				(uint64_t)result.initiator_id, (uint64_t)result.responder_id,
				result.distance_mm, result.quality, (int)result.rsl_dbm,
				(unsigned int)delay_uus, (unsigned int)result.round_index);
#endif
			flash_range_failure(result.status, true);
		} else if (ret == -ETIMEDOUT) {
			timeout_count++;
		} else if (ret != -ETIMEDOUT) {
			fail_count++;
#if TWR_LOG_EACH_RANGE
			LOG_WRN("TWR_RESPONDER ok=0 count=%" PRIu32 " ret=%d status=%s(%u)"
				" exchange_started=%u seq=%u initiator=0x%016" PRIx64
				" responder=0x%016" PRIx64 " distance_mm=%d"
				" quality=%u rsl_dbm=%d",
				fail_count, ret, range_status_name(result.status),
				(unsigned int)result.status,
				result.exchange_started ? 1u : 0u, result.seq,
				(uint64_t)result.initiator_id, (uint64_t)result.responder_id,
				result.distance_mm, result.quality, (int)result.rsl_dbm);
#endif
			flash_range_failure(result.status, false);
		}

		if (k_uptime_get() >= next_report_ms) {
			LOG_INF("TWR_RESPONDER_IDLE ok_count=%" PRIu32
				" fail_count=%" PRIu32 " timeout_count=%" PRIu32
				" exchange_started_count=%" PRIu32
				" waiting_for=0x%016" PRIx64,
				ok_count, fail_count, timeout_count, started_count,
				(uint64_t)TWR_INITIATOR_ID);
			if (started_count == 0u) {
				flash_rgb(true, true, true, 25u); /* white: alive, idle/listening */
			}
			next_report_ms = k_uptime_get() + RESPONDER_REPORT_PERIOD_MS;
		}
	}
}
#endif

int main(void)
{
	int ret;

	configure_leds();
	LOG_INF("IMEC DS-TWR range test boot");

	ret = configure_radio();
	if (ret < 0) {
		LOG_ERR("radio setup failed: %d", ret);
		led_set(&led_red, true);
		return 0;
	}
	flash_rgb(true, true, true, 80u); /* white: boot and DWM3000 init OK */

#if defined(TWR_RANGE_INITIATOR)
	run_initiator();
#elif defined(TWR_RANGE_RESPONDER)
	run_responder();
#else
#error "Build must define TWR_RANGE_INITIATOR or TWR_RANGE_RESPONDER"
#endif

	return 0;
}
