#include "app_click_event_sequence.h"

#include "posix_board_if.h"

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <stdint.h>

BUILD_ASSERT(APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR == 0x01000000u,
             "native migration fixture expects the documented install floor");
BUILD_ASSERT(APP_CLICK_EVENT_SEQUENCE_MAX_BOOT_RESERVATIONS == 16711679u,
             "native migration fixture expects the documented boot bound");

int main(void)
{
    uint32_t first;
    uint32_t second;
    int ret;

    ret = app_click_event_sequence_init();
    if (ret < 0) {
        printk("CLICK_EVENT_SEQUENCE init_failed=%d\n", ret);
        posix_exit(1);
        return 1;
    }
    ret = app_click_event_sequence_next(&first);
    if (ret < 0) {
        printk("CLICK_EVENT_SEQUENCE first_failed=%d\n", ret);
        posix_exit(2);
        return 2;
    }
    ret = app_click_event_sequence_next(&second);
    if (ret < 0) {
        printk("CLICK_EVENT_SEQUENCE second_failed=%d\n", ret);
        posix_exit(3);
        return 3;
    }
    if (first == 0u || second != first + 1u ||
        ((first - 1u) % APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE) != 0u) {
        printk("CLICK_EVENT_SEQUENCE invalid first=%u second=%u\n",
               first,
               second);
        posix_exit(4);
        return 4;
    }

    printk("CLICK_EVENT_SEQUENCE first=%u second=%u block=%u max_boots=%u\n",
           first,
           second,
           APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE,
           APP_CLICK_EVENT_SEQUENCE_MAX_BOOT_RESERVATIONS);
    posix_exit(0);
    return 0;
}
