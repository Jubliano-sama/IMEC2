#include "app_clicker_rtt_control.h"

#include "app_board.h"
#include "app_clicker.h"
#include "clicker_rtt_command.h"

#include <SEGGER_RTT.h>
#include <zephyr/kernel.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLICKER_RTT_POLL_MS 20u
#define CLICKER_RTT_LINE_CAPACITY 8u
#define CLICKER_RTT_READ_CAPACITY 16u

static struct k_work_delayable app_clicker_rtt_poll_work;
static char app_clicker_rtt_line[CLICKER_RTT_LINE_CAPACITY];
static size_t app_clicker_rtt_line_length;
static bool app_clicker_rtt_discard_line;
static bool app_clicker_rtt_started;

static void app_clicker_rtt_dispatch_line(void)
{
    enum app_clicker_test_gesture gesture;
    enum clicker_rtt_command command;
    const char *name;
    int ret;

    if (app_clicker_rtt_discard_line ||
        !clicker_rtt_command_parse(app_clicker_rtt_line,
                                   app_clicker_rtt_line_length,
                                   &command)) {
        status_debug_printf("DBG_CLICKER_RTT command=INVALID accepted=0");
        return;
    }

    if (command == CLICKER_RTT_COMMAND_READY) {
        status_debug_printf("DBG_CLICKER_RTT ready=1 commands=CLICK,LONG,READY");
        return;
    }

    if (command == CLICKER_RTT_COMMAND_CLICK) {
        gesture = APP_CLICKER_TEST_GESTURE_SHORT;
        name = "CLICK";
    } else {
        gesture = APP_CLICKER_TEST_GESTURE_LONG;
        name = "LONG";
    }
    ret = app_clicker_inject_button_gesture(gesture);
    status_debug_printf("DBG_CLICKER_RTT command=%s accepted=%u ret=%d",
                        name,
                        ret == 0 ? 1u : 0u,
                        ret);
}

static void app_clicker_rtt_consume(uint8_t byte)
{
    if (byte == '\r') {
        return;
    }
    if (byte == '\n') {
        app_clicker_rtt_dispatch_line();
        app_clicker_rtt_line_length = 0u;
        app_clicker_rtt_discard_line = false;
        return;
    }
    if (app_clicker_rtt_discard_line) {
        return;
    }
    if (app_clicker_rtt_line_length >= sizeof(app_clicker_rtt_line)) {
        app_clicker_rtt_discard_line = true;
        return;
    }
    app_clicker_rtt_line[app_clicker_rtt_line_length++] = (char)byte;
}

static void app_clicker_rtt_poll_work_handler(struct k_work *work)
{
    uint8_t bytes[CLICKER_RTT_READ_CAPACITY];
    unsigned int count;
    int ret;

    ARG_UNUSED(work);

    do {
        count = SEGGER_RTT_Read(0u, bytes, sizeof(bytes));
        for (unsigned int index = 0u; index < count; index++) {
            app_clicker_rtt_consume(bytes[index]);
        }
    } while (count == sizeof(bytes));

    ret = k_work_reschedule(&app_clicker_rtt_poll_work,
                            K_MSEC(CLICKER_RTT_POLL_MS));
    if (ret < 0) {
        status_debug_printf("DBG_CLICKER_RTT poll_schedule_failed=%d", ret);
    }
}

int app_clicker_rtt_control_start(void)
{
    int ret;

    if (app_clicker_rtt_started) {
        return -EALREADY;
    }
    app_clicker_rtt_line_length = 0u;
    app_clicker_rtt_discard_line = false;
    k_work_init_delayable(&app_clicker_rtt_poll_work,
                          app_clicker_rtt_poll_work_handler);
    ret = k_work_reschedule(&app_clicker_rtt_poll_work,
                            K_NO_WAIT);
    if (ret < 0) {
        return ret;
    }
    app_clicker_rtt_started = true;
    status_debug_printf("DBG_CLICKER_RTT ready=1 commands=CLICK,LONG,READY");
    return 0;
}
