#include "clicker_rtt_command.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_exact_commands(void)
{
    enum clicker_rtt_command command;

    assert(clicker_rtt_command_parse("CLICK", 5u, &command));
    assert(command == CLICKER_RTT_COMMAND_CLICK);
    assert(clicker_rtt_command_parse("LONG", 4u, &command));
    assert(command == CLICKER_RTT_COMMAND_LONG);
    assert(clicker_rtt_command_parse("READY", 5u, &command));
    assert(command == CLICKER_RTT_COMMAND_READY);
}

static void test_noncanonical_input_is_rejected(void)
{
    enum clicker_rtt_command command = CLICKER_RTT_COMMAND_CLICK;

    assert(!clicker_rtt_command_parse("click", 5u, &command));
    assert(!clicker_rtt_command_parse("CLICK ", 6u, &command));
    assert(!clicker_rtt_command_parse("LONG\n", 5u, &command));
    assert(!clicker_rtt_command_parse("ready", 5u, &command));
    assert(!clicker_rtt_command_parse("", 0u, &command));
    assert(!clicker_rtt_command_parse(NULL, 0u, &command));
    assert(!clicker_rtt_command_parse("CLICK", 5u, NULL));
}

int main(void)
{
    test_exact_commands();
    test_noncanonical_input_is_rejected();
    puts("clicker_rtt_command: ok");
    return 0;
}
