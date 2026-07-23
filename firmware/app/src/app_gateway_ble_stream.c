#include "app_gateway_ble_stream.h"
#include "app_gateway_command_observability.h"

#include <errno.h>
#include <string.h>

_Static_assert(GATEWAY_BLE_STREAM_RECORD_HEADER_LEN <=
               GATEWAY_BLE_STREAM_RECORD_MAX_LEN,
               "gateway BLE stream header must fit a record");
_Static_assert(sizeof(struct gateway_ble_stream_state) <=
               GATEWAY_BLE_STREAM_RAM_BUDGET_BYTES,
               "gateway BLE stream queue exceeds RAM budget");
_Static_assert(GATEWAY_BLE_STREAM_RECORD_POOL_BYTES >=
               GATEWAY_BLE_STREAM_CLICK_CIR_BURST_BYTES,
               "gateway BLE stream pool must hold one click and both CIR records");
_Static_assert(GATEWAY_BLE_STREAM_RECORD_POOL_BYTES >=
               GATEWAY_BLE_STREAM_CLICK_CIR_BURST_BYTES +
               GATEWAY_BLE_STREAM_RECORD_POOL_SAFETY_MARGIN_BYTES,
               "gateway BLE stream pool must retain its verified burst margin");
_Static_assert(GATEWAY_BLE_STREAM_QUEUE_DEPTH > 0u,
               "gateway BLE stream reservation requires a queue slot");

#define STREAM_FLAG_TRUNCATED 0x01u

uint32_t gateway_ble_recovery_backoff_ms(uint8_t retry_round,
                                         uint32_t random_value)
{
    uint32_t base_ms = GATEWAY_BLE_RECOVERY_BACKOFF_BASE_MS;

    while (retry_round > 0u && base_ms < GATEWAY_BLE_RECOVERY_BACKOFF_MAX_MS) {
        if (base_ms > GATEWAY_BLE_RECOVERY_BACKOFF_MAX_MS / 2u) {
            base_ms = GATEWAY_BLE_RECOVERY_BACKOFF_MAX_MS;
            break;
        }
        base_ms *= 2u;
        retry_round--;
    }
    if (base_ms >= GATEWAY_BLE_RECOVERY_BACKOFF_MAX_MS) {
        return GATEWAY_BLE_RECOVERY_BACKOFF_MAX_MS;
    }
    return base_ms + (random_value % base_ms);
}

static void put_u8(uint8_t *record, size_t *offset, uint8_t value)
{
    if (record != NULL) {
        record[*offset] = value;
    }
    *offset += sizeof(value);
}

static void put_u16(uint8_t *record, size_t *offset, uint16_t value)
{
    if (record != NULL) {
        proto_put_u16_le(&record[*offset], value);
    }
    *offset += sizeof(value);
}

static void put_u32(uint8_t *record, size_t *offset, uint32_t value)
{
    if (record != NULL) {
        proto_put_u32_le(&record[*offset], value);
    }
    *offset += sizeof(value);
}

static void put_u64(uint8_t *record, size_t *offset, uint64_t value)
{
    if (record != NULL) {
        proto_put_u64_le(&record[*offset], value);
    }
    *offset += sizeof(value);
}

static uint8_t priority_for_class(enum gateway_ble_stream_class packet_class)
{
    switch (packet_class) {
    case GATEWAY_BLE_STREAM_CLASS_CLICK:
        return 0u;
    case GATEWAY_BLE_STREAM_CLASS_RESULT:
    case GATEWAY_BLE_STREAM_CLASS_SURVEY:
    case GATEWAY_BLE_STREAM_CLASS_COMMAND_EVENT:
        return 1u;
    case GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC:
        return 2u;
    case GATEWAY_BLE_STREAM_CLASS_STATUS:
        return 3u;
    case GATEWAY_BLE_STREAM_CLASS_UNKNOWN:
    default:
        return UINT8_MAX;
    }
}

static bool class_can_truncate(enum gateway_ble_stream_class packet_class)
{
    return packet_class == GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC ||
           packet_class == GATEWAY_BLE_STREAM_CLASS_STATUS;
}

