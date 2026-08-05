#ifndef APP_GATEWAY_COMMAND_RESULT_H
#define APP_GATEWAY_COMMAND_RESULT_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN \
    ((2u * PROTO_TLV_U16_ENCODED_LEN) + PROTO_TLV_U8_ENCODED_LEN)
/*
 * Four complete GATT frames may already be buffered while one frame is being
 * decoded.  The BLE worker reserves one slot before removing a frame from that
 * ingress queue, so no sixth command can enter the protocol state machines
 * until a retained result advances into the BLE stream.
 */
#define APP_GATEWAY_COMMAND_RESULT_BUFFERED_INGRESS_DEPTH 4u
#define APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH \
    (APP_GATEWAY_COMMAND_RESULT_BUFFERED_INGRESS_DEPTH + 1u)
#define APP_GATEWAY_COMMAND_RESULT_QUEUE_RAM_BUDGET_BYTES 320u

struct app_gateway_command_result_item {
    struct proto_packet packet;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN];
    uint8_t payload_len;
};

struct app_gateway_command_result_command_identity {
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint16_t seq;
};

struct app_gateway_command_result_queued {
    uint64_t src_id;
    uint64_t dst_id;
    uint32_t session_id;
    uint32_t message_age_ms;
    uint16_t seq;
    uint8_t flags;
    uint8_t ttl;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN];
};

struct app_gateway_command_result_reserved {
    struct app_gateway_command_result_command_identity original;
    struct app_gateway_command_result_command_identity prepared;
    uint32_t token;
    uint16_t command_id;
    uint8_t flags;
};

union app_gateway_command_result_slot {
    struct app_gateway_command_result_queued queued;
    struct app_gateway_command_result_reserved reserved;
};

struct app_gateway_command_result_queue {
    union app_gateway_command_result_slot
        slots[APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH];
    uint32_t next_token;
    uint8_t queued_order[APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
    uint8_t reservation_count;
    uint8_t queued_mask;
    uint8_t reserved_mask;
    bool flush_active;
};

_Static_assert(APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH == 5u,
               "gateway command result custody must retain five credits");
_Static_assert(sizeof(union app_gateway_command_result_slot) <= 56u,
               "a command result credit must use one compact tagged slot");
_Static_assert(sizeof(struct app_gateway_command_result_queue) <=
               APP_GATEWAY_COMMAND_RESULT_QUEUE_RAM_BUDGET_BYTES,
               "gateway command result custody exceeds its RAM budget");

void app_gateway_command_result_queue_init(
    struct app_gateway_command_result_queue *queue);
int app_gateway_command_result_queue_push(
    struct app_gateway_command_result_queue *queue,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len);
int app_gateway_command_result_reserve(
    struct app_gateway_command_result_queue *queue,
    uint32_t *token);
/*
 * The reservation token is the authoritative ownership key from admission
 * through terminal conversion.  Packet identity is validated as a stale-call
 * guard, but it is not unique while identical host commands are buffered.
 */
int app_gateway_command_result_bind(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id);
int app_gateway_command_result_rebind(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result_command);
int app_gateway_command_result_release(
    struct app_gateway_command_result_queue *queue,
    uint32_t token);
int app_gateway_command_result_release_command(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id);
int app_gateway_command_result_release_terminal(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id);
int app_gateway_command_result_commit(
    struct app_gateway_command_result_queue *queue,
    uint32_t token,
    const struct proto_packet *command,
    enum command_id command_id,
    const struct proto_packet *result,
    const uint8_t *payload,
    size_t payload_len);
bool app_gateway_command_result_flush_begin(
    struct app_gateway_command_result_queue *queue);
void app_gateway_command_result_flush_end(
    struct app_gateway_command_result_queue *queue);
int app_gateway_command_result_queue_peek(
    const struct app_gateway_command_result_queue *queue,
    struct app_gateway_command_result_item *item);
int app_gateway_command_result_queue_pop(
    struct app_gateway_command_result_queue *queue);
uint8_t app_gateway_command_result_queue_depth(
    const struct app_gateway_command_result_queue *queue);
uint8_t app_gateway_command_result_reservation_depth(
    const struct app_gateway_command_result_queue *queue);
uint8_t app_gateway_command_result_occupancy(
    const struct app_gateway_command_result_queue *queue);

#ifdef __cplusplus
}
#endif

#endif
