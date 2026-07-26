#ifndef ZEPHYR_INIT_H_
#define ZEPHYR_INIT_H_

#define APPLICATION 0
#define SYS_INIT(function, level, priority) \
    static int (*const zephyr_shim_init_reference_##function)(void) \
        __attribute__((unused)) = function

#endif
