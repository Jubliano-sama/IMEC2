#include "app_discovery_assignment_policy.h"
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
#define CLAIM_SESSION UINT32_C(0x41000001)
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
    uint32_t persisted_table_fingerprint;
    uint8_t persisted_slot;
    uint8_t assigned_slot;
    uint8_t claim_replies;
    uint8_t ack_replies;
    bool persisted;
    bool persisted_provisioned;
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
    size_t claim_count;
    uint16_t duplicate_claims;
    uint16_t duplicate_acks;
};

static int failures;

#define CHECK(expression, ...) do {                                           \
    if (!(expression)) {                                                      \
        fprintf(stderr, "FAIL line=%d ", __LINE__);                         \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                   \
        failures++;                                                           \
        return false;                                                         \
    }                                                                         \
} while (0)

static bool build_result(uint64_t anchor_id,
                         enum discovery_assignment_phase phase,
                         uint32_t epoch,
                         uint32_t session,
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
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(
            payload, UWB_MESH_MAX_PAYLOAD_LEN, &length,
            discovery_assignment_hash(anchor_id));
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

static int gateway_accept_result(struct gateway_model *gateway,
                                 const struct proto_packet *packet,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 enum discovery_assignment_phase expected_phase,
                                 uint32_t expected_epoch,
                                 uint32_t expected_session)
{
    enum discovery_assignment_phase phase = 0;
    enum command_id command_id = CMD_VENDOR_BASE;
    const uint8_t *status = NULL;
    uint64_t hash = 0u;
    uint32_t epoch = 0u;
    uint8_t status_len = 0u;
    size_t index = SIZE_MAX;

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
    if (gateway_command_extract_id(payload, payload_len, &command_id) !=
        PROTO_OK) {
        return -EBADMSG;
    }
    if (command_id != CMD_ASSIGN_DISCOVERY_SLOTS) {
        return -ENOENT;
    }
    if (tlv_find(payload, payload_len, TLV_COMMAND_STATUS, &status,
                 &status_len) != PROTO_OK ||
        status_len != sizeof(uint16_t) ||
        proto_get_u16_le(status) != COMMAND_OK ||
        discovery_assignment_extract_control_tlvs(payload, payload_len,
                                                   &phase, &epoch) != PROTO_OK ||
        discovery_assignment_extract_claim_hash(payload, payload_len, &hash) !=
            PROTO_OK || hash != discovery_assignment_hash(packet->src_id)) {
        return -EBADMSG;
    }
    if (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
        phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        return -EPROTO;
    }
    if (phase != expected_phase || epoch != expected_epoch ||
        packet->session_id != expected_session) {
        return -ESTALE;
    }

    for (size_t i = 0u; i < gateway->claim_count; i++) {
        if (gateway->claim_ids[i] == packet->src_id) {
            index = i;
            break;
        }
    }
    if (expected_phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        if (index != SIZE_MAX) {
            gateway->duplicate_claims++;
            return 0;
        }
        if (gateway->claim_count >= MAX_ANCHORS) {
            return -ENOSPC;
        }
        gateway->claim_ids[gateway->claim_count++] = packet->src_id;
        return 0;
    }
    if (index == SIZE_MAX) {
        return -ESTALE;
    }
    if ((gateway->ack_mask & (UINT64_C(1) << index)) != 0u) {
        gateway->duplicate_acks++;
    }
    gateway->ack_mask |= UINT64_C(1) << index;
    return 0;
}

static bool build_table(const struct gateway_model *gateway,
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
            DISCOVERY_ASSIGNMENT_PHASE_TABLE, ASSIGNMENT_EPOCH) != PROTO_OK ||
        discovery_assignment_append_table_tlvs(
            payload, PACKET_EXT_MAX_PAYLOAD_LEN, &length,
            entries, gateway->claim_count) != PROTO_OK) {
        return false;
    }
    *payload_len = length;
    return true;
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
    uint32_t epoch = 0u;
    uint32_t table_fingerprint;
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
    table_fingerprint = discovery_assignment_table_fingerprint(
        decoded, count, slot_count);
    if (table_fingerprint == 0u) {
        return false;
    }
    decision = app_discovery_assignment_policy_note_table(
        &anchor->policy, epoch, table_seq, table_fingerprint);
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
        if (!app_discovery_assignment_policy_note_unassigned(
                &anchor->policy, epoch, table_seq, table_fingerprint)) {
            return false;
        }
        anchor->persisted = true;
        anchor->persisted_provisioned = false;
        anchor->persisted_epoch = epoch;
        anchor->persisted_table_seq = table_seq;
        anchor->persisted_table_fingerprint = table_fingerprint;
        anchor->persisted_slot = UINT8_MAX;
        anchor->assigned_slot = UINT8_MAX;
        return false;
    }
    if (decision == APP_DISCOVERY_ASSIGNMENT_TABLE_REPLAY) {
        return true;
    }
    if (!persist_ok) {
        anchor->persistence_retried = true;
        return false;
    }
    anchor->persisted = true;
    anchor->persisted_provisioned = true;
    anchor->persisted_epoch = epoch;
    anchor->persisted_table_seq = table_seq;
    anchor->persisted_table_fingerprint = table_fingerprint;
    anchor->persisted_slot = decoded[match].slot;
    if (!app_discovery_assignment_policy_commit(
            &anchor->policy, epoch, table_seq, table_fingerprint)) {
        return false;
    }
    anchor->assigned_slot = decoded[match].slot;
    anchor->table_applied = true;
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
    struct gateway_model gateway = {0};
    struct discovery_assignment_entry entries[MAX_ANCHORS];
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
        app_discovery_assignment_policy_init(
            &anchors[i].policy, false, false, 0u, 0u, 0u);
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
                  ASSIGNMENT_EPOCH, CLAIM_SESSION) == -ESTALE,
              "stale claim accepted count=%zu", anchor_count);
        stale_payload[stale_len - 1u] ^= 1u;
        CHECK(gateway_accept_result(
                  &gateway, &stale_packet, stale_payload, stale_len,
                  DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                  ASSIGNMENT_EPOCH - 1u, CLAIM_SESSION) == -EBADMSG,
              "wrong hash accepted count=%zu", anchor_count);
    }

    CHECK(build_table(&gateway, entries, table_payload, &table_payload_len),
          "table build count=%zu", anchor_count);
    CHECK(compare_deterministic_order(&gateway, entries),
          "deterministic order count=%zu", anchor_count);
    CHECK(app_discovery_assignment_policy_note_table(
              &anchors[0].policy, ASSIGNMENT_EPOCH - 1u,
              TABLE_GENERATION_1,
              discovery_assignment_table_fingerprint(
                  entries, gateway.claim_count,
                  MAX_ANCHORS)) ==
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
                  ASSIGNMENT_EPOCH, TABLE_SESSION) == -ESTALE,
              "wrong-epoch ACK accepted count=%zu", anchor_count);
        wrong_ack.session_id = TABLE_SESSION - 1u;
        CHECK(gateway_accept_result(
                  &gateway, &wrong_ack, wrong_payload, wrong_len,
                  DISCOVERY_ASSIGNMENT_PHASE_ACK,
                  ASSIGNMENT_EPOCH - 1u, TABLE_SESSION) == -ESTALE,
              "wrong-session ACK accepted count=%zu", anchor_count);
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
              anchors[i].persisted_table_fingerprint ==
                  discovery_assignment_table_fingerprint(
                      entries, anchor_count, MAX_ANCHORS) &&
              anchors[i].persisted_slot == entries[expected].slot &&
              anchors[i].assigned_slot == entries[expected].slot,
              "persistence mismatch count=%zu anchor=%zu persisted=%u provisioned=%u epoch=%u seq=%u fingerprint=0x%08x slot=%u assigned=%u expected_slot=%u expected_fingerprint=0x%08x",
              anchor_count, i, anchors[i].persisted ? 1u : 0u,
              anchors[i].persisted_provisioned ? 1u : 0u,
              anchors[i].persisted_epoch,
              anchors[i].persisted_table_seq,
              anchors[i].persisted_table_fingerprint,
              anchors[i].persisted_slot,
              anchors[i].assigned_slot,
              entries[expected].slot,
              discovery_assignment_table_fingerprint(
                  entries, anchor_count, MAX_ANCHORS));
        app_discovery_assignment_policy_init(
            &restored, true, anchors[i].persisted_provisioned,
            anchors[i].persisted_epoch, anchors[i].persisted_table_seq,
            anchors[i].persisted_table_fingerprint);
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
        &late, false, false, 0u, 0u, 0u);
    CHECK(app_discovery_assignment_policy_note_table(
              &late, ASSIGNMENT_EPOCH, TABLE_GENERATION_1,
              UINT32_C(0x7e57f001)) ==
              APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM,
          "late table did not require claim");
    CHECK(!app_discovery_assignment_policy_commit(
              &late, ASSIGNMENT_EPOCH, TABLE_GENERATION_1,
              UINT32_C(0x7e57f001)),
          "late table committed without claim");
    CHECK(!app_discovery_assignment_policy_normal_click_reply_allowed(&late),
          "hash fallback hid missing assignment");
    return true;
}

