#include "discovery_assignment.h"
#include "enumeration_response_lane.h"
#include "mesh.h"
#include "protocol_rx_lifecycle.h"
#include "survey.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ENUMERATION_EPOCH UINT32_C(0xe1500001)
#define SURVEY_GENERATION UINT32_C(0x5a770001)
#define TABLE_SEQUENCE UINT32_C(0x7ab1e001)
#define TEST_ORIGIN_MS UINT32_C(100000)

_Static_assert(UWB_ENUM_MAX_HOPS == DISCOVERY_ASSIGNMENT_MAX_HOPS,
               "enumeration, survey, and mesh routing must share one depth horizon");
_Static_assert(UWB_ENUM_MAX_HOPS == MESH_NETWORK_MAX_HOPS,
               "radio ownership coverage must reach every production mesh hop");

enum visible_owner {
    VISIBLE_OWNER_NONE = 0,
    VISIBLE_OWNER_ENUMERATION,
    VISIBLE_OWNER_SURVEY,
};

struct anchor_model {
    struct protocol_rx_lifecycle enumeration;
    struct protocol_rx_lifecycle survey;
    struct protocol_rx_downstream_activation downstream;
    uint8_t depth;
    uint32_t table_sequence;
    bool prearmed;
    bool survey_follows;
    bool survey_handoff;
    bool survey_published;
};

static int failures;
static uint8_t active_topology_depth;
static uint8_t active_anchor_depth;

#define CHECK(expression, ...) do {                                           \
    if (!(expression)) {                                                      \
        fprintf(stderr,                                                       \
                "FAIL state-alignment topology=%u anchor=%u line=%d: ",      \
                active_topology_depth, active_anchor_depth, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                   \
        failures++;                                                           \
        return false;                                                         \
    }                                                                         \
} while (0)

static uint32_t compact_arrival_ms(uint32_t origin_ms, uint8_t depth)
{
    return origin_ms + DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS +
           (uint32_t)depth *
               DISCOVERY_ASSIGNMENT_RELAY_BEFORE_RESPONSE_MAX_MS;
}

static uint32_t activation_arrival_ms(uint32_t origin_ms, uint8_t depth)
{
    return origin_ms + DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS +
           (uint32_t)depth *
               DISCOVERY_ASSIGNMENT_ACTIVATION_RELAY_HOP_MAX_MS;
}

static void anchor_init(struct anchor_model *anchor, uint8_t depth)
{
    memset(anchor, 0, sizeof(*anchor));
    protocol_rx_lifecycle_init(&anchor->enumeration);
    protocol_rx_lifecycle_init(&anchor->survey);
    protocol_rx_downstream_activation_init(&anchor->downstream);
    anchor->depth = depth;
}

static enum visible_owner anchor_owner(const struct anchor_model *anchor)
{
    if (anchor->survey_published &&
        anchor->survey.operation == PROTOCOL_RX_OPERATION_SURVEY) {
        return VISIBLE_OWNER_SURVEY;
    }
    if (anchor->enumeration.operation ==
        PROTOCOL_RX_OPERATION_ENUMERATION) {
        return VISIBLE_OWNER_ENUMERATION;
    }
    return VISIBLE_OWNER_NONE;
}

static bool anchor_state_valid(const struct anchor_model *anchor)
{
    enum visible_owner owner = anchor_owner(anchor);

    CHECK(!(anchor->survey_published &&
            anchor->enumeration.operation != PROTOCOL_RX_OPERATION_NONE),
          "published survey overlaps enumeration ownership");
    CHECK(!anchor->survey_published ||
              anchor->survey.operation == PROTOCOL_RX_OPERATION_SURVEY,
          "published survey lacks its exact lifecycle");
    CHECK(anchor->survey_published ||
              anchor->survey.operation == PROTOCOL_RX_OPERATION_NONE,
          "unpublished survey lifecycle leaked past an atomic handoff");
    if (owner == VISIBLE_OWNER_ENUMERATION) {
        CHECK(anchor->enumeration.mode ==
                  PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5,
              "enumeration owner is not continuously listening");
    } else if (owner == VISIBLE_OWNER_SURVEY) {
        CHECK(anchor->survey.mode == PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5 ||
                  anchor->survey.mode == PROTOCOL_RX_MODE_OWNED_RF_WORK,
              "survey owner is neither listening nor doing owned RF work");
    }
    return true;
}

