#include "app_gateway_command_result.h"

#include <errno.h>
#include <string.h>

_Static_assert(APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH > 1u,
               "gateway command results need bounded admission custody");
_Static_assert(APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN <= UINT8_MAX,
               "gateway command result payload length must fit the packet");

#define COMMAND_RESULT_RESERVED_BOUND 0x01u
#define COMMAND_RESULT_RESERVED_PREPARED 0x02u
#define COMMAND_RESULT_SLOT_NONE UINT8_MAX

void app_gateway_command_result_queue_init(
    struct app_gateway_command_result_queue *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

static uint8_t command_result_slot_bit(uint8_t index)
{
    return (uint8_t)(1u << index);
}

static bool command_result_slot_free(
    const struct app_gateway_command_result_queue *queue,
    uint8_t index)
{
    uint8_t bit = command_result_slot_bit(index);

    return ((queue->queued_mask | queue->reserved_mask) & bit) == 0u;
}

static uint8_t command_result_free_slot(
    const struct app_gateway_command_result_queue *queue)
{
    for (uint8_t i = 0u;
         i < APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         i++) {
        if (command_result_slot_free(queue, i)) {
            return i;
        }
    }
    return COMMAND_RESULT_SLOT_NONE;
}

static void command_result_identity_store(
    struct app_gateway_command_result_command_identity *identity,
    const struct proto_packet *command)
{
    identity->src_id = command->src_id;
    identity->dst_id = command->dst_id;
    identity->session_id = command->session_id;
    identity->seq = command->seq;
}

static bool command_result_identity_matches(
    const struct app_gateway_command_result_command_identity *identity,
    const struct proto_packet *command)
{
    return identity != NULL && command != NULL &&
           command->msg_type == MSG_COMMAND &&
           identity->src_id == command->src_id &&
           identity->dst_id == command->dst_id &&
           identity->session_id == command->session_id &&
           identity->seq == command->seq;
}

static uint8_t reservation_index_for_token(
    const struct app_gateway_command_result_queue *queue,
    uint32_t token)
{
    if (queue == NULL || token == 0u) {
        return COMMAND_RESULT_SLOT_NONE;
    }
    for (uint8_t i = 0u;
         i < APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH;
         i++) {
        const struct app_gateway_command_result_reserved *reservation =
            &queue->slots[i].reserved;

        if ((queue->reserved_mask & command_result_slot_bit(i)) != 0u &&
            reservation->token == token) {
            return i;
        }
    }
    return COMMAND_RESULT_SLOT_NONE;
}

static bool reservation_matches_original_command(
    const struct app_gateway_command_result_reserved *reservation,
    const struct proto_packet *command,
    enum command_id command_id)
{
    return reservation != NULL &&
           (reservation->flags & COMMAND_RESULT_RESERVED_BOUND) != 0u &&
           reservation->command_id == (uint16_t)command_id &&
           command_result_identity_matches(&reservation->original, command);
}

static uint8_t reservation_index_for_original_command(
    const struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    uint8_t index = reservation_index_for_token(queue, token);

    if (index == COMMAND_RESULT_SLOT_NONE || command == NULL ||
        !reservation_matches_original_command(
            &queue->slots[index].reserved, command, command_id)) {
        return COMMAND_RESULT_SLOT_NONE;
    }
    return index;
}

static uint8_t reservation_index_for_terminal_command(
    const struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    uint8_t index = reservation_index_for_token(queue, token);
    const struct app_gateway_command_result_reserved *reservation;

    if (index == COMMAND_RESULT_SLOT_NONE || command == NULL) {
        return COMMAND_RESULT_SLOT_NONE;
    }
    reservation = &queue->slots[index].reserved;
    if ((reservation->flags & COMMAND_RESULT_RESERVED_BOUND) == 0u ||
        reservation->command_id != (uint16_t)command_id) {
        return COMMAND_RESULT_SLOT_NONE;
    }
    if ((reservation->flags & COMMAND_RESULT_RESERVED_PREPARED) != 0u) {
        return command_result_identity_matches(&reservation->prepared, command) ?
               index : COMMAND_RESULT_SLOT_NONE;
    }
    return command_result_identity_matches(&reservation->original, command) ?
           index : COMMAND_RESULT_SLOT_NONE;
}

static void reservation_clear(
    struct app_gateway_command_result_queue *queue,
    uint8_t index)
{
    uint8_t bit;

    if (queue == NULL || index >= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH) {
        return;
    }
    bit = command_result_slot_bit(index);
    if ((queue->reserved_mask & bit) == 0u) {
        return;
    }
    memset(&queue->slots[index], 0, sizeof(queue->slots[index]));
    queue->reserved_mask &= (uint8_t)~bit;
    queue->reservation_count--;
}

static void command_result_queue_append(
    struct app_gateway_command_result_queue *queue,
    uint8_t index,
    const struct proto_packet *packet,
    const uint8_t *payload)
{
    struct app_gateway_command_result_queued *queued =
        &queue->slots[index].queued;
    uint8_t tail = (uint8_t)((queue->head + queue->count) %
                             APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);

    memset(&queue->slots[index], 0, sizeof(queue->slots[index]));
    queued->src_id = packet->src_id;
    queued->dst_id = packet->dst_id;
    queued->session_id = packet->session_id;
    queued->message_age_ms = packet->message_age_ms;
    queued->seq = packet->seq;
    queued->flags = packet->flags;
    queued->ttl = packet->ttl;
    memcpy(queued->payload, payload, sizeof(queued->payload));
    queue->queued_mask |= command_result_slot_bit(index);
    queue->queued_order[tail] = index;
    queue->count++;
}

int app_gateway_command_result_queue_push(
    struct app_gateway_command_result_queue *queue,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t index;

    if (queue == NULL || packet == NULL ||
        (payload == NULL && payload_len != 0u) ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        packet->payload_len != payload_len ||
        payload_len != APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN) {
        return -EINVAL;
    }
    if (app_gateway_command_result_occupancy(queue) >=
        APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH) {
        return -ENOSPC;
    }

    index = command_result_free_slot(queue);
    if (index == COMMAND_RESULT_SLOT_NONE) {
        return -EFAULT;
    }
    command_result_queue_append(queue, index, packet, payload);
    return 0;
}

int app_gateway_command_result_reserve(
    struct app_gateway_command_result_queue *queue,
    uint32_t *token)
{
    struct app_gateway_command_result_reserved *reservation;
    uint8_t index;

    if (queue == NULL || token == NULL) {
        return -EINVAL;
    }
    if (app_gateway_command_result_occupancy(queue) >=
        APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH) {
        return -ENOSPC;
    }
    index = command_result_free_slot(queue);
    if (index == COMMAND_RESULT_SLOT_NONE) {
        return -EFAULT;
    }
    reservation = &queue->slots[index].reserved;

    do {
        queue->next_token++;
        if (queue->next_token == 0u) {
            queue->next_token++;
        }
    } while (reservation_index_for_token(queue, queue->next_token) !=
             COMMAND_RESULT_SLOT_NONE);
    memset(&queue->slots[index], 0, sizeof(queue->slots[index]));
    *reservation = (struct app_gateway_command_result_reserved) {
        .token = queue->next_token,
    };
    queue->reserved_mask |= command_result_slot_bit(index);
    queue->reservation_count++;
    *token = reservation->token;
    return 0;
}

int app_gateway_command_result_bind(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    uint8_t index = reservation_index_for_token(queue, token);
    struct app_gateway_command_result_reserved *reservation;

    if (index == COMMAND_RESULT_SLOT_NONE || command == NULL ||
        command->msg_type != MSG_COMMAND) {
        return -EINVAL;
    }
    reservation = &queue->slots[index].reserved;
    if ((reservation->flags & COMMAND_RESULT_RESERVED_BOUND) != 0u) {
        return -EALREADY;
    }
    command_result_identity_store(&reservation->original, command);
    reservation->command_id = (uint16_t)command_id;
    reservation->flags |= COMMAND_RESULT_RESERVED_BOUND;
    return 0;
}

int app_gateway_command_result_rebind(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result_command)
{
    struct app_gateway_command_result_reserved *reservation;
    uint8_t index;

    if (queue == NULL || command == NULL || result_command == NULL ||
        result_command->msg_type != MSG_COMMAND) {
        return -EINVAL;
    }
    index = reservation_index_for_original_command(
        queue, token, command, command_id);
    if (index == COMMAND_RESULT_SLOT_NONE) {
        return -ENOENT;
    }
    reservation = &queue->slots[index].reserved;
    command_result_identity_store(&reservation->prepared, result_command);
    reservation->flags |= COMMAND_RESULT_RESERVED_PREPARED;
    return 0;
}

int app_gateway_command_result_release(
    struct app_gateway_command_result_queue *queue,
    uint32_t token)
{
    uint8_t index = reservation_index_for_token(queue, token);

    if (index == COMMAND_RESULT_SLOT_NONE) {
        return -ENOENT;
    }
    reservation_clear(queue, index);
    return 0;
}

int app_gateway_command_result_release_command(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    uint8_t index = reservation_index_for_original_command(
        queue, token, command, command_id);

    if (index == COMMAND_RESULT_SLOT_NONE) {
        return -ENOENT;
    }
    reservation_clear(queue, index);
    return 0;
}

int app_gateway_command_result_release_terminal(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id)
{
    uint8_t index = reservation_index_for_terminal_command(
        queue, token, command, command_id);

    if (index == COMMAND_RESULT_SLOT_NONE) {
        return -ENOENT;
    }
    reservation_clear(queue, index);
    return 0;
}

int app_gateway_command_result_commit(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result,
    const uint8_t *payload,
    size_t payload_len)
{
    uint8_t index;
    uint8_t bit;

    if (queue == NULL || command == NULL) {
        return -EINVAL;
    }
    if (result == NULL || payload == NULL ||
        result->msg_type != MSG_COMMAND_RESULT ||
        result->payload_len != payload_len ||
        payload_len != APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN) {
        return -EINVAL;
    }
    index = reservation_index_for_terminal_command(
        queue, token, command, command_id);
    if (index == COMMAND_RESULT_SLOT_NONE) {
        /*
         * Host-visible terminal results must convert an admission reservation
         * into queue custody.  Accepting an unowned result could exceed the
         * proved ingress bound or strand the real reservation forever.
         */
        return -ENOENT;
    }
    bit = command_result_slot_bit(index);
    queue->reserved_mask &= (uint8_t)~bit;
    queue->reservation_count--;
    command_result_queue_append(queue, index, result, payload);
    return 0;
}

bool app_gateway_command_result_flush_begin(
    struct app_gateway_command_result_queue *queue)
{
    if (queue == NULL || queue->flush_active) {
        return false;
    }
    queue->flush_active = true;
    return true;
}

void app_gateway_command_result_flush_end(
    struct app_gateway_command_result_queue *queue)
{
    if (queue != NULL) {
        queue->flush_active = false;
    }
}

int app_gateway_command_result_queue_peek(
    const struct app_gateway_command_result_queue *queue,
    struct app_gateway_command_result_item *item)
{
    const struct app_gateway_command_result_queued *queued;
    uint8_t index;

    if (queue == NULL || item == NULL) {
        return -EINVAL;
    }
    if (queue->count == 0u) {
        return -ENOENT;
    }
    index = queue->queued_order[queue->head];
    if (index >= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH ||
        (queue->queued_mask & command_result_slot_bit(index)) == 0u) {
        return -EFAULT;
    }
    queued = &queue->slots[index].queued;
    memset(item, 0, sizeof(*item));
    item->packet.msg_type = MSG_COMMAND_RESULT;
    item->packet.flags = queued->flags;
    item->packet.src_id = queued->src_id;
    item->packet.dst_id = queued->dst_id;
    item->packet.session_id = queued->session_id;
    item->packet.seq = queued->seq;
    item->packet.ttl = queued->ttl;
    item->packet.payload_len = APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN;
    item->packet.message_age_ms = queued->message_age_ms;
    memcpy(item->payload, queued->payload, sizeof(item->payload));
    item->payload_len = APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN;
    return 0;
}

int app_gateway_command_result_queue_pop(
    struct app_gateway_command_result_queue *queue)
{
    uint8_t index;

    if (queue == NULL || queue->count == 0u) {
        return -ENOENT;
    }

    index = queue->queued_order[queue->head];
    if (index >= APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH ||
        (queue->queued_mask & command_result_slot_bit(index)) == 0u) {
        return -EFAULT;
    }
    memset(&queue->slots[index], 0, sizeof(queue->slots[index]));
    queue->queued_mask &= (uint8_t)~command_result_slot_bit(index);
    queue->queued_order[queue->head] = 0u;
    queue->head = (uint8_t)((queue->head + 1u) %
                            APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH);
    queue->count--;
    if (queue->count == 0u) {
        queue->head = 0u;
    }
    return 0;
}

uint8_t app_gateway_command_result_queue_depth(
    const struct app_gateway_command_result_queue *queue)
{
    return queue == NULL ? 0u : queue->count;
}

uint8_t app_gateway_command_result_reservation_depth(
    const struct app_gateway_command_result_queue *queue)
{
    return queue == NULL ? 0u : queue->reservation_count;
}

uint8_t app_gateway_command_result_occupancy(
    const struct app_gateway_command_result_queue *queue)
{
    return queue == NULL ? 0u :
           (uint8_t)(queue->count + queue->reservation_count);
}
