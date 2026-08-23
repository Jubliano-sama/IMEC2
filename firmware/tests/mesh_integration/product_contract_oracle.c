#include "discovery_assignment.h"
#include "operation_policy.h"
#include "app_mesh_c5_priority.h"
#include "survey_gateway_transaction.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int decode_hex(const char *hex,
                      uint8_t *bytes,
                      size_t capacity,
                      size_t *length)
{
    size_t hex_length;

    if (hex == NULL || bytes == NULL || length == NULL) {
        return -1;
    }
    hex_length = strlen(hex);
    if ((hex_length & 1u) != 0u || hex_length / 2u > capacity) {
        return -1;
    }
    *length = hex_length / 2u;
    for (size_t i = 0u; i < *length; i++) {
        int high = hex_nibble(hex[2u * i]);
        int low = hex_nibble(hex[2u * i + 1u]);

        if (high < 0 || low < 0) {
            return -1;
        }
        bytes[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static int policy_oracle(const char *hex)
{
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    struct operation_policy_set set;
    size_t payload_len = 0u;
    int ret;

    if (decode_hex(hex, payload, sizeof(payload), &payload_len) != 0) {
        return 2;
    }
    ret = operation_policy_set_from_tlvs(payload, payload_len, &set);
    if (ret != PROTO_OK) {
        printf("decode_error=%d\n", ret);
        return 1;
    }
    printf("expected=%u assignment_budget=%" PRIu32
           " assignment_spread=%u discovery_start=%" PRIu32
           " discovery_slots=%u discovery_budget=%" PRIu32
           " pair_reruns=%u pair_parallel=%u ram_only=%u\n",
           set.assignment.expected_anchor_count,
           set.assignment.operation_budget_ms,
           set.assignment.response_spread_ms,
           set.discovery.start_delay_ms,
           set.discovery.slot_count,
           set.discovery.operation_budget_ms,
           set.pair.max_reruns,
           set.pair.max_parallel_pairs,
           set.assignment.ram_only_iteration ? 1u : 0u);
    return 0;
}

static bool command_outranks_click(enum command_id command_id)
{
    uint8_t payload[PROTO_TLV_HEADER_LEN + sizeof(uint16_t)];
    size_t payload_len = 0u;

    if (tlv_append_u16(payload, sizeof(payload), &payload_len,
                       TLV_COMMAND_ID, (uint16_t)command_id) != PROTO_OK) {
        return false;
    }
    return app_mesh_c5_gateway_operation_outranks_unaccepted_click(
        MSG_COMMAND, payload, payload_len);
}

static int control_oracle(void)
{
    const bool enumeration = command_outranks_click(
        CMD_ASSIGN_DISCOVERY_SLOTS);
    const bool survey = command_outranks_click(CMD_SURVEY_REACHABILITY);
    const bool pair = command_outranks_click(CMD_SURVEY_PREPARE_PAIR) &&
                      command_outranks_click(CMD_SURVEY_START_PAIR);
    const bool abort = command_outranks_click(CMD_SURVEY_ABORT);
    const bool here_i_am =
        app_mesh_c5_gateway_operation_outranks_unaccepted_click(
            MSG_GATEWAY_ROUTE_ADV, NULL, 0u);
    const bool unrelated = command_outranks_click(CMD_PING);

    printf("enumeration=%u survey=%u pair=%u abort=%u here_i_am=%u unrelated=%u\n",
           enumeration ? 1u : 0u,
           survey ? 1u : 0u,
           pair ? 1u : 0u,
           abort ? 1u : 0u,
           here_i_am ? 1u : 0u,
           unrelated ? 1u : 0u);
    return enumeration && survey && pair && abort && here_i_am && !unrelated ?
        0 : 1;
}

static int pair_custody_oracle(void)
{
    struct survey_gateway_transaction transaction;
    const struct survey_pair first = {
        .initiator_id = UINT64_C(0xa001),
        .responder_id = UINT64_C(0xa002),
        .operation_generation = UINT64_C(0x000000010000004d),
        .survey_id = 77u,
        .sample_count = 5u,
    };
    struct survey_pair second = first;
    int first_ret;
    int second_ret;

    second.initiator_id = UINT64_C(0xa003);
    second.responder_id = UINT64_C(0xa004);
    survey_gateway_transaction_init(&transaction);
    first_ret = survey_gateway_transaction_load_pair(&transaction, &first);
    second_ret = survey_gateway_transaction_load_pair(&transaction, &second);
    printf("first=%d replacement=%d retained=%u\n",
           first_ret,
           second_ret,
           transaction.pair.initiator_id == first.initiator_id &&
                   transaction.pair.responder_id == first.responder_id ?
               1u : 0u);
    return first_ret == 0 && second_ret == -EBUSY &&
                   transaction.pair.initiator_id == first.initiator_id &&
                   transaction.pair.responder_id == first.responder_id ?
        0 : 1;
}

static int ordering_oracle(void)
{
    uint64_t anchor_ids[] = {
        UINT64_C(0xd004),
        UINT64_C(0xd002),
        UINT64_C(0xd001),
        UINT64_C(0xd003),
    };
    uint8_t hop_counts[] = {2u, 0u, 1u, 0u};
    int ret = discovery_assignment_order_roster_extension(
        anchor_ids, hop_counts,
        sizeof(anchor_ids) / sizeof(anchor_ids[0]), 0u);

    if (ret != PROTO_OK) {
        printf("order_error=%d\n", ret);
        return 1;
    }
    for (size_t i = 0u; i < sizeof(anchor_ids) / sizeof(anchor_ids[0]); i++) {
        printf("%s%" PRIu64 ":%u", i == 0u ? "" : ",",
               anchor_ids[i], hop_counts[i]);
    }
    putchar('\n');
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--policy") == 0) {
        return policy_oracle(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "--order") == 0) {
        return ordering_oracle();
    }
    if (argc == 2 && strcmp(argv[1], "--control") == 0) {
        return control_oracle();
    }
    if (argc == 2 && strcmp(argv[1], "--pair-custody") == 0) {
        return pair_custody_oracle();
    }
    return 2;
}
