#include "app_mesh_local_delivery.h"
#include "mesh.h"
#include "survey.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct journal_store {
    struct app_mesh_local_delivery_snapshot persisted;
    unsigned int save_count;
    unsigned int clear_count;
    int save_result;
    int clear_result;
    bool present;
};

static int journal_save(void *ctx,
                        const struct app_mesh_local_delivery_snapshot *snapshot)
{
    struct journal_store *store = ctx;

    store->save_count++;
    if (store->save_result != 0) {
        return store->save_result;
    }
    if (!app_mesh_local_delivery_snapshot_valid(snapshot)) {
        return -EINVAL;
    }
    store->persisted = *snapshot;
    store->present = true;
    return 0;
}

static int journal_clear(void *ctx)
{
    struct journal_store *store = ctx;

    store->clear_count++;
    if (store->clear_result != 0) {
        return store->clear_result;
    }
    memset(&store->persisted, 0, sizeof(store->persisted));
    store->present = false;
    return 0;
}

static struct app_mesh_local_delivery make_delivery(struct journal_store *store)
{
    struct app_mesh_local_delivery delivery;
    const struct app_mesh_local_delivery_ops ops = {
        .save = journal_save,
        .clear = journal_clear,
        .ctx = store,
    };

    app_mesh_local_delivery_init(&delivery, &ops);
    return delivery;
}

static struct mesh_outbound make_report_from(uint64_t anchor_id,
                                             uint32_t survey_id,
                                             uint16_t seq)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;

    assert(survey_append_reach_report_tlvs(
               outbound.payload,
               sizeof(outbound.payload),
               &payload_len,
               survey_id,
               anchor_id,
               NULL,
               0u) == PROTO_OK);
    assert(survey_operation_generation_append_tlv(
               outbound.payload,
               sizeof(outbound.payload),
               &payload_len,
               survey_id) == PROTO_OK);
    assert(tlv_append_u32(outbound.payload,
                          sizeof(outbound.payload),
                          &payload_len,
                          TLV_NODE_BOOT_COUNTER,
                          1u) == PROTO_OK);
    assert(tlv_append_u16(outbound.payload,
                          sizeof(outbound.payload),
                          &payload_len,
                          TLV_COMMAND_STATUS,
                          COMMAND_OK) == PROTO_OK);
    assert(survey_init_discovery_report_packet(
               &outbound.packet,
               anchor_id,
               UINT64_C(0x8877665544332211),
               survey_id,
               survey_id,
               1u,
               seq,
               (uint8_t)payload_len) == PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    outbound.radio_channel = 9u;
    outbound.next_hop_id = UINT64_C(0x0102030405060708);
    outbound.queued_at_ms = 1234u;
    outbound.queued_at_valid = true;
    outbound.earliest_tx_ms = 1275u;
    outbound.earliest_tx_valid = true;
    return outbound;
}

static struct mesh_outbound make_report(uint32_t survey_id, uint16_t seq)
{
    return make_report_from(UINT64_C(0x1020304050607080),
                            survey_id,
                            seq);
}

static struct mesh_outbound make_pair_result(uint32_t survey_id,
                                             uint16_t seq)
{
    struct mesh_outbound outbound = {0};
    const struct survey_sample sample = {
        .pair = {
            .initiator_id = UINT64_C(0x1020304050607080),
            .responder_id = UINT64_C(0x2030405060708090),
            .operation_generation = survey_id,
            .survey_id = survey_id,
            .sample_count = 1u,
        },
        .round_id = 1u,
        .sample_index = 0u,
        .distance_mm = 2500,
        .quality = 90u,
        .range_status = RANGE_OK,
    };
    size_t payload_len = 0u;

    assert(survey_append_sample_tlvs(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     &sample) == PROTO_OK);
    assert(tlv_append_u64(outbound.payload,
                          sizeof(outbound.payload),
                          &payload_len,
                          TLV_TIMESTAMP_MS,
                          UINT64_C(0x0102030405060708)) == PROTO_OK);
    assert(tlv_append_i8(outbound.payload,
                         sizeof(outbound.payload),
                         &payload_len,
                         TLV_UWB_RSL_DBM,
                         -73) == PROTO_OK);
    assert(tlv_append_u16(outbound.payload,
                          sizeof(outbound.payload),
                          &payload_len,
                          TLV_UWB_CLOCK_OFFSET_RAW,
                          UINT16_C(0xfedc)) == PROTO_OK);
    assert(tlv_append_u16(outbound.payload,
                          sizeof(outbound.payload),
                          &payload_len,
                          TLV_CLICKER_CLOCK_OFFSET_RAW,
                          UINT16_C(0x8123)) == PROTO_OK);
    assert(tlv_append_i32(outbound.payload,
                          sizeof(outbound.payload),
                          &payload_len,
                          TLV_UWB_CARRIER_INTEGRATOR,
                          INT32_C(-1234567)) == PROTO_OK);
    {
        uint8_t raw_timestamps[6u * sizeof(uint32_t)];

        for (size_t i = 0u; i < 6u; i++) {
            proto_put_u32_le(&raw_timestamps[i * sizeof(uint32_t)],
                             UINT32_C(0x10203040) + (uint32_t)i);
        }
        assert(tlv_append_bytes(outbound.payload,
                                sizeof(outbound.payload),
                                &payload_len,
                                TLV_UWB_RAW_TIMESTAMPS,
                                raw_timestamps,
                                sizeof(raw_timestamps)) == PROTO_OK);
    }
    assert(survey_init_result_packet_from_reporter(
               &outbound.packet,
               &sample,
               sample.pair.responder_id,
               UINT64_C(0x8877665544332211),
               seq,
               (uint8_t)payload_len) == PROTO_OK);
    outbound.payload_len = (uint16_t)payload_len;
    outbound.radio_channel = 9u;
    outbound.next_hop_id = UINT64_C(0x0102030405060708);
    outbound.queued_at_ms = 1234u;
    outbound.queued_at_valid = true;
    return outbound;
}

