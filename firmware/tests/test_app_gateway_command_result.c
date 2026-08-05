#include "app_gateway_command_result.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static struct proto_packet command_result_packet(uint16_t seq)
{
    return (struct proto_packet) {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = UINT64_C(0x9999888877776666),
        .dst_id = UINT64_C(0x9999888877776666),
        .session_id = 1000u + seq,
        .seq = seq,
        .ttl = 1u,
        .payload_len = APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN,
    };
}

static struct proto_packet command_packet(uint16_t seq,
                                          enum command_id command_id)
{
    struct proto_packet packet = command_result_packet(seq);

    (void)command_id;
    packet.msg_type = MSG_COMMAND;
    return packet;
}

static bool packet_semantically_equal(const struct proto_packet *left,
                                      const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->ttl == right->ttl &&
           left->payload_len == right->payload_len &&
           left->message_age_ms == right->message_age_ms;
}

static struct app_gateway_command_result_item queue_peek(
    const struct app_gateway_command_result_queue *queue)
{
    struct app_gateway_command_result_item item;

    assert(app_gateway_command_result_queue_peek(queue, &item) == 0);
    return item;
}

static void test_five_credit_queue_stays_within_ram_budget(void)
{
    assert(APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH == 5u);
    assert(sizeof(struct app_gateway_command_result_queue) <=
           APP_GATEWAY_COMMAND_RESULT_QUEUE_RAM_BUDGET_BYTES);
}

static void test_fifo_retains_exact_results_until_explicit_completion(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN];

    memset(payload, 0x5a, sizeof(payload));
    app_gateway_command_result_queue_init(&queue);
    for (uint16_t seq = 1u;
         seq <= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         seq++) {
        struct proto_packet packet = command_result_packet(seq);

        payload[0] = (uint8_t)seq;
        assert(app_gateway_command_result_queue_push(
                   &queue, &packet, payload, sizeof(payload)) == 0);
    }
    assert(app_gateway_command_result_queue_depth(&queue) ==
           APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);

    for (uint16_t seq = 1u;
         seq <= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         seq++) {
        struct app_gateway_command_result_item item = queue_peek(&queue);

        assert(item.packet.seq == seq);
        assert(item.payload_len == sizeof(payload));
        assert(item.payload[0] == (uint8_t)seq);
        assert(app_gateway_command_result_queue_pop(&queue) == 0);
    }
    assert(app_gateway_command_result_queue_depth(&queue) == 0u);
    {
        struct app_gateway_command_result_item item;

        assert(app_gateway_command_result_queue_peek(&queue, &item) ==
               -ENOENT);
    }
}

static void test_disconnect_and_queue_pressure_cannot_erase_custody(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    struct proto_packet packet;

    app_gateway_command_result_queue_init(&queue);
    for (uint16_t seq = 1u;
         seq <= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         seq++) {
        packet = command_result_packet(seq);
        assert(app_gateway_command_result_queue_push(
                   &queue, &packet, payload, sizeof(payload)) == 0);
    }

    packet = command_result_packet(99u);
    assert(app_gateway_command_result_queue_push(
               &queue, &packet, payload, sizeof(payload)) == -ENOSPC);
    assert(app_gateway_command_result_queue_depth(&queue) ==
           APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);

    /* A disconnect cancels asynchronous notification but does not acknowledge
     * the head.  Only the later completion boundary may remove it. */
    struct app_gateway_command_result_item before_disconnect =
        queue_peek(&queue);

    assert(before_disconnect.packet.seq == 1u);
    assert(queue_peek(&queue).packet.seq == 1u);
    assert(app_gateway_command_result_queue_depth(&queue) ==
           APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);
    assert(app_gateway_command_result_queue_pop(&queue) == 0);
    assert(queue_peek(&queue).packet.seq == 2u);

    assert(app_gateway_command_result_queue_push(
               &queue, &packet, payload, sizeof(payload)) == 0);
    assert(app_gateway_command_result_queue_depth(&queue) ==
           APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);
}

static void test_compact_slot_reconstructs_exact_result_fields(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN];
    struct proto_packet packet = command_result_packet(23u);
    struct app_gateway_command_result_item item;

    memset(payload, 0xa7, sizeof(payload));
    packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    packet.ttl = 7u;
    packet.message_age_ms = 0x12345678u;
    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_queue_push(
               &queue, &packet, payload, sizeof(payload)) == 0);
    item = queue_peek(&queue);
    assert(packet_semantically_equal(&item.packet, &packet));
    assert(item.payload_len == sizeof(payload));
    assert(memcmp(item.payload, payload, sizeof(payload)) == 0);
}

