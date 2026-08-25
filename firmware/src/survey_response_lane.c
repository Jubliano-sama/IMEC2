#include "survey_response_lane.h"

#include "protocol.h"

#include <string.h>

_Static_assert(SURVEY_RESPONSE_MAX_BUNDLES == 5u,
               "one compact lane must carry all one hundred pair results");
_Static_assert(sizeof(struct survey_response_lane) <= 896u,
               "survey response custody must remain below 896 bytes");

static bool kind_valid(enum survey_response_kind kind)
{
    return kind == SURVEY_RESPONSE_NEIGHBORS ||
           kind == SURVEY_RESPONSE_RANGES;
}

static uint8_t max_records_for_kind(enum survey_response_kind kind)
{
    return kind == SURVEY_RESPONSE_NEIGHBORS ?
        SURVEY_MAX_ANCHORS : SURVEY_RESPONSE_MAX_RECORDS;
}

static uint8_t bundle_count_for_records(uint8_t record_count)
{
    return (uint8_t)((record_count + SURVEY_RESPONSE_RECORDS_PER_BUNDLE - 1u) /
                     SURVEY_RESPONSE_RECORDS_PER_BUNDLE);
}

static int record_index(const struct survey_response_lane *lane,
                        const struct survey_response_record *record)
{
    uint8_t key = record->bytes[0];

    for (uint8_t i = 0u; i < lane->record_count; i++) {
        if (lane->records[i].bytes[0] == key) {
            return i;
        }
    }
    return -1;
}

static bool record_valid(enum survey_response_kind kind,
                         const struct survey_response_record *record)
{
    if (record == NULL) {
        return false;
    }
    if (kind == SURVEY_RESPONSE_NEIGHBORS) {
        struct survey_neighbor_report decoded;

        return survey_neighbor_report_decode(record->bytes,
                                             sizeof(record->bytes),
                                             &decoded) == PROTO_OK;
    }
    if (kind == SURVEY_RESPONSE_RANGES) {
        struct survey_range_result decoded;

        return survey_range_result_decode(record->bytes,
                                          sizeof(record->bytes),
                                          &decoded) == PROTO_OK;
    }
    return false;
}

int survey_response_lane_begin(
    struct survey_response_lane *lane,
    uint32_t network_id,
    uint32_t generation,
    uint64_t local_id,
    uint64_t parent_id,
    enum survey_response_kind kind,
    uint8_t hop_count,
    uint8_t max_hop_count,
    uint64_t start_ms)
{
    if (lane == NULL) {
        return PROTO_ERR_ARG;
    }
    if (network_id == 0u || generation == 0u || local_id == 0u ||
        parent_id == 0u || local_id == parent_id || !kind_valid(kind) ||
        hop_count == 0u || hop_count > max_hop_count ||
        max_hop_count == 0u || max_hop_count > UWB_ENUM_MAX_HOPS ||
        start_ms == 0u) {
        return PROTO_ERR_MALFORMED;
    }
    memset(lane, 0, sizeof(*lane));
    memset(lane->round_offsets_ms, SURVEY_RESPONSE_NO_OFFSET,
           sizeof(lane->round_offsets_ms));
    lane->network_id = network_id;
    lane->generation = generation;
    lane->local_id = local_id;
    lane->parent_id = parent_id;
    lane->kind = kind;
    lane->hop_count = hop_count;
    lane->max_hop_count = max_hop_count;
    lane->start_ms = start_ms;
    lane->prepared_round = UINT8_MAX;
    lane->active = true;
    return PROTO_OK;
}

void survey_response_lane_stop(struct survey_response_lane *lane)
{
    if (lane != NULL) {
        lane->active = false;
    }
}

int survey_response_lane_add_record(
    struct survey_response_lane *lane,
    const struct survey_response_record *record,
    bool *added)
{
    int existing;

    if (lane == NULL || record == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active || !record_valid(lane->kind, record)) {
        return PROTO_ERR_MALFORMED;
    }
    existing = record_index(lane, record);
    if (existing >= 0) {
        if (added != NULL) {
            *added = false;
        }
        return memcmp(&lane->records[existing], record, sizeof(*record)) == 0 ?
            PROTO_OK : PROTO_ERR_STALE;
    }
    if (lane->record_count >= max_records_for_kind(lane->kind)) {
        return PROTO_ERR_NO_SPACE;
    }
    lane->records[lane->record_count++] = *record;
    lane->prepared_round = UINT8_MAX;
    if (added != NULL) {
        *added = true;
    }
    return PROTO_OK;
}

int survey_response_lane_merge_bundle(
    struct survey_response_lane *lane,
    const struct survey_response_bundle *bundle,
    bool *added_records)
{
    uint8_t prior_count;
    bool any_added = false;

    if (lane == NULL || bundle == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active || bundle->network_id != lane->network_id ||
        bundle->generation != lane->generation ||
        bundle->parent_id != lane->local_id ||
        bundle->sender_id == 0u || bundle->sender_id == lane->local_id ||
        bundle->kind != lane->kind || bundle->record_count == 0u ||
        bundle->record_count > SURVEY_RESPONSE_RECORDS_PER_BUNDLE) {
        return PROTO_ERR_MALFORMED;
    }
    prior_count = lane->record_count;
    for (uint8_t i = 0u; i < bundle->record_count; i++) {
        bool added = false;
        int ret = survey_response_lane_add_record(lane,
                                                  &bundle->records[i],
                                                  &added);

        if (ret != PROTO_OK) {
            return ret;
        }
        any_added |= added;
    }
    if (any_added) {
        uint8_t bundle_count = bundle_count_for_records(lane->record_count);
        uint8_t first_changed =
            (uint8_t)(prior_count / SURVEY_RESPONSE_RECORDS_PER_BUNDLE);

        for (uint8_t sequence = first_changed;
             sequence < bundle_count; sequence++) {
            lane->acked_mask &= (uint16_t)~(UINT16_C(1) << sequence);
            lane->attempted_mask &= (uint16_t)~(UINT16_C(1) << sequence);
        }
        lane->prepared_round = UINT8_MAX;
    }
    if (added_records != NULL) {
        *added_records = any_added;
    }
    return PROTO_OK;
}

int survey_response_lane_prepare_round(
    struct survey_response_lane *lane,
    uint8_t round,
    uint32_t random_value)
{
    uint8_t pending[SURVEY_RESPONSE_MAX_BUNDLES];
    uint8_t pending_count = 0u;
    uint8_t bundle_count;

    if (lane == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active ||
        round >= ENUMERATION_RESPONSE_MAX_ROUNDS_PER_DEPTH) {
        return PROTO_ERR_MALFORMED;
    }
    if (lane->prepared_round == round) {
        return PROTO_OK;
    }
    lane->attempted_mask = 0u;
    memset(lane->round_offsets_ms, SURVEY_RESPONSE_NO_OFFSET,
           sizeof(lane->round_offsets_ms));
    bundle_count = bundle_count_for_records(lane->record_count);
    for (uint8_t sequence = 0u; sequence < bundle_count; sequence++) {
        if ((lane->acked_mask & (UINT16_C(1) << sequence)) == 0u) {
            pending[pending_count++] = sequence;
        }
    }
    for (uint8_t i = pending_count; i > 1u; i--) {
        uint8_t swap = (uint8_t)(random_value % i);
        uint8_t tmp = pending[i - 1u];

        pending[i - 1u] = pending[swap];
        pending[swap] = tmp;
        random_value = random_value * UINT32_C(1664525) +
                       UINT32_C(1013904223);
    }
    for (uint8_t i = 0u; i < pending_count; i++) {
        uint8_t sequence = pending[i];
        uint8_t span = ENUMERATION_RESPONSE_TX_WINDOW_MS -
                       ENUMERATION_RESPONSE_TX_LATE_GUARD_MS;
        uint8_t offset = (uint8_t)(random_value % span);

        while (true) {
            bool collision = false;

            for (uint8_t prior = 0u; prior < i; prior++) {
                uint8_t prior_offset =
                    lane->round_offsets_ms[pending[prior]];
                uint8_t distance = offset > prior_offset ?
                    (uint8_t)(offset - prior_offset) :
                    (uint8_t)(prior_offset - offset);

                if (distance < ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS) {
                    collision = true;
                    break;
                }
            }
            if (!collision) {
                break;
            }
            offset = (uint8_t)((offset +
                ENUMERATION_RESPONSE_MIN_LOCAL_TX_SPACING_MS) % span);
        }
        lane->round_offsets_ms[sequence] = offset;
        random_value = random_value * UINT32_C(1664525) +
                       UINT32_C(1013904223);
    }
    lane->prepared_round = round;
    return PROTO_OK;
}

