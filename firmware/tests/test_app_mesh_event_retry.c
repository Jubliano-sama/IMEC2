#include "app_mesh_event_retry.h"
#include "mesh.h"

#include "protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOCAL_ID UINT64_C(0x1111111111111111)
#define PEER_ID UINT64_C(0x2222222222222222)
#define EVENT_INTERVAL_MS 80u

static struct app_mesh_event_request_identity request_identity(
    uint64_t source_id,
    uint32_t session_id,
    uint16_t sequence,
    const uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_event_request_identity request = {
        .source_id = source_id,
        .session_id = session_id,
        .sequence = sequence,
        .message_type = MSG_MESH_EVENT_PROPOSE,
    };

    assert(app_mesh_event_payload_digest(payload, payload_len,
                                         request.payload_digest));
    return request;
}

static struct app_mesh_rf_retry_key retry_key(uint32_t session_id,
                                               uint16_t sequence,
                                               uint8_t operation)
{
    return (struct app_mesh_rf_retry_key) {
        .source_id = LOCAL_ID,
        .destination_id = PEER_ID,
        .session_id = session_id,
        .sequence = sequence,
        .message_type = operation == APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT ?
                        MSG_MESH_EVENT_ACCEPT : MSG_MESH_EVENT_PROPOSE,
        .operation = operation,
    };
}

static void assert_event_timing_equal(
    const struct mesh_event_timing *actual,
    const struct mesh_event_timing *expected)
{
    assert(actual != NULL);
    assert(expected != NULL);
    assert(actual->mesh_channel == expected->mesh_channel);
    assert(actual->event_interval_ms == expected->event_interval_ms);
    assert(actual->event_window_ms == expected->event_window_ms);
    assert(actual->next_event_time_ms == expected->next_event_time_ms);
    assert(actual->event_counter == expected->event_counter);
    assert(actual->guard_ms == expected->guard_ms);
    assert(actual->peer_clock_skew_estimate_ppm ==
           expected->peer_clock_skew_estimate_ppm);
    assert(actual->max_missed_events == expected->max_missed_events);
    assert(actual->missed_event_count == expected->missed_event_count);
    assert(actual->supervision_timeout_ms ==
           expected->supervision_timeout_ms);
    assert(actual->last_successful_ch9_event_ms ==
           expected->last_successful_ch9_event_ms);
    assert(actual->local_tx_on_even_events ==
           expected->local_tx_on_even_events);
    assert(actual->route_fresh == expected->route_fresh);
    assert(actual->timing_fresh == expected->timing_fresh);
    assert(actual->fallback_required == expected->fallback_required);
}

static void test_counterphase_shift_preserves_frozen_proposal_shape(void)
{
    enum {
        EVENT_INTERVAL = 520u,
        COUNTERPHASE_SHIFT = EVENT_INTERVAL / 2u,
    };
    const struct mesh_event_timing proposed = {
        .mesh_channel = MESH_EVENT_CHANNEL,
        .event_interval_ms = EVENT_INTERVAL,
        .event_window_ms = 120u,
        .next_event_time_ms = 10000u,
        .event_counter = UINT32_C(0x13579bdf),
        .guard_ms = 60u,
        .peer_clock_skew_estimate_ppm = 17,
        .max_missed_events = 8u,
        .missed_event_count = 2u,
        .supervision_timeout_ms = 300000u,
        .last_successful_ch9_event_ms = 10000u,
        .local_tx_on_even_events = true,
        .route_fresh = true,
        .timing_fresh = true,
    };
    struct mesh_event_timing shifted = proposed;
    struct mesh_event_timing expected = proposed;
    struct mesh_event_timing invalid;

    expected.next_event_time_ms += COUNTERPHASE_SHIFT;
    expected.last_successful_ch9_event_ms += COUNTERPHASE_SHIFT;
    assert(app_mesh_event_timing_apply_phase_shift(
        &shifted, COUNTERPHASE_SHIFT));
    assert_event_timing_equal(&shifted, &expected);

    invalid = proposed;
    assert(!app_mesh_event_timing_apply_phase_shift(
        &invalid, EVENT_INTERVAL));
    assert_event_timing_equal(&invalid, &proposed);
    invalid.event_interval_ms = 0u;
    assert(!app_mesh_event_timing_apply_phase_shift(&invalid, 0u));
    assert(!app_mesh_event_timing_apply_phase_shift(
        NULL, COUNTERPHASE_SHIFT));
}

static void test_duplicate_conflict_and_busy_are_distinct(void)
{
    const uint8_t payload[] = {1u, 2u, 3u, 4u};
    const uint8_t conflict_payload[] = {1u, 2u, 3u, 5u};
    struct app_mesh_event_request_identity request = request_identity(
        PEER_ID, 0x12345678u, 19u, payload, sizeof(payload));
    struct app_mesh_event_request_identity conflict = request_identity(
        PEER_ID, 0x12345678u, 19u,
        conflict_payload, sizeof(conflict_payload));
    struct app_mesh_event_request_identity different = request_identity(
        PEER_ID, 0x12345678u, 20u, payload, sizeof(payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_event_retry_state state = {0};

    assert(app_mesh_event_retry_match(&state, PEER_ID, &request) ==
           APP_MESH_EVENT_REQUEST_NEW);
    assert(app_mesh_event_retry_begin(&state,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      1000u,
                                      10000u,
                                      EVENT_INTERVAL_MS,
                                      10u) == 0);
    assert(app_mesh_event_retry_match(&state, PEER_ID, &request) ==
           APP_MESH_EVENT_REQUEST_DUPLICATE);
    assert(app_mesh_event_retry_match(&state, PEER_ID, &conflict) ==
           APP_MESH_EVENT_REQUEST_CONFLICT);
    assert(app_mesh_event_retry_match(&state, PEER_ID, &different) ==
           APP_MESH_EVENT_REQUEST_BUSY);
    assert(app_mesh_event_retry_match(&state, PEER_ID + 1u, &request) ==
           APP_MESH_EVENT_REQUEST_BUSY);
}

static void test_old_fnv_collision_is_a_digest_conflict(void)
{
    /*
     * These different byte strings both produce FNV-1a 0xd5d16ac6. The prior
     * 32-bit identity accepted the second request as an exact duplicate.
     */
    const uint8_t first_payload[] = {
        0xd6u, 0x69u, 0x98u, 0x8du, 0x5eu, 0xc8u, 0x75u, 0xfeu,
    };
    const uint8_t collision_payload[] = {
        0x61u, 0xcdu, 0x17u, 0x2au, 0xdeu, 0x66u, 0xfeu, 0x0au,
    };
    struct app_mesh_event_request_identity first = request_identity(
        PEER_ID, UINT32_C(0x12345679), 20u, first_payload,
        sizeof(first_payload));
    struct app_mesh_event_request_identity collision = request_identity(
        PEER_ID, first.session_id, first.sequence, collision_payload,
        sizeof(collision_payload));
    struct app_mesh_rf_retry_key key = retry_key(
        first.session_id, first.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_event_retry_state state = {0};

    assert(!app_mesh_event_request_payload_equal(&first, &collision));
    assert(app_mesh_event_retry_begin(&state, PEER_ID, &first, &key, 1000u,
                                      10000u, EVENT_INTERVAL_MS, 10u) == 0);
    assert(app_mesh_event_retry_match(&state, PEER_ID, &collision) ==
           APP_MESH_EVENT_REQUEST_CONFLICT);
}

static void test_accept_correlates_with_exact_and_stable_release_identities(void)
{
    const uint8_t payload[] = {0x31u, 0x32u, 0x33u};
    struct app_mesh_event_request_identity request = request_identity(
        LOCAL_ID, 0x12345678u, 19u, payload, sizeof(payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    struct app_mesh_event_retry_state proposal = {0};

    assert(app_mesh_event_retry_begin(&proposal,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      1000u,
                                      10000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID,
                                          PEER_ID,
                                          request.session_id,
                                          request.sequence,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_EXACT);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID,
                                          PEER_ID,
                                          0x87654321u,
                                          77u,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_LEGACY);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID,
                                          PEER_ID,
                                          0u,
                                          77u,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_REJECT);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID,
                                          PEER_ID,
                                          0x87654321u,
                                          0u,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_REJECT);

    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID + 1u,
                                          LOCAL_ID,
                                          PEER_ID + 1u,
                                          request.session_id,
                                          request.sequence,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_REJECT);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID + 1u,
                                          PEER_ID,
                                          request.session_id,
                                          request.sequence,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_REJECT);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID,
                                          PEER_ID,
                                          request.session_id,
                                          request.sequence,
                                          false) ==
           APP_MESH_EVENT_ACCEPT_REJECT);
    app_mesh_event_retry_clear(&proposal);
    assert(app_mesh_event_accept_classify(&proposal,
                                          PEER_ID,
                                          LOCAL_ID,
                                          PEER_ID,
                                          request.session_id,
                                          request.sequence,
                                          true) ==
           APP_MESH_EVENT_ACCEPT_REJECT);
}