static size_t pair_result_core_len(const struct mesh_outbound *outbound)
{
    struct survey_sample sample = {0};
    uint8_t canonical[SURVEY_SAMPLE_TLV_MAX_LEN];
    size_t canonical_len = 0u;

    assert(survey_extract_sample_tlvs(outbound->payload,
                                      outbound->payload_len,
                                      &sample) == PROTO_OK);
    assert(survey_append_sample_tlvs(canonical,
                                     sizeof(canonical),
                                     &canonical_len,
                                     &sample) == PROTO_OK);
    return canonical_len;
}

static size_t find_tlv_offset(const uint8_t *payload,
                              size_t payload_len,
                              uint8_t wanted_type)
{
    size_t offset = 0u;

    while (offset < payload_len) {
        size_t value_len;

        assert(payload_len - offset >= PROTO_TLV_HEADER_LEN);
        value_len = payload[offset + 1u];
        assert(value_len <= payload_len - offset - PROTO_TLV_HEADER_LEN);
        if (payload[offset] == wanted_type) {
            return offset;
        }
        offset += PROTO_TLV_HEADER_LEN + value_len;
    }
    assert(false);
    return 0u;
}

static void outbound_digest(
    const struct mesh_outbound *outbound,
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN])
{
    assert(mesh_packet_semantic_digest(&outbound->packet,
                                       outbound->payload,
                                       outbound->payload_len,
                                       digest));
}

static int commit_ack_for_outbound(
    struct app_mesh_local_delivery *delivery,
    const struct mesh_outbound *outbound)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

    outbound_digest(outbound, digest);
    return app_mesh_local_delivery_commit_ack(
        delivery, &outbound->packet, digest);
}

static int note_ack_for_outbound(
    struct app_mesh_local_delivery *delivery,
    const struct mesh_outbound *outbound)
{
    uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];

    outbound_digest(outbound, digest);
    return app_mesh_local_delivery_note_ack(
        delivery, &outbound->packet, digest);
}

static void snapshot_rechecksum(
    struct app_mesh_local_delivery_snapshot *snapshot)
{
    const uint8_t *bytes = (const uint8_t *)snapshot;
    uint32_t hash = UINT32_C(2166136261);

    snapshot->checksum = 0u;
    for (size_t i = 0u; i < sizeof(*snapshot); i++) {
        uint8_t value =
            i >= offsetof(struct app_mesh_local_delivery_snapshot, checksum) &&
            i < offsetof(struct app_mesh_local_delivery_snapshot, checksum) +
                    sizeof(snapshot->checksum) ?
            0u : bytes[i];

        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    snapshot->checksum = hash;
}

static void test_transactional_stage_and_back_to_back_rejection(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound first = make_report(101u, 7u);
    const struct mesh_outbound second = make_report(102u, 8u);
    struct app_mesh_local_delivery_snapshot saved;

    store.save_result = -EIO;
    assert(app_mesh_local_delivery_stage(&delivery, &first, 101u) == -EIO);
    assert(!app_mesh_local_delivery_active(&delivery));
    assert(!store.present);

    store.save_result = 0;
    assert(app_mesh_local_delivery_stage(&delivery, &first, 101u) == 0);
    assert(store.present);
    assert(app_mesh_local_delivery_snapshot_valid(&store.persisted));
    assert(memcmp(&store.persisted.outbound, &first, sizeof(first)) == 0);
    saved = store.persisted;

    assert(app_mesh_local_delivery_stage(&delivery, &second, 102u) == -EBUSY);
    assert(memcmp(&store.persisted, &saved, sizeof(saved)) == 0);
}

static void test_reboot_and_exact_ack_identity(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct mesh_outbound outbound = make_report(201u, 19u);
    struct mesh_outbound wrong;
    unsigned int saves_before;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 201u) == 0);
    assert(app_mesh_local_delivery_note_state(
               &delivery, APP_MESH_LOCAL_DELIVERY_ROUTE_WAIT) == 0);
    assert(app_mesh_local_delivery_note_tracked(&delivery) == 0);

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(memcmp(app_mesh_local_delivery_outbound(&rebooted), &outbound,
                  sizeof(outbound)) == 0);
    assert(rebooted.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS - 1u);

    saves_before = store.save_count;
    wrong = outbound;
    wrong.packet.seq++;
    assert(note_ack_for_outbound(&rebooted, &wrong) ==
           -EKEYREJECTED);
    assert(app_mesh_local_delivery_note_ack(&rebooted, NULL, NULL) ==
           -EINVAL);
    assert(store.save_count == saves_before);
    assert(store.clear_count == 0u);

    assert(note_ack_for_outbound(&rebooted, &outbound) == 0);
    assert(!store.present);
    assert(!app_mesh_local_delivery_active(&rebooted));
    assert(store.clear_count == 1u);
}