enum gateway_ble_stream_class gateway_ble_stream_classify_packet(uint8_t msg_type,
                                                                 uint8_t flags)
{
    if ((flags & FLAG_COUNT_AS_CLICK) != 0u || msg_type == MSG_CLICK_REPORT) {
        return GATEWAY_BLE_STREAM_CLASS_CLICK;
    }

    switch (msg_type) {
    case MSG_MESH_DATA:
        return (flags & FLAG_DIAGNOSTIC) != 0u ?
               GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC :
               GATEWAY_BLE_STREAM_CLASS_UNKNOWN;
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
        return GATEWAY_BLE_STREAM_CLASS_RESULT;
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return GATEWAY_BLE_STREAM_CLASS_SURVEY;
    case MSG_GATEWAY_COMMAND_EVENT:
        return GATEWAY_BLE_STREAM_CLASS_COMMAND_EVENT;
    case MSG_UWB_CLICKER_DIAG:
    case MSG_UWB_ANCHOR_DIAG:
    case MSG_UWB_ANCHOR_DIAG_FRAGMENT:
    case MSG_SELF_TEST_REPORT:
        return GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC;
    case MSG_ANCHOR_HEARTBEAT:
        return GATEWAY_BLE_STREAM_CLASS_STATUS;
    default:
        if ((flags & FLAG_DIAGNOSTIC) != 0u) {
            return GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC;
        }
        return GATEWAY_BLE_STREAM_CLASS_UNKNOWN;
    }
}

bool gateway_ble_should_stream_packet(uint8_t msg_type,
                                      uint8_t flags,
                                      enum gateway_ble_stream_class packet_class)
{
    enum gateway_ble_stream_class classified = packet_class;

    if (classified == GATEWAY_BLE_STREAM_CLASS_UNKNOWN) {
        classified = gateway_ble_stream_classify_packet(msg_type, flags);
    }

    return priority_for_class(classified) != UINT8_MAX;
}

