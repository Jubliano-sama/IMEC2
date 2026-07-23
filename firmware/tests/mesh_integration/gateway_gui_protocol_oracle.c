#include "gateway_command.h"
#include "report.h"
#include "serial_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    uint8_t frame[PACKET_MAX_LEN + 8u];
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    struct proto_packet packet = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    bool validate_click = false;
    size_t frame_len;
    size_t payload_len = 0u;
    int ret;

    if (argc == 2) {
        validate_click = false;
    } else if (argc == 3 && strcmp(argv[1], "--validate-click") == 0) {
        validate_click = true;
    } else {
        return 2;
    }
    if (strlen(argv[validate_click ? 2 : 1]) % 2u != 0u) return 2;
    frame_len = strlen(argv[validate_click ? 2 : 1]) / 2u;
    if (frame_len > sizeof(frame)) return 2;
    for (size_t i = 0u; i < frame_len; i++) {
        const char *hex = argv[validate_click ? 2 : 1];
        int high = hex_nibble(hex[i * 2u]);
        int low = hex_nibble(hex[i * 2u + 1u]);
        if (high < 0 || low < 0) return 2;
        frame[i] = (uint8_t)((high << 4) | low);
    }
    ret = serial_frame_decode_packet(frame, frame_len, &packet, payload,
                                     sizeof(payload), &payload_len);
    if (ret != PROTO_OK) {
        printf("decode_error=%d\n", ret);
        return 1;
    }
    if (validate_click) {
        ret = report_validate_click_payload(&packet, payload, payload_len);
        printf("click_validation=%d\n", ret);
        return 0;
    }
    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK) {
        printf("command_error=%d\n", ret);
        return 1;
    }
    printf("msg_type=%u src=%llu dst=%llu session=%u seq=%u ttl=%u payload_len=%zu command_id=%u payload=",
           packet.msg_type, (unsigned long long)packet.src_id,
           (unsigned long long)packet.dst_id, packet.session_id, packet.seq,
           packet.ttl, payload_len, (unsigned int)command_id);
    for (size_t i = 0u; i < payload_len; i++) printf("%02x", payload[i]);
    putchar('\n');
    return 0;
}
