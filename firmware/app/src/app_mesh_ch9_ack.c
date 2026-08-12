#include "app_mesh_ch9_ack.h"

#include <string.h>

_Static_assert(APP_MESH_CH9_ACK_PEER_MAX == 2u,
               "ACK table must cover one upstream and one downstream peer");
_Static_assert(APP_MESH_CH9_ACK_BATCH_ENTRY_MAX * sizeof(uint32_t) <= UINT8_MAX,
               "ACK batch TLV lengths must fit in one byte");
_Static_assert(APP_MESH_CH9_ACK_BATCH_ENTRY_MAX <=
                   MESH_ACK_SEMANTIC_IDENTITY_MAX,
               "ACK batch capacity must fit the wire identity bound");

static bool ack_template_supported(const struct mesh_outbound *ack);

static int ack_semantic_identity_append(
    uint8_t *payload,
    size_t payload_cap,
    size_t *offset,
    const struct mesh_ack_semantic_identity *identity)
{
    uint8_t value[MESH_ACK_SEMANTIC_IDENTITY_VALUE_LEN];

    if (payload == NULL || offset == NULL || identity == NULL ||
        identity->session_id == 0u || identity->seq == 0u) {
        return PROTO_ERR_ARG;
    }
    proto_put_u32_le(value, identity->session_id);
    proto_put_u16_le(&value[sizeof(uint32_t)], identity->seq);
    memcpy(&value[sizeof(uint32_t) + sizeof(uint16_t)],
           identity->digest,
           sizeof(identity->digest));
    return tlv_append_bytes(payload,
                            payload_cap,
                            offset,
                            TLV_MESH_ACK_SEMANTIC_IDENTITY,
                            value,
                            (uint8_t)sizeof(value));
}

static int ack_single_semantic_identity(
    const struct mesh_outbound *ack,
    struct mesh_ack_semantic_identity *identity)
{
    struct mesh_ack_semantic_identity extra;
    const uint8_t *requested_seq = NULL;
    const uint8_t *diagnostic = NULL;
    uint8_t requested_seq_len = 0u;
    uint8_t diagnostic_len = 0u;
    int ret;

    if (!ack_template_supported(ack) || identity == NULL ||
        ack->packet.payload_len != ack->payload_len ||
        ack->payload_len != MESH_ACK_SINGLE_PAYLOAD_LEN) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find_unique(ack->payload,
                          ack->payload_len,
                          TLV_REQUESTED_MSG_SEQ,
                          &requested_seq,
                          &requested_seq_len);
    if (ret != PROTO_OK || requested_seq_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    if (tlv_find_unique(ack->payload,
                        ack->payload_len,
                        TLV_MESH_ACK_SEQ_LIST,
                        &diagnostic,
                        &diagnostic_len) != PROTO_ERR_NOT_FOUND ||
        tlv_find_unique(ack->payload,
                        ack->payload_len,
                        TLV_MESH_ACK_SESSION_LIST,
                        &diagnostic,
                        &diagnostic_len) != PROTO_ERR_NOT_FOUND ||
        tlv_find_unique(ack->payload,
                        ack->payload_len,
                        TLV_MESH_ACK_PACKET_ID_LIST,
                        &diagnostic,
                        &diagnostic_len) != PROTO_ERR_NOT_FOUND) {
        return PROTO_ERR_MALFORMED;
    }
    ret = mesh_ack_semantic_identity_at(ack->payload,
                                        ack->payload_len,
                                        0u,
                                        identity);
    if (ret != PROTO_OK ||
        mesh_ack_semantic_identity_at(ack->payload,
                                      ack->payload_len,
                                      1u,
                                      &extra) != PROTO_ERR_NOT_FOUND ||
        identity->session_id != ack->packet.session_id ||
        identity->seq != proto_get_u16_le(requested_seq)) {
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}

static void ack_queue_result_set(enum app_mesh_ch9_ack_queue_result *result,
                                 enum app_mesh_ch9_ack_queue_result value)
{
    if (result != NULL) {
        *result = value;
    }
}

static bool ack_template_supported(const struct mesh_outbound *ack)
{
    return ack != NULL && ack->next_hop_id != 0u &&
           (ack->packet.msg_type == MSG_GATEWAY_ACK ||
            ack->packet.msg_type == MSG_MESH_HOP_ACK);
}

static struct app_mesh_ch9_ack_batch *ack_table_find_peer(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id)
{
    if (table == NULL || peer_id == 0u) {
        return NULL;
    }

    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        if (table->batches[i].valid &&
            table->batches[i].peer_id == peer_id) {
            return &table->batches[i];
        }
    }
    return NULL;
}

static const struct app_mesh_ch9_ack_batch *ack_table_find_peer_const(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id)
{
    if (table == NULL || peer_id == 0u) {
        return NULL;
    }

    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        if (table->batches[i].valid &&
            table->batches[i].peer_id == peer_id) {
            return &table->batches[i];
        }
    }
    return NULL;
}

static struct app_mesh_ch9_ack_batch *ack_table_find_free(
    struct app_mesh_ch9_ack_table *table)
{
    if (table == NULL) {
        return NULL;
    }

    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        if (!table->batches[i].valid) {
            return &table->batches[i];
        }
    }
    return NULL;
}