static void test_stream_full_and_overflow_full_stop_command_admission(void)
{
    struct app_gateway_command_result_queue queue;
    uint32_t tokens[APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH];
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};

    app_gateway_command_result_queue_init(&queue);
    for (uint16_t seq = 1u;
         seq <= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         seq++) {
        struct proto_packet command = command_packet(seq, CMD_SET_ROLE);

        assert(app_gateway_command_result_reserve(
                   &queue, &tokens[seq - 1u]) == 0);
        assert(app_gateway_command_result_bind(
                   &queue, tokens[seq - 1u], &command, CMD_SET_ROLE) == 0);
    }
    assert(app_gateway_command_result_occupancy(&queue) ==
           APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);
    {
        uint32_t denied_token = 0u;

        assert(app_gateway_command_result_reserve(
                   &queue, &denied_token) == -ENOSPC);
        assert(denied_token == 0u);
    }

    /*
     * A terminal result converts its reservation into compact custody.  If
     * the retained BLE stream is also full, occupancy stays full and the next
     * command remains unadmitted until async completion frees custody.
     */
    {
        struct proto_packet command = command_packet(1u, CMD_SET_ROLE);
        struct proto_packet result = command_result_packet(1u);
        uint32_t denied_token = 0u;

        assert(app_gateway_command_result_commit(
                   &queue, tokens[0], &command, CMD_SET_ROLE, &result,
                   payload, sizeof(payload)) == 0);
        assert(app_gateway_command_result_queue_depth(&queue) == 1u);
        assert(app_gateway_command_result_reservation_depth(&queue) ==
               APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH - 1u);
        assert(app_gateway_command_result_reserve(
                   &queue, &denied_token) == -ENOSPC);
    }

    assert(app_gateway_command_result_queue_pop(&queue) == 0);
    {
        uint32_t resumed_token = 0u;

        assert(app_gateway_command_result_reserve(
                   &queue, &resumed_token) == 0);
        assert(resumed_token != 0u);
    }
}

static void test_result_commit_matches_explicitly_rebound_command(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    struct proto_packet host_command = command_packet(0u, CMD_SET_ROLE);
    struct proto_packet outbound = host_command;
    struct proto_packet result;
    uint32_t token = 0u;

    host_command.session_id = 0u;
    outbound.src_id = UINT64_C(0x9999888877776666);
    outbound.session_id = 1234u;
    outbound.seq = 77u;
    result = command_result_packet(outbound.seq);
    result.session_id = outbound.session_id;

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_reserve(&queue, &token) == 0);
    assert(app_gateway_command_result_bind(
               &queue, token, &host_command, CMD_SET_ROLE) == 0);
    assert(app_gateway_command_result_rebind(
               &queue, token, &host_command, CMD_SET_ROLE, &outbound) == 0);
    assert(app_gateway_command_result_commit(
               &queue, token, &outbound, CMD_SET_ROLE, &result,
               payload, sizeof(payload)) == 0);
    assert(app_gateway_command_result_reservation_depth(&queue) == 0u);
    assert(app_gateway_command_result_queue_depth(&queue) == 1u);
}

static void test_rebound_identity_selects_exact_same_id_reservation(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    struct proto_packet first = command_packet(102u, CMD_SET_ROLE);
    struct proto_packet second = command_packet(2u, CMD_SET_ROLE);
    struct proto_packet first_outbound = command_packet(101u, CMD_SET_ROLE);
    struct proto_packet second_outbound = command_packet(102u, CMD_SET_ROLE);
    struct proto_packet result = command_result_packet(102u);
    uint32_t first_token = 0u;
    uint32_t exact_token = 0u;

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_reserve(&queue, &first_token) == 0);
    assert(app_gateway_command_result_bind(
               &queue, first_token, &first, CMD_SET_ROLE) == 0);
    assert(app_gateway_command_result_reserve(&queue, &exact_token) == 0);
    assert(app_gateway_command_result_bind(
               &queue, exact_token, &second, CMD_SET_ROLE) == 0);
    assert(app_gateway_command_result_rebind(
               &queue, first_token, &first, CMD_SET_ROLE,
               &first_outbound) == 0);
    assert(app_gateway_command_result_rebind(
               &queue, exact_token, &second, CMD_SET_ROLE,
               &second_outbound) == 0);

    /*
     * The second prepared identity intentionally equals the first original
     * identity.  Terminal lookup must select the prepared alias first.
     */
    assert(app_gateway_command_result_commit(
               &queue, exact_token, &second_outbound, CMD_SET_ROLE, &result,
               payload, sizeof(payload)) == 0);
    assert(app_gateway_command_result_release(&queue, exact_token) == -ENOENT);
    assert(app_gateway_command_result_release(&queue, first_token) == 0);
}