void gateway_ble_stream_init(struct gateway_ble_stream_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static void remove_item(struct gateway_ble_stream_state *state, uint8_t index)
{
    uint16_t removed_offset;
    uint16_t removed_len;

    if (state == NULL || index >= state->count) {
        return;
    }
    removed_offset = state->items[index].offset;
    removed_len = state->items[index].len;
    memmove(&state->record_pool[removed_offset],
            &state->record_pool[removed_offset + removed_len],
            state->pool_used - removed_offset - removed_len);
    state->pool_used -= removed_len;
    for (uint8_t i = 0u; i < state->count; i++) {
        if (i != index && state->items[i].offset > removed_offset) {
            state->items[i].offset -= removed_len;
        }
    }
    for (uint8_t i = index; i + 1u < state->count; i++) {
        state->items[i] = state->items[i + 1u];
    }
    state->count--;
}

static void note_drop(struct gateway_ble_stream_state *state,
                      uint8_t packet_type,
                      enum gateway_ble_stream_drop_reason reason)
{
    if (state == NULL) {
        return;
    }

    switch (reason) {
    case GATEWAY_BLE_STREAM_DROP_QUEUE_FULL:
        state->diagnostics.drops_queue_full++;
        break;
    case GATEWAY_BLE_STREAM_DROP_TOO_LARGE:
        state->diagnostics.drops_too_large++;
        break;
    case GATEWAY_BLE_STREAM_DROP_NOT_READY:
        state->diagnostics.drops_not_ready++;
        break;
    case GATEWAY_BLE_STREAM_DROP_PRIORITY:
        state->diagnostics.drops_priority++;
        break;
    case GATEWAY_BLE_STREAM_DROP_NONE:
    default:
        break;
    }
    state->diagnostics.last_dropped_packet_type = packet_type;
    state->diagnostics.last_drop_reason = reason;
}

static bool drop_one_lower_priority(struct gateway_ble_stream_state *state,
                                    uint8_t incoming_priority)
{
    uint8_t best_offset = UINT8_MAX;
    uint8_t best_priority = incoming_priority;
    uint8_t first_offset = state->head_send_active ? 1u : 0u;

    for (uint8_t i = first_offset; i < state->count; i++) {
        uint8_t queued_priority = state->items[i].priority;

        if (state->items[i].retain_until_sent) {
            continue;
        }
        /*
         * Results, surveys, and command events may already own durable
         * protocol custody.  Only best-effort diagnostics and status records
         * may be displaced by a higher-priority click.
         */
        if (queued_priority <
            priority_for_class(GATEWAY_BLE_STREAM_CLASS_DIAGNOSTIC)) {
            continue;
        }
        if (queued_priority > best_priority) {
            best_priority = queued_priority;
            best_offset = i;
        }
    }
    if (best_offset == UINT8_MAX) {
        return false;
    }

    note_drop(state,
              state->items[best_offset].packet_type,
              GATEWAY_BLE_STREAM_DROP_PRIORITY);
    remove_item(state, best_offset);
    return true;
}

static struct gateway_ble_stream_item *reservation_item(
    struct gateway_ble_stream_state *state)
{
    return &state->items[GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u];
}

static const struct gateway_ble_stream_item *reservation_item_const(
    const struct gateway_ble_stream_state *state)
{
    return &state->items[GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u];
}

static bool queue_capacity_available(
    const struct gateway_ble_stream_state *state,
    uint16_t record_len)
{
    size_t occupied_pool;
    uint8_t occupied_slots;

    occupied_slots = state->count + (state->reservation_active ? 1u : 0u);
    occupied_pool = state->pool_used;
    if (state->reservation_active) {
        occupied_pool += reservation_item_const(state)->len;
    }
    return occupied_slots < GATEWAY_BLE_STREAM_QUEUE_DEPTH &&
           occupied_pool + record_len <= sizeof(state->record_pool);
}

static bool packet_identity_matches(const struct proto_packet *left,
                                    const struct proto_packet *right)
{
    return left->msg_type == right->msg_type &&
           left->flags == right->flags &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq &&
           left->ttl == right->ttl &&
           left->payload_len == right->payload_len &&
           left->message_age_ms == right->message_age_ms;
}

static int build_record(const struct proto_packet *packet,
                        const uint8_t *payload,
                        size_t payload_len,
                        enum gateway_ble_stream_class packet_class,
                        uint32_t received_at_ms,
                        uint32_t now_ms,
                        uint8_t *record,
                        size_t record_cap,
                        struct gateway_ble_stream_item *item)
{
    size_t offset = 0u;
    uint8_t record_flags = 0u;
    uint8_t priority;
    size_t copy_len = payload_len;

    if (packet == NULL || item == NULL ||
        (record == NULL && record_cap != 0u) ||
        (payload == NULL && payload_len != 0u)) {
        return -EINVAL;
    }
    if (payload_len > GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN) {
        if (!class_can_truncate(packet_class)) {
            return -EMSGSIZE;
        }
        copy_len = GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN;
        record_flags |= STREAM_FLAG_TRUNCATED;
    }
    if (record != NULL &&
        record_cap < GATEWAY_BLE_STREAM_RECORD_HEADER_LEN + copy_len) {
        return -EINVAL;
    }

    priority = priority_for_class(packet_class);
    if (packet_class == GATEWAY_BLE_STREAM_CLASS_COMMAND_EVENT &&
        payload_len == GATEWAY_COMMAND_EVENT_WIRE_LEN &&
        payload[0] == GATEWAY_COMMAND_EVENT_SCHEMA_VERSION &&
        payload[1] == GATEWAY_COMMAND_EVENT_WIRE_LEN &&
        (payload[4] & GATEWAY_COMMAND_EVENT_FLAG_TERMINAL) != 0u) {
        priority = 0u;
    }

    memset(item, 0, sizeof(*item));
    put_u16(record, &offset, GATEWAY_BLE_STREAM_MAGIC);
    put_u8(record, &offset, GATEWAY_BLE_STREAM_VERSION);
    put_u8(record, &offset, GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    put_u8(record, &offset, GATEWAY_BLE_STREAM_RECORD_PACKET);
    put_u8(record, &offset, (uint8_t)packet_class);
    put_u8(record, &offset, priority);
    put_u8(record, &offset, record_flags);
    put_u8(record, &offset, packet->msg_type);
    put_u8(record, &offset, packet->flags);
    put_u16(record, &offset, packet->seq);
    put_u32(record, &offset, packet->session_id);
    put_u64(record, &offset, packet->src_id);
    put_u64(record, &offset, packet->dst_id);
    put_u32(record, &offset, now_ms - received_at_ms);
    put_u16(record, &offset, (uint16_t)copy_len);
    put_u16(record, &offset, proto_crc16_ccitt_false(payload, copy_len));
    if (offset != GATEWAY_BLE_STREAM_RECORD_HEADER_LEN) {
        return -EINVAL;
    }
    if (record != NULL && copy_len > 0u) {
        /* The startup journal restore may use an in-pool staging tail whose
         * bytes overlap the final record destination. */
        memmove(&record[offset], payload, copy_len);
    }
    item->len = (uint16_t)(offset + copy_len);
    item->packet_type = packet->msg_type;
    item->priority = priority;
    item->queued_at_ms = now_ms;
    item->received_at_ms = received_at_ms;
    item->packet = *packet;
    return 0;
}

int gateway_ble_stream_enqueue_packet(struct gateway_ble_stream_state *state,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms,
                                      uint32_t now_ms,
                                      bool ble_ready)
{
    enum gateway_ble_stream_class packet_class;
    struct gateway_ble_stream_item item;
    int ret;

    if (state == NULL || packet == NULL) {
        return -EINVAL;
    }
    if (state->restore_staging_active) {
        return -EAGAIN;
    }
    state->diagnostics.enqueue_attempts++;
    packet_class = gateway_ble_stream_classify_packet(packet->msg_type,
                                                      packet->flags);
    if (!gateway_ble_should_stream_packet(packet->msg_type,
                                          packet->flags,
                                          packet_class)) {
        return 0;
    }

    ret = build_record(packet,
                       payload,
                       payload_len,
                       packet_class,
                       received_at_ms,
                       now_ms,
                       NULL,
                       0u,
                       &item);
    if (ret == -EMSGSIZE) {
        note_drop(state, packet->msg_type, GATEWAY_BLE_STREAM_DROP_TOO_LARGE);
        return ret;
    }
    if (ret < 0) {
        return ret;
    }

    while (!queue_capacity_available(state, item.len) &&
           drop_one_lower_priority(state, item.priority)) {
    }
    if (!queue_capacity_available(state, item.len)) {
        note_drop(state,
                  packet->msg_type,
                  ble_ready ? GATEWAY_BLE_STREAM_DROP_QUEUE_FULL :
                              GATEWAY_BLE_STREAM_DROP_NOT_READY);
        return ble_ready ? -ENOSPC : -ENOTCONN;
    }

    item.offset = state->pool_used;
    ret = build_record(packet,
                       payload,
                       payload_len,
                       packet_class,
                       received_at_ms,
                       now_ms,
                       &state->record_pool[state->pool_used],
                       sizeof(state->record_pool) - state->pool_used,
                       &item);
    if (ret < 0) {
        return ret;
    }
    item.offset = state->pool_used;
    state->pool_used += item.len;
    state->items[state->count] = item;
    state->count++;
    if (state->count > state->diagnostics.max_queue_depth_observed) {
        state->diagnostics.max_queue_depth_observed = state->count;
    }
    return 1;
}

int gateway_ble_stream_enqueue_staged_packet(
    struct gateway_ble_stream_state *state,
    const struct proto_packet *packet,
    size_t payload_len,
    uint32_t received_at_ms,
    uint32_t now_ms,
    bool ble_ready)
{
    enum gateway_ble_stream_class packet_class;
    struct gateway_ble_stream_item item;
    size_t staging_offset;
    size_t destination_offset;
    uint8_t *destination_payload;
    int ret;

    if (state == NULL || packet == NULL ||
        !state->restore_staging_active) {
        return -EINVAL;
    }
    if (payload_len > GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN) {
        note_drop(state, packet->msg_type, GATEWAY_BLE_STREAM_DROP_TOO_LARGE);
        return -EMSGSIZE;
    }
    staging_offset = state->restore_staging_offset;
    if (staging_offset > sizeof(state->record_pool) ||
        payload_len > sizeof(state->record_pool) - staging_offset) {
        return -EINVAL;
    }
    /* The staging tail must remain outside the used prefix.  Removing a
     * lower-priority item can only shorten that prefix, so the source stays
     * stable while queue pressure is resolved below. */
    if (state->pool_used > staging_offset || state->reservation_active) {
        return -EAGAIN;
    }

    state->diagnostics.enqueue_attempts++;
    packet_class = gateway_ble_stream_classify_packet(packet->msg_type,
                                                      packet->flags);
    if (!gateway_ble_should_stream_packet(packet->msg_type,
                                          packet->flags,
                                          packet_class)) {
        return 0;
    }

    ret = build_record(packet,
                       &state->record_pool[staging_offset],
                       payload_len,
                       packet_class,
                       received_at_ms,
                       now_ms,
                       NULL,
                       0u,
                       &item);
    if (ret == -EMSGSIZE) {
        note_drop(state, packet->msg_type, GATEWAY_BLE_STREAM_DROP_TOO_LARGE);
        return ret;
    }
    if (ret < 0) {
        return ret;
    }

    while (!queue_capacity_available(state, item.len) &&
           drop_one_lower_priority(state, item.priority)) {
    }
    if (!queue_capacity_available(state, item.len)) {
        note_drop(state,
                  packet->msg_type,
                  ble_ready ? GATEWAY_BLE_STREAM_DROP_QUEUE_FULL :
                              GATEWAY_BLE_STREAM_DROP_NOT_READY);
        return ble_ready ? -ENOSPC : -ENOTCONN;
    }

    destination_offset = state->pool_used;
    destination_payload = &state->record_pool[
        destination_offset + GATEWAY_BLE_STREAM_RECORD_HEADER_LEN];
    /* Move the complete payload before writing the destination header.  A
     * plain memcpy/header-first build would destroy bytes when the 958-byte
     * tail overlaps the record being appended. */
    memmove(destination_payload,
            &state->record_pool[staging_offset],
            payload_len);
    ret = build_record(packet,
                       destination_payload,
                       payload_len,
                       packet_class,
                       received_at_ms,
                       now_ms,
                       &state->record_pool[destination_offset],
                       sizeof(state->record_pool) - destination_offset,
                       &item);
    if (ret < 0) {
        return ret;
    }
    item.offset = (uint16_t)destination_offset;
    state->pool_used += item.len;
    state->items[state->count] = item;
    state->count++;
    if (state->count > state->diagnostics.max_queue_depth_observed) {
        state->diagnostics.max_queue_depth_observed = state->count;
    }
    return 1;
}

int gateway_ble_stream_reserve_packet(struct gateway_ble_stream_state *state,
                                      const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint32_t received_at_ms,
                                      uint32_t now_ms,
                                      bool ble_ready)
{
    enum gateway_ble_stream_class packet_class;
    struct gateway_ble_stream_item item;
    int ret;

    if (state == NULL || packet == NULL ||
        (payload == NULL && payload_len != 0u)) {
        return -EINVAL;
    }
    if (state->restore_staging_active) {
        return -EAGAIN;
    }
    state->diagnostics.enqueue_attempts++;
    if (state->reservation_active) {
        return -EBUSY;
    }
    if (payload_len > UINT16_MAX) {
        note_drop(state, packet->msg_type, GATEWAY_BLE_STREAM_DROP_TOO_LARGE);
        return -EMSGSIZE;
    }

    packet_class = gateway_ble_stream_classify_packet(packet->msg_type,
                                                      packet->flags);
    if (!gateway_ble_should_stream_packet(packet->msg_type,
                                          packet->flags,
                                          packet_class)) {
        return 0;
    }
    ret = build_record(packet,
                       payload,
                       payload_len,
                       packet_class,
                       received_at_ms,
                       now_ms,
                       NULL,
                       0u,
                       &item);
    if (ret == -EMSGSIZE) {
        note_drop(state, packet->msg_type, GATEWAY_BLE_STREAM_DROP_TOO_LARGE);
        return ret;
    }
    if (ret < 0) {
        return ret;
    }

    while (!queue_capacity_available(state, item.len) &&
           drop_one_lower_priority(state, item.priority)) {
    }
    if (!queue_capacity_available(state, item.len)) {
        note_drop(state,
                  packet->msg_type,
                  ble_ready ? GATEWAY_BLE_STREAM_DROP_QUEUE_FULL :
                              GATEWAY_BLE_STREAM_DROP_NOT_READY);
        return ble_ready ? -ENOSPC : -ENOTCONN;
    }

    *reservation_item(state) = item;
    state->reservation_payload_len = (uint16_t)payload_len;
    state->reservation_payload_crc =
        proto_crc16_ccitt_false(payload, payload_len);
    state->reservation_active = true;
    return 1;
}

int gateway_ble_stream_commit_reservation(
    struct gateway_ble_stream_state *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    const struct gateway_ble_stream_item *reserved;
    enum gateway_ble_stream_class packet_class;
    struct gateway_ble_stream_item item;
    uint8_t insert_index;
    int ret;

    if (state == NULL || packet == NULL ||
        (payload == NULL && payload_len != 0u)) {
        return -EINVAL;
    }
    if (state->restore_staging_active) {
        return -EAGAIN;
    }
    if (!state->reservation_active) {
        return -ENOENT;
    }
    reserved = reservation_item_const(state);
    if (payload_len != state->reservation_payload_len ||
        !packet_identity_matches(packet, &reserved->packet) ||
        proto_crc16_ccitt_false(payload, payload_len) !=
            state->reservation_payload_crc) {
        return -ESTALE;
    }

    packet_class = gateway_ble_stream_classify_packet(packet->msg_type,
                                                      packet->flags);
    ret = build_record(packet,
                       payload,
                       payload_len,
                       packet_class,
                       reserved->received_at_ms,
                       reserved->queued_at_ms,
                       NULL,
                       0u,
                       &item);
    if (ret < 0 || item.len != reserved->len ||
        item.packet_type != reserved->packet_type ||
        item.priority != reserved->priority) {
        return ret < 0 ? ret : -ESTALE;
    }
    if (state->count >= GATEWAY_BLE_STREAM_QUEUE_DEPTH ||
        state->pool_used + item.len > sizeof(state->record_pool)) {
        return -ENOSPC;
    }

    item.offset = state->pool_used;
    ret = build_record(packet,
                       payload,
                       payload_len,
                       packet_class,
                       reserved->received_at_ms,
                       reserved->queued_at_ms,
                       &state->record_pool[state->pool_used],
                       sizeof(state->record_pool) - state->pool_used,
                       &item);
    if (ret < 0) {
        return ret;
    }

    insert_index = state->count;
    item.offset = state->pool_used;
    state->pool_used += item.len;
    state->items[insert_index] = item;
    state->count++;
    state->reservation_active = false;
    state->reservation_payload_len = 0u;
    state->reservation_payload_crc = 0u;
    if (insert_index != GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u) {
        memset(reservation_item(state), 0, sizeof(*reservation_item(state)));
    }
    if (state->count > state->diagnostics.max_queue_depth_observed) {
        state->diagnostics.max_queue_depth_observed = state->count;
    }
    return 1;
}

void gateway_ble_stream_cancel_reservation(
    struct gateway_ble_stream_state *state)
{
    if (state == NULL || !state->reservation_active) {
        return;
    }
    memset(reservation_item(state), 0, sizeof(*reservation_item(state)));
    state->reservation_payload_len = 0u;
    state->reservation_payload_crc = 0u;
    state->reservation_active = false;
}

unsigned int gateway_ble_stream_drain(struct gateway_ble_stream_state *state,
                                      gateway_ble_stream_send_fn send_fn,
                                      void *send_ctx,
                                      uint32_t now_ms,
                                      bool ble_ready,
                                      unsigned int max_records)
{
    unsigned int sent = 0u;

    if (state == NULL || send_fn == NULL || max_records == 0u) {
        return 0u;
    }
    if (state->restore_staging_active) {
        return 0u;
    }
    if (!ble_ready) {
        if (state->count > 0u) {
            state->diagnostics.oldest_queued_age_ms =
                now_ms - state->items[0].queued_at_ms;
        }
        return 0u;
    }

    while (state->count > 0u && sent < max_records) {
        struct gateway_ble_stream_item *item = &state->items[0];
        int ret = send_fn(&state->record_pool[item->offset], item->len, send_ctx);

        if (ret < 0) {
            break;
        }
        state->diagnostics.packets_sent++;
        state->diagnostics.bytes_sent += item->len;
        remove_item(state, 0u);
        sent++;
    }
    state->diagnostics.oldest_queued_age_ms =
        state->count == 0u ? 0u : now_ms - state->items[0].queued_at_ms;
    return sent;
}

int gateway_ble_stream_peek(const struct gateway_ble_stream_state *state,
                            const uint8_t **record,
                            size_t *record_len)
{
    if (state == NULL || record == NULL || record_len == NULL) {
        return -EINVAL;
    }
    if (state->restore_staging_active) {
        return -EAGAIN;
    }
    if (state->count == 0u) {
        return -ENOENT;
    }

    *record = &state->record_pool[state->items[0].offset];
    *record_len = state->items[0].len;
    return 0;
}

int gateway_ble_stream_begin_send(struct gateway_ble_stream_state *state,
                                  uint8_t *record,
                                  size_t record_cap,
                                  size_t *record_len)
{
    const uint8_t *item_record = NULL;
    size_t item_len = 0u;
    int ret;

    if (state == NULL || record == NULL || record_len == NULL) {
        return -EINVAL;
    }
    ret = gateway_ble_stream_begin_send_view(state, &item_record, &item_len);
    if (ret < 0) {
        return ret;
    }
    if (item_len > record_cap) {
        state->head_send_active = false;
        return -EMSGSIZE;
    }
    memcpy(record, item_record, item_len);
    *record_len = item_len;
    return 0;
}

int gateway_ble_stream_begin_send_view(struct gateway_ble_stream_state *state,
                                       const uint8_t **record,
                                       size_t *record_len)
{
    const struct gateway_ble_stream_item *item;

    if (state == NULL || record == NULL || record_len == NULL) {
        return -EINVAL;
    }
    if (state->restore_staging_active) {
        return -EAGAIN;
    }
    if (state->count == 0u) {
        return -ENOENT;
    }
    if (state->head_send_active) {
        return -EBUSY;
    }

    item = &state->items[0];
    *record = &state->record_pool[item->offset];
    *record_len = item->len;
    state->head_send_active = true;
    return 0;
}

void gateway_ble_stream_cancel_send(struct gateway_ble_stream_state *state)
{
    if (state != NULL) {
        state->head_send_active = false;
    }
}

void gateway_ble_stream_mark_sent(struct gateway_ble_stream_state *state,
                                  uint32_t now_ms)
{
    struct gateway_ble_stream_item *item;

    if (state == NULL || state->count == 0u ||
        state->restore_staging_active) {
        return;
    }

    item = &state->items[0];
    state->diagnostics.packets_sent++;
    state->diagnostics.bytes_sent += item->len;
    remove_item(state, 0u);
    state->head_send_active = false;
    state->diagnostics.oldest_queued_age_ms =
        state->count == 0u ? 0u : now_ms - state->items[0].queued_at_ms;
}

void gateway_ble_stream_get_diagnostics(
    const struct gateway_ble_stream_state *state,
    uint32_t now_ms,
    struct gateway_ble_stream_diagnostics *diagnostics)
{
    if (state == NULL || diagnostics == NULL) {
        return;
    }
    *diagnostics = state->diagnostics;
    diagnostics->oldest_queued_age_ms =
        state->count == 0u ? 0u : now_ms - state->items[0].queued_at_ms;
}

uint8_t gateway_ble_stream_depth(const struct gateway_ble_stream_state *state)
{
    return state == NULL ? 0u : state->count;
}

int gateway_ble_stream_head_packet(const struct gateway_ble_stream_state *state,
                                   struct proto_packet *packet)
{
    if (state == NULL || packet == NULL) {
        return -EINVAL;
    }
    if (state->restore_staging_active) {
        return -EAGAIN;
    }
    if (state->count == 0u) {
        return -ENOENT;
    }
    *packet = state->items[0].packet;
    return 0;
}
