#include "button_action_handoff.h"

#include <assert.h>

static enum button_action indexed_action(uint8_t index)
{
    return (enum button_action)(
        BUTTON_ACTION_NORMAL_CLICK +
        (index % (BUTTON_ACTION_SELF_TEST_CANCELLED -
                  BUTTON_ACTION_NORMAL_CLICK + 1u)));
}

static void test_completion_edge_submission_remains_owned(void)
{
    struct button_action_handoff handoff;
    enum button_action action = BUTTON_ACTION_NONE;

    button_action_handoff_init(&handoff);
    assert(button_action_handoff_submit(
               &handoff, BUTTON_ACTION_NORMAL_CLICK) ==
           BUTTON_ACTION_HANDOFF_START_OWNER);
    assert(button_action_handoff_take(&handoff, &action));
    assert(action == BUTTON_ACTION_NORMAL_CLICK);

    /* A press arriving before completion releases ownership must be queued. */
    assert(button_action_handoff_submit(
               &handoff, BUTTON_ACTION_SELF_TEST_ARMED) ==
           BUTTON_ACTION_HANDOFF_QUEUED);
    assert(!button_action_handoff_release_if_empty(&handoff));
    assert(button_action_handoff_take(&handoff, &action));
    assert(action == BUTTON_ACTION_SELF_TEST_ARMED);
    assert(button_action_handoff_release_if_empty(&handoff));
    assert(button_action_handoff_release_if_empty(&handoff));

    /* A press arriving after release starts a fresh worker owner. */
    assert(button_action_handoff_submit(
               &handoff, BUTTON_ACTION_NORMAL_CLICK) ==
           BUTTON_ACTION_HANDOFF_START_OWNER);
}

static void test_full_queue_rejects_without_corrupting_order(void)
{
    struct button_action_handoff handoff;
    enum button_action action = BUTTON_ACTION_NONE;

    button_action_handoff_init(&handoff);
    for (uint8_t index = 0u; index < BUTTON_ACTION_HANDOFF_CAPACITY; index++) {
        enum button_action expected =
            (index & 1u) != 0u ? BUTTON_ACTION_SELF_TEST_ARMED :
                                 BUTTON_ACTION_NORMAL_CLICK;
        enum button_action_handoff_submit_result result =
            button_action_handoff_submit(&handoff, expected);

        assert(result == (index == 0u ?
                          BUTTON_ACTION_HANDOFF_START_OWNER :
                          BUTTON_ACTION_HANDOFF_QUEUED));
    }
    assert(button_action_handoff_submit(
               &handoff, BUTTON_ACTION_SELF_TEST_CANCELLED) ==
           BUTTON_ACTION_HANDOFF_FULL);

    for (uint8_t index = 0u; index < BUTTON_ACTION_HANDOFF_CAPACITY; index++) {
        enum button_action expected =
            (index & 1u) != 0u ? BUTTON_ACTION_SELF_TEST_ARMED :
                                 BUTTON_ACTION_NORMAL_CLICK;

        assert(button_action_handoff_take(&handoff, &action));
        assert(action == expected);
    }
    assert(button_action_handoff_release_if_empty(&handoff));
}

static void test_wrapped_fifo_preserves_order(void)
{
    struct button_action_handoff handoff;
    enum button_action action = BUTTON_ACTION_NONE;

    button_action_handoff_init(&handoff);
    for (uint8_t index = 0u; index < BUTTON_ACTION_HANDOFF_CAPACITY; index++) {
        assert(button_action_handoff_submit(&handoff,
                                             indexed_action(index)) !=
               BUTTON_ACTION_HANDOFF_FULL);
    }
    for (uint8_t index = 0u; index < 7u; index++) {
        assert(button_action_handoff_take(&handoff, &action));
        assert(action == indexed_action(index));
    }
    for (uint8_t index = 0u; index < 7u; index++) {
        assert(button_action_handoff_submit(
                   &handoff,
                   indexed_action(
                       (uint8_t)(BUTTON_ACTION_HANDOFF_CAPACITY + index))) ==
               BUTTON_ACTION_HANDOFF_QUEUED);
    }
    for (uint8_t index = 7u;
         index < BUTTON_ACTION_HANDOFF_CAPACITY + 7u;
         index++) {
        assert(button_action_handoff_take(&handoff, &action));
        assert(action == indexed_action(index));
    }
    assert(button_action_handoff_release_if_empty(&handoff));
}

int main(void)
{
    test_completion_edge_submission_remains_owned();
    test_full_queue_rejects_without_corrupting_order();
    test_wrapped_fifo_preserves_order();
    return 0;
}