static bool population_continuously_owned(
    const struct anchor_model anchors[DISCOVERY_ASSIGNMENT_MAX_HOPS],
    uint8_t anchor_count)
{
    for (uint8_t index = 0u; index < anchor_count; index++) {
        enum visible_owner owner;

        active_anchor_depth = anchors[index].depth;
        CHECK(anchor_state_valid(&anchors[index]),
              "invalid anchor ownership state");
        owner = anchor_owner(&anchors[index]);
        CHECK(owner != VISIBLE_OWNER_NONE,
              "anchor dropped to low duty inside an active wave");
        if (owner == VISIBLE_OWNER_ENUMERATION) {
            CHECK(anchors[index].enumeration.mode ==
                      PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5,
                  "enumeration anchor is not on continuous Channel 5");
        } else {
            CHECK(anchors[index].survey.mode ==
                      PROTOCOL_RX_MODE_CONTINUOUS_CHANNEL5,
                  "survey anchor is not on continuous Channel 5");
        }
    }
    return true;
}

static bool begin_here_i_am(struct anchor_model *anchor,
                            uint32_t now_ms,
                            bool survey_follows)
{
    uint32_t deadline_ms = now_ms + DISCOVERY_ASSIGNMENT_PREARM_HOLD_MS;

    CHECK(protocol_rx_lifecycle_begin(
              &anchor->enumeration,
              PROTOCOL_RX_OPERATION_ENUMERATION,
              ENUMERATION_EPOCH,
              now_ms,
              deadline_ms) == PROTOCOL_RX_BEGIN_ACCEPTED,
          "Here-I-Am did not establish enumeration RX ownership");
    CHECK(protocol_rx_downstream_activation_mark(
              &anchor->downstream,
              PROTOCOL_RX_OPERATION_ENUMERATION,
              ENUMERATION_EPOCH,
              now_ms,
              deadline_ms),
          "Here-I-Am did not bind downstream activation");
    anchor->prearmed = true;
    anchor->survey_follows = survey_follows;
    return anchor_state_valid(anchor);
}

static bool apply_claim(struct anchor_model *anchor, uint32_t now_ms)
{
    uint32_t deadline_ms = now_ms +
        DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS;

    CHECK(anchor->prearmed, "CLAIM arrived without Here-I-Am prearm");
    CHECK(!protocol_rx_downstream_activation_needs_wake(
              &anchor->downstream,
              PROTOCOL_RX_OPERATION_ENUMERATION,
              ENUMERATION_EPOCH,
              now_ms),
          "same-epoch CLAIM unexpectedly requires another wake train");
    CHECK(protocol_rx_lifecycle_begin(
              &anchor->enumeration,
              PROTOCOL_RX_OPERATION_ENUMERATION,
              ENUMERATION_EPOCH,
              now_ms,
              deadline_ms) == PROTOCOL_RX_BEGIN_DUPLICATE,
          "CLAIM did not join the exact prearmed lifecycle");
    CHECK(protocol_rx_lifecycle_set_deadline(
              &anchor->enumeration,
              PROTOCOL_RX_OPERATION_ENUMERATION,
              ENUMERATION_EPOCH,
              now_ms,
              deadline_ms),
          "CLAIM did not promote the prearm lease");
    CHECK(protocol_rx_downstream_activation_mark(
              &anchor->downstream,
              PROTOCOL_RX_OPERATION_ENUMERATION,
              ENUMERATION_EPOCH,
              now_ms,
              deadline_ms),
          "CLAIM did not promote downstream activation");
    anchor->prearmed = false;
    return anchor_state_valid(anchor);
}

