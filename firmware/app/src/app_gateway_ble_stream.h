#ifndef APP_GATEWAY_BLE_STREAM_H
#define APP_GATEWAY_BLE_STREAM_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_BLE_STREAM_MAGIC 0x5747u
#define GATEWAY_BLE_STREAM_VERSION 1u
#define GATEWAY_BLE_STREAM_RECORD_HEADER_LEN 40u
#define GATEWAY_BLE_STREAM_RECORD_MAX_LEN 192u
#define GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN \
    (GATEWAY_BLE_STREAM_RECORD_MAX_LEN - GATEWAY_BLE_STREAM_RECORD_HEADER_LEN)
#define GATEWAY_BLE_STREAM_QUEUE_DEPTH 8u
#define GATEWAY_BLE_STREAM_RAM_BUDGET_BYTES 2048u

enum gateway_ble_stream_class {
    GATEWAY_BLE_STREAM_CLASS_UNKNOWN = 0,
    GATEWAY_BLE_STREAM_CLASS_CLICK = 1,
    GATEWAY_BLE_STREAM_CLASS_RESULT = 2,
    GATEWAY_BLE_STREAM_CLASS_SURVEY = 3,
    GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC = 4,
    GATEWAY_BLE_STREAM_CLASS_STATUS = 5,
};

enum gateway_ble_stream_record_type {
    GATEWAY_BLE_STREAM_RECORD_PACKET = 1,
};

enum gateway_ble_stream_drop_reason {
    GATEWAY_BLE_STREAM_DROP_NONE = 0,
    GATEWAY_BLE_STREAM_DROP_QUEUE_FULL = 1,
    GATEWAY_BLE_STREAM_DROP_TOO_LARGE = 2,
    GATEWAY_BLE_STREAM_DROP_NOT_READY = 3,
    GATEWAY_BLE_STREAM_DROP_PRIORITY = 4,
};

struct gateway_ble_stream_diagnostics {
    uint32_t enqueue_attempts;
    uint32_t packets_sent;
    uint32_t bytes_sent;
    uint32_t drops_queue_full;
    uint32_t drops_too_large;
    uint32_t drops_not_ready;
    uint32_t drops_priority;
    uint8_t max_queue_depth_observed;
    uint32_t oldest_queued_age_ms;
    uint8_t last_dropped_packet_type;
    enum gateway_ble_stream_drop_reason last_drop_reason;
};

struct gateway_ble_stream_item {
    uint8_t record[GATEWAY_BLE_STREAM_RECORD_MAX_LEN];
    uint16_t len;
    uint8_t packet_type;
    uint8_t priority;
    uint32_t queued_at_ms;
};

struct gateway_ble_stream_state {
    struct gateway_ble_stream_item items[GATEWAY_BLE_STREAM_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
    struct gateway_ble_stream_diagnostics diagnostics;
};

typedef int (*gateway_ble_stream_send_fn)(const uint8_t *record,
                                          size_t record_len,
                                          void *ctx);

void gateway_ble_stream_init(struct gateway_ble_stream_state *state);
bool gateway_ble_should_stream_packet(uint8_t msg_type,
                                      uint8_t flags,
                                      enum gateway_ble_stream_class packet_class);
enum gateway_ble_stream_class gateway_ble_stream_classify_packet(uint8_t msg_type,
                                                                 uint8_t flags);
int gateway_ble_stream_enqueue_packet(struct gateway_ble_stream_state *state,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms,
                                      uint32_t now_ms,
                                      bool ble_ready);
unsigned int gateway_ble_stream_drain(struct gateway_ble_stream_state *state,
                                      gateway_ble_stream_send_fn send_fn,
                                      void *send_ctx,
                                      uint32_t now_ms,
                                      bool ble_ready,
                                      unsigned int max_records);
int gateway_ble_stream_peek(const struct gateway_ble_stream_state *state,
                            const uint8_t **record,
                            size_t *record_len);
void gateway_ble_stream_mark_sent(struct gateway_ble_stream_state *state,
                                  uint32_t now_ms);
void gateway_ble_stream_get_diagnostics(
    const struct gateway_ble_stream_state *state,
    uint32_t now_ms,
    struct gateway_ble_stream_diagnostics *diagnostics);
uint8_t gateway_ble_stream_depth(const struct gateway_ble_stream_state *state);

#ifdef __cplusplus
}
#endif

#endif