static int ack_batch_reset_generated(
    struct app_mesh_ch9_ack_batch *batch,
    const struct mesh_outbound *ack,
    const struct mesh_ack_semantic_identity *identity)
{
    size_t payload_len = 0u;
    int ret;

    if (batch == NULL || ack == NULL || identity == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(batch, 0, sizeof(*batch));
    batch->template_ack = *ack;
    batch->template_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    ret = mesh_append_requested_seq(batch->template_ack.payload,
                                    sizeof(batch->template_ack.payload),
                                    &payload_len,
                                    identity->seq);
    if (ret != PROTO_OK) {
        memset(batch, 0, sizeof(*batch));
        return ret;
    }
    ret = ack_semantic_identity_append(batch->template_ack.payload,
                                       sizeof(batch->template_ack.payload),
                                       &payload_len,
                                       identity);
    if (ret != PROTO_OK) {
        memset(batch, 0, sizeof(*batch));
        return ret;
    }
    batch->template_ack.payload_len = (uint16_t)payload_len;
    batch->template_ack.packet.payload_len = (uint16_t)payload_len;
    batch->peer_id = ack->next_hop_id;
    batch->valid = true;
    return PROTO_OK;
}

static bool ack_batch_matches_generated(
    const struct app_mesh_ch9_ack_batch *batch,
    const struct mesh_outbound *ack)
{
    return batch != NULL && ack != NULL && batch->valid &&
           !batch->preserve_payload &&
           batch->peer_id == ack->next_hop_id &&
           batch->template_ack.packet.msg_type == ack->packet.msg_type &&
           batch->template_ack.packet.flags == ack->packet.flags &&
           batch->template_ack.packet.src_id == ack->packet.src_id &&
           batch->template_ack.packet.dst_id == ack->packet.dst_id;
}

static bool ack_batch_is_forwarded_gateway_ack(
    const struct app_mesh_ch9_ack_batch *batch)
{
    return batch != NULL && batch->valid && batch->preserve_payload &&
           batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK;
}

static bool forwarded_ack_matches_exact(
    const struct app_mesh_ch9_ack_batch *batch,
    const struct mesh_outbound *ack)
{
    const struct proto_packet *queued;
    const struct proto_packet *candidate;

    if (!ack_batch_is_forwarded_gateway_ack(batch) || ack == NULL ||
        batch->template_ack.next_hop_id != ack->next_hop_id ||
        batch->template_ack.payload_len != ack->payload_len ||
        ack->payload_len > UWB_MESH_MAX_PAYLOAD_LEN ||
        memcmp(batch->template_ack.payload,
               ack->payload,
               ack->payload_len) != 0) {
        return false;
    }

    queued = &batch->template_ack.packet;
    candidate = &ack->packet;
    return queued->msg_type == candidate->msg_type &&
           queued->flags == candidate->flags &&
           queued->src_id == candidate->src_id &&
           queued->dst_id == candidate->dst_id &&
           queued->session_id == candidate->session_id &&
           queued->seq == candidate->seq &&
           queued->ttl == candidate->ttl &&
           queued->payload_len == candidate->payload_len &&
           queued->message_age_ms == candidate->message_age_ms;
}

void app_mesh_ch9_ack_table_init(struct app_mesh_ch9_ack_table *table)
{
    if (table != NULL) {
        memset(table, 0, sizeof(*table));
    }
}

uint8_t app_mesh_ch9_ack_table_peer_count(
    const struct app_mesh_ch9_ack_table *table)
{
    uint8_t count = 0u;

    if (table == NULL) {
        return 0u;
    }
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        if (table->batches[i].valid) {
            count++;
        }
    }
    return count;
}

bool app_mesh_ch9_ack_table_any_pending(
    const struct app_mesh_ch9_ack_table *table)
{
    return app_mesh_ch9_ack_table_peer_count(table) > 0u;
}

bool app_mesh_ch9_ack_table_pending_for_peer(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id)
{
    const struct app_mesh_ch9_ack_batch *batch =
        ack_table_find_peer_const(table, peer_id);

    return batch != NULL && batch->count > 0u;
}

const struct app_mesh_ch9_ack_batch *app_mesh_ch9_ack_table_get_peer(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id)
{
    return ack_table_find_peer_const(table, peer_id);
}

int app_mesh_ch9_ack_table_queue(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    const struct app_mesh_ch9_ack_batch_entry *entry,
    enum app_mesh_ch9_ack_queue_result *result)
{
    struct app_mesh_ch9_ack_batch *batch;
    struct mesh_ack_semantic_identity candidate_identity;
    bool reset_generated = false;
    bool replaced = false;
    int ret;

    if (table == NULL || entry == NULL || !ack_template_supported(ack)) {
        return PROTO_ERR_ARG;
    }
    ret = ack_single_semantic_identity(ack, &candidate_identity);
    if (ret != PROTO_OK ||
        candidate_identity.session_id != entry->session_id ||
        candidate_identity.seq != entry->seq) {
        ack_queue_result_set(
            result,
            APP_MESH_CH9_ACK_QUEUE_SEMANTIC_CONFLICT);
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }

    batch = ack_table_find_peer(table, ack->next_hop_id);
    if (batch == NULL) {
        batch = ack_table_find_free(table);
        if (batch == NULL) {
            ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_TABLE_FULL);
            return PROTO_ERR_NO_SPACE;
        }
        ret = ack_batch_reset_generated(batch, ack, &candidate_identity);
        if (ret != PROTO_OK) {
            return ret;
        }
        reset_generated = true;
    } else if (ack_batch_is_forwarded_gateway_ack(batch)) {
        if (ack->packet.msg_type == MSG_MESH_HOP_ACK) {
            ack_queue_result_set(
                result,
                APP_MESH_CH9_ACK_QUEUE_SUPPRESSED_BY_FORWARDED_ACK);
            return PROTO_OK;
        }
        ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_FORWARDED_BUSY);
        return PROTO_ERR_NO_SPACE;
    } else if (!ack_batch_matches_generated(batch, ack)) {
        ret = ack_batch_reset_generated(batch, ack, &candidate_identity);
        if (ret != PROTO_OK) {
            return ret;
        }
        reset_generated = true;
        replaced = true;
    }

    for (uint8_t i = 0u; i < batch->count; i++) {
        if (batch->entries[i].session_id == entry->session_id &&
            batch->entries[i].seq == entry->seq) {
            struct mesh_ack_semantic_identity existing_identity;

            if (mesh_ack_semantic_identity_at(
                    batch->template_ack.payload,
                    batch->template_ack.payload_len,
                    i,
                    &existing_identity) != PROTO_OK ||
                !semantic_digest_equal(existing_identity.digest,
                                       candidate_identity.digest,
                                       sizeof(existing_identity.digest))) {
                ack_queue_result_set(
                    result,
                    APP_MESH_CH9_ACK_QUEUE_SEMANTIC_CONFLICT);
                return PROTO_ERR_MALFORMED;
            }
            ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
            return PROTO_OK;
        }
    }

    if (batch->count >= APP_MESH_CH9_ACK_BATCH_ENTRY_MAX) {
        ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_BATCH_FULL);
        return PROTO_ERR_NO_SPACE;
    }

    if (!reset_generated) {
        size_t payload_len = batch->template_ack.payload_len;

        ret = ack_semantic_identity_append(batch->template_ack.payload,
                                           sizeof(batch->template_ack.payload),
                                           &payload_len,
                                           &candidate_identity);
        if (ret != PROTO_OK) {
            return ret;
        }
        batch->template_ack.payload_len = (uint16_t)payload_len;
        batch->template_ack.packet.payload_len = (uint16_t)payload_len;
    }
    batch->entries[batch->count] = *entry;
    if (!batch->entries[batch->count].has_packet_id) {
        batch->entries[batch->count].packet_id = 0u;
    }
    batch->count++;
    ack_queue_result_set(result,
                         replaced ? APP_MESH_CH9_ACK_QUEUE_REPLACED :
                                    APP_MESH_CH9_ACK_QUEUE_ADDED);
    return PROTO_OK;
}

static int ack_table_queue_forwarded_owned(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    enum app_mesh_ch9_ack_owner owner,
    enum app_mesh_ch9_ack_queue_result *result)
{
    struct app_mesh_ch9_ack_batch *batch;
    bool replaced;

    if (table == NULL || !ack_template_supported(ack) ||
        (owner != APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE &&
         owner != APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD) ||
        ack->packet.msg_type != MSG_GATEWAY_ACK) {
        return PROTO_ERR_ARG;
    }

    batch = ack_table_find_peer(table, ack->next_hop_id);
    replaced = batch != NULL;
    if (batch == NULL) {
        batch = ack_table_find_free(table);
        if (batch == NULL) {
            ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_TABLE_FULL);
            return PROTO_ERR_NO_SPACE;
        }
    } else if (ack_batch_is_forwarded_gateway_ack(batch)) {
        if (forwarded_ack_matches_exact(batch, ack)) {
            if (owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE) {
                batch->owner = APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE;
            }
            ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_DUPLICATE);
            return PROTO_OK;
        }
        ack_queue_result_set(result, APP_MESH_CH9_ACK_QUEUE_FORWARDED_BUSY);
        return PROTO_ERR_NO_SPACE;
    }

    memset(batch, 0, sizeof(*batch));
    batch->template_ack = *ack;
    batch->template_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    batch->peer_id = ack->next_hop_id;
    batch->count = 1u;
    batch->valid = true;
    batch->preserve_payload = true;
    batch->owner = (uint8_t)owner;
    ack_queue_result_set(result,
                         replaced ? APP_MESH_CH9_ACK_QUEUE_REPLACED :
                                    APP_MESH_CH9_ACK_QUEUE_ADDED);
    return PROTO_OK;
}

int app_mesh_ch9_ack_table_queue_forwarded(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    enum app_mesh_ch9_ack_queue_result *result)
{
    return ack_table_queue_forwarded_owned(
        table, ack, APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE, result);
}

int app_mesh_ch9_ack_table_queue_late_forwarded(
    struct app_mesh_ch9_ack_table *table,
    const struct mesh_outbound *ack,
    enum app_mesh_ch9_ack_queue_result *result)
{
    return ack_table_queue_forwarded_owned(
        table, ack, APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD, result);
}

