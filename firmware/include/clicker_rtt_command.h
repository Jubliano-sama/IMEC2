#ifndef CLICKER_RTT_COMMAND_H
#define CLICKER_RTT_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

enum clicker_rtt_command {
    CLICKER_RTT_COMMAND_CLICK = 1,
    CLICKER_RTT_COMMAND_LONG = 2,
};

bool clicker_rtt_command_parse(const char *line,
                               size_t length,
                               enum clicker_rtt_command *command);

#endif