static void test_reboot_clears_pretransport_delivery_times(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    const struct mesh_outbound outbound = make_report(211u, 20u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 211u) == 0);
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(app_mesh_local_delivery_rebase_after_boot(&rebooted, 17u) == 0);
    assert(rebooted.snapshot.outbound.queued_at_ms == 0u);
    assert(!rebooted.snapshot.outbound.queued_at_valid);
    assert(rebooted.snapshot.outbound.earliest_tx_ms == 0u);
    assert(!rebooted.snapshot.outbound.earliest_tx_valid);
    assert(store.persisted.outbound.queued_at_ms == 0u);
    assert(!store.persisted.outbound.queued_at_valid);
    assert(store.persisted.outbound.earliest_tx_ms == 0u);
    assert(!store.persisted.outbound.earliest_tx_valid);

    store.save_result = -EIO;
    assert(app_mesh_local_delivery_rebase_after_boot(&rebooted, 33u) == -EIO);
    assert(rebooted.snapshot.outbound.earliest_tx_ms == 0u);
    assert(!rebooted.snapshot.outbound.earliest_tx_valid);
    assert(app_mesh_local_delivery_snapshot_valid(&rebooted.snapshot));
    assert(store.persisted.outbound.earliest_tx_ms == 0u);
}

static void test_elapsed_not_before_is_retired_before_long_resource_wait(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct mesh_outbound outbound = make_report(216u, 22u);
    const uint32_t due_ms = UINT32_C(0xfffffff0);

    outbound.earliest_tx_ms = due_ms;
    outbound.earliest_tx_valid = true;
    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 216u) == 0);
    assert(app_mesh_local_delivery_retire_elapsed_not_before(
               &delivery, due_ms - 10u) == -EAGAIN);
    assert(delivery.snapshot.outbound.earliest_tx_valid);

    store.save_result = -EIO;
    assert(app_mesh_local_delivery_retire_elapsed_not_before(
               &delivery, due_ms) == -EIO);
    assert(delivery.snapshot.outbound.earliest_tx_valid);

    store.save_result = 0;
    assert(app_mesh_local_delivery_retire_elapsed_not_before(
               &delivery, due_ms) == 0);
    assert(!delivery.snapshot.outbound.earliest_tx_valid);
    assert(!store.persisted.outbound.earliest_tx_valid);
    /*
     * Model an unrelated reliable owner holding nodecomm beyond the signed
     * 32-bit comparison horizon and through uptime wrap. The source record no
     * longer carries the old response slot, so eventual retry cannot wait for
     * another wrap.
     */
    assert(app_mesh_local_delivery_retire_elapsed_not_before(
               &delivery, due_ms + UINT32_C(0x80000020)) == 0);
    assert(!delivery.snapshot.outbound.earliest_tx_valid);
}

static void test_pair_result_is_a_supported_exact_delivery_owner(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct mesh_outbound pair_result = make_pair_result(221u, 21u);

    assert(app_mesh_local_delivery_stage(
               &delivery, &pair_result, 221u) == 0);
    assert(store.present);
    assert(store.persisted.outbound.packet.msg_type ==
           MSG_SURVEY_PAIR_RESULT);
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(
               &rebooted, &store.persisted) == 0);
    assert(app_mesh_local_delivery_rebase_after_boot(
               &rebooted, 600000u) == 0);
    assert(!rebooted.snapshot.outbound.queued_at_valid);
    assert(rebooted.snapshot.outbound.queued_at_ms == 0u);
    assert(!rebooted.snapshot.outbound.earliest_tx_valid);
    assert(!store.persisted.outbound.queued_at_valid);
    assert(note_ack_for_outbound(&rebooted, &pair_result) == 0);
    assert(!store.present);
}

static void test_pair_result_accepts_exact_producer_extension_schema(void)
{
    struct journal_store full_store = {0};
    struct journal_store timestamp_store = {0};
    struct app_mesh_local_delivery full_delivery = make_delivery(&full_store);
    struct app_mesh_local_delivery timestamp_delivery =
        make_delivery(&timestamp_store);
    struct mesh_outbound full = make_pair_result(222u, 22u);
    struct mesh_outbound timestamp_only = full;
    size_t core_len = pair_result_core_len(&full);
    size_t timestamp_len = core_len + PROTO_TLV_U64_ENCODED_LEN;

    assert(full.payload[core_len] == TLV_TIMESTAMP_MS);
    assert(full.payload_len > timestamp_len);
    assert(app_mesh_local_delivery_stage(
               &full_delivery, &full, 222u) == 0);
    assert(full_store.present);
    assert(memcmp(full_store.persisted.outbound.payload,
                  full.payload,
                  full.payload_len) == 0);

    timestamp_only.payload_len = (uint16_t)timestamp_len;
    timestamp_only.packet.payload_len = (uint16_t)timestamp_len;
    assert(app_mesh_local_delivery_stage(
               &timestamp_delivery, &timestamp_only, 222u) == 0);
    assert(timestamp_store.present);
}