static bool test_gateway_semantic_acceptance_categories(void)
{
    struct gateway_model gateway = {0};
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
                                CLAIM_SESSION) == -ESTALE,
          "wrong-epoch claim was not classified as stale");

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
                                TABLE_SESSION) == -ESTALE,
          "wrong-session table ACK was not classified as stale");

    gateway.claim_count = MAX_ANCHORS;
    for (size_t i = 0u; i < MAX_ANCHORS; i++) {
        gateway.claim_ids[i] = ANCHOR_BASE + i;
    }
    CHECK(build_result(ANCHOR_BASE + MAX_ANCHORS,
                       DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                       ASSIGNMENT_EPOCH,
                       CLAIM_SESSION,
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

static bool test_same_epoch_expansion_and_unassigned_reboot_are_monotonic(void)
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
    uint32_t expanded_fingerprint;
    uint32_t smaller_fingerprint;
    uint32_t omitted_fingerprint;
    uint8_t expanded_slot;

    expanded.claim_count = 6u;
    for (size_t i = 0u; i < expanded.claim_count; i++) {
        expanded.claim_ids[i] = ANCHOR_BASE + i;
    }
    CHECK(build_table(&expanded, expanded_entries, expanded_payload,
                      &expanded_len),
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
    CHECK(build_table(&omitted, omitted_entries, omitted_payload,
                      &omitted_len),
          "omitted table build failed");

    smaller_fingerprint = discovery_assignment_table_fingerprint(
        smaller_entries, smaller.claim_count, MAX_ANCHORS);
    expanded_fingerprint = discovery_assignment_table_fingerprint(
        expanded_entries, expanded.claim_count, MAX_ANCHORS);
    omitted_fingerprint = discovery_assignment_table_fingerprint(
        omitted_entries, omitted.claim_count, MAX_ANCHORS);
    CHECK(smaller_fingerprint != 0u && expanded_fingerprint != 0u &&
              omitted_fingerprint != 0u &&
              smaller_fingerprint != expanded_fingerprint &&
              expanded_fingerprint != omitted_fingerprint,
          "table fingerprints did not distinguish roster generations");

    anchor.id = smaller_entries[0].anchor_id;
    anchor.assigned_slot = UINT8_MAX;
    anchor.persisted_slot = UINT8_MAX;
    app_discovery_assignment_policy_init(
        &anchor.policy, false, false, 0u, 0u, 0u);
    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, ASSIGNMENT_EPOCH) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "smaller generation claim rejected");
    CHECK(anchor_apply_table(&anchor, smaller_payload, smaller_len,
                             TABLE_GENERATION_1, true),
          "smaller generation assignment failed");
    CHECK(anchor.persisted_table_fingerprint == smaller_fingerprint,
          "smaller fingerprint was not persisted");

    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, ASSIGNMENT_EPOCH) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "expanded generation claim rejected");
    CHECK(anchor_apply_table(&anchor, expanded_payload, expanded_len,
                             TABLE_GENERATION_2, true),
          "expanded generation assignment failed");
    expanded_slot = anchor.assigned_slot;
    CHECK(expanded_slot == 4u &&
              anchor.persisted_table_seq == TABLE_GENERATION_2 &&
              anchor.persisted_table_fingerprint == expanded_fingerprint,
          "expanded generation did not replace smaller assignment");

    CHECK(app_discovery_assignment_policy_note_claim(
              &anchor.policy, ASSIGNMENT_EPOCH) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "post-expansion claim rejected");
    CHECK(!anchor_apply_table(&anchor, smaller_payload, smaller_len,
                              TABLE_GENERATION_1, true),
          "older smaller generation reapplied");
    CHECK(anchor.assigned_slot == expanded_slot &&
              anchor.persisted_provisioned &&
              anchor.persisted_table_seq == TABLE_GENERATION_2 &&
              anchor.persisted_table_fingerprint == expanded_fingerprint,
          "older smaller generation mutated expanded assignment");

    CHECK(!anchor_apply_table(&anchor, omitted_payload, omitted_len,
                              TABLE_GENERATION_2, true),
          "same-sequence conflicting roster applied");
    CHECK(anchor.persisted_provisioned &&
              anchor.persisted_table_fingerprint == expanded_fingerprint,
          "same-sequence conflict erased assignment");

    CHECK(!anchor_apply_table(&anchor, omitted_payload, omitted_len,
                              TABLE_GENERATION_3, true),
          "authoritative omission unexpectedly returned assigned");
    CHECK(anchor.persisted && !anchor.persisted_provisioned &&
              anchor.persisted_epoch == ASSIGNMENT_EPOCH &&
              anchor.persisted_table_seq == TABLE_GENERATION_3 &&
              anchor.persisted_table_fingerprint == omitted_fingerprint &&
              anchor.persisted_slot == UINT8_MAX,
          "unassigned generation tombstone was not retained");

    restored.id = anchor.id;
    restored.assigned_slot = UINT8_MAX;
    restored.persisted_slot = anchor.persisted_slot;
    restored.persisted = anchor.persisted;
    restored.persisted_provisioned = anchor.persisted_provisioned;
    restored.persisted_epoch = anchor.persisted_epoch;
    restored.persisted_table_seq = anchor.persisted_table_seq;
    restored.persisted_table_fingerprint =
        anchor.persisted_table_fingerprint;
    app_discovery_assignment_policy_init(
        &restored.policy, restored.persisted,
        restored.persisted_provisioned, restored.persisted_epoch,
        restored.persisted_table_seq,
        restored.persisted_table_fingerprint);
    CHECK(!app_discovery_assignment_policy_normal_click_reply_allowed(
              &restored.policy),
          "rebooted omission became provisioned");
    CHECK(app_discovery_assignment_policy_note_claim(
              &restored.policy, ASSIGNMENT_EPOCH) ==
              APP_DISCOVERY_ASSIGNMENT_CLAIM_RESPOND,
          "rebooted omission claim rejected");
    CHECK(!anchor_apply_table(&restored, expanded_payload, expanded_len,
                              TABLE_GENERATION_2, true),
          "pre-reboot generation resurrected assignment");
    CHECK(restored.policy.committed_table_seq == TABLE_GENERATION_3 &&
              restored.policy.committed_table_fingerprint ==
                  omitted_fingerprint &&
              !app_discovery_assignment_policy_normal_click_reply_allowed(
                  &restored.policy),
          "replay changed rebooted unassigned watermark");
    return true;
}

