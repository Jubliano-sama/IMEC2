#include "app_stack_diag_record.h"

#include <errno.h>
#include <stdio.h>

int app_stack_diag_record_vformat(char *record,
                                  size_t record_capacity,
                                  const char *format,
                                  va_list args)
{
    int length;

    if (record == NULL || record_capacity == 0u || format == NULL) {
        return -EINVAL;
    }
    length = vsnprintf(record, record_capacity, format, args);
    if (length <= 0) {
        record[0] = '\0';
        return -EINVAL;
    }
    if ((size_t)length >= record_capacity) {
        record[0] = '\0';
        return -EMSGSIZE;
    }
    if (record[length - 1] != '\n') {
        record[0] = '\0';
        return -EBADMSG;
    }
    return length;
}

int app_stack_diag_record_format(char *record,
                                 size_t record_capacity,
                                 const char *format,
                                 ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = app_stack_diag_record_vformat(record, record_capacity, format,
                                           args);
    va_end(args);
    return result;
}
