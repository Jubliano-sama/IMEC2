#ifndef STATUS_H
#define STATUS_H

#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_DEBOUNCE_MS 50u
#define BUTTON_LONG_PRESS_MS 1500u
#define SELF_TEST_ARM_WINDOW_MS 3000u
#define STATUS_PASS_DURATION_MS 2000u
#define STATUS_FAILURE_REPEAT_COUNT 3u

enum button_signal {
    BUTTON_SIGNAL_PRESS = 1,
    BUTTON_SIGNAL_RELEASE = 2,
    BUTTON_SIGNAL_TICK = 3,
};

enum button_action {
    BUTTON_ACTION_NONE = 0,
    BUTTON_ACTION_NORMAL_CLICK = 1,
    BUTTON_ACTION_SELF_TEST_ARMED = 2,
    BUTTON_ACTION_SELF_TEST_START = 3,
    BUTTON_ACTION_SELF_TEST_CANCELLED = 4,
};

enum status_pattern {
    STATUS_PATTERN_OFF = 0,
    STATUS_PATTERN_BLUE_PULSE = 1,
    STATUS_PATTERN_BLUE_CHASE = 2,
    STATUS_PATTERN_GREEN_SOLID = 3,
    STATUS_PATTERN_AMBER_BLINK_ONCE = 4,
    STATUS_PATTERN_AMBER_SLOW_BLINK = 5,
    STATUS_PATTERN_GREEN_SLOW_BLINK = 6,
    STATUS_PATTERN_RED_BLINK_CODE = 7,
};

enum self_test_failure {
    SELF_TEST_FAILURE_NONE = 0,
    SELF_TEST_FAILURE_BATTERY = 1,
    SELF_TEST_FAILURE_DISCOVERY = 2,
    SELF_TEST_FAILURE_DWM3000 = 3,
    SELF_TEST_FAILURE_NO_ANCHOR = 4,
    SELF_TEST_FAILURE_UWB = 5,
    SELF_TEST_FAILURE_INTERNAL = 6,
};

enum click_failure {
    CLICK_FAILURE_NONE = 0,
    CLICK_FAILURE_NO_ANCHOR = 1,
    CLICK_FAILURE_INSUFFICIENT_RANGES = 2,
};

struct button_fsm {
    bool pressed;
    bool armed;
    bool confirm_press;
    bool have_last_edge_ms;
    uint32_t pressed_at_ms;
    uint32_t armed_at_ms;
    uint32_t last_edge_ms;
};

struct status_inputs {
    bool self_test_armed;
    bool self_test_running;
    bool self_test_passed;
    bool click_accepted;
    bool low_battery;
    bool charging;
    bool charged;
    enum self_test_failure failure;
    enum click_failure click_failure;
};

struct status_indication {
    enum status_pattern pattern;
    uint8_t red_blink_count;
    uint8_t repeat_count;
    uint32_t duration_ms;
};

void button_fsm_init(struct button_fsm *fsm);
int button_fsm_handle(struct button_fsm *fsm,
                           enum button_signal signal,
                           uint32_t now_ms,
                           enum button_action *action);
int status_select(const struct status_inputs *inputs,
                       struct status_indication *indication);

#ifdef __cplusplus
}
#endif

#endif