static bool test_semantic_quorum_survives_redundant_flood_tail_failure(void)
{
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              2u, 2u, 2u, 4u, false, false),
          "complete one-hop/two-hop claim quorum did not supersede flood tail failure");
    CHECK(!app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              2u, 1u, 2u, 4u, false, false),
          "incomplete claim quorum hid flood failure");
    CHECK(!app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_CLAIM,
              0u, 2u, 2u, 4u, false, false),
          "unknown roster bypassed delivered-flood requirement");
    CHECK(app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
              2u, 2u, 0u, 4u, false, false),
          "complete table ACK quorum did not supersede flood tail failure");
    CHECK(!app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
              0u, 0u, 0u, 4u, false, false),
          "empty roster created a table quorum");
    CHECK(!app_discovery_assignment_semantic_terminal_success(
              APP_DISCOVERY_ASSIGNMENT_TERMINAL_TABLE,
              2u, 2u, 0u, 4u, false, true),
          "cancelled table flood was accepted from stale ACK state");
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
    if (!test_gateway_semantic_acceptance_categories()) {
        return EXIT_FAILURE;
    }
    if (!test_explicit_budget_clips_current_window_without_division()) {
        return EXIT_FAILURE;
    }
    if (!test_same_epoch_expansion_and_unassigned_reboot_are_monotonic()) {
        return EXIT_FAILURE;
    }
    if (!test_semantic_quorum_survives_redundant_flood_tail_failure()) {
        return EXIT_FAILURE;
    }
    printf("PASS discovery_assignment_adversarial counts=2,6,16,32,50 "
           "command=0x%04x fallback=forbidden\n",
           CMD_ASSIGN_DISCOVERY_SLOTS);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
