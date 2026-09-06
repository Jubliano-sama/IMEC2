/* Reuse the current C5 application include and the existing fake radio seam. */
#define main existing_c5_test_main
#include "../../../../firmware/tests/test_app_mesh_c5_batch.c"
#undef main

int main(void)
{
    reset_fixture(2u);
    queue[0].packet.src_id = UINT64_C(0xb100); /* old transit packet */
    queue[0].packet.seq = 99u;
    queue[1].packet.seq = 100u; /* later local packet */
    receive_error = -ETIMEDOUT;
    assert(mesh_report_delivery_step() == 0);
    assert(mesh_report_delivery.count == 1u && queue_count == 1u);
    unsigned horizon_s = mesh_relay_outbox_expiry_s_for_packet(
        &mesh_report_delivery.entries[0].outbound.packet,
        mesh_report_delivery.entries[0].outbound.payload,
        mesh_report_delivery.entries[0].outbound.payload_len);
    now_ms += (horizon_s + 1u) * 1000u;
    struct route_candidate route = {
        .next_hop_id = GATEWAY_ID, .gateway_id = GATEWAY_ID,
        .route_epoch = 1u, .last_seen_ms = now_ms,
        .hop_count = 0u, .link_quality = 90u, .valid = true,
    };
    assert(route_upsert_candidate(&mesh_runtime.upstream, &route) == PROTO_OK);
    sent_count = ack_reads = 0u;
    assert(mesh_report_delivery_step() == 0);
    printf("after_core_expiry_s=%u bank=%u bank_seq=%u "
           "queued_local=%u local_seq=%u sent_seq=%u\n",
           horizon_s, mesh_report_delivery.count,
           mesh_report_delivery.entries[0].outbound.packet.seq,
           queue_count, queue[0].packet.seq, sent_count ? sent[0].packet.seq : 0u);
    assert(mesh_report_delivery.count == 1u && queue_count == 1u);
    assert(mesh_report_delivery.entries[0].outbound.packet.seq == 99u);
    assert(queue[0].packet.seq == 100u);
    return 0;
}
