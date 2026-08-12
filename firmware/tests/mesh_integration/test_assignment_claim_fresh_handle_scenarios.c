#include "discovery_assignment.h"
#include "node_comm.h"
#include "semantic_digest.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct claim_identity {
    uint32_t session_id;
    uint16_t sequence;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
};

struct claim_owner {
    struct claim_identity identity;
    uint64_t absolute_deadline_ms;
    uint64_t next_attempt_not_before_ms;
    uint32_t generation;
    uint8_t hop_count;
    uint8_t terminal_retry_count;
    uint8_t handles_created;
    bool active;
};

static int failures;

#define CHECK(expression, message) do {                                      \
    if (!(expression)) {                                                     \
        fprintf(stderr, "FAIL line=%d %s\n", __LINE__, (message));          \
        failures++;                                                          \
        return false;                                                        \
    }                                                                        \
} while (0)

static bool same_identity(const struct claim_identity *left,
                          const struct claim_identity *right)
{
    return left != NULL && right != NULL &&
           left->session_id == right->session_id &&
           left->sequence == right->sequence &&
           memcmp(left->semantic_digest, right->semantic_digest,
                  sizeof(left->semantic_digest)) == 0;
}

static bool exact_late_ack_matches(const struct claim_owner *owner,
                                   const struct claim_identity *identity,
                                   uint64_t now_ms)
{
    return owner != NULL && owner->active &&
           now_ms < owner->absolute_deadline_ms &&
           same_identity(&owner->identity, identity);
}

static bool service_terminal(struct claim_owner *owner,
                             enum node_comm_terminal_reason reason,
                             uint64_t now_ms,
                             uint32_t random_value)
{
    uint32_t retry_ms;

    if (owner == NULL || !owner->active ||
        reason == NODE_COMM_TERMINAL_DELIVERED ||
        reason == NODE_COMM_TERMINAL_CANCELLED ||
        owner->terminal_retry_count >=
            DISCOVERY_ASSIGNMENT_CLAIM_FAST_HANDLE_RETRIES) {
        if (owner != NULL) {
            owner->active = false;
        }
        return false;
    }
    retry_ms = discovery_assignment_retry_backoff_ms(
        owner->terminal_retry_count, random_value);
    owner->terminal_retry_count++;
    owner->handles_created++;
    owner->next_attempt_not_before_ms = now_ms + retry_ms;
    owner->absolute_deadline_ms = discovery_assignment_response_deadline_ms(
        now_ms, retry_ms, owner->hop_count);
    return true;
}

static struct claim_owner owner_at_first_handle(void)
{
    struct claim_owner owner = {
        .identity = {
            .session_id = UINT32_C(0x10203040),
            .sequence = UINT16_C(0x5060),
        },
        .absolute_deadline_ms = UINT64_C(41000),
        .generation = 7u,
        .hop_count = 2u,
        .handles_created = 1u,
        .active = true,
    };

    for (size_t i = 0u; i < sizeof(owner.identity.semantic_digest); i++) {
        owner.identity.semantic_digest[i] = (uint8_t)(i + 1u);
    }
    return owner;
}