static void test_identical_seq_zero_commands_keep_opaque_reservations(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t first_payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    uint8_t second_payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    struct proto_packet original = command_packet(0u, CMD_SET_ROLE);
    struct proto_packet first_outbound = original;
    struct proto_packet second_outbound = original;
    struct proto_packet first_result = command_result_packet(71u);
    struct proto_packet second_result = command_result_packet(72u);
    uint32_t first_token = 0u;
    uint32_t second_token = 0u;

    original.session_id = 0u;
    first_outbound.src_id = UINT64_C(0x1111222233334444);
    first_outbound.session_id = 7001u;
    first_outbound.seq = 71u;
    second_outbound.src_id = first_outbound.src_id;
    second_outbound.session_id = 7002u;
    second_outbound.seq = 72u;
    first_result.session_id = first_outbound.session_id;
    second_result.session_id = second_outbound.session_id;
    first_payload[0] = 0xa1u;
    second_payload[0] = 0xb2u;

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_reserve(&queue, &first_token) == 0);
    assert(app_gateway_command_result_bind(
               &queue, first_token, &original, CMD_SET_ROLE) == 0);
    assert(app_gateway_command_result_reserve(&queue, &second_token) == 0);
    assert(app_gateway_command_result_bind(
               &queue, second_token, &original, CMD_SET_ROLE) == 0);
    assert(first_token != second_token);

    assert(app_gateway_command_result_rebind(
               &queue, first_token, &original, CMD_SET_ROLE,
               &first_outbound) == 0);
    assert(app_gateway_command_result_rebind(
               &queue, second_token, &original, CMD_SET_ROLE,
               &second_outbound) == 0);

    /* A valid token cannot consume another token's prepared terminal alias. */
    assert(app_gateway_command_result_commit(
               &queue, first_token, &second_outbound, CMD_SET_ROLE,
               &second_result, second_payload, sizeof(second_payload)) ==
           -ENOENT);
    assert(app_gateway_command_result_reservation_depth(&queue) == 2u);

    /* The second command can terminate BUSY while the first remains pending. */
    assert(app_gateway_command_result_commit(
               &queue, second_token, &second_outbound, CMD_SET_ROLE,
               &second_result, second_payload, sizeof(second_payload)) == 0);
    assert(app_gateway_command_result_commit(
               &queue, first_token, &first_outbound, CMD_SET_ROLE,
               &first_result, first_payload, sizeof(first_payload)) == 0);
    assert(app_gateway_command_result_reservation_depth(&queue) == 0u);
    assert(app_gateway_command_result_queue_depth(&queue) == 2u);
    assert(app_gateway_command_result_occupancy(&queue) == 2u);

    {
        struct app_gateway_command_result_item item = queue_peek(&queue);

        assert(item.packet.seq == second_result.seq);
        assert(item.payload[0] == second_payload[0]);
        assert(app_gateway_command_result_queue_pop(&queue) == 0);
        item = queue_peek(&queue);
        assert(item.packet.seq == first_result.seq);
        assert(item.payload[0] == first_payload[0]);
        assert(app_gateway_command_result_queue_pop(&queue) == 0);
    }
    assert(app_gateway_command_result_occupancy(&queue) == 0u);
}

static void test_rebound_terminal_release_cannot_leak_or_cross_tokens(void)
{
    struct app_gateway_command_result_queue queue;

    app_gateway_command_result_queue_init(&queue);
    for (uint16_t round = 1u;
         round <= (APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH * 2u);
         round++) {
        struct proto_packet original =
            command_packet(0u, CMD_SET_ROLE);
        struct proto_packet outbound =
            command_packet((uint16_t)(100u + round), CMD_SET_ROLE);
        struct proto_packet wrong = outbound;
        uint32_t token = 0u;

        original.session_id = 0u;
        outbound.src_id = UINT64_C(0x1111222233334444);
        outbound.session_id = 8000u + round;
        wrong.seq++;
        assert(app_gateway_command_result_reserve(&queue, &token) == 0);
        assert(app_gateway_command_result_bind(
                   &queue, token, &original, CMD_SET_ROLE) == 0);
        assert(app_gateway_command_result_rebind(
                   &queue, token, &original, CMD_SET_ROLE, &outbound) == 0);

        assert(app_gateway_command_result_release_terminal(
                   &queue, token + 1u, &outbound, CMD_SET_ROLE) == -ENOENT);
        assert(app_gateway_command_result_release_terminal(
                   &queue, token, &wrong, CMD_SET_ROLE) == -ENOENT);
        assert(app_gateway_command_result_release_terminal(
                   &queue, token, &original, CMD_SET_ROLE) == -ENOENT);
        assert(app_gateway_command_result_reservation_depth(&queue) == 1u);

        assert(app_gateway_command_result_release_terminal(
                   &queue, token, &outbound, CMD_SET_ROLE) == 0);
        assert(app_gateway_command_result_release_terminal(
                   &queue, token, &outbound, CMD_SET_ROLE) == -ENOENT);
        assert(app_gateway_command_result_reservation_depth(&queue) == 0u);
        assert(app_gateway_command_result_occupancy(&queue) == 0u);
    }
}