static void test_pair_result_extension_schema_fails_closed(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct mesh_outbound canonical = make_pair_result(223u, 23u);
    struct mesh_outbound mutation;
    size_t core_len = pair_result_core_len(&canonical);
    size_t timestamp_len = core_len + PROTO_TLV_U64_ENCODED_LEN;
    size_t extension_offset;
    size_t payload_len;

    mutation = canonical;
    mutation.payload_len = (uint16_t)core_len;
    mutation.packet.payload_len = (uint16_t)core_len;
    assert(app_mesh_local_delivery_stage(
               &delivery, &mutation, 223u) == -EINVAL);

    mutation = canonical;
    mutation.payload[core_len + 1u] = sizeof(uint64_t) - 1u;
    assert(app_mesh_local_delivery_stage(
               &delivery, &mutation, 223u) == -EINVAL);

    mutation = canonical;
    extension_offset = find_tlv_offset(mutation.payload,
                                       mutation.payload_len,
                                       TLV_UWB_RSL_DBM);
    mutation.payload[extension_offset] = UINT8_C(0x27);
    assert(app_mesh_local_delivery_stage(
               &delivery, &mutation, 223u) == -EINVAL);

    mutation = canonical;
    mutation.payload_len = (uint16_t)timestamp_len;
    mutation.packet.payload_len = (uint16_t)timestamp_len;
    payload_len = timestamp_len;
    assert(tlv_append_u64(mutation.payload,
                          sizeof(mutation.payload),
                          &payload_len,
                          TLV_TIMESTAMP_MS,
                          UINT64_C(0x1112131415161718)) == PROTO_OK);
    mutation.payload_len = (uint16_t)payload_len;
    mutation.packet.payload_len = (uint16_t)payload_len;
    assert(app_mesh_local_delivery_stage(
               &delivery, &mutation, 223u) == -EINVAL);

    mutation = canonical;
    mutation.payload_len = (uint16_t)timestamp_len;
    mutation.packet.payload_len = (uint16_t)timestamp_len;
    payload_len = timestamp_len;
    assert(tlv_append_i32(mutation.payload,
                          sizeof(mutation.payload),
                          &payload_len,
                          TLV_UWB_CARRIER_INTEGRATOR,
                          INT32_C(-91)) == PROTO_OK);
    assert(tlv_append_i8(mutation.payload,
                         sizeof(mutation.payload),
                         &payload_len,
                         TLV_UWB_RSL_DBM,
                         -81) == PROTO_OK);
    mutation.payload_len = (uint16_t)payload_len;
    mutation.packet.payload_len = (uint16_t)payload_len;
    assert(app_mesh_local_delivery_stage(
               &delivery, &mutation, 223u) == -EINVAL);

    mutation = canonical;
    extension_offset = find_tlv_offset(mutation.payload,
                                       core_len,
                                       TLV_RANGE_STATUS);
    mutation.payload[extension_offset + PROTO_TLV_HEADER_LEN] =
        UINT8_C(0xff);
    assert(app_mesh_local_delivery_stage(
               &delivery, &mutation, 223u) == -EINVAL);
    assert(!store.present);
}

static void test_canonical_payload_and_envelope_validation_fail_closed(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct mesh_outbound discovery = make_report(231u, 23u);
    struct mesh_outbound pair = make_pair_result(232u, 24u);
    struct app_mesh_local_delivery_snapshot corrupt;

    discovery.packet.flags ^= FLAG_DIAGNOSTIC;
    assert(app_mesh_local_delivery_stage(
               &delivery, &discovery, 231u) == -EINVAL);
    discovery = make_report(231u, 23u);
    discovery.packet.ttl--;
    assert(app_mesh_local_delivery_stage(
               &delivery, &discovery, 231u) == -EINVAL);
    discovery = make_report(231u, 23u);
    discovery.packet.dst_id = discovery.packet.src_id;
    assert(app_mesh_local_delivery_stage(
               &delivery, &discovery, 231u) == -EINVAL);
    discovery = make_report(231u, 23u);
    discovery.packet.session_id++;
    assert(app_mesh_local_delivery_stage(
               &delivery, &discovery, 231u) == -EINVAL);
    discovery = make_report(231u, 23u);
    discovery.payload[0] ^= 1u;
    assert(app_mesh_local_delivery_stage(
               &delivery, &discovery, 231u) == -EINVAL);

    pair.packet.flags ^= FLAG_DIAGNOSTIC;
    assert(app_mesh_local_delivery_stage(
               &delivery, &pair, 232u) == -EINVAL);
    pair = make_pair_result(232u, 24u);
    pair.packet.ttl--;
    assert(app_mesh_local_delivery_stage(
               &delivery, &pair, 232u) == -EINVAL);
    pair = make_pair_result(232u, 24u);
    pair.packet.dst_id = pair.packet.src_id;
    assert(app_mesh_local_delivery_stage(
               &delivery, &pair, 232u) == -EINVAL);
    pair = make_pair_result(232u, 24u);
    pair.packet.session_id++;
    assert(app_mesh_local_delivery_stage(
               &delivery, &pair, 232u) == -EINVAL);
    pair = make_pair_result(232u, 24u);
    pair.payload[find_tlv_offset(pair.payload,
                                pair_result_core_len(&pair),
                                TLV_RANGE_STATUS) +
                 PROTO_TLV_HEADER_LEN] = UINT8_C(0xff);
    assert(app_mesh_local_delivery_stage(
               &delivery, &pair, 232u) == -EINVAL);

    discovery = make_report(233u, 25u);
    assert(app_mesh_local_delivery_stage(
               &delivery, &discovery, 233u) == 0);
    corrupt = store.persisted;
    corrupt.outbound.packet.flags ^= FLAG_DIAGNOSTIC;
    snapshot_rechecksum(&corrupt);
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &corrupt) == -EINVAL);
    corrupt = store.persisted;
    corrupt.outbound.payload[0] ^= 1u;
    snapshot_rechecksum(&corrupt);
    assert(app_mesh_local_delivery_restore(&rebooted, &corrupt) == -EINVAL);
}

