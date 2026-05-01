#ifndef SERIAL_FRAME_H
#define SERIAL_FRAME_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_FRAME_DELIMITER 0u
#define SERIAL_FRAME_MAX_LEN (PACKET_MAX_LEN + (PACKET_MAX_LEN / 254u) + 2u)

int serial_frame_encode_packet(const struct proto_packet *packet,
                               const uint8_t *payload,
                               uint8_t *out,
                               size_t out_cap,
                               size_t *written);
int serial_frame_decode_packet(const uint8_t *frame,
                               size_t frame_len,
                               struct proto_packet *packet,
                               uint8_t *payload,
                               size_t payload_cap,
                               size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif
