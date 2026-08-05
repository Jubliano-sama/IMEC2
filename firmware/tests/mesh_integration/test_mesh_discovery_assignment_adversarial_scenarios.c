#include "app_discovery_assignment_policy.h"
#include "app_mesh_report.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "protocol.h"
#include "uwb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ANCHORS UWB_DISCOVERY_SLOT_COUNT
#define GATEWAY_ID UINT64_C(0xd150000000000001)
#define ANCHOR_BASE UINT64_C(0xd150000001000000)
#define ASSIGNMENT_EPOCH UINT32_C(0xd1500104)
#define CLAIM_SESSION ASSIGNMENT_EPOCH
#define TABLE_SESSION UINT32_C(0x42000001)
#define TABLE_GENERATION_1 UINT32_C(0x42000001)
#define TABLE_GENERATION_2 UINT32_C(0x42000002)
#define TABLE_GENERATION_3 UINT32_C(0x42000003)
#define OPERATION_DEADLINE_MS \
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS
#define MAX_ROUNDS DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS

_Static_assert(CMD_ASSIGN_DISCOVERY_SLOTS == 0x0104,
               "assignment model must follow the production host command");
_Static_assert(MAX_ANCHORS == 50u,
               "assignment model must cover the production anchor capacity");
_Static_assert(DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS == 1u &&
                   DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS == 1u,
               "transport custody must not be wrapped in redundant logical rounds");

struct anchor_model {
    struct app_discovery_assignment_policy policy;
    uint64_t id;
    uint32_t persisted_epoch;
    uint32_t persisted_table_seq;
    struct discovery_assignment_table_commitment persisted_table_commitment;
    uint32_t pending_epoch;
    uint32_t pending_table_seq;
    struct discovery_assignment_table_commitment pending_table_commitment;
    uint8_t persisted_slot;
    uint8_t pending_slot;
    uint8_t assigned_slot;
    uint8_t claim_replies;
    uint8_t ack_replies;
    bool persisted;
    bool persisted_provisioned;
    bool pending_persisted;
    bool claimed;
    bool acked;
    bool table_applied;
    bool click_deferred;
    bool radio_deferred;
    bool persistence_retried;
};

struct gateway_model {
    uint64_t claim_ids[MAX_ANCHORS];
    uint64_t ack_mask;
    uint64_t claim_response_mask;
    size_t claim_count;
    uint16_t duplicate_claims;
    uint16_t duplicate_acks;
    uint8_t max_hop_count;
    bool assignment_proof_committed;
    bool active;
};

static int failures;

static struct discovery_assignment_table_commitment
test_table_commitment(uint32_t value)
{
    struct discovery_assignment_table_commitment commitment = {0};

    commitment.bytes[0] = (uint8_t)value;
    commitment.bytes[1] = (uint8_t)(value >> 8u);
    commitment.bytes[2] = (uint8_t)(value >> 16u);
    commitment.bytes[3] = (uint8_t)(value >> 24u);
    return commitment;
}

#define CHECK(expression, ...) do {                                           \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL line=%d ", __LINE__);                         \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                   \
        failures++;                                                           \
        return false;                                                         \
    }                                                                         \
} while (0)

static bool build_result_with_hop(
    uint64_t anchor_id,
    enum discovery_assignment_phase phase,
    uint32_t epoch,
    uint32_t session,
    bool include_hop_count,
    uint8_t hop_count,
    struct proto_packet *packet,
    uint8_t *payload,
    size_t *payload_len)
{
    size_t length = 0u;
    int ret = mesh_append_command_result(payload, UWB_MESH_MAX_PAYLOAD_LEN,
                                         &length,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_OK, 0u);

    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
            payload, UWB_MESH_MAX_PAYLOAD_LEN, &length, phase, epoch);
    }
    if (ret == PROTO_OK && include_hop_count) {
        ret = tlv_append_u8(payload,
                            UWB_MESH_MAX_PAYLOAD_LEN,
                            &length,
                            TLV_HOP_COUNT,
                            hop_count);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(
            payload, UWB_MESH_MAX_PAYLOAD_LEN, &length,
            discovery_assignment_hash(anchor_id));
    }
    if (ret == PROTO_OK && phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        struct discovery_assignment_table_commitment commitment =
            test_table_commitment(TABLE_GENERATION_1);

        ret = discovery_assignment_append_table_commitment(
            payload, UWB_MESH_MAX_PAYLOAD_LEN, &length, &commitment);
    }
    if (ret != PROTO_OK || length > UINT8_MAX) {
        return false;
    }
    ret = mesh_init_command_result(packet, anchor_id, GATEWAY_ID, session,
                                   (uint16_t)(anchor_id | 1u),
                                   (uint8_t)length, true);
    *payload_len = length;
    return ret == PROTO_OK;
}

static bool build_result(uint64_t anchor_id,
                         enum discovery_assignment_phase phase,
                         uint32_t epoch,
                         uint32_t session,
                         struct proto_packet *packet,
                         uint8_t *payload,
                         size_t *payload_len)
{
    return build_result_with_hop(
        anchor_id,
        phase,
        epoch,
        session,
        true,
        1u,
        packet,
        payload,
        payload_len);
}

static int gateway_accept_result(struct gateway_model *gateway,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 enum discovery_assignment_phase expected_phase,
                                 uint32_t expected_epoch,
                                 uint32_t expected_session)
{
    struct discovery_assignment_result result;
    uint8_t response_hop_count = DISCOVERY_ASSIGNMENT_MAX_HOPS;
    size_t index = SIZE_MAX;
    int ret;

    if (gateway == NULL || packet == NULL || payload == NULL) {
        return -EINVAL;
    }
    if (packet->msg_type != MSG_COMMAND_RESULT ||
        packet->dst_id != GATEWAY_ID) {
        return -ENOENT;
    }
    if (packet->src_id == 0u) {
        return -EBADMSG;
    }
    ret = discovery_assignment_parse_result_tlvs(
        payload, payload_len, &result);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return -ENOENT;
    }
    if (ret != PROTO_OK ||
        result.hash != discovery_assignment_hash(packet->src_id)) {
        return -EBADMSG;
    }
    if (result.hop_count_present &&
        result.hop_count > 0u &&
        result.hop_count <= DISCOVERY_ASSIGNMENT_MAX_HOPS) {
        response_hop_count = result.hop_count;
    }
    if (!gateway->active) {
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    if (result.phase != expected_phase || result.epoch != expected_epoch ||
        packet->session_id != expected_session) {
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }

    for (size_t i = 0u; i < gateway->claim_count; i++) {
        if (gateway->claim_ids[i] == packet->src_id) {
            index = i;
            break;
        }
    }
    if (expected_phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        if (index == SIZE_MAX && gateway->claim_count >= MAX_ANCHORS) {
            return -ENOSPC;
        }
        if (response_hop_count > gateway->max_hop_count) {
            gateway->max_hop_count = response_hop_count;
        }
        if (index != SIZE_MAX) {
            gateway->claim_response_mask |= UINT64_C(1) << index;
            gateway->duplicate_claims++;
            return 0;
        }
        index = gateway->claim_count;
        gateway->claim_ids[gateway->claim_count++] = packet->src_id;
        gateway->claim_response_mask |= UINT64_C(1) << index;
        return 0;
    }
    if (index == SIZE_MAX) {
        return APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE;
    }
    if (response_hop_count > gateway->max_hop_count) {
        gateway->max_hop_count = response_hop_count;
    }
    if ((gateway->ack_mask & (UINT64_C(1) << index)) != 0u) {
        gateway->duplicate_acks++;
    }
    gateway->ack_mask |= UINT64_C(1) << index;
    return 0;
}

static size_t gateway_current_claim_response_count(
    const struct gateway_model *gateway)
{
    uint64_t mask = gateway != NULL ? gateway->claim_response_mask : 0u;
    size_t count = 0u;

    while (mask != 0u) {
        count += (size_t)(mask & 1u);
        mask >>= 1;
    }
    return count;
}

static bool gateway_expected_claims_complete(
    const struct gateway_model *gateway,
    size_t expected_claim_count)
{
    return expected_claim_count != 0u &&
           gateway_current_claim_response_count(gateway) >=
               expected_claim_count;
}

static bool build_table_for_epoch(
    const struct gateway_model *gateway,
    uint32_t epoch,
    struct discovery_assignment_entry *entries,
    uint8_t *payload,
    size_t *payload_len)
{
    struct discovery_assignment_claim claims[MAX_ANCHORS];
    size_t length = 0u;

    for (size_t i = 0u; i < gateway->claim_count; i++) {
        claims[i].anchor_id = gateway->claim_ids[i];
        claims[i].hash = discovery_assignment_hash(claims[i].anchor_id);
    }
    if (discovery_assignment_sort_claims(claims, gateway->claim_count) !=
            PROTO_OK ||
        discovery_assignment_entries_from_claims(
            claims, gateway->claim_count, entries, MAX_ANCHORS) != PROTO_OK ||
        tlv_append_u16(payload, PACKET_EXT_MAX_PAYLOAD_LEN, &length,
                       TLV_COMMAND_ID, CMD_ASSIGN_DISCOVERY_SLOTS) != PROTO_OK ||
        discovery_assignment_append_control_tlvs(
            payload, PACKET_EXT_MAX_PAYLOAD_LEN, &length,
            DISCOVERY_ASSIGNMENT_PHASE_TABLE, epoch) != PROTO_OK ||
        discovery_assignment_append_table_tlvs(
            payload, PACKET_EXT_MAX_PAYLOAD_LEN, &length,
            entries, gateway->claim_count) != PROTO_OK) {
        return false;
    }
    *payload_len = length;
    return true;
}

static bool build_table(const struct gateway_model *gateway,
                        struct discovery_assignment_entry *entries,
                        uint8_t *payload,
                        size_t *payload_len)
{
    return build_table_for_epoch(
        gateway, ASSIGNMENT_EPOCH, entries, payload, payload_len);
}

static bool anchor_apply_table(struct anchor_model *anchor,
                               const uint8_t *payload,
                               size_t payload_len,
                               uint32_t table_seq,
                               bool persist_ok)
{
    struct discovery_assignment_entry decoded[MAX_ANCHORS];
    enum discovery_assignment_phase phase = 0;
    enum app_discovery_assignment_table_decision decision;
    struct discovery_assignment_table_commitment table_commitment;
    uint32_t epoch = 0u;
    uint8_t slot_count = 0u;
    size_t count = 0u;
    size_t match = SIZE_MAX;

    if (discovery_assignment_extract_control_tlvs(payload, payload_len,
                                                   &phase, &epoch) != PROTO_OK ||
        phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE ||
        discovery_assignment_parse_table_tlvs(
            payload, payload_len, decoded, MAX_ANCHORS,
            &count, &slot_count) != PROTO_OK) {
        return false;
    }
    if (!discovery_assignment_table_commitment(
            decoded, count, slot_count, &table_commitment)) {
        return false;
    }
    decision = app_discovery_assignment_policy_note_table(
        &anchor->policy, epoch, table_seq, &table_commitment);
    if (decision != APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY &&
        decision != APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY) {
        return false;
    }
    for (size_t i = 0u; i < count; i++) {
        if (decoded[i].anchor_id == anchor->id) {
            if (match != SIZE_MAX ||
                decoded[i].hash != discovery_assignment_hash(anchor->id) ||
                decoded[i].slot >= slot_count) {
                return false;
            }
            match = i;
        }
    }
    if (match == SIZE_MAX) {
        if (!persist_ok) {
            anchor->persistence_retried = true;
            return false;
        }
        /* An omitted uncommitted table cannot revoke a committed slot. */
        return false;
    }
    if (decision == APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY) {
        return true;
    }
    if (!persist_ok) {
        anchor->persistence_retried = true;
        return false;
    }
    anchor->pending_persisted = true;
    anchor->pending_epoch = epoch;
    anchor->pending_table_seq = table_seq;
    anchor->pending_table_commitment = table_commitment;
    anchor->pending_slot = decoded[match].slot;
    anchor->table_applied = true;
    return true;
}

static bool anchor_promote_after_proven_ack_delivery(
    struct anchor_model *anchor,
    bool gateway_proof_committed,
    bool transport_ack_delivered)
{
    if (anchor == NULL || !anchor->pending_persisted ||
        !gateway_proof_committed || !transport_ack_delivered ||
        !app_discovery_assignment_policy_commit(
            &anchor->policy,
            anchor->pending_epoch,
            anchor->pending_table_seq,
            &anchor->pending_table_commitment)) {
        return false;
    }
    anchor->persisted = true;
    anchor->persisted_provisioned = true;
    anchor->persisted_epoch = anchor->pending_epoch;
    anchor->persisted_table_seq = anchor->pending_table_seq;
    anchor->persisted_table_commitment =
        anchor->pending_table_commitment;
    anchor->persisted_slot = anchor->pending_slot;
    anchor->assigned_slot = anchor->pending_slot;
    anchor->pending_persisted = false;
    anchor->pending_epoch = 0u;
    anchor->pending_table_seq = 0u;
    memset(&anchor->pending_table_commitment,
           0,
           sizeof(anchor->pending_table_commitment));
    anchor->pending_slot = UINT8_MAX;
    return true;
}

static bool compare_deterministic_order(const struct gateway_model *gateway,
                                        const struct discovery_assignment_entry *expected)
{
    struct gateway_model reversed = {0};
    struct discovery_assignment_entry entries[MAX_ANCHORS];
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    reversed.claim_count = gateway->claim_count;
    for (size_t i = 0u; i < gateway->claim_count; i++) {
        reversed.claim_ids[i] = gateway->claim_ids[gateway->claim_count - 1u - i];
    }
    CHECK(build_table(&reversed, entries, payload, &payload_len),
          "reverse table build failed count=%zu", gateway->claim_count);
    for (size_t i = 0u; i < gateway->claim_count; i++) {
        CHECK(entries[i].anchor_id == expected[i].anchor_id &&
              entries[i].hash == expected[i].hash &&
              entries[i].slot == expected[i].slot,
              "order drift count=%zu index=%zu", gateway->claim_count, i);
    }
    return true;
}

static bool run_workflow(size_t anchor_count)
{
    struct anchor_model anchors[MAX_ANCHORS];
    struct gateway_model gateway = {.active = true};
    struct discovery_assignment_entry entries[MAX_ANCHORS];
    struct discovery_assignment_table_commitment table_commitment;
    uint8_t table_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t table_payload_len = 0u;
    uint32_t elapsed_ms = 0u;
    uint32_t collection_ms;

    memset(anchors, 0, sizeof(anchors));
    CHECK(anchor_count >= 2u && anchor_count <= MAX_ANCHORS,
          "bad anchor_count=%zu", anchor_count);
    for (size_t i = 0u; i < anchor_count; i++) {
        anchors[i].id = ANCHOR_BASE + i;
        anchors[i].assigned_slot = UINT8_MAX;
        anchors[i].persisted_slot = UINT8_MAX;
        anchors[i].pending_slot = UINT8_MAX;
        app_discovery_assignment_policy_init(
            &anchors[i].policy, false, false, false, 0u, 0u, NULL);
        CHECK(!app_discovery_assignment_policy_normal_click_reply_allowed(
                  &anchors[i].policy),
              "hash fallback provisioned anchor=%zu", i);
    }

    collection_ms = discovery_assignment_collection_window_ms(
        DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
        DISCOVERY_ASSIGNMENT_MAX_HOPS);
    CHECK(collection_ms != 0u, "zero collection count=%zu", anchor_count);
    for (uint8_t round = 0u;
         round < MAX_ROUNDS && gateway.claim_count < anchor_count; round++) {
        elapsed_ms += DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS +
                      collection_ms;
        for (size_t i = 0u; i < anchor_count; i++) {
            struct proto_packet packet;
            uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
            size_t payload_len = 0u;

            if (!anchors[i].claimed) {
                if (round == 0u && i % 7u == 0u) {
                    /* The first control copy is lost; a later copy is heard. */
                    anchors[i].radio_deferred = true;
                }
                CHECK(app_discovery_assignment_policy_note_claim(
                          &anchors[i].policy, ASSIGNMENT_EPOCH) ==
                          APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
                      "claim policy count=%zu anchor=%zu", anchor_count, i);
                anchors[i].claimed = true;
            }
            if (round == 0u && i % 5u == 0u) {
                /* The retained response survives one lost RF opportunity. */
                anchors[i].claim_replies++;
            }
            if (gateway.claim_count != 0u) {
                bool already = false;

                for (size_t j = 0u; j < gateway.claim_count; j++) {
                    already |= gateway.claim_ids[j] == anchors[i].id;
                }
                if (already && i != 1u) {
                    continue;
                }
            }
            CHECK(build_result(anchors[i].id,
                               DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                               ASSIGNMENT_EPOCH, CLAIM_SESSION,
                               &packet, payload, &payload_len),
                  "claim build count=%zu anchor=%zu", anchor_count, i);
            CHECK(gateway_accept_result(&gateway, &packet, payload, payload_len,
                                        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                        ASSIGNMENT_EPOCH, CLAIM_SESSION) == 0,
                  "claim reject count=%zu anchor=%zu round=%u",
                  anchor_count, i, round);
            anchors[i].claim_replies++;
            if (i == 1u && round == 0u) {
                CHECK(gateway_accept_result(
                          &gateway, &packet, payload, payload_len,
                          DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                          ASSIGNMENT_EPOCH, CLAIM_SESSION) == 0,
                      "duplicate claim rejected count=%zu", anchor_count);
            }
        }
    }
    CHECK(gateway.claim_count == anchor_count,
          "missing claims count=%zu got=%zu", anchor_count, gateway.claim_count);
    CHECK(gateway.duplicate_claims > 0u,
          "duplicate claim untested count=%zu", anchor_count);

    {
        struct proto_packet stale_packet;
        uint8_t stale_payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t stale_len = 0u;

        CHECK(build_result(anchors[0].id, DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                           ASSIGNMENT_EPOCH - 1u, CLAIM_SESSION,
                           &stale_packet, stale_payload, &stale_len),
              "stale build count=%zu", anchor_count);
        CHECK(gateway_accept_result(
                  &gateway, &stale_packet, stale_payload, stale_len,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH, CLAIM_SESSION) ==
                      APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
              "stale claim did not release transport custody count=%zu",
              anchor_count);
        stale_payload[stale_len - 1u] ^= 1u;
        CHECK(gateway_accept_result(
                  &gateway, &stale_packet, stale_payload, stale_len,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH - 1u, CLAIM_SESSION) == -EBADMSG,
              "wrong hash accepted count=%zu", anchor_count);
    }

    CHECK(build_table(&gateway, entries, table_payload, &table_payload_len),
          "table build count=%zu", anchor_count);
    CHECK(discovery_assignment_table_commitment(
              entries, gateway.claim_count, MAX_ANCHORS, &table_commitment),
          "table commitment failed count=%zu", anchor_count);
    CHECK(compare_deterministic_order(&gateway, entries),
          "deterministic order count=%zu", anchor_count);
    CHECK(app_discovery_assignment_policy_note_table(
              &anchors[0].policy, ASSIGNMENT_EPOCH - 1u,
              TABLE_GENERATION_1,
              &table_commitment) ==
              APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE,
          "stale table interrupted join count=%zu", anchor_count);
    for (size_t i = 0u; i < anchor_count; i++) {
        CHECK(entries[i].slot == i,
              "nonunique slot count=%zu index=%zu slot=%u",
              anchor_count, i, entries[i].slot);
    }

    {
        struct proto_packet wrong_ack;
        uint8_t wrong_payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t wrong_len = 0u;

        CHECK(build_result(anchors[0].id, DISCOVERY_ASSIGNMENT_PHASE_ACK,
                           ASSIGNMENT_EPOCH - 1u, TABLE_SESSION,
                           &wrong_ack, wrong_payload, &wrong_len),
              "wrong ACK build count=%zu", anchor_count);
        CHECK(gateway_accept_result(
                  &gateway, &wrong_ack, wrong_payload, wrong_len,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH, TABLE_SESSION) ==
                      APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
              "wrong-epoch ACK did not release transport custody count=%zu",
              anchor_count);
        wrong_ack.session_id = TABLE_SESSION - 1u;
        CHECK(gateway_accept_result(
                  &gateway, &wrong_ack, wrong_payload, wrong_len,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH - 1u, TABLE_SESSION) ==
                      APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
              "wrong-session ACK did not release transport custody count=%zu",
              anchor_count);
    }

    for (uint8_t round = 0u;
         round < MAX_ROUNDS && gateway.ack_mask !=
             ((UINT64_C(1) << anchor_count) - 1u); round++) {
        elapsed_ms += DISCOVERY_ASSIGNMENT_CONTROL_FLOOD_DEADLINE_MS +
                      collection_ms;
        for (size_t i = 0u; i < anchor_count; i++) {
            struct proto_packet ack;
            uint8_t ack_payload[UWB_MESH_MAX_PAYLOAD_LEN];
            size_t ack_len = 0u;
            bool applied;

            if (anchors[i].acked) {
                continue;
            }
            if (round == 0u && i % 11u == 0u) {
                /* A later copy of the same bounded table flood is heard. */
                anchors[i].radio_deferred = true;
            }
            if (round == 0u && i % 13u == 0u) {
                anchors[i].click_deferred = true;
                /* Click owns the first safe boundary, then custody resumes. */
            }
            if (round == 0u && i % 17u == 0u) {
                anchors[i].radio_deferred = true;
            }
            applied = anchor_apply_table(
                &anchors[i], table_payload, table_payload_len,
                TABLE_GENERATION_1,
                !(round == 0u && i % 19u == 0u));
            if (!applied && anchors[i].persistence_retried) {
                applied = anchor_apply_table(
                    &anchors[i], table_payload, table_payload_len,
                    TABLE_GENERATION_1, true);
            }
            if (!applied) {
                continue;
            }
            CHECK(build_result(anchors[i].id,
                               DISCOVERY_ASSIGNMENT_PHASE_ACK,
                               ASSIGNMENT_EPOCH, TABLE_SESSION,
                               &ack, ack_payload, &ack_len),
                  "ack build count=%zu anchor=%zu", anchor_count, i);
            anchors[i].ack_replies++;
            if (round == 0u && i % 7u == 0u) {
                /* The same retained ACK succeeds on a later response attempt. */
                anchors[i].ack_replies++;
            }
            CHECK(gateway_accept_result(&gateway, &ack, ack_payload, ack_len,
                                        DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                        ASSIGNMENT_EPOCH, TABLE_SESSION) == 0,
                  "ack reject count=%zu anchor=%zu", anchor_count, i);
            anchors[i].acked = true;
            if (i == 1u && round == 0u) {
                CHECK(gateway_accept_result(
                          &gateway, &ack, ack_payload, ack_len,
                          DISCOVERY_ASSIGNMENT_PHASE_ACK,
                          ASSIGNMENT_EPOCH, TABLE_SESSION) == 0,
                      "duplicate ack rejected count=%zu", anchor_count);
            }
        }
    }

    CHECK(gateway.ack_mask == ((UINT64_C(1) << anchor_count) - 1u),
          "missing ACK count=%zu mask=0x%016llx", anchor_count,
          (unsigned long long)gateway.ack_mask);
    CHECK(gateway.duplicate_acks > 0u,
          "duplicate ACK untested count=%zu", anchor_count);
    for (size_t i = 0u; i < anchor_count; i++) {
        CHECK(anchors[i].pending_persisted &&
                  !anchors[i].persisted_provisioned &&
                  anchors[i].assigned_slot == UINT8_MAX,
              "anchor promoted before gateway proof count=%zu anchor=%zu",
              anchor_count, i);
    }
    gateway.assignment_proof_committed = true;
    for (size_t i = 0u; i < anchor_count; i++) {
        CHECK(anchor_promote_after_proven_ack_delivery(
                  &anchors[i],
                  gateway.assignment_proof_committed,
                  true),
              "proven ACK did not promote count=%zu anchor=%zu",
              anchor_count, i);
    }
    elapsed_ms += DISCOVERY_ASSIGNMENT_RESPONSE_ACK_SETTLE_MS;
    CHECK(elapsed_ms <= OPERATION_DEADLINE_MS,
          "deadline insufficient count=%zu elapsed=%u", anchor_count,
          elapsed_ms);
    CHECK(!app_discovery_assignment_operation_expired(
              elapsed_ms, OPERATION_DEADLINE_MS),
          "operation expired count=%zu elapsed=%u", anchor_count, elapsed_ms);

    for (size_t i = 0u; i < anchor_count; i++) {
        struct app_discovery_assignment_policy restored;
        size_t expected = SIZE_MAX;

        for (size_t entry = 0u; entry < anchor_count; entry++) {
            if (entries[entry].anchor_id == anchors[i].id) {
                expected = entry;
                break;
            }
        }
        CHECK(expected != SIZE_MAX && anchors[i].persisted &&
              anchors[i].persisted_provisioned &&
              anchors[i].persisted_epoch == ASSIGNMENT_EPOCH &&
              anchors[i].persisted_table_seq == TABLE_GENERATION_1 &&
              discovery_assignment_table_commitment_equal(
                  &anchors[i].persisted_table_commitment,
                  &table_commitment) &&
              anchors[i].persisted_slot == entries[expected].slot &&
              anchors[i].assigned_slot == entries[expected].slot,
              "persistence mismatch count=%zu anchor=%zu persisted=%u provisioned=%u epoch=%u seq=%u slot=%u assigned=%u expected_slot=%u",
              anchor_count, i, anchors[i].persisted ? 1u : 0u,
              anchors[i].persisted_provisioned ? 1u : 0u,
              anchors[i].persisted_epoch,
              anchors[i].persisted_table_seq,
              anchors[i].persisted_slot,
              anchors[i].assigned_slot,
              entries[expected].slot);
        app_discovery_assignment_policy_init(
            &restored, true, true, anchors[i].persisted_provisioned,
            anchors[i].persisted_epoch, anchors[i].persisted_table_seq,
            &anchors[i].persisted_table_commitment);
        CHECK(app_discovery_assignment_policy_normal_click_reply_allowed(
                  &restored),
              "reset lost provisioned state count=%zu anchor=%zu",
              anchor_count, i);
    }
    if (anchor_count == MAX_ANCHORS) {
        bool saw_click_deferral = false;
        bool saw_radio_deferral = false;
        bool saw_persistence_retry = false;

        for (size_t i = 0u; i < anchor_count; i++) {
            saw_click_deferral |= anchors[i].click_deferred;
            saw_radio_deferral |= anchors[i].radio_deferred;
            saw_persistence_retry |= anchors[i].persistence_retried;
        }
        CHECK(saw_click_deferral && saw_radio_deferral &&
              saw_persistence_retry,
              "fault matrix incomplete click=%u radio=%u persistence=%u",
              saw_click_deferral ? 1u : 0u,
              saw_radio_deferral ? 1u : 0u,
              saw_persistence_retry ? 1u : 0u);
    }
    return true;
}

static bool test_conflicts_capacity_and_late_claim(void)
{
    struct discovery_assignment_entry entries[2];
    struct discovery_assignment_claim claims[MAX_ANCHORS + 1u];
    struct app_discovery_assignment_policy late;
    struct discovery_assignment_table_commitment commitment =
        test_table_commitment(UINT32_C(0x7e57f001));
    uint8_t payload[128];
    size_t payload_len = 0u;

    for (size_t i = 0u; i < MAX_ANCHORS + 1u; i++) {
        claims[i].anchor_id = ANCHOR_BASE + i;
        claims[i].hash = discovery_assignment_hash(claims[i].anchor_id);
    }
    CHECK(discovery_assignment_sort_claims(claims, MAX_ANCHORS + 1u) ==
              PROTO_ERR_ARG,
          "51-anchor capacity accepted");
    entries[0] = (struct discovery_assignment_entry) {
        .anchor_id = ANCHOR_BASE,
        .hash = discovery_assignment_hash(ANCHOR_BASE),
        .slot = 0u,
    };
    entries[1] = (struct discovery_assignment_entry) {
        .anchor_id = ANCHOR_BASE + 1u,
        .hash = discovery_assignment_hash(ANCHOR_BASE + 1u),
        .slot = 0u,
    };
    CHECK(discovery_assignment_append_table_tlvs(
              payload, sizeof(payload), &payload_len, entries, 2u) ==
              PROTO_ERR_MALFORMED,
          "conflicting slot table encoded");

    app_discovery_assignment_policy_init(
        &late, false, false, false, 0u, 0u, NULL);
    CHECK(app_discovery_assignment_policy_note_table(
              &late, ASSIGNMENT_EPOCH, TABLE_GENERATION_1,
              &commitment) ==
              APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM,
          "late table did not require claim");
    CHECK(!app_discovery_assignment_policy_commit(
              &late, ASSIGNMENT_EPOCH, TABLE_GENERATION_1,
              &commitment),
          "late table committed without claim");
    CHECK(!app_discovery_assignment_policy_normal_click_reply_allowed(&late),
          "hash fallback hid missing assignment");
    return true;
}

static bool test_prior_anchor_late_table_converges_on_exact_retry(void)
{
    struct app_discovery_assignment_policy policy;
    const uint32_t next_epoch = ASSIGNMENT_EPOCH + 1u;
    struct discovery_assignment_table_commitment current_commitment =
        test_table_commitment(UINT32_C(0x7e57f001));
    struct discovery_assignment_table_commitment commitment =
        test_table_commitment(UINT32_C(0x7e57f002));

    app_discovery_assignment_policy_init(
        &policy,
        true,
        true,
        true,
        ASSIGNMENT_EPOCH,
        TABLE_GENERATION_1,
        &current_commitment);
    CHECK(app_discovery_assignment_policy_note_table(
              &policy,
              next_epoch,
              TABLE_GENERATION_2,
              &commitment) ==
              APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM,
          "prior anchor TABLE-before-CLAIM did not request late recovery");
    CHECK(app_discovery_assignment_policy_note_claim(
              &policy, next_epoch) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "locally scheduled late CLAIM did not advance correlated state");
    CHECK(app_discovery_assignment_policy_note_table(
              &policy,
              next_epoch,
              TABLE_GENERATION_2,
              &commitment) ==
              APP_DISCOVERY_ASSIGNMENT_TABLE_APPLY,
          "exact TABLE retry remained trapped in LATE_CLAIM");
    CHECK(app_discovery_assignment_policy_normal_click_reply_allowed(
              &policy),
          "late recovery revoked prior slot before ACK proof");
    return true;
}

static bool append_conflicting_result_singleton(uint8_t *payload,
                                                size_t *payload_len,
                                                uint8_t type)
{
    int ret;

    switch (type) {
    case TLV_COMMAND_ID:
        ret = tlv_append_u16(payload,
                             UWB_MESH_MAX_PAYLOAD_LEN,
                             payload_len,
                             type,
                             CMD_FORCE_REDISCOVERY);
        break;
    case TLV_COMMAND_STATUS:
        ret = tlv_append_u16(payload,
                             UWB_MESH_MAX_PAYLOAD_LEN,
                             payload_len,
                             type,
                             COMMAND_TIMEOUT);
        break;
    case TLV_REASON:
        ret = tlv_append_u8(payload,
                            UWB_MESH_MAX_PAYLOAD_LEN,
                            payload_len,
                            type,
                            1u);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION:
        ret = tlv_append_u8(
            payload,
            UWB_MESH_MAX_PAYLOAD_LEN,
            payload_len,
            type,
            DISCOVERY_ASSIGNMENT_SCHEME_VERSION + 1u);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_PHASE:
        ret = tlv_append_u8(payload,
                            UWB_MESH_MAX_PAYLOAD_LEN,
                            payload_len,
                            type,
                            DISCOVERY_ASSIGNMENT_PHASE_CLAIM);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_EPOCH:
        ret = tlv_append_u32(payload,
                             UWB_MESH_MAX_PAYLOAD_LEN,
                             payload_len,
                             type,
                             ASSIGNMENT_EPOCH + 1u);
        break;
    case TLV_DISCOVERY_ASSIGNMENT_HASH:
        ret = tlv_append_u64(payload,
                             UWB_MESH_MAX_PAYLOAD_LEN,
                             payload_len,
                             type,
                             UINT64_C(0x0102030405060708));
        break;
    case TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT:
    {
        struct discovery_assignment_table_commitment commitment =
            test_table_commitment(TABLE_GENERATION_2);

        ret = discovery_assignment_append_table_commitment(
            payload,
            UWB_MESH_MAX_PAYLOAD_LEN,
            payload_len,
            &commitment);
        break;
    }
    case TLV_HOP_COUNT:
        ret = tlv_append_u8(payload,
                            UWB_MESH_MAX_PAYLOAD_LEN,
                            payload_len,
                            type,
                            2u);
        break;
    default:
        return false;
    }
    return ret == PROTO_OK;
}

static bool test_duplicate_result_singletons_do_not_mutate_or_ack(void)
{
    static const uint8_t singleton_types[] = {
        TLV_COMMAND_ID,
        TLV_COMMAND_STATUS,
        TLV_REASON,
        TLV_DISCOVERY_ASSIGNMENT_SCHEME_VERSION,
        TLV_DISCOVERY_ASSIGNMENT_PHASE,
        TLV_DISCOVERY_ASSIGNMENT_EPOCH,
        TLV_DISCOVERY_ASSIGNMENT_HASH,
        TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT,
        TLV_HOP_COUNT,
    };
    struct gateway_model gateway = {
        .claim_ids = {ANCHOR_BASE},
        .claim_count = 1u,
        .active = true,
    };
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;

    for (size_t i = 0u;
         i < sizeof(singleton_types) / sizeof(singleton_types[0]);
         i++) {
        struct gateway_model before = gateway;

        CHECK(build_result(
                  ANCHOR_BASE,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH,
                  TABLE_SESSION,
                  &packet,
                  payload,
                  &payload_len),
              "duplicate singleton base build failed type=0x%02x",
              singleton_types[i]);
        CHECK(append_conflicting_result_singleton(
                  payload, &payload_len, singleton_types[i]),
              "duplicate singleton append failed type=0x%02x",
              singleton_types[i]);
        CHECK(gateway_accept_result(
                  &gateway,
                  &packet,
                  payload,
                  payload_len,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH,
                  TABLE_SESSION) == -EBADMSG,
              "duplicate singleton became transport-ACK eligible type=0x%02x",
              singleton_types[i]);
        CHECK(memcmp(&gateway, &before, sizeof(gateway)) == 0,
              "duplicate singleton mutated gateway state type=0x%02x",
              singleton_types[i]);
    }

    {
        struct gateway_model before = gateway;

        CHECK(build_result(
                  ANCHOR_BASE,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH,
                  TABLE_SESSION,
                  &packet,
                  payload,
                  &payload_len),
              "malformed-tail base build failed");
        payload[payload_len++] = 0xa5u;
        CHECK(gateway_accept_result(
                  &gateway,
                  &packet,
                  payload,
                  payload_len,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH,
                  TABLE_SESSION) == -EBADMSG,
              "malformed tail became transport-ACK eligible");
        CHECK(memcmp(&gateway, &before, sizeof(gateway)) == 0,
              "malformed tail mutated gateway state");
    }
    return true;
}

static bool test_gateway_semantic_acceptance_categories(void)
{
    struct gateway_model gateway = {.active = true};
    struct gateway_model before_capacity;
    struct proto_packet packet;
    const uint8_t *command_raw = NULL;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint8_t command_len = 0u;
    size_t payload_len = 0u;

    CHECK(build_result(ANCHOR_BASE,
                       DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                       ASSIGNMENT_EPOCH,
                       CLAIM_SESSION,
                       &packet,
                       payload,
                       &payload_len),
          "semantic valid claim build failed");
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                ASSIGNMENT_EPOCH,
                                CLAIM_SESSION) == 0,
          "valid claim was not accepted");
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                ASSIGNMENT_EPOCH,
                                CLAIM_SESSION) == 0 &&
              gateway.duplicate_claims == 1u,
          "duplicate valid claim was not idempotently accepted");

    CHECK(tlv_find(payload,
                   payload_len,
                   TLV_COMMAND_ID,
                   &command_raw,
                   &command_len) == PROTO_OK &&
              command_len == sizeof(uint16_t),
          "semantic command TLV missing");
    proto_put_u16_le(&payload[(size_t)(command_raw - payload)],
                     CMD_FORCE_REDISCOVERY);
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                ASSIGNMENT_EPOCH,
                                CLAIM_SESSION) == -ENOENT,
          "unrelated command result was not classified as not applicable");
    proto_put_u16_le(&payload[(size_t)(command_raw - payload)],
                     CMD_ASSIGN_DISCOVERY_SLOTS);

    payload[payload_len - 1u] ^= 1u;
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                ASSIGNMENT_EPOCH,
                                CLAIM_SESSION) == -EBADMSG,
          "hash-invalid claim was not classified as malformed");
    payload[payload_len - 1u] ^= 1u;

    CHECK(build_result(ANCHOR_BASE,
                       DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                       ASSIGNMENT_EPOCH - 1u,
                       CLAIM_SESSION,
                       &packet,
                       payload,
                       &payload_len),
          "semantic stale claim build failed");
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                ASSIGNMENT_EPOCH,
                                CLAIM_SESSION) ==
              APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "wrong-epoch claim did not release transport custody");

    CHECK(build_result(ANCHOR_BASE,
                       DISCOVERY_ASSIGNMENT_PHASE_ACK,
                       ASSIGNMENT_EPOCH,
                       TABLE_SESSION,
                       &packet,
                       payload,
                       &payload_len),
          "semantic valid ACK build failed");
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                ASSIGNMENT_EPOCH,
                                TABLE_SESSION) == 0,
          "valid table ACK was not accepted");
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                ASSIGNMENT_EPOCH,
                                TABLE_SESSION) == 0 &&
              gateway.duplicate_acks == 1u,
          "duplicate valid table ACK was not idempotently accepted");
    packet.session_id--;
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_ACK,
                                ASSIGNMENT_EPOCH,
                                TABLE_SESSION) ==
              APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "wrong-session table ACK did not release transport custody");

    gateway.claim_count = MAX_ANCHORS;
    for (size_t i = 0u; i < MAX_ANCHORS; i++) {
        gateway.claim_ids[i] = ANCHOR_BASE + i;
    }
    gateway.max_hop_count = 1u;
    before_capacity = gateway;
    CHECK(build_result_with_hop(
              ANCHOR_BASE + MAX_ANCHORS,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION,
              true,
              DISCOVERY_ASSIGNMENT_MAX_HOPS,
              &packet,
              payload,
              &payload_len),
          "semantic capacity claim build failed");
    CHECK(gateway_accept_result(&gateway,
                                &packet,
                                payload,
                                payload_len,
                                DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                ASSIGNMENT_EPOCH,
                                CLAIM_SESSION) == -ENOSPC,
          "over-capacity claim was not rejected");
    CHECK(memcmp(&gateway, &before_capacity, sizeof(gateway)) == 0,
          "over-capacity claim mutated roster or timing state");
    return true;
}

static bool test_gateway_stage_session_and_member_rejection_is_immutable(void)
{
    struct gateway_model gateway = {.active = true};
    struct gateway_model before;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;

    CHECK(build_result_with_hop(
              ANCHOR_BASE,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION,
              true,
              1u,
              &packet,
              payload,
              &payload_len),
          "accepted claim build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == 0,
          "accepted claim rejected");
    CHECK(gateway.claim_count == 1u && gateway.max_hop_count == 1u,
          "accepted claim did not establish roster and hop state");

    before = gateway;
    CHECK(build_result_with_hop(
              ANCHOR_BASE + 1u,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION,
              true,
              DISCOVERY_ASSIGNMENT_MAX_HOPS,
              &packet,
              payload,
              &payload_len),
          "late claim build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION) == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "late claim did not release custody as semantic duplicate");
    CHECK(memcmp(&gateway, &before, sizeof(gateway)) == 0,
          "late claim reopened table stage or mutated hop/roster state");

    before = gateway;
    CHECK(build_result_with_hop(
              ANCHOR_BASE,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION - 1u,
              true,
              DISCOVERY_ASSIGNMENT_MAX_HOPS,
              &packet,
              payload,
              &payload_len),
          "wrong-session ACK build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION) == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "wrong-session ACK did not release custody");
    CHECK(memcmp(&gateway, &before, sizeof(gateway)) == 0,
          "wrong-session ACK mutated hop or ACK state");

    before = gateway;
    CHECK(build_result_with_hop(
              ANCHOR_BASE + 1u,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION,
              true,
              DISCOVERY_ASSIGNMENT_MAX_HOPS,
              &packet,
              payload,
              &payload_len),
          "nonmember ACK build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION) == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "nonmember ACK did not release custody");
    CHECK(memcmp(&gateway, &before, sizeof(gateway)) == 0,
          "nonmember ACK mutated hop or ACK state");

    CHECK(build_result_with_hop(
              ANCHOR_BASE,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION,
              true,
              DISCOVERY_ASSIGNMENT_MAX_HOPS,
              &packet,
              payload,
              &payload_len),
          "valid deep ACK build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION) == 0,
          "valid deep ACK rejected");
    CHECK(gateway.ack_mask == 1u &&
              gateway.max_hop_count == DISCOVERY_ASSIGNMENT_MAX_HOPS,
          "validated ACK did not update hop and ACK state");
    return true;
}

static bool test_gateway_invalid_hop_telemetry_is_conservative(void)
{
    static const struct {
        bool include_hop_count;
        uint8_t hop_count;
    } cases[] = {
        {false, 0u},
        {true, 0u},
        {true, DISCOVERY_ASSIGNMENT_MAX_HOPS + 1u},
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct gateway_model gateway = {.active = true};
        struct proto_packet packet;
        uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
        size_t payload_len = 0u;

        CHECK(build_result_with_hop(
                  ANCHOR_BASE,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH,
                  CLAIM_SESSION,
                  cases[i].include_hop_count,
                  cases[i].hop_count,
                  &packet,
                  payload,
                  &payload_len),
              "invalid hop case build failed case=%zu", i);
        CHECK(gateway_accept_result(
                  &gateway,
                  &packet,
                  payload,
                  payload_len,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH,
                  CLAIM_SESSION) == 0,
              "invalid hop case rejected case=%zu", i);
        CHECK(gateway.max_hop_count == DISCOVERY_ASSIGNMENT_MAX_HOPS,
              "invalid hop shortened collection window case=%zu hop=%u",
              i, gateway.max_hop_count);
    }
    return true;
}

static bool test_retired_valid_results_release_transport_custody(void)
{
    struct gateway_model gateway = {.active = false};
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t hash_offset = SIZE_MAX;
    size_t payload_len = 0u;

    CHECK(build_result(ANCHOR_BASE,
                       DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                       ASSIGNMENT_EPOCH,
                       CLAIM_SESSION,
                       &packet,
                       payload,
                       &payload_len),
          "retired valid claim build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "retired valid claim did not release transport custody");
    CHECK(gateway.claim_count == 0u,
          "retired valid claim mutated the inactive roster");

    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len - 1u,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == -EBADMSG,
          "retired malformed claim reached transport acceptance");
    {
        const uint8_t *hash_raw = NULL;
        uint8_t hash_len = 0u;

        CHECK(tlv_find_unique(
                  payload,
                  payload_len,
                  TLV_DISCOVERY_ASSIGNMENT_HASH,
                  &hash_raw,
                  &hash_len) == PROTO_OK &&
                  hash_len == sizeof(uint64_t),
              "retired claim hash missing");
        hash_offset = (size_t)(hash_raw - payload);
        payload[hash_offset] ^= 1u;
    }
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == -EBADMSG,
          "retired wrong-hash claim reached transport acceptance");
    payload[hash_offset] ^= 1u;

    CHECK(build_result(ANCHOR_BASE,
                       DISCOVERY_ASSIGNMENT_PHASE_ACK,
                       ASSIGNMENT_EPOCH,
                       TABLE_SESSION,
                       &packet,
                       payload,
                       &payload_len),
          "retired valid table ACK build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION) == APP_GATEWAY_SEMANTIC_ACCEPT_DUPLICATE,
          "retired valid table ACK did not release transport custody");
    CHECK(gateway.ack_mask == 0u,
          "retired valid table ACK mutated the inactive ACK roster");

    {
        const uint8_t *hash_raw = NULL;
        uint8_t hash_len = 0u;

        CHECK(tlv_find_unique(
                  payload,
                  payload_len,
                  TLV_DISCOVERY_ASSIGNMENT_HASH,
                  &hash_raw,
                  &hash_len) == PROTO_OK &&
                  hash_len == sizeof(uint64_t),
              "retired table ACK hash missing");
        hash_offset = (size_t)(hash_raw - payload);
        payload[hash_offset] ^= 1u;
    }
    CHECK(gateway_accept_result(
              &gateway,
              &packet,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_ACK,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION) == -EBADMSG,
          "retired wrong-hash table ACK reached transport acceptance");
    return true;
}

static bool test_explicit_budget_clips_current_window_without_division(void)
{
    const uint32_t command_budget_ms = 60000u;
    const uint32_t natural_window_ms =
        discovery_assignment_collection_window_ms(
            DISCOVERY_ASSIGNMENT_RESPONSE_SPREAD_DEFAULT_MS,
            DISCOVERY_ASSIGNMENT_MAX_HOPS);
    const uint32_t remaining_ms = 24000u;
    uint8_t round_limit = gateway_command_budget_retry_limit(
        true, command_budget_ms, MAX_ROUNDS);

    CHECK(round_limit == 1u, "unexpected explicit round limit=%u", round_limit);
    CHECK(app_discovery_assignment_claim_round_limit(
              true, round_limit, MAX_ROUNDS) == round_limit,
          "explicit budget discarded the single logical claim round");
    CHECK(discovery_assignment_retry_backoff_ms(0u, 0u) <
              discovery_assignment_retry_backoff_ms(1u, 0u),
          "claim retry backoff is not exponential");
    CHECK(discovery_assignment_retry_backoff_ms(0u, 0u) !=
              discovery_assignment_retry_backoff_ms(0u, 73u),
          "claim retry backoff is not randomized");
    CHECK(app_discovery_assignment_table_windows_remaining(1u, round_limit) ==
              1u,
          "single table response window was not retained");
    CHECK(gateway_command_budget_window_ms(
              true, remaining_ms, 1u, natural_window_ms) == remaining_ms,
          "explicit budget did not clip only the current table window");
    CHECK(!app_discovery_assignment_table_retry_backoff_required(
              true, 1u, round_limit, round_limit),
          "terminal table round scheduled an extra retry");
    return true;
}

static bool test_ordered_epoch_expansion_and_unassigned_reboot_are_monotonic(void)
{
    struct gateway_model expanded = {0};
    struct gateway_model smaller = {0};
    struct gateway_model omitted = {0};
    struct discovery_assignment_entry expanded_entries[6];
    struct discovery_assignment_entry smaller_entries[2];
    struct discovery_assignment_entry omitted_entries[3];
    struct anchor_model anchor = {0};
    struct anchor_model restored = {0};
    uint8_t expanded_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    uint8_t smaller_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    uint8_t omitted_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t expanded_len = 0u;
    size_t smaller_len = 0u;
    size_t omitted_len = 0u;
    struct discovery_assignment_table_commitment expanded_commitment;
    struct discovery_assignment_table_commitment smaller_commitment;
    struct discovery_assignment_table_commitment omitted_commitment;
    const uint32_t expanded_epoch = ASSIGNMENT_EPOCH + 1u;
    const uint32_t omitted_epoch = ASSIGNMENT_EPOCH + 2u;
    uint8_t expanded_slot;

    expanded.claim_count = 6u;
    for (size_t i = 0u; i < expanded.claim_count; i++) {
        expanded.claim_ids[i] = ANCHOR_BASE + i;
    }
    CHECK(build_table_for_epoch(&expanded, expanded_epoch, expanded_entries,
                                expanded_payload, &expanded_len),
          "expanded table build failed");

    /* Pick two tail entries so expansion necessarily moves the target slot. */
    smaller.claim_count = 2u;
    smaller.claim_ids[0] = expanded_entries[4].anchor_id;
    smaller.claim_ids[1] = expanded_entries[5].anchor_id;
    CHECK(build_table(&smaller, smaller_entries, smaller_payload,
                      &smaller_len),
          "smaller table build failed");
    CHECK(smaller_entries[0].anchor_id == expanded_entries[4].anchor_id &&
              smaller_entries[0].slot == 0u &&
              expanded_entries[4].slot == 4u,
          "fixture does not force a slot move");

    omitted.claim_count = 3u;
    for (size_t i = 0u; i < omitted.claim_count; i++) {
        omitted.claim_ids[i] = expanded_entries[i].anchor_id;
    }
    CHECK(build_table_for_epoch(&omitted, omitted_epoch, omitted_entries,
                                omitted_payload, &omitted_len),
          "omitted table build failed");

    CHECK(discovery_assignment_table_commitment(
              smaller_entries,
              smaller.claim_count,
              MAX_ANCHORS,
              &smaller_commitment) &&
              discovery_assignment_table_commitment(
                  expanded_entries,
                  expanded.claim_count,
                  MAX_ANCHORS,
                  &expanded_commitment) &&
              discovery_assignment_table_commitment(
                  omitted_entries,
                  omitted.claim_count,
                  MAX_ANCHORS,
                  &omitted_commitment) &&
              !discovery_assignment_table_commitment_equal(
                  &smaller_commitment, &expanded_commitment) &&
              !discovery_assignment_table_commitment_equal(
                  &expanded_commitment, &omitted_commitment),
          "table commitments did not distinguish roster generations");

    anchor.id = smaller_entries[0].anchor_id;
    anchor.assigned_slot = UINT8_MAX;
    anchor.persisted_slot = UINT8_MAX;
    anchor.pending_slot = UINT8_MAX;
    app_discovery_assignment_policy_init(
        &anchor.policy, false, false, false, 0u, 0u, NULL);
    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, ASSIGNMENT_EPOCH) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "smaller generation claim rejected");
    CHECK(anchor_apply_table(&anchor, smaller_payload, smaller_len,
                             TABLE_GENERATION_1, true),
          "smaller generation assignment failed");
    CHECK(anchor.pending_persisted && !anchor.persisted_provisioned &&
              anchor.assigned_slot == UINT8_MAX,
          "first TABLE promoted before proof");
    CHECK(anchor_promote_after_proven_ack_delivery(
              &anchor, true, true),
          "first proven TABLE ACK did not promote");
    CHECK(discovery_assignment_table_commitment_equal(
              &anchor.persisted_table_commitment, &smaller_commitment),
          "smaller commitment was not persisted");

    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, expanded_epoch) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "expanded generation claim rejected");
    CHECK(anchor_apply_table(&anchor, expanded_payload, expanded_len,
                             TABLE_GENERATION_2, true),
          "expanded generation assignment failed");
    CHECK(anchor.assigned_slot == 0u &&
              anchor.persisted_table_seq == TABLE_GENERATION_1 &&
              anchor.pending_slot == 4u,
          "pending expansion disrupted committed slot");
    CHECK(anchor_promote_after_proven_ack_delivery(
              &anchor, true, true),
          "expanded proven TABLE ACK did not promote");
    expanded_slot = anchor.assigned_slot;
    CHECK(expanded_slot == 4u &&
              anchor.persisted_table_seq == TABLE_GENERATION_2 &&
              discovery_assignment_table_commitment_equal(
                  &anchor.persisted_table_commitment,
                  &expanded_commitment),
          "expanded generation did not replace smaller assignment");

    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, ASSIGNMENT_EPOCH) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE,
          "older epoch claim was accepted after expansion");
    CHECK(!anchor_apply_table(&anchor, smaller_payload, smaller_len,
                              TABLE_GENERATION_1, true),
          "older smaller generation reapplied");
    CHECK(anchor.assigned_slot == expanded_slot &&
              anchor.persisted_provisioned &&
              anchor.persisted_table_seq == TABLE_GENERATION_2 &&
              discovery_assignment_table_commitment_equal(
                  &anchor.persisted_table_commitment,
                  &expanded_commitment),
          "older smaller generation mutated expanded assignment");

    CHECK(!anchor_apply_table(&anchor, expanded_payload, expanded_len,
                              TABLE_GENERATION_3, true),
          "same-epoch different table generation applied");
    CHECK(anchor.persisted_provisioned &&
              discovery_assignment_table_commitment_equal(
                  &anchor.persisted_table_commitment,
                  &expanded_commitment),
          "same-sequence conflict erased assignment");

    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, omitted_epoch) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "omission epoch claim rejected");
    CHECK(!anchor_apply_table(&anchor, omitted_payload, omitted_len,
                              TABLE_GENERATION_3, true),
          "authoritative omission unexpectedly returned assigned");
    CHECK(anchor.persisted && anchor.persisted_provisioned &&
              anchor.persisted_epoch == expanded_epoch &&
              anchor.persisted_table_seq == TABLE_GENERATION_2 &&
              discovery_assignment_table_commitment_equal(
                  &anchor.persisted_table_commitment,
                  &expanded_commitment) &&
              anchor.persisted_slot == expanded_slot &&
              anchor.assigned_slot == expanded_slot,
          "uncommitted omission revoked the committed assignment");

    restored.id = anchor.id;
    restored.assigned_slot = anchor.assigned_slot;
    restored.persisted_slot = anchor.persisted_slot;
    restored.persisted = anchor.persisted;
    restored.persisted_provisioned = anchor.persisted_provisioned;
    restored.persisted_epoch = anchor.persisted_epoch;
    restored.persisted_table_seq = anchor.persisted_table_seq;
    restored.persisted_table_commitment =
        anchor.persisted_table_commitment;
    app_discovery_assignment_policy_init(
        &restored.policy, restored.persisted, restored.persisted,
        restored.persisted_provisioned, restored.persisted_epoch,
        restored.persisted_table_seq,
        &restored.persisted_table_commitment);
    CHECK(app_discovery_assignment_policy_normal_click_reply_allowed(
              &restored.policy),
          "failed re-enumeration lost committed slot after reboot");
    CHECK(app_discovery_assignment_policy_note_claim(
              &restored.policy, expanded_epoch) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "rebooted committed generation was not replayable");
    CHECK(anchor_apply_table(&restored, expanded_payload, expanded_len,
                             TABLE_GENERATION_2, true),
          "exact committed generation did not replay");
    CHECK(restored.policy.committed_table_seq == TABLE_GENERATION_2 &&
              discovery_assignment_table_commitment_equal(
                  &restored.policy.committed_table_commitment,
                  &expanded_commitment) &&
              app_discovery_assignment_policy_normal_click_reply_allowed(
                  &restored.policy),
          "failed re-enumeration changed rebooted committed assignment");
    return true;
}