static void test_ack_identity_rejects_header_flags_and_payload_aliases(void)
{
    struct journal_store discovery_store = {0};
    struct journal_store pair_store = {0};
    struct app_mesh_local_delivery discovery_delivery =
        make_delivery(&discovery_store);
    struct app_mesh_local_delivery pair_delivery = make_delivery(&pair_store);
    const struct mesh_outbound discovery = make_report(241u, 26u);
    const struct mesh_outbound pair = make_pair_result(242u, 27u);
    struct mesh_outbound mutation;

    assert(app_mesh_local_delivery_stage(
               &discovery_delivery, &discovery, 241u) == 0);
    mutation = discovery;
    mutation.packet.session_id++;
    assert(commit_ack_for_outbound(
               &discovery_delivery, &mutation) == -EKEYREJECTED);
    mutation = discovery;
    mutation.packet.flags ^= FLAG_DIAGNOSTIC;
    assert(commit_ack_for_outbound(
               &discovery_delivery, &mutation) == -EKEYREJECTED);
    mutation = discovery;
    mutation.payload[mutation.payload_len - 1u] ^= 1u;
    assert(commit_ack_for_outbound(
               &discovery_delivery, &mutation) == -EKEYREJECTED);
    assert(commit_ack_for_outbound(
               &discovery_delivery, &discovery) == 0);

    assert(app_mesh_local_delivery_stage(
               &pair_delivery, &pair, 242u) == 0);
    mutation = pair;
    mutation.packet.session_id++;
    assert(commit_ack_for_outbound(
               &pair_delivery, &mutation) == -EKEYREJECTED);
    mutation = pair;
    mutation.packet.flags ^= FLAG_DIAGNOSTIC;
    assert(commit_ack_for_outbound(
               &pair_delivery, &mutation) == -EKEYREJECTED);
    mutation = pair;
    mutation.payload[mutation.payload_len - 1u] ^= 1u;
    assert(commit_ack_for_outbound(
               &pair_delivery, &mutation) == -EKEYREJECTED);
    assert(commit_ack_for_outbound(&pair_delivery, &pair) == 0);
}

static void test_blocked_start_does_not_consume_attempt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(251u, 24u);
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 251u) == 0);
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_ROUTE_WAIT) == 0);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);

    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, attempt_token) == 0);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS - 1u);
}

static void test_ack_commit_survives_clear_failure(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(301u, 29u);
    const struct mesh_outbound next = make_report(302u, 30u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 301u) == 0);
    store.clear_result = -EIO;
    assert(commit_ack_for_outbound(&delivery, &outbound) == 0);
    assert(store.present);
    assert(store.persisted.state == APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED);
    assert(!app_mesh_local_delivery_active(&delivery));
    assert(app_mesh_local_delivery_ack_committed(&delivery));
    assert(app_mesh_local_delivery_occupied(&delivery));
    assert(app_mesh_local_delivery_stage(&delivery, &next, 302u) == -EBUSY);
    assert(app_mesh_local_delivery_cleanup_ack(&delivery) == -EIO);
    assert(store.present);
    assert(app_mesh_local_delivery_ack_committed(&delivery));
    assert(app_mesh_local_delivery_stage(&delivery, &next, 302u) == -EBUSY);

    store.clear_result = 0;
    assert(app_mesh_local_delivery_cleanup_ack(&delivery) == 0);
    assert(!store.present);
    assert(!app_mesh_local_delivery_occupied(&delivery));
    assert(app_mesh_local_delivery_stage(&delivery, &next, 302u) == 0);
}

static void test_ack_write_failure_preserves_owner_and_terminal_order(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(321u, 31u);
    struct mesh_outbound stale = outbound;
    unsigned int clears_before;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 321u) == 0);
    stale.packet.seq++;
    clears_before = store.clear_count;
    assert(commit_ack_for_outbound(&delivery, &stale) ==
           -EKEYREJECTED);
    assert(store.clear_count == clears_before);
    assert(app_mesh_local_delivery_active(&delivery));

    store.save_result = -EIO;
    assert(commit_ack_for_outbound(&delivery, &outbound) == -EIO);
    assert(store.clear_count == clears_before);
    assert(store.persisted.state != APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED);
    assert(app_mesh_local_delivery_active(&delivery));

    store.save_result = 0;
    assert(commit_ack_for_outbound(&delivery, &outbound) == 0);
    assert(store.persisted.state == APP_MESH_LOCAL_DELIVERY_ACK_COMMITTED);
    assert(store.clear_count == clears_before);
    assert(app_mesh_local_delivery_cleanup_ack(&delivery) == 0);
}

static void test_recovery_retains_ack_cleanup_debt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct app_mesh_local_delivery_recovery recovery;
    const struct mesh_outbound outbound = make_report(331u, 32u);
    const struct mesh_outbound next = make_report(332u, 33u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 331u) == 0);
    assert(commit_ack_for_outbound(&delivery, &outbound) == 0);
    store.clear_result = -EIO;
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(
               &rebooted, &store.persisted, 1, &recovery) == 0);
    assert(!recovery.restored);
    assert(recovery.retry_required);
    assert(!recovery.quarantined);
    assert(recovery.source_error == -EIO);
    assert(app_mesh_local_delivery_ack_committed(&rebooted));
    assert(app_mesh_local_delivery_stage(&rebooted, &next, 332u) == -EBUSY);

    store.clear_result = 0;
    assert(app_mesh_local_delivery_cleanup_ack(&rebooted) == 0);
    assert(!store.present);
    assert(!app_mesh_local_delivery_occupied(&rebooted));
}