int survey_response_lane_bundle_for_offset(
    struct survey_response_lane *lane,
    const struct enumeration_response_timing *timing,
    struct survey_response_bundle *bundle)
{
    uint8_t bundle_count;

    if (lane == NULL || timing == NULL || bundle == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!lane->active || lane->prepared_round != timing->round ||
        timing->round_offset_ms >= ENUMERATION_RESPONSE_TX_WINDOW_MS ||
        !enumeration_response_lane_tx_round_allowed(
            timing->depth, lane->hop_count, timing->round)) {
        return PROTO_ERR_NOT_FOUND;
    }
    bundle_count = bundle_count_for_records(lane->record_count);
    for (uint8_t sequence = 0u; sequence < bundle_count; sequence++) {
        uint8_t start;
        uint8_t count;

        if (lane->round_offsets_ms[sequence] == SURVEY_RESPONSE_NO_OFFSET ||
            timing->round_offset_ms < lane->round_offsets_ms[sequence] ||
            (uint8_t)(timing->round_offset_ms -
                      lane->round_offsets_ms[sequence]) >
                ENUMERATION_RESPONSE_TX_LATE_GUARD_MS ||
            (lane->acked_mask & (UINT16_C(1) << sequence)) != 0u) {
            continue;
        }
        start = (uint8_t)(sequence * SURVEY_RESPONSE_RECORDS_PER_BUNDLE);
        count = (uint8_t)(lane->record_count - start);
        if (count > SURVEY_RESPONSE_RECORDS_PER_BUNDLE) {
            count = SURVEY_RESPONSE_RECORDS_PER_BUNDLE;
        }
        memset(bundle, 0, sizeof(*bundle));
        bundle->network_id = lane->network_id;
        bundle->generation = lane->generation;
        bundle->sender_id = lane->local_id;
        bundle->parent_id = lane->parent_id;
        bundle->kind = lane->kind;
        bundle->sequence = sequence;
        bundle->record_count = count;
        memcpy(bundle->records, &lane->records[start],
               (size_t)count * sizeof(bundle->records[0]));
        lane->attempted_mask |= (uint16_t)(UINT16_C(1) << sequence);
        return PROTO_OK;
    }
    return PROTO_ERR_NOT_FOUND;
}

bool survey_response_lane_note_ack(
    struct survey_response_lane *lane,
    const struct survey_response_hop_ack *ack)
{
    uint8_t bundle_count;

    if (lane == NULL || ack == NULL || !lane->active ||
        ack->network_id != lane->network_id ||
        ack->generation != lane->generation ||
        ack->parent_id != lane->parent_id || ack->child_id != lane->local_id ||
        ack->kind != lane->kind) {
        return false;
    }
    bundle_count = bundle_count_for_records(lane->record_count);
    if (ack->sequence >= bundle_count ||
        (lane->attempted_mask & (UINT16_C(1) << ack->sequence)) == 0u) {
        return false;
    }
    lane->acked_mask |= (uint16_t)(UINT16_C(1) << ack->sequence);
    return true;
}

