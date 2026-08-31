#include "clicker_rtt_command.h"

#include <string.h>

bool clicker_rtt_command_parse(const char *line,
                               size_t length,
                               enum clicker_rtt_command *command)
{
    if (line == NULL || command == NULL) {
        return false;
    }
    if (length == 5u && memcmp(line, "CLICK", 5u) == 0) {
        *command = CLICKER_RTT_COMMAND_CLICK;
        return true;
    }
    if (length == 4u && memcmp(line, "LONG", 4u) == 0) {
        *command = CLICKER_RTT_COMMAND_LONG;
        return true;
    }
    if (length == 5u && memcmp(line, "READY", 5u) == 0) {
        *command = CLICKER_RTT_COMMAND_READY;
        return true;
    }
    return false;
}