static bool apply_table(struct anchor_model *anchor, uint32_t sequence)
{
    CHECK(anchor_owner(anchor) == VISIBLE_OWNER_ENUMERATION,
          "TABLE arrived without enumeration ownership");
    CHECK(sequence != 0u, "TABLE identity is empty");
    if (anchor->table_sequence != 0u) {
        CHECK(anchor->table_sequence == sequence,
              "TABLE replay changed immutable identity");
    } else {
        anchor->table_sequence = sequence;
    }
    return anchor_state_valid(anchor);
}

static bool finish_enumeration(struct anchor_model *anchor, uint32_t now_ms)
{
    CHECK(anchor->table_sequence == TABLE_SEQUENCE,
          "END did not match the committed TABLE");
    if (anchor->survey_follows) {
        CHECK(protocol_rx_lifecycle_set_deadline(
                  &anchor->enumeration,
                  PROTOCOL_RX_OPERATION_ENUMERATION,
                  ENUMERATION_EPOCH,
                  now_ms,
                  now_ms + SURVEY_ENUMERATION_HANDOFF_HOLD_MS),
              "survey END did not retain enumeration RX ownership");
        CHECK(protocol_rx_downstream_activation_mark(
                  &anchor->downstream,
                  PROTOCOL_RX_OPERATION_ENUMERATION,
                  ENUMERATION_EPOCH,
                  now_ms,
                  now_ms + SURVEY_ENUMERATION_HANDOFF_HOLD_MS),
              "survey END did not retain downstream activation");
        anchor->survey_handoff = true;
    } else {
        CHECK(protocol_rx_lifecycle_terminate(
                  &anchor->enumeration,
                  PROTOCOL_RX_OPERATION_ENUMERATION,
                  ENUMERATION_EPOCH),
              "ordinary END did not release enumeration RX ownership");
        CHECK(protocol_rx_downstream_activation_clear(
                  &anchor->downstream,
                  PROTOCOL_RX_OPERATION_ENUMERATION,
                  ENUMERATION_EPOCH),
              "ordinary END did not release downstream activation");
        anchor->table_sequence = 0u;
        anchor->survey_follows = false;
    }
    return anchor_state_valid(anchor);
}

static bool start_survey(struct anchor_model *anchor,
                         uint32_t assignment_epoch,
                         uint32_t survey_generation,
                         uint32_t now_ms,
                         uint32_t stop_ms)
{
    enum protocol_rx_begin_result begin;
    bool consumed = false;

    if (anchor->survey_published) {
        return anchor->survey.generation == survey_generation;
    }
    if (protocol_rx_lifecycle_expire(&anchor->enumeration, now_ms)) {
        anchor->prearmed = false;
        anchor->survey_handoff = false;
        anchor->survey_follows = false;
        anchor->table_sequence = 0u;
    }
    begin = protocol_rx_lifecycle_begin(
        &anchor->survey,
        PROTOCOL_RX_OPERATION_SURVEY,
        survey_generation,
        now_ms,
        stop_ms);
    if (begin != PROTOCOL_RX_BEGIN_ACCEPTED) {
        return false;
    }
    if (anchor->enumeration.operation ==
            PROTOCOL_RX_OPERATION_ENUMERATION &&
        anchor->enumeration.generation == assignment_epoch &&
        anchor->survey_follows && anchor->survey_handoff &&
        anchor->table_sequence == TABLE_SEQUENCE) {
        consumed = protocol_rx_lifecycle_terminate(
            &anchor->enumeration,
            PROTOCOL_RX_OPERATION_ENUMERATION,
            assignment_epoch);
    }
    if (!consumed) {
        CHECK(protocol_rx_lifecycle_terminate(
                  &anchor->survey,
                  PROTOCOL_RX_OPERATION_SURVEY,
                  survey_generation),
              "rejected START leaked staged survey ownership");
        return false;
    }
    anchor->survey_handoff = false;
    anchor->survey_follows = false;
    anchor->table_sequence = 0u;
    anchor->survey_published = true;
    return anchor_state_valid(anchor);
}