static void test_last_inflight_attempt_can_still_be_acked(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(351u, 34u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 351u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    store.persisted = delivery.snapshot;
    assert(app_mesh_local_delivery_note_tracked(&delivery) == 0);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(note_ack_for_outbound(&delivery, &outbound) == 0);
    assert(!store.present);
}

static void test_corruption_and_bounded_attempts_fail_closed(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    const struct mesh_outbound outbound = make_report(401u, 39u);
    struct app_mesh_local_delivery_snapshot corrupt;
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 401u) == 0);
    corrupt = store.persisted;
    corrupt.outbound.payload[0] ^= 1u;
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &corrupt) == -EINVAL);
    assert(!app_mesh_local_delivery_active(&rebooted));

    corrupt = store.persisted;
    corrupt.version++;
    assert(!app_mesh_local_delivery_snapshot_valid(&corrupt));
    corrupt = store.persisted;
    corrupt.size--;
    assert(!app_mesh_local_delivery_snapshot_valid(&corrupt));

    for (unsigned int i = 0u;
         i < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS; i++) {
        assert(app_mesh_local_delivery_begin_attempt(
                   &delivery, &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &delivery, attempt_token) == 0);
        if (i + 1u < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS) {
            assert(app_mesh_local_delivery_note_attempt_released(
                       &delivery, attempt_token,
                       APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
        }
    }
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_TRACKED);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_snapshot_valid(&store.persisted));
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == -EINPROGRESS);
    assert(app_mesh_local_delivery_note_attempt_released(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == -ETIMEDOUT);
    assert(app_mesh_local_delivery_rearm_attempts(&delivery) == 0);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_RETRY);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);
    assert(app_mesh_local_delivery_rearm_attempts(&delivery) == -EINVAL);
    for (unsigned int i = 0u;
         i < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS; i++) {
        assert(app_mesh_local_delivery_begin_attempt(
                   &delivery, &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &delivery, attempt_token) == 0);
        if (i + 1u < APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS) {
            assert(app_mesh_local_delivery_note_attempt_released(
                       &delivery, attempt_token,
                       APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
        }
    }
    assert(app_mesh_local_delivery_note_attempt_released(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(app_mesh_local_delivery_note_failed(&delivery) == 0);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_FAILED);

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(rebooted.snapshot.state == APP_MESH_LOCAL_DELIVERY_FAILED);

    store.clear_result = -EIO;
    assert(app_mesh_local_delivery_discard_failed(&rebooted) == -EIO);
    assert(app_mesh_local_delivery_active(&rebooted));
    assert(store.present);

    store.clear_result = 0;
    assert(app_mesh_local_delivery_discard_failed(&rebooted) == 0);
    assert(!app_mesh_local_delivery_active(&rebooted));
    assert(!store.present);
    assert(app_mesh_local_delivery_stage(&rebooted, &outbound, 402u) == 0);
}

static void test_synchronous_ack_wins_post_send_resolution(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(451u, 44u);
    uint8_t attempt_token;
    unsigned int saves_after_ack;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 451u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_snapshot_valid(&store.persisted));
    assert(note_ack_for_outbound(&delivery, &outbound) == 0);
    saves_after_ack = store.save_count;
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, attempt_token) == -EALREADY);
    assert(store.save_count == saves_after_ack);
    assert(!app_mesh_local_delivery_active(&delivery));
}

static void test_reset_after_start_does_not_refund_attempt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    const struct mesh_outbound outbound = make_report(501u, 49u);
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 501u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(store.persisted.state == APP_MESH_LOCAL_DELIVERY_STARTING);
    assert(store.persisted.attempts_remaining == 0u);

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_restore(&rebooted, &store.persisted) == 0);
    assert(app_mesh_local_delivery_note_attempt_released(
               &rebooted, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(rebooted.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_begin_attempt(
               &rebooted, &attempt_token) == -ETIMEDOUT);
}

static void test_stale_attempt_callback_cannot_overwrite_new_attempt(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(551u, 54u);
    uint8_t first_token;
    uint8_t second_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 551u) == 0);
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &first_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, first_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &second_token) == 0);
    assert(second_token != first_token);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, first_token) == -ESTALE);
    assert(delivery.snapshot.state == APP_MESH_LOCAL_DELIVERY_STARTING);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, second_token) == 0);
}

static void test_recovery_preserves_custody_on_transient_read_failure(void)
{
    const int transient_errors[] = {-EAGAIN, -EIO};

    for (size_t i = 0u; i < sizeof(transient_errors) /
                                  sizeof(transient_errors[0]); ++i) {
        struct journal_store store = {0};
        struct app_mesh_local_delivery delivery = make_delivery(&store);
        struct app_mesh_local_delivery_recovery recovery;

        assert(app_mesh_local_delivery_recover(
                   &delivery, NULL, transient_errors[i], &recovery) == 0);
        assert(!recovery.restored);
        assert(recovery.retry_required);
        assert(!recovery.quarantined);
        assert(recovery.source_error == transient_errors[i]);
        assert(recovery.clear_error == 0);
        assert(store.clear_count == 0u);
        assert(app_mesh_local_delivery_active(&delivery));

        assert(app_mesh_local_delivery_recover(&delivery, NULL, -ENOENT,
                                               &recovery) == 0);
        assert(!recovery.restored);
        assert(!recovery.retry_required);
        assert(!recovery.quarantined);
        assert(recovery.source_error == 0);
        assert(store.clear_count == 0u);
        assert(!app_mesh_local_delivery_active(&delivery));
    }
}

