#!/usr/bin/env python3
from pathlib import Path

from source_text import read_composed_source


SOURCE = Path(__file__).resolve().parents[2] / "app/src/app_mesh_report.c"
PRIORITY = Path(__file__).resolve().parents[2] / "app/src/app_mesh_c5_priority.c"


def body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def main() -> None:
    source = read_composed_source(SOURCE)
    priority = PRIORITY.read_text(encoding="utf-8")
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

    assert "mesh_send_outbound_with_release_on_channel(" in c5_send
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