static bool finish_survey(struct anchor_model *anchor)
{
    CHECK(anchor->survey_published,
          "survey terminal arrived without published ownership");
    CHECK(protocol_rx_lifecycle_terminate(
              &anchor->survey,
              PROTOCOL_RX_OPERATION_SURVEY,
              SURVEY_GENERATION),
          "survey terminal did not release exact generation");
    anchor->survey_published = false;
    return anchor_state_valid(anchor);
}

static bool run_ordinary_enumeration(uint8_t topology_depth)
{
    struct anchor_model anchors[DISCOVERY_ASSIGNMENT_MAX_HOPS];
    uint32_t hia_origin_ms = TEST_ORIGIN_MS +
        (uint32_t)topology_depth * UINT32_C(1000000);
    uint32_t claim_origin_ms =
        activation_arrival_ms(hia_origin_ms, topology_depth) + 1u;
    uint32_t table_origin_ms = claim_origin_ms +
        ENUMERATION_RESPONSE_START_DELAY_MS +
        enumeration_response_duration_ms(topology_depth) + 1u;
    uint32_t end_origin_ms = compact_arrival_ms(
        table_origin_ms, topology_depth) + 1u;

    active_topology_depth = topology_depth;
    CHECK(compact_arrival_ms(claim_origin_ms, topology_depth) +
              ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS <=
              claim_origin_ms + ENUMERATION_RESPONSE_START_DELAY_MS,
          "shared response edge precedes worst-case CLAIM propagation");
    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        anchor_init(&anchors[depth - 1u], depth);
        CHECK(begin_here_i_am(&anchors[depth - 1u],
                              activation_arrival_ms(hia_origin_ms, depth),
                              false),
              "ordinary Here-I-Am failed");
    }
    CHECK(population_continuously_owned(anchors, topology_depth),
          "Here-I-Am did not align every reached anchor");

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        uint32_t arrival_ms = compact_arrival_ms(claim_origin_ms, depth);

        active_anchor_depth = depth;
        CHECK(arrival_ms < anchors[depth - 1u].enumeration.deadline_ms,
              "worst-case CLAIM missed the Here-I-Am prearm lease");
        CHECK(apply_claim(&anchors[depth - 1u], arrival_ms),
              "ordinary CLAIM failed");
        CHECK(population_continuously_owned(anchors, topology_depth),
              "CLAIM wave created a low-duty gap");
    }

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        CHECK(apply_table(&anchors[depth - 1u], TABLE_SEQUENCE),
              "ordinary TABLE failed");
        CHECK(population_continuously_owned(anchors, topology_depth),
              "TABLE wave created a low-duty gap");
    }

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        CHECK(finish_enumeration(
                  &anchors[depth - 1u],
                  compact_arrival_ms(end_origin_ms, depth)),
              "ordinary END failed");
        for (uint8_t index = 0u; index < topology_depth; index++) {
            active_anchor_depth = anchors[index].depth;
            CHECK(anchor_state_valid(&anchors[index]),
                  "END wave exposed invalid ownership");
            CHECK(index < depth ?
                      anchor_owner(&anchors[index]) == VISIBLE_OWNER_NONE :
                      anchor_owner(&anchors[index]) ==
                          VISIBLE_OWNER_ENUMERATION,
                  "END wave released the wrong depth");
        }
    }
    return true;
}