static void test_unowned_result_fails_closed_without_consuming_capacity(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    struct proto_packet command = command_packet(1u, CMD_SET_ROLE);
    struct proto_packet result = command_result_packet(1u);

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_commit(
               &queue, 1u, &command, CMD_SET_ROLE, &result,
               payload, sizeof(payload)) == -ENOENT);
    assert(app_gateway_command_result_occupancy(&queue) == 0u);
}

static void test_direct_queue_push_cannot_consume_reserved_capacity(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    uint32_t token = 0u;

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_reserve(&queue, &token) == 0);
    for (uint16_t seq = 1u;
         seq < APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         seq++) {
        struct proto_packet result = command_result_packet(seq);

        assert(app_gateway_command_result_queue_push(
                   &queue, &result, payload, sizeof(payload)) == 0);
    }
    {
        struct proto_packet denied = command_result_packet(99u);

        assert(app_gateway_command_result_queue_push(
                   &queue, &denied, payload, sizeof(payload)) == -ENOSPC);
    }
    assert(app_gateway_command_result_occupancy(&queue) ==
           APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);
}

static void test_malformed_commit_keeps_the_admission_reservation(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN] = {0};
    struct proto_packet command = command_packet(1u, CMD_SET_ROLE);
    struct proto_packet result = command_result_packet(1u);
    uint32_t token = 0u;

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_reserve(&queue, &token) == 0);
    assert(app_gateway_command_result_bind(
               &queue, token, &command, CMD_SET_ROLE) == 0);
    result.payload_len--;
    assert(app_gateway_command_result_commit(
               &queue, token, &command, CMD_SET_ROLE, &result,
               payload, sizeof(payload) - 1u) == -EINVAL);
    assert(app_gateway_command_result_reservation_depth(&queue) == 1u);
    assert(app_gateway_command_result_queue_depth(&queue) == 0u);
    assert(app_gateway_command_result_release(&queue, token) == 0);
}

static void test_token_wrap_skips_a_live_token(void)
{
    struct app_gateway_command_result_queue queue;
    uint32_t token = 0u;
    uint32_t wrapped = 0u;

    app_gateway_command_result_queue_init(&queue);
    assert(app_gateway_command_result_reserve(&queue, &token) == 0);
    assert(token == 1u);
    queue.next_token = UINT32_MAX;
    assert(app_gateway_command_result_reserve(&queue, &wrapped) == 0);
    assert(wrapped == 2u);
    assert(wrapped != token);
}

static void test_rejects_nonterminal_or_oversized_records(void)
{
    struct app_gateway_command_result_queue queue;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN + 1u] = {0};
    struct proto_packet packet = command_result_packet(1u);

    app_gateway_command_result_queue_init(&queue);
    packet.msg_type = MSG_COMMAND;
    assert(app_gateway_command_result_queue_push(
               &queue, &packet, payload,
               APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN) == -EINVAL);
    packet.msg_type = MSG_COMMAND_RESULT;
    packet.payload_len = sizeof(payload);
    assert(app_gateway_command_result_queue_push(
               &queue, &packet, payload, sizeof(payload)) == -EINVAL);
    assert(app_gateway_command_result_queue_depth(&queue) == 0u);
}

int main(void)
{
    test_five_credit_queue_stays_within_ram_budget();
    test_fifo_retains_exact_results_until_explicit_completion();
    test_disconnect_and_queue_pressure_cannot_erase_custody();
    test_compact_slot_reconstructs_exact_result_fields();
    test_stream_full_and_overflow_full_stop_command_admission();
    test_result_commit_matches_explicitly_rebound_command();
    test_rebound_identity_selects_exact_same_id_reservation();
    test_identical_seq_zero_commands_keep_opaque_reservations();
    test_rebound_terminal_release_cannot_leak_or_cross_tokens();
    test_unowned_result_fails_closed_without_consuming_capacity();
    test_direct_queue_push_cannot_consume_reserved_capacity();
    test_malformed_commit_keeps_the_admission_reservation();
    test_token_wrap_skips_a_live_token();
    test_rejects_nonterminal_or_oversized_records();
    return 0;
}