static void test_delayed_legacy_accept_cannot_complete_newer_proposal(void)
{
    const uint8_t payload[] = {0x41u, 0x42u, 0x43u};
    const uint32_t old_session = UINT32_C(0x2468ace0);
    const uint32_t new_session = UINT32_C(0x13579bdf);
    struct app_mesh_event_request_identity request = request_identity(
        LOCAL_ID, new_session, 20u, payload, sizeof(payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    struct app_mesh_event_retry_state proposal = {0};
    struct mesh_event_timing old_timing = {
        .mesh_channel = MESH_EVENT_CHANNEL,
        .event_interval_ms = EVENT_INTERVAL_MS,
        .event_window_ms = 20u,
        .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 10,
        .max_missed_events = 4u,
        .supervision_timeout_ms = 1000u,
    };
    struct mesh_event_timing new_timing = old_timing;
    struct mesh_event_timing delayed_accept;
    struct mesh_event_timing current_accept;

    assert(mesh_event_timing_bind_proposal_session(&old_timing, old_session));
    assert(old_timing.event_counter == old_session);
    assert(mesh_event_timing_local_tx_slot(&old_timing));
    assert(mesh_event_timing_bind_proposal_session(&new_timing, new_session));
    assert(new_timing.event_counter == new_session);
    assert(mesh_event_timing_local_tx_slot(&new_timing));
    assert(!mesh_event_timing_bind_proposal_session(&new_timing, 0u));

    delayed_accept = old_timing;
    current_accept = new_timing;
    assert(app_mesh_event_retry_begin(&proposal,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      1000u,
                                      10000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);

    assert(!app_mesh_event_accept_timing_compatible(&delayed_accept,
                                                    &new_timing));
    assert(app_mesh_event_accept_classify(
               &proposal,
               PEER_ID,
               LOCAL_ID,
               PEER_ID,
               UINT32_C(0x87654321),
               77u,
               app_mesh_event_accept_timing_compatible(&delayed_accept,
                                                       &new_timing)) ==
           APP_MESH_EVENT_ACCEPT_REJECT);

    /*
     * The last stable release uses a fresh ACCEPT packet identity, but echoes
     * the proposal timing. Its current-session counter remains compatible.
     */
    assert(app_mesh_event_accept_timing_compatible(&current_accept,
                                                   &new_timing));
    assert(app_mesh_event_accept_classify(
               &proposal,
               PEER_ID,
               LOCAL_ID,
               PEER_ID,
               UINT32_C(0x87654321),
               77u,
               app_mesh_event_accept_timing_compatible(&current_accept,
                                                       &new_timing)) ==
           APP_MESH_EVENT_ACCEPT_LEGACY);
}

static void test_accept_phase_is_compatibility_agnostic_and_parity_can_be_bound(void)
{
    struct mesh_event_timing proposed = {
        .mesh_channel = MESH_EVENT_CHANNEL,
        .next_event_time_ms = 1000u,
        .event_interval_ms = EVENT_INTERVAL_MS,
        .event_window_ms = 20u,
        .event_counter = UINT32_C(0x13579bdf),
        .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 10,
        .max_missed_events = 4u,
        .supervision_timeout_ms = 1000u,
        .local_tx_on_even_events = false,
    };
    struct mesh_event_timing accepted = proposed;
    uint32_t accepted_next_event_time_ms;

    /*
     * The wire validator deliberately excludes absolute phase from ACCEPT
     * compatibility because relative-time decoding moves with response
     * latency.  The app still restores the retained successful PROPOSE phase;
     * this pure helper only proves the immutable shape and parity contract.
     */
    accepted.next_event_time_ms += EVENT_INTERVAL_MS;
    accepted.local_tx_on_even_events = true;
    assert(accepted.next_event_time_ms != proposed.next_event_time_ms);
    assert(app_mesh_event_accept_timing_compatible(&accepted, &proposed));

    accepted_next_event_time_ms = accepted.next_event_time_ms;
    mesh_event_timing_set_local_first_slot_tx(&accepted, true);

    assert(accepted.next_event_time_ms == accepted_next_event_time_ms);
    assert(mesh_event_timing_local_tx_slot(&accepted));
}

static void test_legacy_accept_survives_wire_validation_before_app_correlation(void)
{
    const uint32_t proposal_session = UINT32_C(0x13579bdf);
    const uint16_t proposal_sequence = 20u;
    const uint8_t proposal_payload[] = {0x51u, 0x52u, 0x53u};
    struct app_mesh_event_request_identity request = request_identity(
        LOCAL_ID,
        proposal_session,
        proposal_sequence,
        proposal_payload,
        sizeof(proposal_payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id,
        request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    struct app_mesh_event_retry_state proposal = {0};
    struct mesh_event_params params = {
        .event_interval_ms = EVENT_INTERVAL_MS,
        .event_window_ms = 20u,
        .first_event_time_ms = 2000u,
        .guard_ms = 5u,
        .peer_clock_skew_estimate_ppm = 10,
        .max_missed_events = 4u,
        .supervision_timeout_ms = 1000u,
    };
    struct mesh_event_timing proposed = {0};
    struct mesh_event_timing decoded = {0};
    struct proto_packet accept = {0};
    uint8_t accept_payload[96];
    size_t accept_payload_len = 0u;

    assert(mesh_event_timing_negotiate(&proposed, &params, true) == PROTO_OK);
    assert(mesh_event_timing_bind_proposal_session(&proposed,
                                                   proposal_session));
    assert(mesh_append_event_timing_tlvs_at(accept_payload,
                                            sizeof(accept_payload),
                                            &accept_payload_len,
                                            &proposed,
                                            1000u) == PROTO_OK);
    assert(app_mesh_event_retry_begin(&proposal,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      1000u,
                                      7000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);

    assert(mesh_init_event_control(&accept,
                                   MSG_MESH_EVENT_ACCEPT,
                                   PEER_ID,
                                   LOCAL_ID,
                                   proposal_session,
                                   proposal_sequence,
                                   (uint8_t)accept_payload_len) == PROTO_OK);
    assert(mesh_packet_rx_envelope_validate(
               &accept,
               accept_payload,
               accept_payload_len,
               PEER_ID,
               LOCAL_ID,
               LOCAL_ID,
               UWB_CHANNEL_WAKE_CONTACT,
               false) == PROTO_OK);
    assert(mesh_event_timing_from_tlvs_at(&decoded,
                                          accept_payload,
                                          accept_payload_len,
                                          1000u,
                                          true) == PROTO_OK);
    assert(app_mesh_event_accept_classify(
               &proposal,
               accept.src_id,
               accept.dst_id,
               PEER_ID,
               accept.session_id,
               accept.seq,
               app_mesh_event_accept_timing_compatible(&decoded,
                                                       &proposed)) ==
           APP_MESH_EVENT_ACCEPT_EXACT);

    accept.session_id = UINT32_C(0x87654321);
    accept.seq = 77u;
    assert(mesh_packet_rx_envelope_validate(
               &accept,
               accept_payload,
               accept_payload_len,
               PEER_ID,
               LOCAL_ID,
               LOCAL_ID,
               UWB_CHANNEL_WAKE_CONTACT,
               false) == PROTO_OK);
    assert(app_mesh_event_accept_classify(
               &proposal,
               accept.src_id,
               accept.dst_id,
               PEER_ID,
               accept.session_id,
               accept.seq,
               app_mesh_event_accept_timing_compatible(&decoded,
                                                       &proposed)) ==
           APP_MESH_EVENT_ACCEPT_LEGACY);

    decoded.event_counter++;
    assert(app_mesh_event_accept_classify(
               &proposal,
               accept.src_id,
               accept.dst_id,
               PEER_ID,
               accept.session_id,
               accept.seq,
               app_mesh_event_accept_timing_compatible(&decoded,
                                                       &proposed)) ==
           APP_MESH_EVENT_ACCEPT_REJECT);
}

static void test_pre_rf_and_actual_failures_share_backoff_without_identity_loss(void)
{
    static const uint32_t entropy[] = {
        0x13579bdfu, 0x2468ace0u, 0xdeadbeefu, 0x55aa55aau,
    };
    const uint8_t payload[] = {9u, 8u, 7u};
    struct app_mesh_event_request_identity request = request_identity(
        LOCAL_ID, 77u, 31u, payload, sizeof(payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    struct app_mesh_event_retry_state state = {0};
    uint32_t now_ms = 1000u;

    assert(app_mesh_event_retry_begin(&state,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      now_ms,
                                      100000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    for (size_t i = 0u; i < sizeof(entropy) / sizeof(entropy[0]); i++) {
        uint32_t delay_ms = 0u;
        bool rf_started = (i & 1u) != 0u;

        assert(app_mesh_event_retry_note_failure(
            &state,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            now_ms,
            entropy[i],
            rf_started,
            &delay_ms));
        assert(delay_ms > 0u);
        assert(state.retry_due_ms == now_ms + delay_ms);
        assert(state.request.session_id == request.session_id);
        assert(state.request.sequence == request.sequence);
        assert(state.retry_key.session_id == key.session_id);
        assert(state.retry_key.sequence == key.sequence);
        assert(!state.timing_installed);
        assert(!state.response_sent);
        now_ms = state.retry_due_ms;
        assert(app_mesh_event_retry_due(&state, now_ms));
    }
    assert(state.retry.retry_round == 4u);
    assert(state.pre_rf_deferrals == 2u);
    assert(state.rf_attempts == 2u);
}

static void test_duplicate_proposal_reuses_response_and_installs_once(void)
{
    const uint8_t proposal[] = {0x41u, 0x42u, 0x43u};
    struct app_mesh_event_request_identity request = request_identity(
        PEER_ID, 0x87654321u, 0x1234u, proposal, sizeof(proposal));
    struct app_mesh_rf_retry_key response = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_event_retry_state state = {0};
    uint32_t delay_ms = 0u;

    assert(app_mesh_event_retry_begin(&state,
                                      PEER_ID,
                                      &request,
                                      &response,
                                      500u,
                                      10000u,
                                      EVENT_INTERVAL_MS,
                                      10u) == 0);
    assert(app_mesh_event_retry_claim_timing_install(&state));
    assert(!app_mesh_event_retry_claim_timing_install(&state));
    app_mesh_event_retry_note_send_success(&state);
    assert(state.response_sent);
    assert(state.retry_key.session_id == request.session_id);
    assert(state.retry_key.sequence == request.sequence);
    assert(app_mesh_event_retry_match(&state, PEER_ID, &request) ==
           APP_MESH_EVENT_REQUEST_DUPLICATE);
    assert(app_mesh_event_retry_note_failure(
        &state,
        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
        700u,
        0xabcdef01u,
        true,
        &delay_ms));
    assert(delay_ms > 0u);
    assert(state.retry_key.session_id == request.session_id);
    assert(state.retry_key.sequence == request.sequence);
    assert(!app_mesh_event_retry_claim_timing_install(&state));
}

static void test_fifty_simultaneous_proposals_diverge(void)
{
    enum { NODE_COUNT = 50 };
    struct app_mesh_event_retry_state states[NODE_COUNT];
    uint32_t delays[NODE_COUNT];
    size_t unique_delays = 0u;

    memset(states, 0, sizeof(states));
    for (size_t i = 0u; i < NODE_COUNT; i++) {
        uint8_t payload[] = {(uint8_t)i, (uint8_t)(i * 3u), 0x5au};
        uint64_t source_id = LOCAL_ID + i + 1u;
        struct app_mesh_event_request_identity request = request_identity(
            source_id, 0x1000u + (uint32_t)i, (uint16_t)(i + 1u),
            payload, sizeof(payload));
        struct app_mesh_rf_retry_key key = {
            .source_id = source_id,
            .destination_id = PEER_ID,
            .session_id = request.session_id,
            .sequence = request.sequence,
            .message_type = MSG_MESH_EVENT_PROPOSE,
            .operation = APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE,
        };

        assert(app_mesh_event_retry_begin(&states[i],
                                          PEER_ID,
                                          &request,
                                          &key,
                                          100u,
                                          10000u,
                                          EVENT_INTERVAL_MS,
                                          0u) == 0);
        assert(app_mesh_event_retry_note_failure(
            &states[i],
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            100u,
            0x31415926u,
            false,
            &delays[i]));
    }
    for (size_t i = 0u; i < NODE_COUNT; i++) {
        bool seen = false;

        for (size_t j = 0u; j < i; j++) {
            if (delays[j] == delays[i]) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique_delays++;
        }
    }
    assert(unique_delays >= 20u);
}

static void test_deadline_is_terminal_and_wrap_safe(void)
{
    const uint8_t payload[] = {1u};
    struct app_mesh_event_request_identity request = request_identity(
        LOCAL_ID, 9u, 2u, payload, sizeof(payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    struct app_mesh_event_retry_state state = {0};
    uint32_t delay_ms = 99u;

    assert(app_mesh_event_retry_begin(&state,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      UINT32_MAX - 100u,
                                      50u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    assert(!app_mesh_event_retry_expired(&state, UINT32_MAX - 50u));
    assert(app_mesh_event_retry_expired(&state, 50u));
    assert(!app_mesh_event_retry_note_failure(
        &state,
        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
        50u,
        1u,
        false,
        &delay_ms));
    assert(delay_ms == 0u);
}

static void test_retry_due_zero_remains_armed_across_uptime_wrap(void)
{
    const uint8_t payload[] = {0x5au};
    struct app_mesh_event_request_identity request = request_identity(
        LOCAL_ID, 10u, 3u, payload, sizeof(payload));
    struct app_mesh_rf_retry_key key = retry_key(
        request.session_id, request.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
    struct app_mesh_event_retry_state probe = {0};
    struct app_mesh_event_retry_state state = {0};
    uint32_t delay_ms = 0u;
    uint32_t wrap_now_ms;

    assert(app_mesh_event_retry_begin(&probe,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      1000u,
                                      10000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    assert(app_mesh_event_retry_note_failure(
        &probe,
        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
        1000u,
        0x10203040u,
        false,
        &delay_ms));
    assert(delay_ms > 0u);

    wrap_now_ms = 0u - delay_ms;
    assert(app_mesh_event_retry_begin(&state,
                                      PEER_ID,
                                      &request,
                                      &key,
                                      wrap_now_ms,
                                      1000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    assert(app_mesh_event_retry_note_failure(
        &state,
        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
        wrap_now_ms,
        0x10203040u,
        false,
        &delay_ms));
    assert(state.retry_due_armed);
    assert(state.retry_due_ms == 0u);
    assert(!app_mesh_event_retry_due(&state, UINT32_MAX));
    assert(app_mesh_event_retry_due(&state, 0u));

    app_mesh_event_retry_note_send_success(&state);
    assert(!state.retry_due_armed);
    assert(!app_mesh_event_retry_due(&state, 0u));
}

static void test_completed_accept_does_not_block_another_peer(void)
{
    const uint8_t first_payload[] = {0x11u, 0x22u};
    const uint8_t changed_payload[] = {0x11u, 0x23u};
    const uint8_t second_payload[] = {0x33u, 0x44u};
    struct app_mesh_event_request_identity first = request_identity(
        PEER_ID, 0x1001u, 1u, first_payload, sizeof(first_payload));
    struct app_mesh_event_request_identity changed = request_identity(
        PEER_ID, 0x1001u, 1u, changed_payload, sizeof(changed_payload));
    struct app_mesh_event_request_identity second = request_identity(
        PEER_ID + 1u, 0x1002u, 1u, second_payload, sizeof(second_payload));
    struct app_mesh_rf_retry_key first_key = retry_key(
        first.session_id, first.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_rf_retry_key second_key = retry_key(
        second.session_id, second.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_event_retry_state active = {0};
    struct app_mesh_event_completion completed = {0};
    uint16_t completed_retry_round;
    uint32_t replay_delay_ms = 0u;

    assert(app_mesh_event_retry_begin(&active,
                                      PEER_ID,
                                      &first,
                                      &first_key,
                                      1000u,
                                      7000u,
                                      EVENT_INTERVAL_MS,
                                      10u) == 0);
    app_mesh_event_retry_note_send_success(&active);
    assert(app_mesh_event_completion_store(&completed,
                                           PEER_ID,
                                           &active.request,
                                           1100u,
                                           active.deadline_ms) == 0);
    app_mesh_event_retry_clear(&active);

    assert(app_mesh_event_retry_begin(&active,
                                      PEER_ID + 1u,
                                      &second,
                                      &second_key,
                                      1101u,
                                      7101u,
                                      EVENT_INTERVAL_MS,
                                      10u) == 0);
    assert(app_mesh_event_completion_match(&completed,
                                           PEER_ID,
                                           &first,
                                           1101u) ==
           APP_MESH_EVENT_REQUEST_DUPLICATE);
    assert(app_mesh_event_completion_match(&completed,
                                           PEER_ID,
                                           &changed,
                                           1101u) ==
           APP_MESH_EVENT_REQUEST_CONFLICT);
    assert(app_mesh_event_completion_match(&completed,
                                           PEER_ID + 1u,
                                           &second,
                                           1101u) ==
           APP_MESH_EVENT_REQUEST_NEW);
    assert(app_mesh_event_completion_match(&completed,
                                           PEER_ID,
                                           &first,
                                           7000u) ==
           APP_MESH_EVENT_REQUEST_NEW);

    app_mesh_event_retry_clear(&active);
    assert(app_mesh_event_retry_begin(&active,
                                      PEER_ID,
                                      &first,
                                      &first_key,
                                      1200u,
                                      7000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    assert(app_mesh_event_retry_resume_backoff(&active, 3u) == 0);
    assert(app_mesh_event_retry_note_failure(
        &active,
        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
        1200u,
        0x55aa00ffu,
        true,
        &replay_delay_ms));
    assert(active.retry.retry_round == 4u);
    assert(replay_delay_ms >= 200u && replay_delay_ms <= 600u);

    /* Preempting a cached replay must resume after its consumed deferral. */
    completed_retry_round = active.retry.retry_round;
    app_mesh_event_retry_clear(&active);
    assert(app_mesh_event_retry_begin(&active,
                                      PEER_ID,
                                      &first,
                                      &first_key,
                                      1300u,
                                      7000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    assert(app_mesh_event_retry_resume_backoff(&active,
                                               completed_retry_round) == 0);
    assert(app_mesh_event_retry_note_failure(
        &active,
        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
        1300u,
        0x1234abcdu,
        false,
        &replay_delay_ms));
    assert(active.retry.retry_round == 5u);
    assert(active.pre_rf_deferrals == 1u);
}

static void test_accept_rx_cache_is_scoped_to_one_live_session(void)
{
    const uint8_t first_payload[] = {0x71u, 0x72u};
    const uint8_t second_payload[] = {0x81u, 0x82u};
    struct app_mesh_event_request_identity first = request_identity(
        PEER_ID, 0x2201u, 4u, first_payload, sizeof(first_payload));
    struct app_mesh_event_request_identity second = request_identity(
        PEER_ID + 1u, 0x2202u, 5u, second_payload, sizeof(second_payload));
    struct app_mesh_rf_retry_key first_key = retry_key(
        first.session_id, first.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_rf_retry_key second_key = retry_key(
        second.session_id, second.sequence,
        APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
    struct app_mesh_event_retry_state rx_cache = {0};

    assert(app_mesh_event_retry_begin(&rx_cache,
                                      PEER_ID,
                                      &first,
                                      &first_key,
                                      1000u,
                                      7000u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    app_mesh_event_retry_note_send_success(&rx_cache);
    assert(app_mesh_event_retry_match(&rx_cache, PEER_ID + 1u, &second) ==
           APP_MESH_EVENT_REQUEST_BUSY);

    /* Beginning another proposal replaces the prior proposal's ACCEPT cache. */
    app_mesh_event_retry_clear(&rx_cache);
    assert(app_mesh_event_retry_match(&rx_cache, PEER_ID + 1u, &second) ==
           APP_MESH_EVENT_REQUEST_NEW);
    assert(app_mesh_event_retry_begin(&rx_cache,
                                      PEER_ID + 1u,
                                      &second,
                                      &second_key,
                                      1100u,
                                      7100u,
                                      EVENT_INTERVAL_MS,
                                      0u) == 0);
    app_mesh_event_retry_note_send_success(&rx_cache);

    /* EVENT_END removes the cache, so late ACCEPT bytes cannot revive it. */
    app_mesh_event_retry_clear(&rx_cache);
    assert(app_mesh_event_retry_match(&rx_cache, PEER_ID + 1u, &second) ==
           APP_MESH_EVENT_REQUEST_NEW);
    assert(app_mesh_event_retry_match(&rx_cache, PEER_ID, &first) ==
           APP_MESH_EVENT_REQUEST_NEW);
}

int main(void)
{
    test_counterphase_shift_preserves_frozen_proposal_shape();
    test_duplicate_conflict_and_busy_are_distinct();
    test_old_fnv_collision_is_a_digest_conflict();
    test_accept_correlates_with_exact_and_stable_release_identities();
    test_delayed_legacy_accept_cannot_complete_newer_proposal();
    test_accept_phase_is_compatibility_agnostic_and_parity_can_be_bound();
    test_legacy_accept_survives_wire_validation_before_app_correlation();
    test_pre_rf_and_actual_failures_share_backoff_without_identity_loss();
    test_duplicate_proposal_reuses_response_and_installs_once();
    test_fifty_simultaneous_proposals_diverge();
    test_deadline_is_terminal_and_wrap_safe();
    test_retry_due_zero_remains_armed_across_uptime_wrap();
    test_completed_accept_does_not_block_another_peer();
    test_accept_rx_cache_is_scoped_to_one_live_session();
    puts("app mesh event retry tests passed");
    return 0;
}