bool survey_response_lane_all_acked(const struct survey_response_lane *lane)
{
    uint8_t count;
    uint16_t expected;

    if (lane == NULL || !lane->active || lane->record_count == 0u) {
        return false;
    }
    count = bundle_count_for_records(lane->record_count);
    expected = (uint16_t)((UINT16_C(1) << count) - 1u);
    return (lane->acked_mask & expected) == expected;
}

uint8_t survey_response_lane_bundle_count(
    const struct survey_response_lane *lane)
{
    return lane == NULL ? 0u : bundle_count_for_records(lane->record_count);
}

uint8_t survey_response_lane_round_offset_ms(
    const struct survey_response_lane *lane,
    uint8_t sequence)
{
    return lane == NULL || sequence >= SURVEY_RESPONSE_MAX_BUNDLES ?
        SURVEY_RESPONSE_NO_OFFSET : lane->round_offsets_ms[sequence];
}

uint8_t survey_response_lane_next_offset_ms(
    const struct survey_response_lane *lane,
    const struct enumeration_response_timing *timing)
{
    uint8_t next = SURVEY_RESPONSE_NO_OFFSET;

    if (lane == NULL || timing == NULL || !lane->active ||
        lane->prepared_round != timing->round) {
        return next;
    }
    for (uint8_t sequence = 0u;
         sequence < SURVEY_RESPONSE_MAX_BUNDLES; sequence++) {
        uint8_t offset = lane->round_offsets_ms[sequence];

        if (offset != SURVEY_RESPONSE_NO_OFFSET &&
            offset > timing->round_offset_ms &&
            (next == SURVEY_RESPONSE_NO_OFFSET || offset < next)) {
            next = offset;
        }
    }
    return next;
}

const struct survey_response_record *survey_response_lane_record(
    const struct survey_response_lane *lane,
    uint8_t index)
{
    return lane == NULL || index >= lane->record_count ?
        NULL : &lane->records[index];
}