static bool test_late_exact_ack_matches_last_allowed_handle(void)
{
    struct claim_owner owner = owner_at_first_handle();
    struct claim_identity wrong = owner.identity;
    uint64_t first_deadline = owner.absolute_deadline_ms;
    uint64_t second_deadline;

    CHECK(service_terminal(&owner, NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
                           first_deadline, 99u),
          "first terminal expiry did not create the first fresh CLAIM handle");
    CHECK(owner.active && owner.handles_created == 2u &&
              owner.terminal_retry_count == 1u,
          "first fresh CLAIM owner/count was not retained");
    CHECK(owner.next_attempt_not_before_ms == first_deadline + 199u,
          "first fresh CLAIM did not use the round-zero worst backoff");
    CHECK(owner.absolute_deadline_ms > first_deadline,
          "first fresh CLAIM reused the expired absolute deadline");

    second_deadline = owner.absolute_deadline_ms;
    CHECK(service_terminal(&owner, NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
                           second_deadline, 199u),
          "second terminal expiry did not create the final CLAIM handle");
    CHECK(owner.active && owner.handles_created == 3u &&
              owner.terminal_retry_count == 2u,
          "final CLAIM owner/count was not retained");
    CHECK(owner.next_attempt_not_before_ms == second_deadline + 399u,
          "final CLAIM did not use the round-one worst backoff");
    CHECK(DISCOVERY_ASSIGNMENT_CLAIM_FAST_RETRY_BACKOFF_MAX_MS ==
              199u + 399u,
          "CLAIM retry budget does not cover both bounded backoffs");
    CHECK(exact_late_ack_matches(
              &owner, &owner.identity, owner.absolute_deadline_ms - 1u),
          "exact late ACK could not match the final same-identity owner");

    wrong.semantic_digest[0] ^= UINT8_C(0x80);
    CHECK(!exact_late_ack_matches(
              &owner, &wrong, owner.absolute_deadline_ms - 1u),
          "wrong-digest ACK matched final CLAIM custody");
    wrong = owner.identity;
    wrong.session_id++;
    CHECK(!exact_late_ack_matches(
              &owner, &wrong, owner.absolute_deadline_ms - 1u),
          "wrong-session ACK matched final CLAIM custody");

    CHECK(!service_terminal(&owner, NODE_COMM_TERMINAL_DELIVERED,
                            owner.absolute_deadline_ms - 1u, UINT32_MAX),
          "delivered final CLAIM incorrectly created another handle");
    CHECK(!owner.active && owner.handles_created == 3u,
          "delivered final CLAIM retained redundant custody");
    return true;
}

static bool test_terminal_failure_after_finite_bound(void)
{
    struct claim_owner owner = owner_at_first_handle();

    CHECK(service_terminal(&owner, NODE_COMM_TERMINAL_DEADLINE_EXPIRED,
                           owner.absolute_deadline_ms, UINT32_MAX),
          "first expiry did not create the second total handle");
    CHECK(service_terminal(&owner, NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
                           owner.absolute_deadline_ms, UINT32_MAX),
          "second expiry did not create the third total handle");

    CHECK(!service_terminal(&owner, NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED,
                            owner.absolute_deadline_ms, 0u),
          "terminal CLAIM incorrectly created a fourth total handle");
    CHECK(!owner.active && owner.handles_created == 3u &&
              owner.terminal_retry_count == 2u,
          "retry cap did not retire CLAIM custody after three total handles");
    return true;
}

static bool test_cancellation_and_supersession_win(void)
{
    struct claim_owner cancelled = owner_at_first_handle();
    struct claim_owner delivered = owner_at_first_handle();
    struct claim_owner superseded = owner_at_first_handle();
    uint32_t old_generation = superseded.generation;

    CHECK(!service_terminal(&cancelled, NODE_COMM_TERMINAL_CANCELLED,
                            cancelled.absolute_deadline_ms, UINT32_MAX),
          "explicit cancellation incorrectly created a replacement handle");
    CHECK(!cancelled.active && cancelled.handles_created == 1u,
          "cancelled CLAIM retained active fresh-handle custody");

    CHECK(!service_terminal(&delivered, NODE_COMM_TERMINAL_DELIVERED,
                            delivered.absolute_deadline_ms, 99u),
          "successful delivery incorrectly created a replacement handle");
    CHECK(!delivered.active && delivered.handles_created == 1u,
          "delivered CLAIM retained redundant fresh-handle custody");

    superseded.generation++;
    superseded.active = false;
    CHECK(superseded.generation != old_generation &&
              !exact_late_ack_matches(
                  &superseded, &superseded.identity,
                  superseded.absolute_deadline_ms - 1u),
          "superseded generation accepted an old exact ACK");
    return true;
}

int main(void)
{
    if (!test_late_exact_ack_matches_last_allowed_handle() ||
        !test_terminal_failure_after_finite_bound() ||
        !test_cancellation_and_supersession_win()) {
        return EXIT_FAILURE;
    }
    printf("PASS assignment_claim_fresh_handle handles=3 retries=2\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
