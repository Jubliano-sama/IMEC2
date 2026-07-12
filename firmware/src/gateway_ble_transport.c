#include "gateway_ble_transport.h"

#include <string.h>

uint16_t gateway_ble_att_payload_max(uint16_t negotiated_mtu)
{
    if (negotiated_mtu < GATEWAY_BLE_ATT_DEFAULT_MTU) {
        return GATEWAY_BLE_ATT_DEFAULT_VALUE_MAX;
    }
    if (negotiated_mtu > GATEWAY_BLE_ATT_MAX_MTU) {
        negotiated_mtu = GATEWAY_BLE_ATT_MAX_MTU;
    }
    return (uint16_t)(negotiated_mtu - GATEWAY_BLE_ATT_VALUE_OVERHEAD);
}

bool gateway_ble_notification_value_fits(uint16_t negotiated_mtu,
                                         size_t value_len)
{
    return value_len <= gateway_ble_att_payload_max(negotiated_mtu);
}

void gateway_ble_tx_cursor_init(struct gateway_ble_tx_cursor *cursor,
                                const uint8_t *frame,
                                size_t frame_len,
                                uint16_t negotiated_mtu)
{
    if (cursor == NULL) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->frame = frame;
    cursor->frame_len = frame_len;
    cursor->negotiated_mtu = negotiated_mtu;
}

int gateway_ble_tx_cursor_set_mtu(struct gateway_ble_tx_cursor *cursor,
                                  uint16_t negotiated_mtu)
{
    if (cursor == NULL || cursor->in_flight) {
        return PROTO_ERR_BUSY;
    }
    cursor->negotiated_mtu = negotiated_mtu;
    return PROTO_OK;
}

int gateway_ble_tx_cursor_begin(struct gateway_ble_tx_cursor *cursor,
                                const uint8_t **chunk,
                                size_t *chunk_len)
{
    size_t remaining;
    size_t cap;

    if (cursor == NULL || chunk == NULL || chunk_len == NULL ||
        cursor->frame == NULL || cursor->frame_len == 0u) {
        return PROTO_ERR_ARG;
    }
    if (cursor->in_flight) {
        return PROTO_ERR_BUSY;
    }
    if (cursor->offset >= cursor->frame_len) {
        return PROTO_ERR_NOT_FOUND;
    }
    cap = gateway_ble_att_payload_max(cursor->negotiated_mtu);
    remaining = cursor->frame_len - cursor->offset;
    cursor->in_flight_len = remaining < cap ? remaining : cap;
    cursor->in_flight = true;
    *chunk = &cursor->frame[cursor->offset];
    *chunk_len = cursor->in_flight_len;
    return PROTO_OK;
}

int gateway_ble_tx_cursor_complete(struct gateway_ble_tx_cursor *cursor,
                                   bool accepted)
{
    if (cursor == NULL || !cursor->in_flight) {
        return PROTO_ERR_ARG;
    }
    if (accepted) {
        cursor->offset += cursor->in_flight_len;
    }
    cursor->in_flight_len = 0u;
    cursor->in_flight = false;
    return PROTO_OK;
}

bool gateway_ble_tx_cursor_done(const struct gateway_ble_tx_cursor *cursor)
{
    return cursor != NULL && cursor->frame_len > 0u &&
           cursor->offset == cursor->frame_len && !cursor->in_flight;
}

void gateway_ble_rx_stream_init(struct gateway_ble_rx_stream *stream)
{
    if (stream != NULL) {
        memset(stream, 0, sizeof(*stream));
    }
}

int gateway_ble_rx_stream_feed(struct gateway_ble_rx_stream *stream,
                               const uint8_t *bytes,
                               size_t byte_count,
                               gateway_ble_rx_packet_fn on_packet,
                               void *ctx,
                               size_t *decoded_count)
{
    size_t decoded = 0u;
    int first_error = PROTO_OK;

    if (stream == NULL || (bytes == NULL && byte_count != 0u) ||
        on_packet == NULL) {
        return PROTO_ERR_ARG;
    }
    for (size_t i = 0u; i < byte_count; i++) {
        uint8_t byte = bytes[i];

        if (byte != SERIAL_FRAME_DELIMITER) {
            if (!stream->overflow) {
                if (stream->frame_len + 1u >= sizeof(stream->frame)) {
                    stream->overflow = true;
                } else {
                    stream->frame[stream->frame_len++] = byte;
                }
            }
            continue;
        }

        if (stream->overflow) {
            stream->rejected_frames++;
            stream->frame_len = 0u;
            stream->overflow = false;
            if (first_error == PROTO_OK) {
                first_error = PROTO_ERR_NO_SPACE;
            }
            continue;
        }
        if (stream->frame_len == 0u) {
            continue;
        }

        stream->frame[stream->frame_len++] = SERIAL_FRAME_DELIMITER;
        {
            struct proto_packet packet;
            uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
            size_t payload_len = 0u;
            int ret = serial_frame_decode_packet(stream->frame,
                                                 stream->frame_len,
                                                 &packet,
                                                 payload,
                                                 sizeof(payload),
                                                 &payload_len);

            if (ret == PROTO_OK) {
                ret = on_packet(&packet, payload, payload_len, ctx);
            }
            if (ret == PROTO_OK) {
                stream->decoded_frames++;
                decoded++;
            } else {
                stream->rejected_frames++;
                if (first_error == PROTO_OK) {
                    first_error = ret;
                }
            }
        }
        stream->frame_len = 0u;
    }
    if (decoded_count != NULL) {
        *decoded_count = decoded;
    }
    return first_error;
}