static void test_recovery_quarantines_bad_message_and_old_schema(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery rebooted;
    struct app_mesh_local_delivery_recovery recovery;
    const struct mesh_outbound outbound = make_report(601u, 59u);
    struct app_mesh_local_delivery_snapshot corrupt;

    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(&rebooted, NULL, -EBADMSG,
                                           &recovery) == 0);
    assert(recovery.quarantined);
    assert(recovery.source_error == -EBADMSG);
    assert(recovery.clear_error == 0);
    assert(store.clear_count == 1u);
    assert(!app_mesh_local_delivery_active(&rebooted));

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 601u) == 0);
    corrupt = store.persisted;
    corrupt.version--;
    rebooted = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(&rebooted, &corrupt, 1,
                                           &recovery) == 0);
    assert(recovery.quarantined);
    assert(recovery.source_error == -EBADMSG);
    assert(recovery.clear_error == 0);
    assert(store.clear_count == 2u);
    assert(!app_mesh_local_delivery_active(&rebooted));
}

static void test_recovery_reports_quarantine_clear_failure(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    struct app_mesh_local_delivery_recovery recovery;
    const struct mesh_outbound outbound = make_report(602u, 60u);
    struct app_mesh_local_delivery_snapshot corrupt;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 602u) == 0);
    corrupt = store.persisted;
    corrupt.checksum ^= 1u;
    store.clear_result = -EIO;
    delivery = make_delivery(&store);
    assert(app_mesh_local_delivery_recover(&delivery, &corrupt, 1,
                                           &recovery) == 0);
    assert(recovery.quarantined);
    assert(recovery.source_error == -EBADMSG);
    assert(recovery.clear_error == -EIO);
    assert(store.clear_count == 1u);
    assert(store.present);
    assert(!app_mesh_local_delivery_active(&delivery));
}

static void test_persistence_mock_rejects_invalid_snapshot_like_production(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(651u, 64u);
    struct app_mesh_local_delivery_snapshot invalid;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 651u) == 0);
    invalid = store.persisted;
    invalid.checksum ^= 1u;
    store.present = false;
    assert(journal_save(&store, &invalid) == -EINVAL);
    assert(!store.present);
}

static void test_sustained_pre_rf_contention_has_constant_write_bound(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(701u, 69u);
    uint8_t attempt_token;
    unsigned int writes_after_reservation;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 701u) == 0);
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &attempt_token) == 0);
    writes_after_reservation = store.save_count;
    assert(app_mesh_local_delivery_note_attempt_blocked(
               &delivery, attempt_token) == 0);

    for (unsigned int i = 0u; i < 10000u; i++) {
        uint8_t resumed_token = 0u;

        assert(app_mesh_local_delivery_attempts_available(&delivery) ==
               APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);
        assert(app_mesh_local_delivery_resume_blocked_attempt(
                   &delivery, &resumed_token) == 0);
        assert(resumed_token == attempt_token);
        assert(app_mesh_local_delivery_note_attempt_blocked(
                   &delivery, resumed_token) == 0);
    }
    assert(store.save_count == writes_after_reservation);
    assert(app_mesh_local_delivery_attempts_available(&delivery) ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);

    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(store.save_count == writes_after_reservation + 1u);
    assert(delivery.snapshot.attempts_remaining ==
           APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);
}

