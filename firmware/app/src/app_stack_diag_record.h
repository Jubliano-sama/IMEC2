#ifndef APP_STACK_DIAG_RECORD_H
#define APP_STACK_DIAG_RECORD_H

#include <stdarg.h>
#include <stddef.h>

/*
 * The longest legal typed record is DBG_STACK_RUN_END with every numeric
 * field at its wire-type maximum and the longest enum names selected. Keep a
 * small alignment-friendly margin while making the static RAM cost explicit.
 */
#define APP_STACK_DIAG_MAX_FORMATTED_LENGTH 368u
#define APP_STACK_DIAG_RECORD_CAPACITY 384u

#if APP_STACK_DIAG_RECORD_CAPACITY <= APP_STACK_DIAG_MAX_FORMATTED_LENGTH
#error "stack diagnostic record must retain room for the terminating NUL"
#endif

int app_stack_diag_record_vformat(char *record,
                                  size_t record_capacity,
                                  const char *format,
                                  va_list args);
int app_stack_diag_record_format(char *record,
                                 size_t record_capacity,
                                 const char *format,
                                 ...);

#endif
