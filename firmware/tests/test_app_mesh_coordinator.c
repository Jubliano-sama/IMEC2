#include "app_mesh_coordinator.h"
#include "protocol.h"

#include <assert.h>
#include <string.h>

static struct mesh_outbound outbound(uint16_t seq)
{
    struct mesh_outbound out = {0};

    out.packet.msg_type = MSG_MESH_DATA;
    out.packet.src_id = 0x1111u;
    out.packet.dst_id = 0x2222u;
    out.packet.session_id = 7u;
    out.packet.seq = seq;
    out.packet.ttl = 4u;
    out.payload_len = 0u;
    out.packet.payload_len = 0u;
    return out;
}

static uint32_t require_loss_tlv(const struct mesh_outbound *out)
{
    const uint8_t *value;
    uint8_t len;

    assert(tlv_find(out->payload,
                    out->payload_len,
                    TLV_MESH_LOST_PACKET_COUNT,
                    &value,
                    &len) == PROTO_OK);
    assert(len == 4u);
    return proto_get_u32_le(value);
}

static void test_click_priority_blocks_mesh_work(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .click_priority = true,
        .rx_queue_pending = true,
        .report_queue_pending = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_CLICK);
    assert(!decision.mesh_work_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(!decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "click") == 0);
}

static void test_mesh_tx_blocks_background_rx(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .route_waiting_tx_active = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_MESH_TX);
    assert(decision.mesh_work_allowed);
    assert(decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(!decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "mesh-tx") == 0);
}

static void test_survey_blocks_mesh_work(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .survey_pending = true,
        .report_queue_pending = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_SURVEY);
    assert(!decision.mesh_work_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(!decision.uwb_rx_allowed);
    assert(strcmp(app_mesh_coordinator_state_name(decision.state), "survey") == 0);
}

static void test_rx_queue_blocks_route_wait_and_report_tx(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .rx_queue_pending = true,
        .route_waiting_tx_active = true,
        .report_queue_pending = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_MESH_RX);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(decision.uwb_rx_allowed);
}

static void test_ch9_ack_wait_allows_rx_and_blocks_new_tx(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .ch9_ack_wait_active = true,
        .report_queue_pending = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-wait") == 0);
}

static void test_ch9_ack_send_preempts_new_tx(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .ch9_ack_send_pending = true,
        .route_waiting_tx_active = true,
        .report_queue_pending = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_MESH_RX);
    assert(decision.mesh_work_allowed);
    assert(!decision.route_wait_allowed);
    assert(!decision.report_tx_allowed);
    assert(decision.uwb_rx_allowed);
    assert(strcmp(decision.reason, "ch9-ack-send") == 0);
}

static void test_report_queue_can_drive_mesh_tx_state(void)
{
    const struct app_mesh_coordinator_inputs inputs = {
        .report_queue_pending = true,
    };
    struct app_mesh_coordinator_decision decision;

    app_mesh_coordinator_decide(&inputs, &decision);

    assert(decision.state == APP_MESH_COORDINATOR_MESH_TX);
    assert(decision.report_tx_allowed);
    assert(!decision.uwb_rx_allowed);
}

static void test_replacing_paused_packet_counts_loss(void)
{
    struct app_mesh_paused_delivery_state state = {0};
    struct app_mesh_paused_delivery_store_result store_result;
    struct app_mesh_paused_delivery_attach_result attach_result;
    struct mesh_outbound first = outbound(1u);
    struct mesh_outbound second = outbound(2u);

    app_mesh_paused_delivery_note_store(&state,
                                        false,
                                        NULL,
                                        &first,
                                        &store_result);
    assert(!store_result.replaced_existing);
    assert(store_result.lost_count == 0u);

    app_mesh_paused_delivery_note_store(&state,
                                        true,
                                        &first,
                                        &second,
                                        &store_result);
    assert(store_result.replaced_existing);
    assert(store_result.lost_count == 1u);

    assert(app_mesh_paused_delivery_attach_loss(&state,
                                                &second,
                                                &attach_result) == PROTO_OK);
    assert(attach_result.tlv_attached);
    assert(require_loss_tlv(&second) == 1u);

    app_mesh_paused_delivery_note_sent(&state, &second);
    assert(app_mesh_paused_delivery_lost_count(&state) == 0u);
}

static void test_same_paused_packet_does_not_count_loss(void)
{
    struct app_mesh_paused_delivery_state state = {0};
    struct app_mesh_paused_delivery_store_result store_result;
    struct mesh_outbound first = outbound(1u);
    struct mesh_outbound same = outbound(1u);

    app_mesh_paused_delivery_note_store(&state,
                                        true,
                                        &first,
                                        &same,
                                        &store_result);

    assert(!store_result.replaced_existing);
    assert(store_result.lost_count == 0u);
}

static void test_loss_tlv_updates_when_more_packets_are_lost(void)
{
    struct app_mesh_paused_delivery_state state = {0};
    struct app_mesh_paused_delivery_attach_result attach_result;
    struct mesh_outbound first = outbound(1u);
    struct mesh_outbound second = outbound(2u);
    struct mesh_outbound third = outbound(3u);

    app_mesh_paused_delivery_note_store(&state, true, &first, &second, NULL);
    assert(app_mesh_paused_delivery_attach_loss(&state,
                                                &second,
                                                &attach_result) == PROTO_OK);
    assert(require_loss_tlv(&second) == 1u);

    app_mesh_paused_delivery_note_store(&state, true, &second, &third, NULL);
    assert(app_mesh_paused_delivery_attach_loss(&state,
                                                &third,
                                                &attach_result) == PROTO_OK);
    assert(require_loss_tlv(&third) == 2u);
}

static void test_loss_count_stays_pending_when_tlv_has_no_space(void)
{
    struct app_mesh_paused_delivery_state state = {0};
    struct app_mesh_paused_delivery_attach_result attach_result;
    struct mesh_outbound first = outbound(1u);
    struct mesh_outbound second = outbound(2u);

    second.payload_len = sizeof(second.payload);
    second.packet.payload_len = (uint16_t)second.payload_len;
    memset(second.payload, 0xAA, sizeof(second.payload));

    app_mesh_paused_delivery_note_store(&state, true, &first, &second, NULL);
    assert(app_mesh_paused_delivery_attach_loss(&state,
                                                &second,
                                                &attach_result) != PROTO_OK);
    assert(attach_result.lost_count_pending);
    assert(app_mesh_paused_delivery_lost_count(&state) == 1u);
}

int main(void)
{
    test_click_priority_blocks_mesh_work();
    test_mesh_tx_blocks_background_rx();
    test_survey_blocks_mesh_work();
    test_rx_queue_blocks_route_wait_and_report_tx();
    test_ch9_ack_wait_allows_rx_and_blocks_new_tx();
    test_ch9_ack_send_preempts_new_tx();
    test_report_queue_can_drive_mesh_tx_state();
    test_replacing_paused_packet_counts_loss();
    test_same_paused_packet_does_not_count_loss();
    test_loss_tlv_updates_when_more_packets_are_lost();
    test_loss_count_stays_pending_when_tlv_has_no_space();
    return 0;
}
