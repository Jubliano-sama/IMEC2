#!/usr/bin/env python3
from pathlib import Path

from source_text import read_composed_source


SOURCE = Path(__file__).resolve().parents[2] / "app/src/app_mesh_report.c"
PRIORITY = Path(__file__).resolve().parents[2] / "app/src/app_mesh_c5_priority.c"
ACK_POLICY = Path(__file__).resolve().parents[2] / "app/src/app_mesh_ch9_ack.c"
ASYNC_ROUTE = (
    Path(__file__).resolve().parents[2]
    / "app/src/app_mesh_async_route_request.c"
)


def body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def implementation_body(source: str, signature: str,
                        next_signature: str) -> str:
    start = source.rindex(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def main() -> None:
    source = read_composed_source(SOURCE)
    priority = PRIORITY.read_text(encoding="utf-8")
    ack_policy = ACK_POLICY.read_text(encoding="utf-8")
    async_route = ASYNC_ROUTE.read_text(encoding="utf-8")
    c5_send = body(
        source,
        "static int mesh_send_c5_control_attempt(",
        "int mesh_send_c5_control(",
    )
    contact_expiry = body(
        priority,
        "bool app_mesh_c5_contact_expired(",
        "bool app_mesh_c5_contact_accepted(",
    )
    contact_accepted = body(
        priority,
        "bool app_mesh_c5_contact_accepted(",
        "bool app_mesh_c5_route_capture_relevant(",
    )
    contact_owner = body(
        source,
        "static bool mesh_c5_contact_expire_if_due(",
        "static void mesh_c5_contact_open(",
    )
    low_level = body(
        source,
        "static int mesh_send_outbound_with_release_on_channel_until(",
        "static int mesh_send_outbound_with_release_on_channel(",
    )
    flood_defer = body(
        source,
        "static bool mesh_c5_flood_defer_active_cb(",
        "static bool mesh_c5_flood_quiet_cb(",
    )
    route_wake = body(
        source,
        "static int mesh_send_route_wake_train(",
        "static uint8_t mesh_c5_listener_purpose(",
    )
    select_ack = body(
        source,
        "static bool mesh_select_channel9_ack_tx_event(",
        "static bool mesh_channel9_ack_pending_for_peer(",
    )
    propose_event = body(
        source,
        "static int mesh_propose_event_after_channel5_contact_authorized(",
        "static int mesh_propose_event_after_channel5_contact(",
    )
    capture_authorization = body(
        ack_policy,
        "bool app_mesh_ch9_c5_repair_authorization_capture(",
        "bool app_mesh_ch9_c5_repair_owner_matches(",
    )
    match_authorization = body(
        ack_policy,
        "bool app_mesh_ch9_c5_repair_owner_matches(",
        "bool app_mesh_ch9_c5_repair_allowed(",
    )
    allow_repair = body(
        ack_policy,
        "bool app_mesh_ch9_c5_repair_allowed(",
        "uint8_t app_mesh_ch9_tx_max_in_flight(",
    )
    async_submit = body(
        async_route,
        "bool app_mesh_async_route_request_submit(",
        "bool app_mesh_async_route_request_snapshot(",
    )
    async_snapshot = body(
        async_route,
        "bool app_mesh_async_route_request_snapshot(",
        "bool app_mesh_async_route_request_complete(",
    )
    causal_classifier = body(
        source,
        "static bool mesh_c5_causal_response_candidate_valid(",
        "static bool mesh_coordinator_c5_tx_allowed_authorized_intent(",
    )
    repair_admission = body(
        source,
        "static bool mesh_coordinator_c5_tx_allowed_authorized_intent(",
        "static bool mesh_coordinator_c5_tx_allowed_authorized(",
    )
    causal_outbound = body(
        source,
        "static int mesh_send_outbound_causal_response(",
        "static int mesh_send_outbound_keep_channel9_awake(",
    )
    causal_control = body(
        source,
        "static int mesh_send_c5_causal_response(",
        "static uint32_t mesh_c5_flood_now_ms(",
    )
    route_reply_response = implementation_body(
        source,
        "static int mesh_send_route_reply_burst(",
        "static int mesh_send_route_reply_train_to_hop(",
    )
    result_grant_response = implementation_body(
        source,
        "static int mesh_handoff_send_result_grant(",
        "static void mesh_handoff_note_tx_sent(",
    )
    event_control = implementation_body(
        source,
        "static int mesh_send_event_control(uint64_t peer_id,",
        "static void mesh_event_propose_clear(",
    )
    delivery = source[source.index(
        "static uint32_t mesh_drain_rx_queue_locked("
    ):]

    assert "mesh_send_outbound_with_release_on_channel_until(" in c5_send
    assert "authorization" in c5_send
    assert "UWB_CHANNEL_WAKE_CONTACT" in c5_send
    assert "out->radio_channel =" not in c5_send
    snapshot = low_level.index("mesh_encode_outbound_tx_snapshot(out,")
    select_channel = low_level.index(
        "radio_channel = forced_radio_channel == 0u ? out->radio_channel :"
    )
    configure = low_level.index(
        "ret = radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?"
    )
    assert select_channel < snapshot < configure
    assert "out->radio_channel =" not in low_level
    assert "*tx = *out;" not in low_level
    assert "contact->state == C5_CONTACT_NONE" in contact_expiry
    assert "(int32_t)(now_ms - contact->expires_at_ms) >= 0" in contact_expiry
    assert "expires_at_ms != 0u" not in contact_expiry
    assert "contact->state != C5_CONTACT_NONE" in contact_accepted
    assert "contact->purpose == purpose" in contact_accepted
    assert "contact->accepted" in contact_accepted
    assert "app_mesh_c5_contact_expired(contact, now_ms)" in contact_accepted
    assert "mesh_c5_contact.expires_at_ms != 0u" not in contact_owner
    assert "mesh_c5_contact_expire_if_due(now_ms)" in contact_owner

    # ACK RX and ACK-send custody deliberately keep generic mesh work and UWB
    # RX available, but no Channel-5 producer may interpret that as TX
    # permission and steal the physical radio during the negotiated slot.
    assert "!coordinator_decision.c5_tx_allowed" in flood_defer
    assert "mesh_coordinator_c5_tx_allowed_authorized_intent(" in route_wake
    assert route_wake.count(
        "mesh_coordinator_c5_tx_allowed_authorized_intent("
    ) >= 5

    lock = low_level.index(
        "k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);"
    )
    c5_recheck = low_level.index(
        "mesh_coordinator_c5_tx_allowed_authorized_intent(", lock
    )
    stop_scan = low_level.index("mesh_stop_role_scan();", c5_recheck)
    assert lock < c5_recheck < stop_scan
    assert "radio_channel == UWB_CHANNEL_WAKE_CONTACT" in low_level[
        lock:stop_scan
    ]
    configure = low_level.index(
        "ret = radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?"
    )
    pre_rf_recheck = low_level.index(
        "mesh_coordinator_c5_tx_allowed_authorized_intent(", configure
    )
    physical_send = low_level.index(
        "dwm3000_driver_send_frame_tracked_until(", pre_rf_recheck
    )
    assert configure < pre_rf_recheck < physical_send

    # Queue occupancy can be bypassed only by the exact synchronous response
    # classes.  The dedicated helpers make opt-in reviewable at every call
    # site; their closed classifier rejects ordinary route discovery, floods,
    # reports, EVENT_PROPOSE, and all future message types by default.
    causal_types = (
        "MSG_MESH_HOP_ACK",
        "MSG_ROUTE_REPLY",
        "MSG_ROUTE_REPLY_ACK",
        "MSG_RELAY_BUSY",
        "MSG_RESULT_BUSY",
        "MSG_RESULT_GRANT",
        "MSG_MESH_EVENT_ACCEPT",
    )
    for msg_type in causal_types:
        assert f"case {msg_type}:" in causal_classifier
    assert causal_classifier.count("case MSG_") == len(causal_types)
    assert "default:" in causal_classifier
    assert "return false;" in causal_classifier
    # One declaration and one definition precede the exact six control-call
    # sites; the outbound form has one declaration, one definition, and the
    # sole Channel-5 hop-ACK call site.
    assert source.count("mesh_send_c5_causal_response(") == 8
    assert source.count("mesh_send_outbound_causal_response(") == 3
    assert "FW_C5_TX_INTENT_CAUSAL_RESPONSE" in causal_outbound
    assert "FW_C5_TX_INTENT_CAUSAL_RESPONSE" in causal_control
    assert "mesh_send_c5_causal_response(" in route_reply_response
    assert "mesh_send_c5_causal_response(" in result_grant_response
    accept_branch = event_control.index(
        "else if (msg_type == MSG_MESH_EVENT_ACCEPT)"
    )
    accept_send = event_control.index(
        "mesh_send_c5_causal_response(", accept_branch
    )
    background_branch = event_control.index("} else {", accept_send)
    background_send = event_control.index(
        "mesh_send_c5_control(", background_branch
    )
    assert accept_branch < accept_send < background_branch < background_send

    hop_ack_start = source.rindex(
        "if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK)"
    )
    hop_ack = source[hop_ack_start:source.index(
        "if (route_reply_downstream_handoff_required)", hop_ack_start
    )]
    assert "received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD" in hop_ack
    assert "mesh_send_outbound(hop_ack, \"hop-ack\")" in hop_ack
    assert "mesh_send_outbound_causal_response(" in hop_ack
    route_reply_ack_start = source.index(
        "if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK)",
        hop_ack_start,
    )
    route_reply_ack = source[route_reply_ack_start:source.index(
        "MESH_RELAY_ACTION_SEND_ROUTE_REQ", route_reply_ack_start
    )]
    assert "mesh_send_c5_causal_response(" in route_reply_ack
    relay_busy_start = source.rindex(
        "if (result->actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY)"
    )
    relay_busy = source[relay_busy_start:source.index(
        "MESH_RELAY_ACTION_SEND_RESULT_BUSY", relay_busy_start
    )]
    result_busy_start = source.rindex(
        "if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_BUSY)"
    )
    result_busy = source[result_busy_start:source.index(
        "MESH_RELAY_ACTION_SEND_RESULT_GRANT", result_busy_start
    )]
    assert "mesh_send_c5_causal_response(" in relay_busy
    assert "mesh_send_c5_causal_response(" in result_busy
    assert "int mesh_send_c5_control(" in source
    assert "int mesh_send_outbound(" in source
    assert "FW_C5_TX_INTENT_BACKGROUND" in body(
        source,
        "int mesh_send_c5_control(",
        "static int mesh_send_c5_causal_response(",
    )

    # Causal intent still traverses the same final coordinator recheck and
    # scoped radio-lease acquisition as background traffic; it is not an
    # RF-owner bypass.
    assert "mesh_transport_radio_claim(" in low_level
    assert low_level.index(
        "mesh_coordinator_c5_tx_allowed_authorized_intent("
    ) < low_level.index("mesh_transport_radio_claim(")

    # A retained ACK capability may escape only the exact relay MESH_TX state
    # or its owner-only MESH_RX wait. It cannot steal a click, survey, gateway
    # RX, or queued RX turn merely because its token is otherwise valid.
    exact_rx_owner = repair_admission.index("exact_ack_rx_repair_state =")
    repair_state_gate = repair_admission.index(
        "if (decision.state != FW_RADIO_ACTIVITY_MESH_TX &&",
        exact_rx_owner,
    )
    repair_authorization = repair_admission.index(
        "if (authorization != NULL && authorization->valid)",
        repair_state_gate,
    )
    repair_validate = repair_admission.index(
        "app_mesh_ch9_c5_repair_allowed(", repair_authorization
    )
    assert repair_state_gate < repair_authorization < repair_validate
    assert "capture.rx_queue_used == 0u" in repair_admission
    assert "!capture.click_active" in repair_admission
    assert "!capture.survey_pending" in repair_admission
    assert "!capture.gateway_continuous_ch9" in repair_admission
    assert "!exact_ack_rx_repair_state" in repair_admission[
        repair_state_gate:repair_authorization
    ]

    # The bypass is an exact capability captured from retained custody.  It
    # remains bound to both semantic digests and is accepted only for the
    # route or event packet type named by that capability.
    assert "mesh_packet_semantic_digest(&pending->packet" in capture_authorization
    assert "authorization->pending_digest" in capture_authorization
    assert "authorization->retained_ack_digest" in capture_authorization
    assert "semantic_digest_equal(pending_digest" in match_authorization
    assert "semantic_digest_equal(ack_digest" in match_authorization
    assert "tlv_find_unique(candidate->payload" in allow_repair
    assert "TLV_RESPONDER_ID" in allow_repair
    assert "MSG_MESH_EVENT_PROPOSE" in allow_repair
    assert "MSG_MESH_EVENT_ACCEPT" in allow_repair

    # Async route ownership must freeze a critical capability across retries;
    # only its exact duplicate may coalesce, and snapshotting copies it into
    # the executing attempt.
    assert "request->pending && request->c5_authorization.valid" in async_submit
    assert "c5_authorization_equal(" in async_submit
    assert "return false;" in async_submit
    assert "attempt->c5_authorization = request->c5_authorization" in async_snapshot

    mint = delivery.index("MESH_RELAY_ACTION_TRANSIT_GATEWAY_ACK_ROUTE_REPAIR")
    capture = delivery.index(
        "app_mesh_ch9_c5_repair_authorization_capture(", mint
    )
    schedule = delivery.index(
        "mesh_schedule_route_request_authorized(", capture
    )
    assert mint < capture < schedule
    assert "app_watchdog_stop_feeding();" in delivery[capture:schedule + 800]

    stale = select_ack.index("ret == PROTO_ERR_STALE")
    event_capture = select_ack.index(
        "app_mesh_ch9_c5_repair_authorization_capture(", stale
    )
    event_schedule = select_ack.index(
        "mesh_propose_event_after_channel5_contact_authorized(",
        event_capture,
    )
    assert stale < event_capture < event_schedule
    stale_repair = select_ack[stale:event_schedule]
    assert "authorization_kind" in stale_repair
    assert "APP_MESH_C5_TX_AUTH_FORWARDED_ACK_EVENT_REPAIR" in stale_repair
    assert "APP_MESH_C5_TX_AUTH_LATE_GATEWAY_ACK_EVENT_REPAIR" in stale_repair
    assert "app_watchdog_stop_feeding();" in select_ack[
        event_capture:event_schedule
    ]
    assert "mesh_forwarded_ack_event_repair_authorization = *authorization" in propose_event
    assert "app_mesh_ch9_c5_repair_owner_matches(" in propose_event
    assert "&mesh_forwarded_ack_event_repair_authorization" in propose_event

    active = c5_send.index(
        "active_exchange = mesh_c5_contact_active(peer_id,"
    )
    wake_branch = c5_send.index(
        "mode == MESH_C5_CONTROL_WAKE_IF_NEEDED && !active_exchange"
    )
    wake = c5_send.index("mesh_send_route_wake_train(", wake_branch)
    assert active < wake_branch < wake


if __name__ == "__main__":
    main()