static int survey_sync_validate(const uint8_t *data,
                                size_t data_len,
                                size_t expected_len,
                                uint8_t expected_type)
{
    uint16_t expected_crc;

    if (data == NULL) {
        return PROTO_ERR_ARG;
    }
    if (data_len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (data[0] != UWB_MARKER) {
        return PROTO_ERR_BAD_MAGIC;
    }
    if (data[1] != UWB_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }
    if (data[2] != expected_type) {
        return PROTO_ERR_MALFORMED;
    }
    expected_crc = proto_get_u16_le(&data[data_len - UWB_FRAME_CRC_LEN]);
    return expected_crc == proto_crc16_ccitt_false(
            data, data_len - UWB_FRAME_CRC_LEN) ?
        PROTO_OK : PROTO_ERR_BAD_CRC;
}

static void survey_sync_begin(uint8_t *out, uint8_t type)
{
    out[0] = UWB_MARKER;
    out[1] = UWB_VERSION;
    out[2] = type;
}

static void survey_sync_finish(uint8_t *out, size_t total_len)
{
    proto_put_u16_le(&out[total_len - UWB_FRAME_CRC_LEN],
                     proto_crc16_ccitt_false(
                         out, total_len - UWB_FRAME_CRC_LEN));
}

int uwb_encode_survey_presence(const struct survey_presence_frame *frame,
                               uint8_t *out,
                               size_t out_cap,
                               size_t *written)
{
    if (frame == NULL || out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (frame->network_id == 0u || frame->generation == 0u ||
        frame->sender_id == 0u || frame->sender_slot >= SURVEY_MAX_ANCHORS) {
        return PROTO_ERR_MALFORMED;
    }
    if (out_cap < UWB_SURVEY_PRESENCE_LEN) {
        return PROTO_ERR_NO_SPACE;
    }
    survey_sync_begin(out, MSG_UWB_SURVEY_PRESENCE);
    proto_put_u32_le(&out[3], frame->network_id);
    proto_put_u32_le(&out[7], frame->generation);
    proto_put_u64_le(&out[11], frame->sender_id);
    out[19] = frame->sender_slot;
    survey_sync_finish(out, UWB_SURVEY_PRESENCE_LEN);
    *written = UWB_SURVEY_PRESENCE_LEN;
    return PROTO_OK;
}

int uwb_decode_survey_presence(const uint8_t *data,
                               size_t data_len,
                               struct survey_presence_frame *frame)
{
    int ret;

    if (frame == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_sync_validate(data, data_len, UWB_SURVEY_PRESENCE_LEN,
                               MSG_UWB_SURVEY_PRESENCE);
    if (ret != PROTO_OK) {
        return ret;
    }
    frame->network_id = proto_get_u32_le(&data[3]);
    frame->generation = proto_get_u32_le(&data[7]);
    frame->sender_id = proto_get_u64_le(&data[11]);
    frame->sender_slot = data[19];
    return frame->network_id != 0u && frame->generation != 0u &&
           frame->sender_id != 0u && frame->sender_slot < SURVEY_MAX_ANCHORS ?
        PROTO_OK : PROTO_ERR_MALFORMED;
}

size_t uwb_survey_bundle_encoded_len(uint8_t record_count)
{
    return record_count == 0u ||
           record_count > SURVEY_RESPONSE_RECORDS_PER_BUNDLE ? 0u :
        UWB_SURVEY_BUNDLE_BASE_LEN +
        (size_t)record_count * SURVEY_RESPONSE_RECORD_WIRE_LEN;
}

static int survey_bundle_validate(const struct survey_response_bundle *bundle)
{
    if (bundle == NULL || bundle->network_id == 0u ||
        bundle->generation == 0u || bundle->sender_id == 0u ||
        bundle->parent_id == 0u || bundle->sender_id == bundle->parent_id ||
        !kind_valid(bundle->kind) || bundle->record_count == 0u ||
        bundle->record_count > SURVEY_RESPONSE_RECORDS_PER_BUNDLE) {
        return PROTO_ERR_MALFORMED;
    }
    for (uint8_t i = 0u; i < bundle->record_count; i++) {
        if (!record_valid(bundle->kind, &bundle->records[i])) {
            return PROTO_ERR_MALFORMED;
        }
        for (uint8_t prior = 0u; prior < i; prior++) {
            if (bundle->records[prior].bytes[0] ==
                bundle->records[i].bytes[0]) {
                return PROTO_ERR_MALFORMED;
            }
        }
    }
    return PROTO_OK;
}

int uwb_encode_survey_bundle(const struct survey_response_bundle *bundle,
                             uint8_t *out,
                             size_t out_cap,
                             size_t *written)
{
    size_t total_len;
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_bundle_validate(bundle);
    if (ret != PROTO_OK) {
        return ret;
    }
    total_len = uwb_survey_bundle_encoded_len(bundle->record_count);
    if (out_cap < total_len) {
        return PROTO_ERR_NO_SPACE;
    }
    survey_sync_begin(out, MSG_UWB_SURVEY_BUNDLE);
    proto_put_u32_le(&out[3], bundle->network_id);
    proto_put_u32_le(&out[7], bundle->generation);
    proto_put_u64_le(&out[11], bundle->sender_id);
    proto_put_u64_le(&out[19], bundle->parent_id);
    out[27] = (uint8_t)bundle->kind;
    out[28] = bundle->sequence;
    out[29] = bundle->record_count;
    memcpy(&out[30], bundle->records,
           (size_t)bundle->record_count * sizeof(bundle->records[0]));
    survey_sync_finish(out, total_len);
    *written = total_len;
    return PROTO_OK;
}

int uwb_decode_survey_bundle(const uint8_t *data,
                             size_t data_len,
                             struct survey_response_bundle *bundle)
{
    size_t expected_len;
    int ret;

    if (data == NULL || bundle == NULL ||
        data_len < UWB_SURVEY_BUNDLE_BASE_LEN +
                   SURVEY_RESPONSE_RECORD_WIRE_LEN) {
        return PROTO_ERR_ARG;
    }
    if (data[0] != UWB_MARKER || data[1] != UWB_VERSION ||
        data[2] != MSG_UWB_SURVEY_BUNDLE) {
        return survey_sync_validate(data, data_len, data_len,
                                    MSG_UWB_SURVEY_BUNDLE);
    }
    memset(bundle, 0, sizeof(*bundle));
    bundle->network_id = proto_get_u32_le(&data[3]);
    bundle->generation = proto_get_u32_le(&data[7]);
    bundle->sender_id = proto_get_u64_le(&data[11]);
    bundle->parent_id = proto_get_u64_le(&data[19]);
    bundle->kind = (enum survey_response_kind)data[27];
    bundle->sequence = data[28];
    bundle->record_count = data[29];
    expected_len = uwb_survey_bundle_encoded_len(bundle->record_count);
    if (expected_len == 0u || data_len != expected_len) {
        return PROTO_ERR_BAD_LENGTH;
    }
    ret = survey_sync_validate(data, data_len, expected_len,
                               MSG_UWB_SURVEY_BUNDLE);
    if (ret != PROTO_OK) {
        return ret;
    }
    memcpy(bundle->records, &data[30],
           (size_t)bundle->record_count * sizeof(bundle->records[0]));
    return survey_bundle_validate(bundle);
}

static int survey_ack_validate(const struct survey_response_hop_ack *ack)
{
    return ack != NULL && ack->network_id != 0u && ack->generation != 0u &&
           ack->parent_id != 0u && ack->child_id != 0u &&
           ack->parent_id != ack->child_id && kind_valid(ack->kind) &&
           ack->sequence < SURVEY_RESPONSE_MAX_BUNDLES ?
        PROTO_OK : PROTO_ERR_MALFORMED;
}

int uwb_encode_survey_hop_ack(const struct survey_response_hop_ack *ack,
                              uint8_t *out,
                              size_t out_cap,
                              size_t *written)
{
    int ret;

    if (out == NULL || written == NULL) {
        return PROTO_ERR_ARG;
    }
    if (out_cap < UWB_SURVEY_HOP_ACK_LEN) {
        return PROTO_ERR_NO_SPACE;
    }
    ret = survey_ack_validate(ack);
    if (ret != PROTO_OK) {
        return ret;
    }
    survey_sync_begin(out, MSG_UWB_SURVEY_HOP_ACK);
    proto_put_u32_le(&out[3], ack->network_id);
    proto_put_u32_le(&out[7], ack->generation);
    proto_put_u64_le(&out[11], ack->parent_id);
    proto_put_u64_le(&out[19], ack->child_id);
    out[27] = (uint8_t)ack->kind;
    out[28] = ack->sequence;
    survey_sync_finish(out, UWB_SURVEY_HOP_ACK_LEN);
    *written = UWB_SURVEY_HOP_ACK_LEN;
    return PROTO_OK;
}

int uwb_decode_survey_hop_ack(const uint8_t *data,
                              size_t data_len,
                              struct survey_response_hop_ack *ack)
{
    int ret;

    if (ack == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = survey_sync_validate(data, data_len, UWB_SURVEY_HOP_ACK_LEN,
                               MSG_UWB_SURVEY_HOP_ACK);
    if (ret != PROTO_OK) {
        return ret;
    }
    ack->network_id = proto_get_u32_le(&data[3]);
    ack->generation = proto_get_u32_le(&data[7]);
    ack->parent_id = proto_get_u64_le(&data[11]);
    ack->child_id = proto_get_u64_le(&data[19]);
    ack->kind = (enum survey_response_kind)data[27];
    ack->sequence = data[28];
    return survey_ack_validate(ack);
}