void gateway_ble_link_init(struct gateway_ble_link *link,
                           uint32_t connection_interval_us,
                           uint8_t credit_capacity)
{
    if (link == NULL) {
        return;
    }
    memset(link, 0, sizeof(*link));
    link->connection_interval_us = connection_interval_us;
    link->credit_capacity = credit_capacity;
}

int gateway_ble_link_connect(struct gateway_ble_link *link,
                             uint64_t now_us,
                             uint16_t negotiated_mtu,
                             bool notify_enabled)
{
    if (link == NULL || link->connection_interval_us == 0u ||
        link->credit_capacity == 0u) {
        return GATEWAY_BLE_LINK_ERR_ARG;
    }
    link->connection_generation++;
    if (link->connection_generation == 0u) {
        link->connection_generation = 1u;
    }
    link->negotiated_mtu = negotiated_mtu < GATEWAY_BLE_ATT_DEFAULT_MTU ?
                           GATEWAY_BLE_ATT_DEFAULT_MTU : negotiated_mtu;
    if (link->negotiated_mtu > GATEWAY_BLE_ATT_MAX_MTU) {
        link->negotiated_mtu = GATEWAY_BLE_ATT_MAX_MTU;
    }
    link->available_credits = link->credit_capacity;
    link->in_flight = 0u;
    link->connected = true;
    link->notify_enabled = notify_enabled;
    link->central_stalled = false;
    link->next_event_us = now_us + link->connection_interval_us;
    return GATEWAY_BLE_LINK_OK;
}

uint8_t gateway_ble_link_disconnect(struct gateway_ble_link *link)
{
    uint8_t dropped;

    if (link == NULL) {
        return 0u;
    }
    dropped = link->in_flight;
    link->notifications_dropped_disconnect += dropped;
    link->available_credits = 0u;
    link->in_flight = 0u;
    link->connected = false;
    link->notify_enabled = false;
    link->central_stalled = false;
    link->next_event_us = 0u;
    return dropped;
}

int gateway_ble_link_set_mtu(struct gateway_ble_link *link,
                             uint16_t negotiated_mtu)
{
    if (link == NULL || negotiated_mtu < GATEWAY_BLE_ATT_DEFAULT_MTU ||
        negotiated_mtu > GATEWAY_BLE_ATT_MAX_MTU) {
        return GATEWAY_BLE_LINK_ERR_ARG;
    }
    if (!link->connected) {
        return GATEWAY_BLE_LINK_ERR_NOT_CONNECTED;
    }
    link->negotiated_mtu = negotiated_mtu;
    return GATEWAY_BLE_LINK_OK;
}

void gateway_ble_link_set_stalled(struct gateway_ble_link *link, bool stalled)
{
    if (link != NULL) {
        link->central_stalled = stalled;
    }
}

int gateway_ble_link_try_notify(struct gateway_ble_link *link,
                                size_t value_len)
{
    if (link == NULL || value_len == 0u) {
        return GATEWAY_BLE_LINK_ERR_ARG;
    }
    if (!link->connected) {
        return GATEWAY_BLE_LINK_ERR_NOT_CONNECTED;
    }
    if (!link->notify_enabled) {
        return GATEWAY_BLE_LINK_ERR_NOTIFY_DISABLED;
    }
    if (!gateway_ble_notification_value_fits(link->negotiated_mtu,
                                             value_len)) {
        return GATEWAY_BLE_LINK_ERR_VALUE_TOO_LONG;
    }
    if (link->available_credits == 0u ||
        link->in_flight >= link->credit_capacity) {
        return GATEWAY_BLE_LINK_ERR_NO_CREDIT;
    }
    link->available_credits--;
    link->in_flight++;
    link->notifications_submitted++;
    return GATEWAY_BLE_LINK_OK;
}

int gateway_ble_link_run_connection_event(struct gateway_ble_link *link,
                                          uint64_t now_us,
                                          uint8_t *completed)
{
    uint64_t elapsed_events;
    uint8_t completed_now = 0u;

    if (link == NULL || completed == NULL) {
        return GATEWAY_BLE_LINK_ERR_ARG;
    }
    if (!link->connected) {
        return GATEWAY_BLE_LINK_ERR_NOT_CONNECTED;
    }
    if (now_us < link->next_event_us) {
        return GATEWAY_BLE_LINK_ERR_EVENT_TIME;
    }
    elapsed_events = ((now_us - link->next_event_us) /
                      link->connection_interval_us) + 1u;
    link->next_event_us += elapsed_events * link->connection_interval_us;
    if (!link->central_stalled) {
        completed_now = link->in_flight;
        link->in_flight = 0u;
        link->available_credits = link->credit_capacity;
        link->notifications_completed += completed_now;
    }
    *completed = completed_now;
    return GATEWAY_BLE_LINK_OK;
}
