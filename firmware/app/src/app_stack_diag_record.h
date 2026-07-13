#ifndef APP_STACK_DIAG_RECORD_H
#define APP_STACK_DIAG_RECORD_H

#include <stdarg.h>
#include <stddef.h>

#define APP_STACK_DIAG_RECORD_CAPACITY 512u

int app_stack_diag_record_vformat(char *record,
                                  size_t record_capacity,
                                  const char *format,
                                  va_list args);
int app_stack_diag_record_format(char *record,
                                 size_t record_capacity,
                                 const char *format,
                                 ...);

#endif