static void test_last_blocked_token_can_retry_send_and_ack(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_report(751u, 74u);
    uint8_t attempt_token;

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 751u) == 0);
    delivery.snapshot.attempts_remaining = 1u;
    assert(app_mesh_local_delivery_begin_attempt(&delivery, &attempt_token) == 0);
    assert(delivery.snapshot.attempts_remaining == 0u);
    assert(app_mesh_local_delivery_note_attempt_blocked(
               &delivery, attempt_token) == 0);
    assert(app_mesh_local_delivery_attempts_available(&delivery) == 1u);

    for (unsigned int i = 0u; i < 32u; i++) {
        uint8_t resumed_token = 0u;

        assert(app_mesh_local_delivery_resume_blocked_attempt(
                   &delivery, &resumed_token) == 0);
        assert(resumed_token == attempt_token);
        assert(app_mesh_local_delivery_note_attempt_blocked(
                   &delivery, resumed_token) == 0);
        assert(app_mesh_local_delivery_attempts_available(&delivery) == 1u);
    }

    assert(app_mesh_local_delivery_note_attempt_not_sent(
               &delivery, attempt_token,
               APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
    assert(delivery.snapshot.attempts_remaining == 1u);
    assert(app_mesh_local_delivery_begin_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_blocked(
               &delivery, attempt_token) == 0);
    assert(app_mesh_local_delivery_resume_blocked_attempt(
               &delivery, &attempt_token) == 0);
    assert(app_mesh_local_delivery_note_attempt_sent(
               &delivery, attempt_token) == 0);
    assert(app_mesh_local_delivery_attempts_available(&delivery) == 0u);
    assert(note_ack_for_outbound(&delivery, &outbound) == 0);
    assert(!app_mesh_local_delivery_active(&delivery));
}

static void test_pending_delivery_rejects_replacement_until_ack(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound old_report = make_report(801u, 81u);
    const struct mesh_outbound new_report = make_report(802u, 82u);

    assert(app_mesh_local_delivery_stage(&delivery, &old_report, 801u) == 0);
    assert(app_mesh_local_delivery_stage(&delivery, &new_report, 802u) == -EBUSY);
    assert(note_ack_for_outbound(&delivery, &old_report) == 0);
    assert(!app_mesh_local_delivery_active(&delivery));
    assert(!store.present);
    assert(app_mesh_local_delivery_stage(&delivery, &new_report, 802u) == 0);
}

static void test_fifty_anchors_survive_back_to_back_survey_and_lost_acks(void)
{
    enum { ANCHOR_COUNT = 50 };
    static struct journal_store stores[ANCHOR_COUNT];
    static struct app_mesh_local_delivery deliveries[ANCHOR_COUNT];
    const uint32_t old_survey_id = 901u;
    const uint32_t new_survey_id = 902u;

    memset(stores, 0, sizeof(stores));
    memset(deliveries, 0, sizeof(deliveries));
    for (size_t i = 0u; i < ANCHOR_COUNT; i++) {
        uint64_t anchor_id =
            UINT64_C(0x1020304050607080) + i;
        struct mesh_outbound old_report = make_report_from(
            anchor_id, old_survey_id, (uint16_t)(100u + i));
        struct mesh_outbound new_report = make_report_from(
            anchor_id, new_survey_id, (uint16_t)(200u + i));
        uint8_t attempt_token;
        unsigned int destructive_collisions =
            (unsigned int)(i % APP_MESH_LOCAL_DELIVERY_MAX_ATTEMPTS);

        deliveries[i] = make_delivery(&stores[i]);
        assert(app_mesh_local_delivery_stage(
                   &deliveries[i], &old_report, old_survey_id) == 0);
        assert(app_mesh_local_delivery_begin_attempt(
                   &deliveries[i], &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &deliveries[i], attempt_token) == 0);

        if ((i % 4u) == 0u) {
            assert(note_ack_for_outbound(
                       &deliveries[i], &old_report) == 0);
        } else {
            assert(app_mesh_local_delivery_stage(
                       &deliveries[i], &new_report, new_survey_id) == -EBUSY);
            assert(note_ack_for_outbound(
                       &deliveries[i], &old_report) == 0);
        }
        assert(app_mesh_local_delivery_stage(
                   &deliveries[i], &new_report, new_survey_id) == 0);
        assert(note_ack_for_outbound(
                   &deliveries[i], &old_report) == -EKEYREJECTED);

        for (unsigned int collision = 0u;
             collision < destructive_collisions;
             collision++) {
            assert(app_mesh_local_delivery_begin_attempt(
                       &deliveries[i], &attempt_token) == 0);
            assert(app_mesh_local_delivery_note_attempt_sent(
                       &deliveries[i], attempt_token) == 0);
            assert(app_mesh_local_delivery_note_attempt_released(
                       &deliveries[i], attempt_token,
                       APP_MESH_LOCAL_DELIVERY_RETRY) == 0);
        }
        assert(app_mesh_local_delivery_begin_attempt(
                   &deliveries[i], &attempt_token) == 0);
        assert(app_mesh_local_delivery_note_attempt_sent(
                   &deliveries[i], attempt_token) == 0);
        assert(note_ack_for_outbound(
                   &deliveries[i], &new_report) == 0);
        assert(!app_mesh_local_delivery_active(&deliveries[i]));
        assert(!stores[i].present);
    }
}

static void test_exact_protocol_cancellation_retires_without_ack(void)
{
    struct journal_store store = {0};
    struct app_mesh_local_delivery delivery = make_delivery(&store);
    const struct mesh_outbound outbound = make_pair_result(91u, 7u);

    assert(app_mesh_local_delivery_stage(&delivery, &outbound, 91u) == 0);
    assert(app_mesh_local_delivery_active(&delivery));
    assert(!app_mesh_local_delivery_ack_committed(&delivery));

    store.clear_result = -EIO;
    assert(app_mesh_local_delivery_cancel(&delivery) == -EIO);
    assert(app_mesh_local_delivery_active(&delivery));
    assert(!app_mesh_local_delivery_ack_committed(&delivery));
    assert(store.present);

    store.clear_result = 0;
    assert(app_mesh_local_delivery_cancel(&delivery) == 0);
    assert(!app_mesh_local_delivery_occupied(&delivery));
    assert(!store.present);
    assert(app_mesh_local_delivery_cancel(&delivery) == -ENOENT);
}

int main(void)
{
    test_transactional_stage_and_back_to_back_rejection();
    test_reboot_and_exact_ack_identity();
    test_reboot_clears_pretransport_delivery_times();
    test_elapsed_not_before_is_retired_before_long_resource_wait();
    test_pair_result_is_a_supported_exact_delivery_owner();
    test_pair_result_accepts_exact_producer_extension_schema();
    test_pair_result_extension_schema_fails_closed();
    test_canonical_payload_and_envelope_validation_fail_closed();
    test_ack_identity_rejects_header_flags_and_payload_aliases();
    test_blocked_start_does_not_consume_attempt();
    test_ack_commit_survives_clear_failure();
    test_ack_write_failure_preserves_owner_and_terminal_order();
    test_recovery_retains_ack_cleanup_debt();
    test_last_inflight_attempt_can_still_be_acked();
    test_corruption_and_bounded_attempts_fail_closed();
    test_synchronous_ack_wins_post_send_resolution();
    test_reset_after_start_does_not_refund_attempt();
    test_stale_attempt_callback_cannot_overwrite_new_attempt();
    test_recovery_preserves_custody_on_transient_read_failure();
    test_recovery_quarantines_bad_message_and_old_schema();
    test_recovery_reports_quarantine_clear_failure();
    test_persistence_mock_rejects_invalid_snapshot_like_production();
    test_sustained_pre_rf_contention_has_constant_write_bound();
    test_last_blocked_token_can_retry_send_and_ack();
    test_pending_delivery_rejects_replacement_until_ack();
    test_fifty_anchors_survive_back_to_back_survey_and_lost_acks();
    test_exact_protocol_cancellation_retires_without_ack();
    puts("app mesh local delivery tests passed");
    return 0;
}