static bool run_enumeration_into_survey(uint8_t topology_depth)
{
    struct anchor_model anchors[DISCOVERY_ASSIGNMENT_MAX_HOPS];
    uint64_t shared_start_ms = 0u;
    uint64_t shared_plan_ms = 0u;
    uint32_t hia_origin_ms = TEST_ORIGIN_MS +
        (uint32_t)topology_depth * UINT32_C(1000000);
    uint32_t claim_origin_ms =
        activation_arrival_ms(hia_origin_ms, topology_depth) + 1u;
    uint32_t table_origin_ms = claim_origin_ms +
        ENUMERATION_RESPONSE_START_DELAY_MS +
        enumeration_response_duration_ms(topology_depth) + 1u;
    uint32_t end_origin_ms = compact_arrival_ms(
        table_origin_ms, topology_depth) + 1u;
    uint32_t start_origin_ms = end_origin_ms +
        SURVEY_ENUMERATION_HANDOFF_HOLD_MS - 1u;
    uint32_t start_stop_ms = start_origin_ms +
        SURVEY_INITIAL_SELF_EXPIRY_MS;
    uint32_t plan_origin_ms = start_origin_ms +
        SURVEY_HOST_PLAN_TIMEOUT_MS - 1u;
    uint32_t plan_stop_ms = plan_origin_ms + SURVEY_INITIAL_SELF_EXPIRY_MS;
    uint32_t abort_origin_ms;

    active_topology_depth = topology_depth;
    CHECK(compact_arrival_ms(claim_origin_ms, topology_depth) +
              ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS <=
              claim_origin_ms + ENUMERATION_RESPONSE_START_DELAY_MS,
          "survey response edge precedes worst-case CLAIM propagation");
    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        anchor_init(&anchors[depth - 1u], depth);
        CHECK(begin_here_i_am(&anchors[depth - 1u],
                              activation_arrival_ms(hia_origin_ms, depth),
                              true),
              "survey-enumeration Here-I-Am failed");
    }
    CHECK(population_continuously_owned(anchors, topology_depth),
          "survey-enumeration Here-I-Am did not align every anchor");

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        uint32_t arrival_ms = compact_arrival_ms(claim_origin_ms, depth);

        active_anchor_depth = depth;
        CHECK(arrival_ms < anchors[depth - 1u].enumeration.deadline_ms,
              "survey CLAIM missed the prearm lease");
        CHECK(apply_claim(&anchors[depth - 1u], arrival_ms),
              "survey CLAIM failed");
    }
    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        CHECK(apply_table(&anchors[depth - 1u], TABLE_SEQUENCE),
              "survey TABLE failed");
        CHECK(finish_enumeration(
                  &anchors[depth - 1u],
                  compact_arrival_ms(end_origin_ms, depth)),
              "survey END failed to create its handoff");
    }
    CHECK(population_continuously_owned(anchors, topology_depth),
          "survey END did not leave every anchor listening");

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        uint32_t arrival_ms = compact_arrival_ms(start_origin_ms, depth);
        uint32_t packet_age_ms = arrival_ms - start_origin_ms;
        uint64_t projected_start_ms = 0u;
        int64_t starts_in_ms = 0;

        active_anchor_depth = depth;
        CHECK(!protocol_rx_downstream_activation_needs_wake(
                  &anchors[depth - 1u].downstream,
                  PROTOCOL_RX_OPERATION_ENUMERATION,
                  ENUMERATION_EPOCH,
                  arrival_ms),
              "START lacks a live downstream survey-enumeration handoff");
        CHECK(enumeration_response_claim_start(
                  arrival_ms,
                  packet_age_ms,
                  survey_control_delivery_delay_ms(topology_depth),
                  &projected_start_ms,
                  &starts_in_ms),
              "START could not reconstruct the gateway clock edge");
        if (shared_start_ms == 0u) {
            shared_start_ms = projected_start_ms;
        }
        CHECK(projected_start_ms == shared_start_ms,
              "START message age produced a depth-dependent start edge");
        CHECK(starts_in_ms > 0,
              "START reached depth after its shared work edge");
        CHECK(start_survey(&anchors[depth - 1u],
                           ENUMERATION_EPOCH,
                           SURVEY_GENERATION,
                           arrival_ms,
                           start_stop_ms),
              "START did not atomically consume the enumeration handoff");
        CHECK(population_continuously_owned(anchors, topology_depth),
              "mixed START propagation created a low-duty ownership gap");
    }
    CHECK(shared_start_ms ==
              (uint64_t)start_origin_ms +
                  survey_control_delivery_delay_ms(topology_depth),
          "START shared edge is not relative to the gateway origin");

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        uint32_t arrival_ms = compact_arrival_ms(plan_origin_ms, depth);
        uint32_t packet_age_ms = arrival_ms - plan_origin_ms;
        uint64_t projected_plan_ms = 0u;
        int64_t starts_in_ms = 0;

        active_anchor_depth = depth;
        CHECK(enumeration_response_claim_start(
                  arrival_ms,
                  packet_age_ms,
                  survey_control_delivery_delay_ms(topology_depth),
                  &projected_plan_ms,
                  &starts_in_ms),
              "PLAN could not reconstruct the gateway clock edge");
        if (shared_plan_ms == 0u) {
            shared_plan_ms = projected_plan_ms;
        }
        CHECK(projected_plan_ms == shared_plan_ms && starts_in_ms > 0,
              "PLAN produced a late or depth-dependent work edge");
        CHECK(protocol_rx_lifecycle_set_deadline(
                  &anchors[depth - 1u].survey,
                  PROTOCOL_RX_OPERATION_SURVEY,
                  SURVEY_GENERATION,
                  arrival_ms,
                  plan_stop_ms),
              "PLAN did not retain exact survey ownership");
        CHECK(population_continuously_owned(anchors, topology_depth),
              "PLAN wave created a low-duty ownership gap");
    }

    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        CHECK(protocol_rx_lifecycle_rf_begin(
                  &anchors[depth - 1u].survey,
                  PROTOCOL_RX_OPERATION_SURVEY,
                  SURVEY_GENERATION),
              "survey work did not acquire exact RF ownership");
        CHECK(anchor_state_valid(&anchors[depth - 1u]),
              "owned survey RF work invalidated lifecycle state");
        CHECK(protocol_rx_lifecycle_rf_end(
                  &anchors[depth - 1u].survey,
                  PROTOCOL_RX_OPERATION_SURVEY,
                  SURVEY_GENERATION),
              "survey work did not return to continuous Channel 5");
    }
    CHECK(population_continuously_owned(anchors, topology_depth),
          "survey work left an anchor outside continuous RX");

    abort_origin_ms = (uint32_t)shared_plan_ms + 1u;
    for (uint8_t depth = 1u; depth <= topology_depth; depth++) {
        active_anchor_depth = depth;
        CHECK(compact_arrival_ms(abort_origin_ms, depth) < plan_stop_ms,
              "ABORT arrived after the survey self-stop");
        CHECK(finish_survey(&anchors[depth - 1u]),
              "ABORT did not release survey ownership");
        for (uint8_t index = 0u; index < topology_depth; index++) {
            active_anchor_depth = anchors[index].depth;
            CHECK(anchor_state_valid(&anchors[index]),
                  "ABORT wave exposed invalid ownership");
            CHECK(index < depth ?
                      anchor_owner(&anchors[index]) == VISIBLE_OWNER_NONE :
                      anchor_owner(&anchors[index]) == VISIBLE_OWNER_SURVEY,
                  "ABORT wave released the wrong depth");
        }
    }
    return true;
}