int app_mesh_ch9_ack_table_build_peer(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    struct mesh_outbound *outbound)
{
    const struct app_mesh_ch9_ack_batch *batch;
    uint8_t seq_list[APP_MESH_CH9_ACK_BATCH_ENTRY_MAX * sizeof(uint16_t)];
    uint8_t session_list[APP_MESH_CH9_ACK_BATCH_ENTRY_MAX * sizeof(uint32_t)];
    uint8_t packet_id_list[APP_MESH_CH9_ACK_BATCH_ENTRY_MAX * sizeof(uint32_t)];
    size_t payload_len = 0u;
    int ret;

    if (table == NULL || outbound == NULL || peer_id == 0u) {
        return PROTO_ERR_ARG;
    }

    batch = ack_table_find_peer_const(table, peer_id);
    if (batch == NULL || batch->count == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }

    *outbound = batch->template_ack;
    outbound->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    if (batch->preserve_payload) {
        return PROTO_OK;
    }

    for (uint8_t i = 0u; i < batch->count; i++) {
        struct mesh_ack_semantic_identity identity;

        proto_put_u16_le(&seq_list[i * sizeof(uint16_t)],
                         batch->entries[i].seq);
        proto_put_u32_le(&session_list[i * sizeof(uint32_t)],
                         batch->entries[i].session_id);
        proto_put_u32_le(&packet_id_list[i * sizeof(uint32_t)],
                         batch->entries[i].packet_id);
        ret = mesh_ack_semantic_identity_at(
            batch->template_ack.payload,
            batch->template_ack.payload_len,
            i,
            &identity);
        if (ret != PROTO_OK ||
            identity.session_id != batch->entries[i].session_id ||
            identity.seq != batch->entries[i].seq) {
            return PROTO_ERR_MALFORMED;
        }
    }

    ret = mesh_append_requested_seq(outbound->payload,
                                    sizeof(outbound->payload),
                                    &payload_len,
                                    batch->entries[0].seq);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_bytes(outbound->payload,
                           sizeof(outbound->payload),
                           &payload_len,
                           TLV_MESH_ACK_SESSION_LIST,
                           session_list,
                           (uint8_t)(batch->count * sizeof(uint32_t)));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_bytes(outbound->payload,
                           sizeof(outbound->payload),
                           &payload_len,
                           TLV_MESH_ACK_SEQ_LIST,
                           seq_list,
                           (uint8_t)(batch->count * sizeof(uint16_t)));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_bytes(outbound->payload,
                           sizeof(outbound->payload),
                           &payload_len,
                           TLV_MESH_ACK_PACKET_ID_LIST,
                           packet_id_list,
                           (uint8_t)(batch->count * sizeof(uint32_t)));
    if (ret != PROTO_OK) {
        return ret;
    }
    for (uint8_t i = 0u; i < batch->count; i++) {
        struct mesh_ack_semantic_identity identity;

        ret = mesh_ack_semantic_identity_at(
            batch->template_ack.payload,
            batch->template_ack.payload_len,
            i,
            &identity);
        if (ret != PROTO_OK) {
            return PROTO_ERR_MALFORMED;
        }
        ret = ack_semantic_identity_append(outbound->payload,
                                           sizeof(outbound->payload),
                                           &payload_len,
                                           &identity);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    outbound->payload_len = (uint16_t)payload_len;
    outbound->packet.payload_len = (uint16_t)payload_len;
    return PROTO_OK;
}

bool app_mesh_ch9_ack_table_clear_peer(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id)
{
    struct app_mesh_ch9_ack_batch *batch =
        ack_table_find_peer(table, peer_id);

    if (batch == NULL) {
        return false;
    }
    memset(batch, 0, sizeof(*batch));
    return true;
}

static bool ack_retry_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

bool app_mesh_ch9_ack_table_retry_ready(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    uint32_t now_ms)
{
    const struct app_mesh_ch9_ack_batch *batch =
        ack_table_find_peer_const(table, peer_id);

    return batch != NULL && batch->count > 0u &&
           (!batch->retry_deferred ||
            ack_retry_deadline_reached(now_ms,
                                       batch->retry_not_before_ms));
}

uint32_t app_mesh_ch9_ack_table_retry_wait_ms(
    const struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    uint32_t now_ms)
{
    const struct app_mesh_ch9_ack_batch *batch =
        ack_table_find_peer_const(table, peer_id);

    if (batch == NULL || batch->count == 0u || !batch->retry_deferred ||
        ack_retry_deadline_reached(now_ms, batch->retry_not_before_ms)) {
        return 0u;
    }
    return batch->retry_not_before_ms - now_ms;
}

int app_mesh_ch9_ack_table_note_send_failure(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    uint32_t now_ms,
    uint32_t attempt_entropy,
    uint32_t *delay_ms_out)
{
    struct app_mesh_ch9_ack_batch *batch =
        ack_table_find_peer(table, peer_id);
    uint32_t base_ms = APP_MESH_CH9_ACK_RETRY_BASE_MS;
    uint32_t delay_ms;
    uint16_t shift;

    if (batch == NULL || batch->count == 0u) {
        return PROTO_ERR_NOT_FOUND;
    }

    if (batch->retry_round < UINT16_MAX) {
        batch->retry_round++;
    }
    shift = batch->retry_round == 0u ? 0u :
            (uint16_t)(batch->retry_round - 1u);
    while (shift > 0u && base_ms < APP_MESH_CH9_ACK_RETRY_BASE_MAX_MS) {
        base_ms *= 2u;
        shift--;
    }
    if (base_ms > APP_MESH_CH9_ACK_RETRY_BASE_MAX_MS) {
        base_ms = APP_MESH_CH9_ACK_RETRY_BASE_MAX_MS;
    }

    delay_ms = base_ms - (base_ms / 2u) +
               (attempt_entropy % (base_ms + 1u));
    batch->retry_not_before_ms = now_ms + delay_ms;
    batch->retry_deferred = true;
    if (delay_ms_out != NULL) {
        *delay_ms_out = delay_ms;
    }
    return PROTO_OK;
}

int app_mesh_ch9_ack_table_flush_peer(
    struct app_mesh_ch9_ack_table *table,
    uint64_t peer_id,
    app_mesh_ch9_ack_flush_fn flush,
    void *ctx)
{
    struct mesh_outbound outbound;
    int ret;

    if (table == NULL || peer_id == 0u || flush == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = app_mesh_ch9_ack_table_build_peer(table, peer_id, &outbound);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = flush(&outbound, ctx);
    if (ret == 0) {
        (void)app_mesh_ch9_ack_table_clear_peer(table, peer_id);
    }
    return ret;
}

static int ack_payload_contains_packet(
    const struct proto_packet *ack_packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct mesh_outbound *acknowledged,
    bool *contains)
{
    if (acknowledged == NULL ||
        acknowledged->packet.payload_len != acknowledged->payload_len) {
        return PROTO_ERR_ARG;
    }
    return mesh_ack_payload_contains_packet(ack_packet,
                                            payload,
                                            payload_len,
                                            &acknowledged->packet,
                                            acknowledged->payload,
                                            acknowledged->payload_len,
                                            contains);
}

int app_mesh_ch9_tx_ack_apply(const struct proto_packet *ack_packet,
                              const uint8_t *payload,
                              size_t payload_len,
                              struct app_mesh_ch9_tx_ack_entry *entries,
                              uint8_t entry_count,
                              struct app_mesh_ch9_tx_ack_result *result)
{
    struct app_mesh_ch9_tx_ack_result local_result;

    if (ack_packet == NULL || payload == NULL ||
        (entry_count > 0u && entries == NULL)) {
        return PROTO_ERR_ARG;
    }
    if (ack_packet->msg_type != MSG_GATEWAY_ACK &&
        ack_packet->msg_type != MSG_MESH_HOP_ACK) {
        return PROTO_ERR_MALFORMED;
    }

    memset(&local_result, 0, sizeof(local_result));

    for (uint8_t i = 0u; i < entry_count; i++) {
        bool contains = false;
        int ret;

        if (entries[i].acked) {
            continue;
        }
        if (entries[i].outbound == NULL) {
            return PROTO_ERR_ARG;
        }

        ret = ack_payload_contains_packet(ack_packet,
                                          payload,
                                          payload_len,
                                          entries[i].outbound,
                                          &contains);
        if (ret != PROTO_OK) {
            return ret;
        }
        if (contains) {
            entries[i].acked = true;
            local_result.acked_now++;
        }
    }

    for (uint8_t i = 0u; i < entry_count; i++) {
        if (!entries[i].acked) {
            local_result.unacked_count++;
        }
    }
    local_result.any_match = local_result.acked_now > 0u;
    local_result.all_acked = local_result.any_match &&
                             local_result.unacked_count == 0u;

    if (result != NULL) {
        *result = local_result;
    }
    return PROTO_OK;
}

int app_mesh_ch9_tx_requeue_unacked(struct app_mesh_ch9_tx_retry_entry *entries,
                                    uint8_t entry_count,
                                    uint32_t now_ms,
                                    const struct app_mesh_ch9_tx_retry_ops *ops,
                                    struct app_mesh_ch9_tx_retry_result *result)
{
    struct app_mesh_ch9_tx_retry_result local_result;

    if ((entry_count > 0u && entries == NULL) ||
        ops == NULL ||
        ops->put == NULL ||
        ops->queue_used == NULL) {
        return PROTO_ERR_ARG;
    }

    memset(&local_result, 0, sizeof(local_result));
    local_result.queued_before = ops->queue_used(ops->ctx);

    for (uint8_t i = 0u; i < entry_count; i++) {
        if (entries[i].outbound == NULL || entries[i].acked == NULL) {
            return PROTO_ERR_ARG;
        }
        if (*entries[i].acked) {
            continue;
        }

        entries[i].outbound->queued_at_ms = now_ms;
        entries[i].outbound->queued_at_valid = true;
        if (ops->put(entries[i].outbound, ops->ctx) == 0) {
            *entries[i].acked = true;
            local_result.requeued++;
        } else {
            local_result.retained++;
        }
    }

    local_result.queued_after = ops->queue_used(ops->ctx);
    if (result != NULL) {
        *result = local_result;
    }
    return PROTO_OK;
}

bool app_mesh_ch9_tx_should_track_ack(const struct proto_packet *packet,
                                      bool relay_collection_result_active)
{
    if (packet == NULL) {
        return false;
    }

    return packet->msg_type != MSG_COMMAND_RESULT ||
           !relay_collection_result_active;
}

bool app_mesh_ch9_tx_should_track_sent(const struct mesh_outbound *sent,
                                       uint64_t local_id)
{
    if (sent == NULL || local_id == 0u) {
        return false;
    }

    return sent->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           sent->next_hop_id != 0u &&
           sent->next_hop_id != local_id &&
           sent->next_hop_id == sent->packet.dst_id &&
           sent->packet.dst_id != local_id &&
           sent->packet.msg_type != MSG_COMMAND_RESULT &&
           sent->packet.msg_type != MSG_RESULT_BUNDLE &&
           (sent->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
}

bool app_mesh_ch9_core_ack_wait_active(const struct mesh_pending_tx *pending,
                                       bool relay_tx_active)
{
    return relay_tx_active && pending != NULL &&
           (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
            pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD) &&
           pending->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           pending->next_hop_id != 0u;
}

bool app_mesh_ch9_core_pending_allows_rx(const struct mesh_pending_tx *pending,
                                         bool relay_tx_active)
{
    return relay_tx_active && pending != NULL &&
           (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
            pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
            pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD) &&
           pending->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           pending->next_hop_id != 0u;
}

static bool c5_repair_pending_state_matches(
    enum app_mesh_c5_tx_authorization kind,
    const struct mesh_pending_tx *pending,
    const struct app_mesh_ch9_ack_batch *batch,
    uint64_t peer_id)
{
    if (kind == APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR) {
        return (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK ||
                pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) &&
               !pending->gateway_ack_forward_pending && batch == NULL;
    }
    if (kind != APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR) {
        return false;
    }
    return (pending->state == MESH_RELAY_TX_WAIT_GATEWAY_ACK_FORWARD ||
            pending->state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF) &&
           pending->gateway_ack_forward_pending && batch != NULL &&
           batch->valid && batch->preserve_payload && batch->count > 0u &&
           batch->owner == APP_MESH_CH9_ACK_OWNER_TRANSIT_CORE &&
           batch->peer_id == peer_id &&
           batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK &&
           batch->template_ack.packet.dst_id == peer_id &&
           batch->template_ack.next_hop_id == peer_id;
}

static bool c5_late_gateway_ack_batch_matches(
    const struct app_mesh_ch9_ack_batch *batch,
    uint64_t peer_id)
{
    return batch != NULL && batch->valid && batch->preserve_payload &&
           batch->count > 0u &&
           batch->owner == APP_MESH_CH9_ACK_OWNER_LATE_TERMINAL_FORWARD &&
           batch->peer_id == peer_id &&
           batch->template_ack.packet.msg_type == MSG_GATEWAY_ACK &&
           batch->template_ack.packet.dst_id == peer_id &&
           batch->template_ack.next_hop_id == peer_id;
}

bool app_mesh_ch9_c5_repair_authorization_capture(
    struct app_mesh_c5_tx_authorization_token *authorization,
    enum app_mesh_c5_tx_authorization kind,
    const struct mesh_pending_tx *pending,
    bool relay_tx_active,
    const struct app_mesh_ch9_ack_batch *batch,
    uint64_t repair_peer_id)
{
    if (authorization == NULL) {
        return false;
    }
    memset(authorization, 0, sizeof(*authorization));
    if (kind == APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR) {
        if (repair_peer_id == 0u ||
            !c5_late_gateway_ack_batch_matches(batch, repair_peer_id) ||
            !mesh_packet_semantic_digest(&batch->template_ack.packet,
                                         batch->template_ack.payload,
                                         batch->template_ack.payload_len,
                                         authorization->retained_ack_digest)) {
            return false;
        }
        authorization->kind = kind;
        authorization->peer_id = repair_peer_id;
        authorization->retained_ack_session_id =
            batch->template_ack.packet.session_id;
        authorization->retained_ack_seq = batch->template_ack.packet.seq;
        authorization->retained_ack_valid = true;
        authorization->valid = true;
        return true;
    }
    if (!relay_tx_active || pending == NULL || repair_peer_id == 0u ||
        pending->packet.src_id != repair_peer_id ||
        pending->packet.payload_len != pending->payload_len ||
        pending->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        (pending->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !c5_repair_pending_state_matches(kind, pending, batch,
                                         repair_peer_id) ||
        !mesh_packet_semantic_digest(&pending->packet,
                                     pending->payload,
                                     pending->payload_len,
                                     authorization->pending_digest)) {
        return false;
    }

    authorization->kind = kind;
    authorization->peer_id = repair_peer_id;
    authorization->pending_session_id = pending->packet.session_id;
    authorization->pending_seq = pending->packet.seq;
    authorization->pending_msg_type = pending->packet.msg_type;
    if (kind == APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR) {
        if (!mesh_packet_semantic_digest(&batch->template_ack.packet,
                                         batch->template_ack.payload,
                                         batch->template_ack.payload_len,
                                         authorization->retained_ack_digest)) {
            memset(authorization, 0, sizeof(*authorization));
            return false;
        }
        authorization->retained_ack_session_id =
            batch->template_ack.packet.session_id;
        authorization->retained_ack_seq = batch->template_ack.packet.seq;
        authorization->retained_ack_valid = true;
    }
    authorization->valid = true;
    return true;
}

bool app_mesh_ch9_c5_repair_owner_matches(
    const struct app_mesh_c5_tx_authorization_token *authorization,
    const struct mesh_pending_tx *pending,
    bool relay_tx_active,
    const struct app_mesh_ch9_ack_batch *batch)
{
    uint8_t pending_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint8_t ack_digest[SEMANTIC_DIGEST_SHA256_LEN];

    if (authorization == NULL || !authorization->valid) {
        return false;
    }
    if (authorization->kind ==
        APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR) {
        return authorization->retained_ack_valid &&
               c5_late_gateway_ack_batch_matches(
                   batch, authorization->peer_id) &&
               batch->template_ack.packet.session_id ==
                   authorization->retained_ack_session_id &&
               batch->template_ack.packet.seq ==
                   authorization->retained_ack_seq &&
               mesh_packet_semantic_digest(&batch->template_ack.packet,
                                           batch->template_ack.payload,
                                           batch->template_ack.payload_len,
                                           ack_digest) &&
               semantic_digest_equal(ack_digest,
                                     authorization->retained_ack_digest,
                                     sizeof(ack_digest));
    }
    if (!relay_tx_active || pending == NULL ||
        pending->packet.src_id != authorization->peer_id ||
        pending->packet.session_id != authorization->pending_session_id ||
        pending->packet.seq != authorization->pending_seq ||
        pending->packet.msg_type != authorization->pending_msg_type ||
        pending->packet.payload_len != pending->payload_len ||
        !c5_repair_pending_state_matches(authorization->kind,
                                         pending,
                                         batch,
                                         authorization->peer_id) ||
        !mesh_packet_semantic_digest(&pending->packet,
                                     pending->payload,
                                     pending->payload_len,
                                     pending_digest) ||
        !semantic_digest_equal(pending_digest,
                               authorization->pending_digest,
                               sizeof(pending_digest))) {
        return false;
    }
    if (!authorization->retained_ack_valid) {
        return authorization->kind ==
               APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR;
    }
    return batch != NULL &&
           batch->template_ack.packet.session_id ==
               authorization->retained_ack_session_id &&
           batch->template_ack.packet.seq ==
               authorization->retained_ack_seq &&
           mesh_packet_semantic_digest(&batch->template_ack.packet,
                                       batch->template_ack.payload,
                                       batch->template_ack.payload_len,
                                       ack_digest) &&
           semantic_digest_equal(ack_digest,
                                 authorization->retained_ack_digest,
                                 sizeof(ack_digest));
}

bool app_mesh_c5_tx_authorization_token_equal(
    const struct app_mesh_c5_tx_authorization_token *a,
    const struct app_mesh_c5_tx_authorization_token *b)
{
    return a != NULL && b != NULL && a->valid && b->valid &&
           a->kind == b->kind && a->peer_id == b->peer_id &&
           a->pending_session_id == b->pending_session_id &&
           a->pending_seq == b->pending_seq &&
           a->pending_msg_type == b->pending_msg_type &&
           semantic_digest_equal(a->pending_digest, b->pending_digest,
                                 sizeof(a->pending_digest)) &&
           a->retained_ack_valid == b->retained_ack_valid &&
           (!a->retained_ack_valid ||
            (a->retained_ack_session_id == b->retained_ack_session_id &&
             a->retained_ack_seq == b->retained_ack_seq &&
             semantic_digest_equal(a->retained_ack_digest,
                                   b->retained_ack_digest,
                                   sizeof(a->retained_ack_digest))));
}

bool app_mesh_ch9_c5_repair_allowed(
    const struct app_mesh_c5_tx_authorization_token *authorization,
    const struct mesh_pending_tx *pending,
    bool relay_tx_active,
    const struct app_mesh_ch9_ack_batch *batch,
    const struct mesh_outbound *candidate)
{
    const uint8_t *target_value = NULL;
    uint8_t target_len = 0u;

    if (!app_mesh_ch9_c5_repair_owner_matches(authorization,
                                               pending,
                                               relay_tx_active,
                                               batch) ||
        candidate == NULL ||
        candidate->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        candidate->packet.payload_len != candidate->payload_len) {
        return false;
    }

    if (authorization->kind ==
        APP_MESH_C5_TX_AUTH_FORWARDED_ACK_ROUTE_REPAIR) {
        return candidate->packet.msg_type == MSG_ROUTE_REQ &&
               tlv_find_unique(candidate->payload,
                               candidate->payload_len,
                               TLV_RESPONDER_ID,
                               &target_value,
                               &target_len) == PROTO_OK &&
               target_len == sizeof(uint64_t) &&
               proto_get_u64_le(target_value) == authorization->peer_id;
    }
    return (authorization->kind ==
                APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR ||
            authorization->kind ==
                APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR) &&
           (candidate->packet.msg_type == MSG_MESH_EVENT_PROPOSE ||
            candidate->packet.msg_type == MSG_MESH_EVENT_ACCEPT) &&
           candidate->packet.dst_id == authorization->peer_id &&
           candidate->next_hop_id == authorization->peer_id;
}

uint8_t app_mesh_ch9_tx_max_in_flight(const struct proto_packet *packet,
                                      uint64_t next_hop_id,
                                      uint8_t configured_max)
{
    (void)next_hop_id;

    if (configured_max == 0u) {
        return 0u;
    }
    if (packet != NULL &&
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u) {
        return 1u;
    }

    return configured_max;
}

bool app_mesh_ch9_tx_requires_tracked_single(const struct proto_packet *packet,
                                             uint64_t next_hop_id,
                                             uint8_t configured_max)
{
    return packet != NULL && configured_max > 0u &&
           app_mesh_ch9_tx_max_in_flight(packet,
                                         next_hop_id,
                                         configured_max) == 1u;
}

bool app_mesh_ch9_retry_next_local_tx_prepare_ms(
    const struct mesh_event_timing *timing,
    uint16_t minimum_guard_ms,
    uint32_t *prepare_ms)
{
    struct mesh_event_timing next;
    uint32_t guard_ms;

    if (timing == NULL || prepare_ms == NULL ||
        timing->event_interval_ms == 0u ||
        mesh_event_timing_local_tx_slot(timing)) {
        return false;
    }

    next = *timing;
    next.next_event_time_ms += next.event_interval_ms;
    next.event_counter++;
    if (!mesh_event_timing_local_tx_slot(&next)) {
        return false;
    }

    guard_ms = next.guard_ms > minimum_guard_ms ?
               next.guard_ms : minimum_guard_ms;
    *prepare_ms = next.next_event_time_ms - guard_ms;
    return true;
}

bool app_mesh_ch9_wait_plan_retry_delay_ms(uint32_t now_ms,
                                          uint32_t event_start_ms,
                                          uint16_t minimum_guard_ms,
                                          uint32_t *delay_ms)
{
    uint32_t prepare_ms;
    int32_t delta_ms;

    if (delay_ms == NULL) {
        return false;
    }

    prepare_ms = event_start_ms - minimum_guard_ms;
    delta_ms = (int32_t)(prepare_ms - now_ms);
    *delay_ms = delta_ms > 0 ? (uint32_t)delta_ms : 1u;
    return true;
}

bool app_mesh_ch9_tx_timeout_counts_route_failure(
    const struct mesh_outbound *outbound,
    uint64_t next_hop_id,
    uint64_t gateway_id)
{
    if (outbound == NULL || next_hop_id == 0u || gateway_id == 0u) {
        return false;
    }

    return outbound->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           outbound->next_hop_id == next_hop_id &&
           outbound->packet.dst_id == gateway_id &&
           (outbound->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
}

enum app_mesh_ch9_timeout_pressure_action
app_mesh_ch9_timeout_pressure_decide(const struct mesh_outbound *outbound,
                                     bool anchor_role,
                                     bool downstream_reserved,
                                     bool local_origin_priority_needs_capacity,
                                     uint64_t local_id)
{
    if (outbound == NULL || !anchor_role || !downstream_reserved ||
        local_id == 0u) {
        return APP_MESH_CH9_TIMEOUT_RETRY;
    }
    if (outbound->packet.src_id != local_id) {
        (void)local_origin_priority_needs_capacity;
        return APP_MESH_CH9_TIMEOUT_RETRY;
    }
    if (outbound->packet.msg_type == MSG_CLICK_REPORT ||
        outbound->packet.msg_type == MSG_COMMAND_RESULT) {
        return APP_MESH_CH9_TIMEOUT_PREEMPT_FOR_LOCAL;
    }
    return APP_MESH_CH9_TIMEOUT_DEFER_LOCAL;
}

bool app_mesh_direct_gateway_ack_matches(const struct mesh_outbound *sent,
                                         const struct proto_packet *ack_packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint64_t previous_hop_id,
                                         uint64_t gateway_id)
{
    bool contains = false;

    if (sent == NULL || ack_packet == NULL || gateway_id == 0u ||
        sent->packet.src_id == 0u) {
        return false;
    }

    if (previous_hop_id != gateway_id ||
        ack_packet->msg_type != MSG_GATEWAY_ACK ||
        ack_packet->src_id != gateway_id ||
        ack_packet->dst_id != sent->packet.src_id) {
        return false;
    }

    return ack_payload_contains_packet(ack_packet,
                                       payload,
                                       payload_len,
                                       sent,
                                       &contains) == PROTO_OK &&
           contains;
}

bool app_mesh_ch9_ack_complete_should_close_timing(
    const struct app_mesh_ch9_ack_complete_state *state)
{
    if (state == NULL || !state->route_test_enabled) {
        return false;
    }

    /*
     * ACK completion only closes the finite payload/ACK attempt. The repeating
     * channel-9 timing remains supervised until explicit policy, replacement,
     * or missed-event expiry clears it.
     */
    return false;
}
