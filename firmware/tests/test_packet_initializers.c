#include "gateway_command.h"
#include "mesh.h"
#include "report.h"
#include <assert.h>
#include <string.h>

#define ANCHOR_ID 0x1111222233334444ull
#define PEER_ID 0x5555666677778888ull
#define GATEWAY_ID 0x9999888877776666ull
#define SESSION_ID 0xAABBCCDDu

static void poison_packet(struct proto_packet *packet)
{
    memset(packet, 0xA5, sizeof(*packet));
    assert(packet->message_age_ms != 0u);
}

static void expect_fresh_packet(int result, const struct proto_packet *packet)
{
    assert(result == PROTO_OK);
    assert(packet->message_age_ms == 0u);
}

static void test_mesh_packet_initializers_reset_message_age(void)
{
    struct proto_packet packet;

    poison_packet(&packet);
    expect_fresh_packet(mesh_init_event_control(&packet,
                                                MSG_MESH_EVENT_PROPOSE,
                                                ANCHOR_ID,
                                                PEER_ID,
                                                SESSION_ID,
                                                1u,
                                                12u),
                        &packet);

    poison_packet(&packet);
    expect_fresh_packet(mesh_init_gateway_ack(&packet,
                                              GATEWAY_ID,
                                              ANCHOR_ID,
                                              SESSION_ID,
                                              2u,
                                              8u),
                        &packet);

    poison_packet(&packet);
    expect_fresh_packet(mesh_init_command(&packet,
                                          GATEWAY_ID,
                                          ANCHOR_ID,
                                          SESSION_ID,
                                          3u,
                                          16u),
                        &packet);

    poison_packet(&packet);
    expect_fresh_packet(mesh_init_command_result(&packet,
                                                 ANCHOR_ID,
                                                 GATEWAY_ID,
                                                 SESSION_ID,
                                                 4u,
                                                 16u,
                                                 true),
                        &packet);
}

static void test_report_packet_initializers_reset_message_age(void)
{
    struct proto_packet packet;

    poison_packet(&packet);
    expect_fresh_packet(report_init_range_packet(&packet,
                                                 ANCHOR_ID,
                                                 GATEWAY_ID,
                                                 SESSION_ID,
                                                 1u,
                                                 FLAG_DIAGNOSTIC,
                                                 20u),
                        &packet);

    poison_packet(&packet);
    expect_fresh_packet(report_init_click_packet(&packet,
                                                 ANCHOR_ID,
                                                 GATEWAY_ID,
                                                 SESSION_ID,
                                                 2u,
                                                 20u),
                        &packet);

    poison_packet(&packet);
    expect_fresh_packet(report_init_self_test_packet(&packet,
                                                     ANCHOR_ID,
                                                     GATEWAY_ID,
                                                     SESSION_ID,
                                                     3u,
                                                     20u),
                        &packet);

    poison_packet(&packet);
    expect_fresh_packet(report_init_anchor_heartbeat_packet(&packet,
                                                            ANCHOR_ID,
                                                            GATEWAY_ID,
                                                            SESSION_ID,
                                                            4u,
                                                            20u),
                        &packet);
}

static void test_gateway_result_initializer_resets_message_age(void)
{
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = GATEWAY_ID,
        .dst_id = ANCHOR_ID,
        .session_id = SESSION_ID,
        .seq = 1u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet result;
    uint8_t payload[32];
    size_t payload_len = 0u;

    poison_packet(&result);
    expect_fresh_packet(gateway_command_build_result(&command,
                                                     GATEWAY_ID,
                                                     CMD_GET_STATUS,
                                                     COMMAND_OK,
                                                     0u,
                                                     1000u,
                                                     &result,
                                                     payload,
                                                     sizeof(payload),
                                                     &payload_len),
                        &result);
}

int main(void)
{
    test_mesh_packet_initializers_reset_message_age();
    test_report_packet_initializers_reset_message_age();
    test_gateway_result_initializer_resets_message_age();
    return 0;
}