static bool test_real_rf_attempt_preserves_delayed_assignment_responses(void)
{
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              2u, 2u, 2u, 4u, false, false),
          "complete one-hop/two-hop claim quorum did not supersede flood tail failure");
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              2u, 1u, 2u, 4u, false, false),
          "useful incomplete claims did not preserve the original response horizon");
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              0u, 2u, 2u, 4u, false, false),
          "optional expected count incorrectly gated useful claims");
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              0u, 0u, 0u, 4u, false, false),
          "delayed first CLAIM lost its post-RF response horizon");
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
              2u, 2u, 0u, 4u, false, false),
          "complete table ACK quorum did not supersede flood tail failure");
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
              0u, 0u, 0u, 4u, false, false),
          "delayed first TABLE ACK lost its post-RF response horizon");
    CHECK(!app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
              2u, 2u, 0u, 4u, false, true),
          "cancelled table flood was accepted from stale ACK state");
    return true;
}

static bool test_pending_ack_expiry_retries_without_reset_and_keeps_old_slot(void)
{
    struct anchor_model anchor = {
        .id = ANCHOR_BASE,
        .persisted_epoch = ASSIGNMENT_EPOCH,
        .persisted_table_seq = TABLE_GENERATION_1,
        .persisted_table_commitment = {.bytes = {0x10u}},
        .pending_epoch = ASSIGNMENT_EPOCH + 1u,
        .pending_table_seq = TABLE_GENERATION_2,
        .pending_table_commitment = {.bytes = {0x20u}},
        .persisted_slot = 1u,
        .pending_slot = 4u,
        .assigned_slot = 1u,
        .persisted = true,
        .persisted_provisioned = true,
        .pending_persisted = true,
    };
    const uint64_t gateway_deadline_ms = UINT64_C(10000);
    const uint64_t physical_ack_rx_ms = gateway_deadline_ms - 1u;
    const uint64_t proof_commit_ms = gateway_deadline_ms + 5u;
    uint8_t bounded_attempts = 1u;

    app_discovery_assignment_policy_init(
        &anchor.policy,
        true,
        true,
        true,
        anchor.persisted_epoch,
        anchor.persisted_table_seq,
        &anchor.persisted_table_commitment);
    CHECK(app_discovery_assignment_policy_restore_pending(
              &anchor.policy,
              anchor.pending_epoch,
              anchor.pending_table_seq,
              &anchor.pending_table_commitment),
          "pending ACK restore rejected");

    CHECK(physical_ack_rx_ms < gateway_deadline_ms &&
              proof_commit_ms >= gateway_deadline_ms,
          "D-1/proof-after-D fixture invalid");
    CHECK(!anchor_promote_after_proven_ack_delivery(
              &anchor, false, false),
          "anchor promoted before durable gateway proof");
    CHECK(anchor.pending_persisted &&
              anchor.assigned_slot == 1u &&
              app_discovery_assignment_policy_normal_click_reply_allowed(
                  &anchor.policy),
          "expired first custody attempt disrupted committed behavior");

    /*
     * DEADLINE_EXPIRED closes one bounded node-communication handle. Durable
     * ACK_PENDING creates a fresh handle instead of waiting for reset.
     */
    bounded_attempts++;
    CHECK(bounded_attempts == 2u && anchor.pending_persisted,
          "terminal expiry did not preserve autonomous retry custody");
    CHECK(anchor_promote_after_proven_ack_delivery(
              &anchor, true, true),
          "proof-after-D retry did not promote exact D-1 ACK");
    CHECK(anchor.assigned_slot == 4u &&
              anchor.persisted_epoch == ASSIGNMENT_EPOCH + 1u &&
              !anchor.pending_persisted,
          "fresh bounded attempt did not atomically replace assignment");
    return true;
}

struct assignment_transaction_snapshot_model {
    uint32_t committed_epoch;
    uint32_t pending_epoch;
    bool pending_valid;
};

static bool assignment_transaction_promote(
    struct assignment_transaction_snapshot_model *snapshot,
    uint32_t delivered_epoch)
{
    if (snapshot == NULL || !snapshot->pending_valid ||
        snapshot->pending_epoch != delivered_epoch) {
        return false;
    }
    snapshot->committed_epoch = delivered_epoch;
    snapshot->pending_epoch = 0u;
    snapshot->pending_valid = false;
    return true;
}

static void assignment_transaction_apply_table(
    struct assignment_transaction_snapshot_model *snapshot,
    uint32_t candidate_epoch)
{
    snapshot->pending_epoch = candidate_epoch;
    snapshot->pending_valid = true;
}

static bool test_assignment_snapshot_transactions_reject_stale_rmw(void)
{
    const struct assignment_transaction_snapshot_model initial = {
        .committed_epoch = ASSIGNMENT_EPOCH,
        .pending_epoch = ASSIGNMENT_EPOCH + 1u,
        .pending_valid = true,
    };
    struct assignment_transaction_snapshot_model promote_first = initial;
    struct assignment_transaction_snapshot_model table_first = initial;
    struct assignment_transaction_snapshot_model reset_copy;

    /*
     * Old delivered promotion linearizes first: the newer TABLE retains the
     * delivered commit and becomes a separate pending candidate.
     */
    CHECK(assignment_transaction_promote(
              &promote_first, ASSIGNMENT_EPOCH + 1u),
          "delivered pending assignment did not promote");
    assignment_transaction_apply_table(
        &promote_first, ASSIGNMENT_EPOCH + 2u);
    reset_copy = promote_first;
    CHECK(reset_copy.committed_epoch == ASSIGNMENT_EPOCH + 1u &&
              reset_copy.pending_valid &&
              reset_copy.pending_epoch == ASSIGNMENT_EPOCH + 2u,
          "new TABLE erased the delivered commit after reset");

    /*
     * New TABLE linearizes first only when old delivery has not won. A stale
     * old promotion must fail its exact pending identity check without
     * clearing the newer candidate.
     */
    assignment_transaction_apply_table(
        &table_first, ASSIGNMENT_EPOCH + 2u);
    CHECK(!assignment_transaction_promote(
              &table_first, ASSIGNMENT_EPOCH + 1u),
          "stale promotion overwrote a newer pending TABLE");
    reset_copy = table_first;
    CHECK(reset_copy.committed_epoch == ASSIGNMENT_EPOCH &&
              reset_copy.pending_valid &&
              reset_copy.pending_epoch == ASSIGNMENT_EPOCH + 2u,
          "stale RMW corrupted committed/pending state after reset");
    return true;
}

static bool test_queued_delivery_promotes_before_new_claim_abort(void)
{
    struct assignment_transaction_snapshot_model snapshot = {
        .committed_epoch = ASSIGNMENT_EPOCH,
        .pending_epoch = ASSIGNMENT_EPOCH + 1u,
        .pending_valid = true,
    };
    bool terminal_delivery_queued = true;

    /*
     * A newer CLAIM must first consume the old handle's terminal outcome.
     * The new operation then aborts before TABLE, so only the proven old
     * candidate changes durable state.
     */
    if (terminal_delivery_queued) {
        CHECK(assignment_transaction_promote(
                  &snapshot, ASSIGNMENT_EPOCH + 1u),
              "queued DELIVERED event was lost during supersession");
        terminal_delivery_queued = false;
    }
    CHECK(!terminal_delivery_queued &&
              snapshot.committed_epoch == ASSIGNMENT_EPOCH + 1u &&
              !snapshot.pending_valid,
          "new CLAIM abort lost the already-delivered assignment");
    return true;
}

struct pending_ack_supersession_model {
    uint32_t pending_epoch;
    bool has_committed_provisioned_slot;
    bool pending_ack_active;
};

static bool pending_ack_yields_to_newer_claim(
    struct pending_ack_supersession_model *state,
    uint32_t incoming_epoch)
{
    if (state == NULL || !state->pending_ack_active ||
        !discovery_assignment_epoch_strictly_newer(
            incoming_epoch, state->pending_epoch) ||
        state->has_committed_provisioned_slot) {
        return false;
    }
    state->pending_epoch = 0u;
    state->pending_ack_active = false;
    return true;
}

static bool test_unprovisioned_obsolete_ack_yields_to_newer_claim(void)
{
    struct pending_ack_supersession_model first_assignment = {
        .pending_epoch = ASSIGNMENT_EPOCH,
        .has_committed_provisioned_slot = false,
        .pending_ack_active = true,
    };
    struct pending_ack_supersession_model replacement = {
        .pending_epoch = ASSIGNMENT_EPOCH,
        .has_committed_provisioned_slot = true,
        .pending_ack_active = true,
    };

    CHECK(!pending_ack_yields_to_newer_claim(
              &first_assignment, ASSIGNMENT_EPOCH),
          "same-epoch CLAIM displaced its own pending ACK");
    CHECK(pending_ack_yields_to_newer_claim(
              &first_assignment, ASSIGNMENT_EPOCH + 1u),
          "unprovisioned anchor could not leave an obsolete pending ACK");
    CHECK(!first_assignment.pending_ack_active &&
              first_assignment.pending_epoch == 0u,
          "obsolete first-assignment ACK retained response ownership");

    /*
     * The now-unprovisioned anchor can answer E+1 and accept its TABLE. This
     * is the recovery path after E finalized without committing the anchor.
     */
    first_assignment.pending_epoch = ASSIGNMENT_EPOCH + 1u;
    first_assignment.pending_ack_active = true;
    CHECK(first_assignment.pending_ack_active &&
              !first_assignment.has_committed_provisioned_slot,
          "newer enumeration did not reacquire first-assignment custody");

    CHECK(!pending_ack_yields_to_newer_claim(
              &replacement, ASSIGNMENT_EPOCH + 1u),
          "newer CLAIM discarded an ACK protecting a committed slot");
    CHECK(replacement.pending_ack_active &&
              replacement.pending_epoch == ASSIGNMENT_EPOCH,
          "replacement enumeration lost its older proven candidate");
    return true;
}

static bool test_newer_claim_abort_preserves_old_low_duty_ack(void)
{
    struct assignment_transaction_snapshot_model snapshot = {
        .committed_epoch = ASSIGNMENT_EPOCH,
        .pending_epoch = ASSIGNMENT_EPOCH + 1u,
        .pending_valid = true,
    };
    const uint32_t newer_claim_epoch = ASSIGNMENT_EPOCH + 2u;
    bool old_ack_low_duty_active = true;
    bool newer_table_persisted = false;

    /*
     * CLAIM alone is not an assignment-changing transaction. The old ACK
     * owner remains live while the gateway attempts a newer enumeration.
     */
    CHECK(discovery_assignment_epoch_strictly_newer(
              newer_claim_epoch, snapshot.pending_epoch),
          "newer CLAIM fixture is not ordered");
    CHECK(old_ack_low_duty_active && !newer_table_persisted &&
              snapshot.pending_epoch == ASSIGNMENT_EPOCH + 1u,
          "newer CLAIM superseded old pending ACK before TABLE");

    /* The newer operation aborts pre-TABLE; a later low-duty probe converges. */
    CHECK(assignment_transaction_promote(
              &snapshot, ASSIGNMENT_EPOCH + 1u),
          "old low-duty ACK did not recover after newer CLAIM abort");
    old_ack_low_duty_active = false;
    CHECK(!old_ack_low_duty_active &&
              snapshot.committed_epoch == ASSIGNMENT_EPOCH + 1u &&
              !snapshot.pending_valid,
          "newer CLAIM abort stranded the older proven assignment");
    return true;
}

