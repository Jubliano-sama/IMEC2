#ifndef GATEWAY_BLE_TRANSPORT_H
#define GATEWAY_BLE_TRANSPORT_H

#include "serial_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_BLE_ATT_DEFAULT_MTU 23u
#define GATEWAY_BLE_ATT_VALUE_OVERHEAD 3u
#define GATEWAY_BLE_ATT_DEFAULT_VALUE_MAX \
    (GATEWAY_BLE_ATT_DEFAULT_MTU - GATEWAY_BLE_ATT_VALUE_OVERHEAD)
#define GATEWAY_BLE_ATT_MAX_MTU 517u
#define GATEWAY_BLE_DEFAULT_CONNECTION_INTERVAL_US 30000u

enum gateway_ble_link_status {
    GATEWAY_BLE_LINK_OK = 0,
    GATEWAY_BLE_LINK_ERR_ARG = -4000,
    GATEWAY_BLE_LINK_ERR_NOT_CONNECTED = -4001,
    GATEWAY_BLE_LINK_ERR_NOTIFY_DISABLED = -4002,
    GATEWAY_BLE_LINK_ERR_VALUE_TOO_LONG = -4003,
    GATEWAY_BLE_LINK_ERR_NO_CREDIT = -4004,
    GATEWAY_BLE_LINK_ERR_EVENT_TIME = -4005,
};

struct gateway_ble_tx_cursor {
    const uint8_t *frame;
    size_t frame_len;
    size_t offset;
    size_t in_flight_len;
    uint16_t negotiated_mtu;
    bool in_flight;
};

struct gateway_ble_rx_stream {
    uint8_t frame[SERIAL_FRAME_MAX_LEN];
    size_t frame_len;
    uint32_t decoded_frames;
    uint32_t rejected_frames;
    bool overflow;
};

/*
 * Deterministic ATT/controller boundary model. It models connection-event
 * notification credits and completion timing, not Bluetooth analog RF.
 */
struct gateway_ble_link {
    uint64_t next_event_us;
    uint32_t connection_interval_us;
    uint32_t connection_generation;
    uint32_t notifications_submitted;
    uint32_t notifications_completed;
    uint32_t notifications_dropped_disconnect;
    uint16_t negotiated_mtu;
    uint8_t credit_capacity;
    uint8_t available_credits;
    uint8_t in_flight;
    bool connected;
    bool notify_enabled;
    bool central_stalled;
};

typedef int (*gateway_ble_rx_packet_fn)(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        void *ctx);

uint16_t gateway_ble_att_payload_max(uint16_t negotiated_mtu);
bool gateway_ble_notification_value_fits(uint16_t negotiated_mtu,
                                         size_t value_len);
void gateway_ble_tx_cursor_init(struct gateway_ble_tx_cursor *cursor,
                                const uint8_t *frame,
                                size_t frame_len,
                                uint16_t negotiated_mtu);
int gateway_ble_tx_cursor_set_mtu(struct gateway_ble_tx_cursor *cursor,
                                  uint16_t negotiated_mtu);
int gateway_ble_tx_cursor_begin(struct gateway_ble_tx_cursor *cursor,
                                const uint8_t **chunk,
                                size_t *chunk_len);
int gateway_ble_tx_cursor_complete(struct gateway_ble_tx_cursor *cursor,
                                   bool accepted);
bool gateway_ble_tx_cursor_done(const struct gateway_ble_tx_cursor *cursor);

void gateway_ble_rx_stream_init(struct gateway_ble_rx_stream *stream);
int gateway_ble_rx_stream_feed(struct gateway_ble_rx_stream *stream,
                               const uint8_t *bytes,
                               size_t byte_count,
                               gateway_ble_rx_packet_fn on_packet,
                               void *ctx,
                               size_t *decoded_count);
void gateway_ble_link_init(struct gateway_ble_link *link,
                           uint32_t connection_interval_us,
                           uint8_t credit_capacity);
int gateway_ble_link_connect(struct gateway_ble_link *link,
                             uint64_t now_us,
                             uint16_t negotiated_mtu,
                             bool notify_enabled);
uint8_t gateway_ble_link_disconnect(struct gateway_ble_link *link);
int gateway_ble_link_set_mtu(struct gateway_ble_link *link,
                             uint16_t negotiated_mtu);
void gateway_ble_link_set_stalled(struct gateway_ble_link *link, bool stalled);
int gateway_ble_link_try_notify(struct gateway_ble_link *link,
                                size_t value_len);
int gateway_ble_link_run_connection_event(struct gateway_ble_link *link,
                                          uint64_t now_us,
                                          uint8_t *completed);

#ifdef __cplusplus
}
#endif

#endif
