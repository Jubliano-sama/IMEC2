#ifndef APP_CLICKER_EVENT_RUNTIME_H
#define APP_CLICKER_EVENT_RUNTIME_H

#include "firmware_state_machines.h"
#include "status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The clicker action work queue is already a serialized owner.  This runtime
 * keeps the event/effect boundary at that owner instead of allocating the
 * device-wide dispatcher queue for a single battery role.
 */
#define APP_CLICKER_EVENT_RUNTIME_EFFECT_CAPACITY 4u

struct app_clicker_event_runtime {
    struct fw_button_sm button;
    struct fw_click_sm click;
    uint32_t button_pressed_at_ms;
    uint32_t button_armed_at_ms;
    uint32_t generation;
    struct fw_effect effects[APP_CLICKER_EVENT_RUNTIME_EFFECT_CAPACITY];
    uint8_t effect_head;
    uint8_t effect_count;
    uint32_t effect_drop_count;
};

void app_clicker_event_runtime_init(
    struct app_clicker_event_runtime *runtime);

int app_clicker_event_runtime_button_signal(
    struct app_clicker_event_runtime *runtime,
    enum button_signal signal,
    uint32_t now_ms,
    enum button_action *action);

int app_clicker_event_runtime_click_start(
    struct app_clicker_event_runtime *runtime,
    uint64_t operation_id);

int app_clicker_event_runtime_click_event(
    struct app_clicker_event_runtime *runtime,
    enum fw_event_type type,
    const struct fw_event_payload *payload);

bool app_clicker_event_runtime_take_effect(
    struct app_clicker_event_runtime *runtime,
    struct fw_effect *effect);

uint32_t app_clicker_event_runtime_effect_drop_count(
    const struct app_clicker_event_runtime *runtime);

enum fw_button_state app_clicker_event_runtime_button_state(
    const struct app_clicker_event_runtime *runtime);

enum fw_click_state app_clicker_event_runtime_click_state(
    const struct app_clicker_event_runtime *runtime);

uint32_t app_clicker_event_runtime_button_pressed_at_ms(
    const struct app_clicker_event_runtime *runtime);

#ifdef __cplusplus
}
#endif

#endif