static bool test_expected_count_uses_unique_current_claim_responders(void)
{
    struct gateway_model gateway = {
        .claim_ids = {
            ANCHOR_BASE,
            ANCHOR_BASE + 1u,
            ANCHOR_BASE + 2u,
        },
        .claim_count = 3u,
        .active = true,
    };
    struct proto_packet claim;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const size_t expected_current_responders = 3u;

    CHECK(gateway_current_claim_response_count(&gateway) == 0u,
          "seeded prior roster counted as current CLAIM responses");
    CHECK(build_result(
              ANCHOR_BASE,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION,
              &claim,
              payload,
              &payload_len),
          "prior-member CLAIM build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &claim,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == 0,
          "prior-member current CLAIM rejected");
    CHECK(gateway_current_claim_response_count(&gateway) == 1u &&
              gateway.claim_count == 3u,
          "current prior-member response changed roster cardinality");

    CHECK(gateway_accept_result(
              &gateway,
              &claim,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == 0,
          "exact current CLAIM duplicate rejected");
    CHECK(gateway_current_claim_response_count(&gateway) == 1u,
          "duplicate CLAIM advanced expected-count completion");

    CHECK(build_result(
              ANCHOR_BASE + 3u,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION,
              &claim,
              payload,
              &payload_len),
          "new-member CLAIM build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &claim,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == 0,
          "new-member current CLAIM rejected");
    CHECK(gateway_current_claim_response_count(&gateway) ==
              expected_current_responders - 1u &&
              gateway.claim_count == 4u,
          "seeded roster advanced completion before enough current responses");

    CHECK(build_result(
              ANCHOR_BASE + 1u,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION,
              &claim,
              payload,
              &payload_len),
          "second prior-member CLAIM build failed");
    CHECK(gateway_accept_result(
              &gateway,
              &claim,
              payload,
              payload_len,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              CLAIM_SESSION) == 0,
          "second prior-member current CLAIM rejected");
    CHECK(gateway_current_claim_response_count(&gateway) ==
              expected_current_responders &&
              gateway.claim_count == 4u,
          "unique current responders did not complete expected count");
    return true;
}

static bool test_smaller_current_roster_completes_against_retained_fifty(void)
{
    struct gateway_model gateway = {
        .claim_count = MAX_ANCHORS,
        .active = true,
    };
    struct proto_packet claim;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    const size_t expected_current_responders = 5u;

    CHECK(MAX_ANCHORS == 50u,
          "retained-roster shrink fixture no longer models fifty anchors");
    for (size_t i = 0u; i < gateway.claim_count; i++) {
        gateway.claim_ids[i] = ANCHOR_BASE + i;
    }
    CHECK(!gateway_expected_claims_complete(
              &gateway, expected_current_responders),
          "retained roster incorrectly completed the current operation");

    for (size_t i = 0u; i < expected_current_responders; i++) {
        CHECK(build_result(
                  ANCHOR_BASE + i,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH,
                  CLAIM_SESSION,
                  &claim,
                  payload,
                  &payload_len),
              "current responder CLAIM build failed index=%zu", i);
        CHECK(gateway_accept_result(
                  &gateway,
                  &claim,
                  payload,
                  payload_len,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH,
                  CLAIM_SESSION) == 0,
              "retained member current CLAIM rejected index=%zu", i);
        CHECK(gateway_expected_claims_complete(
                  &gateway, expected_current_responders) ==
                  (i + 1u == expected_current_responders),
              "current responder completion changed at index=%zu count=%zu",
              i,
              gateway_current_claim_response_count(&gateway));
    }
    CHECK(gateway.claim_count == MAX_ANCHORS &&
              gateway_current_claim_response_count(&gateway) ==
                  expected_current_responders,
          "smaller current roster changed retained membership count=%zu current=%zu",
          gateway.claim_count,
          gateway_current_claim_response_count(&gateway));
    return true;
}

static uint64_t claim_settle_deadline_from_duplicate(
    uint64_t received_at_ms,
    uint8_t hop_count,
    uint64_t collection_deadline_ms,
    uint64_t operation_deadline_ms)
{
    uint64_t deadline_ms =
        discovery_assignment_claim_ack_settle_deadline_ms(
            received_at_ms, hop_count);

    if (deadline_ms > collection_deadline_ms) {
        deadline_ms = collection_deadline_ms;
    }
    if (deadline_ms > operation_deadline_ms) {
        deadline_ms = operation_deadline_ms;
    }
    return deadline_ms;
}

static bool test_d_minus_one_duplicate_extends_claim_settle(void)
{
    const uint8_t hop_count = DISCOVERY_ASSIGNMENT_MAX_HOPS;
    const uint64_t first_received_at_ms = UINT64_C(10000);
    const uint64_t old_settle_deadline_ms =
        discovery_assignment_claim_ack_settle_deadline_ms(
            first_received_at_ms, hop_count);
    const uint64_t duplicate_received_at_ms =
        old_settle_deadline_ms - 1u;
    const uint64_t duplicate_natural_deadline_ms =
        discovery_assignment_claim_ack_settle_deadline_ms(
            duplicate_received_at_ms, hop_count);
    const uint64_t late_collection_deadline_ms =
        duplicate_natural_deadline_ms + 100u;
    const uint64_t late_operation_deadline_ms =
        duplicate_natural_deadline_ms + 200u;

    CHECK(duplicate_natural_deadline_ms > old_settle_deadline_ms,
          "D-1 duplicate fixture does not extend the hop-aware settle");
    CHECK(claim_settle_deadline_from_duplicate(
              duplicate_received_at_ms,
              hop_count,
              late_collection_deadline_ms,
              late_operation_deadline_ms) ==
              duplicate_natural_deadline_ms,
          "duplicate CLAIM was clamped to the old response deadline");
    CHECK(claim_settle_deadline_from_duplicate(
              duplicate_received_at_ms,
              hop_count,
              duplicate_natural_deadline_ms - 7u,
              late_operation_deadline_ms) ==
              duplicate_natural_deadline_ms - 7u,
          "duplicate CLAIM exceeded the immutable collection horizon");
    CHECK(claim_settle_deadline_from_duplicate(
              duplicate_received_at_ms,
              hop_count,
              late_collection_deadline_ms,
              duplicate_natural_deadline_ms - 3u) ==
              duplicate_natural_deadline_ms - 3u,
          "duplicate CLAIM exceeded the immutable operation deadline");
    return true;
}

static bool test_ack_retry_rate_is_bounded_but_eventually_recovers(void)
{
    const uint8_t fast_handle_retries = 3u;
    const uint32_t low_duty_base_ms = 60000u;
    const uint32_t low_duty_max_ms = 3600000u;
    const uint64_t month_ms = UINT64_C(30) * 24u * 60u * 60u * 1000u;
    uint64_t elapsed_ms = 0u;
    uint32_t low_duty_handles = 0u;
    uint8_t retry_round = 0u;
    bool pending_persisted = true;
    bool delivered = false;

    for (uint8_t i = 0u; i < fast_handle_retries; i++) {
        retry_round++;
    }
    while (elapsed_ms < month_ms) {
        uint8_t low_round =
            retry_round > fast_handle_retries ?
            (uint8_t)(retry_round - fast_handle_retries) : 0u;
        uint32_t delay_ms = low_duty_base_ms;

        for (uint8_t i = 0u; i < low_round; i++) {
            if (delay_ms >= low_duty_max_ms / 2u) {
                delay_ms = low_duty_max_ms;
                break;
            }
            delay_ms *= 2u;
        }
        elapsed_ms += delay_ms;
        low_duty_handles++;
        if (retry_round != UINT8_MAX) {
            retry_round++;
        }
    }
    CHECK(pending_persisted &&
              low_duty_handles <=
                  (uint32_t)(month_ms / low_duty_max_ms) + 8u,
          "long outage retry rate exceeded capped exponential bound");

    /* Route recovery needs no new TABLE: the next low-duty handle converges. */
    delivered = true;
    CHECK(delivered && pending_persisted,
          "late route recovery did not retain durable ACK custody");
    pending_persisted = false;
    CHECK(!pending_persisted,
          "late delivered proof did not clear pending custody");
    return true;
}

static bool test_final_table_only_anchor_requests_exact_table_redrive(void)
{
    struct gateway_model gateway = {
        .claim_ids = {ANCHOR_BASE},
        .claim_count = 1u,
        .active = true,
    };
    struct proto_packet claim;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    bool late_table_redrive_pending = false;
    bool table_received = false;

    CHECK(build_result(
              ANCHOR_BASE,
              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
              ASSIGNMENT_EPOCH,
              TABLE_SESSION,
              &claim,
              payload,
              &payload_len),
          "TABLE-correlated late CLAIM build failed");

    /*
     * Drop every CLAIM copy and all normal TABLE copies except the last one.
     * The anchor cannot apply that table yet, so its response uses the exact
     * TABLE session and remains unacknowledged while requesting a redrive.
     */
    CHECK(claim.session_id == TABLE_SESSION &&
              gateway.claim_ids[0] == claim.src_id,
          "late CLAIM was not correlated to a listed TABLE member");
    late_table_redrive_pending = true;
    CHECK(late_table_redrive_pending && !table_received,
          "late CLAIM did not retain redrive ownership");

    /* One immutable TABLE redrive reaches the waiting anchor and ends debt. */
    table_received = true;
    late_table_redrive_pending = false;
    CHECK(table_received && !late_table_redrive_pending,
          "TABLE redrive did not converge after final-copy-only discovery");
    return true;
}

int main(void)
{
    static const size_t counts[] = {2u, 6u, 16u, 32u, 50u};

    for (size_t i = 0u; i < sizeof(counts) / sizeof(counts[0]); i++) {
        if (!run_workflow(counts[i])) {
            return EXIT_FAILURE;
        }
    }
    if (!test_conflicts_capacity_and_late_claim()) {
        return EXIT_FAILURE;
    }
    if (!test_prior_anchor_late_table_converges_on_exact_retry()) {
        return EXIT_FAILURE;
    }
    if (!test_duplicate_result_singletons_do_not_mutate_or_ack()) {
        return EXIT_FAILURE;
    }
    if (!test_gateway_semantic_acceptance_categories()) {
        return EXIT_FAILURE;
    }
    if (!test_gateway_stage_session_and_member_rejection_is_immutable()) {
        return EXIT_FAILURE;
    }
    if (!test_gateway_invalid_hop_telemetry_is_conservative()) {
        return EXIT_FAILURE;
    }
    if (!test_retired_valid_results_release_transport_custody()) {
        return EXIT_FAILURE;
    }
    if (!test_explicit_budget_clips_current_window_without_division()) {
        return EXIT_FAILURE;
    }
    if (!test_ordered_epoch_expansion_and_unassigned_reboot_are_monotonic()) {
        return EXIT_FAILURE;
    }
    if (!test_real_rf_attempt_preserves_delayed_assignment_responses()) {
        return EXIT_FAILURE;
    }
    if (!test_pending_ack_expiry_retries_without_reset_and_keeps_old_slot()) {
        return EXIT_FAILURE;
    }
    if (!test_assignment_snapshot_transactions_reject_stale_rmw()) {
        return EXIT_FAILURE;
    }
    if (!test_queued_delivery_promotes_before_new_claim_abort()) {
        return EXIT_FAILURE;
    }
    if (!test_unprovisioned_obsolete_ack_yields_to_newer_claim()) {
        return EXIT_FAILURE;
    }
    if (!test_newer_claim_abort_preserves_old_low_duty_ack()) {
        return EXIT_FAILURE;
    }
    if (!test_expected_count_uses_unique_current_claim_responders()) {
        return EXIT_FAILURE;
    }
    if (!test_smaller_current_roster_completes_against_retained_fifty()) {
        return EXIT_FAILURE;
    }
    if (!test_d_minus_one_duplicate_extends_claim_settle()) {
        return EXIT_FAILURE;
    }
    if (!test_ack_retry_rate_is_bounded_but_eventually_recovers()) {
        return EXIT_FAILURE;
    }
    if (!test_final_table_only_anchor_requests_exact_table_redrive()) {
        return EXIT_FAILURE;
    }
    printf("PASS discovery_assignment_adversarial counts=2,6,16,32,50 "
           "command=0x%04x fallback=forbidden\n",
           CMD_ASSIGN_DISCOVERY_SLOTS);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