static bool test_rejected_starts_preserve_or_release_the_right_owner(void)
{
    struct anchor_model anchor;
    uint32_t hia_ms = TEST_ORIGIN_MS;
    uint32_t claim_ms = hia_ms + 1000u;
    uint32_t end_ms = claim_ms + 1000u;
    uint32_t valid_start_ms = end_ms + 1000u;

    active_topology_depth = DISCOVERY_ASSIGNMENT_MAX_HOPS;
    for (uint8_t depth = 1u;
         depth <= DISCOVERY_ASSIGNMENT_MAX_HOPS;
         depth++) {
        active_anchor_depth = depth;
        anchor_init(&anchor, depth);
        CHECK(begin_here_i_am(&anchor, hia_ms, true),
              "boundary Here-I-Am failed");
        CHECK(apply_claim(&anchor, claim_ms), "boundary CLAIM failed");
        CHECK(apply_table(&anchor, TABLE_SEQUENCE),
              "boundary TABLE failed");

        CHECK(!start_survey(&anchor,
                            ENUMERATION_EPOCH,
                            SURVEY_GENERATION,
                            valid_start_ms,
                            valid_start_ms + SURVEY_INITIAL_SELF_EXPIRY_MS),
              "START before END consumed an incomplete enumeration");
        CHECK(anchor_owner(&anchor) == VISIBLE_OWNER_ENUMERATION,
              "rejected early START released enumeration ownership");
        CHECK(anchor_state_valid(&anchor),
              "rejected early START leaked staged ownership");

        CHECK(finish_enumeration(&anchor, end_ms),
              "boundary END failed");
        CHECK(!start_survey(&anchor,
                            ENUMERATION_EPOCH + 1u,
                            SURVEY_GENERATION,
                            valid_start_ms,
                            valid_start_ms + SURVEY_INITIAL_SELF_EXPIRY_MS),
              "wrong assignment epoch consumed a survey handoff");
        CHECK(anchor_owner(&anchor) == VISIBLE_OWNER_ENUMERATION,
              "wrong-epoch START damaged the live handoff");
        CHECK(anchor_state_valid(&anchor),
              "wrong-epoch START leaked staged ownership");

        CHECK(start_survey(&anchor,
                           ENUMERATION_EPOCH,
                           SURVEY_GENERATION,
                           valid_start_ms,
                           valid_start_ms + SURVEY_INITIAL_SELF_EXPIRY_MS),
              "exact START did not consume the handoff");
        CHECK(start_survey(&anchor,
                           ENUMERATION_EPOCH,
                           SURVEY_GENERATION,
                           valid_start_ms + 1u,
                           valid_start_ms + SURVEY_INITIAL_SELF_EXPIRY_MS),
              "exact duplicate START was not idempotent");
        CHECK(!start_survey(&anchor,
                            ENUMERATION_EPOCH,
                            SURVEY_GENERATION + 1u,
                            valid_start_ms + 1u,
                            valid_start_ms + SURVEY_INITIAL_SELF_EXPIRY_MS),
              "new survey generation stole live survey ownership");
        CHECK(anchor_owner(&anchor) == VISIBLE_OWNER_SURVEY,
              "rejected generation changed the visible owner");
        CHECK(finish_survey(&anchor),
              "boundary survey terminal failed");

        anchor_init(&anchor, depth);
        CHECK(begin_here_i_am(&anchor, hia_ms, true),
              "expiry Here-I-Am failed");
        CHECK(apply_claim(&anchor, claim_ms), "expiry CLAIM failed");
        CHECK(apply_table(&anchor, TABLE_SEQUENCE),
              "expiry TABLE failed");
        CHECK(finish_enumeration(&anchor, end_ms), "expiry END failed");
        CHECK(!start_survey(
                  &anchor,
                  ENUMERATION_EPOCH,
                  SURVEY_GENERATION,
                  end_ms + SURVEY_ENUMERATION_HANDOFF_HOLD_MS,
                  end_ms + SURVEY_ENUMERATION_HANDOFF_HOLD_MS +
                      SURVEY_INITIAL_SELF_EXPIRY_MS),
              "START at the exact handoff deadline was accepted");
        CHECK(anchor_owner(&anchor) == VISIBLE_OWNER_NONE,
              "expired handoff did not return to low duty");
        CHECK(anchor_state_valid(&anchor),
              "expired START leaked ownership");
    }
    return true;
}

int main(void)
{
    for (uint8_t topology_depth = 1u;
         topology_depth <= DISCOVERY_ASSIGNMENT_MAX_HOPS;
         topology_depth++) {
        if (!run_ordinary_enumeration(topology_depth) ||
            !run_enumeration_into_survey(topology_depth)) {
            return 1;
        }
    }
    if (!test_rejected_starts_preserve_or_release_the_right_owner()) {
        return 1;
    }
    if (failures != 0) {
        fprintf(stderr, "state-alignment failures=%d\n", failures);
        return 1;
    }
    printf("enumeration/survey state alignment passed for depths 1..%u\n",
           DISCOVERY_ASSIGNMENT_MAX_HOPS);
    return 0;
}
