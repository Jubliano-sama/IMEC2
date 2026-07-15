#!/usr/bin/env python3
from pathlib import Path

from source_text import read_composed_source


SOURCE = Path(__file__).resolve().parents[2] / "app/src/app_mesh_report.c"


def body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def main() -> None:
    source = read_composed_source(SOURCE)
    c5_send = body(
        source,
        "static int mesh_send_c5_control_attempt(",
        "int mesh_send_c5_control(",
    )
    low_level = body(
        source,
        "static int mesh_send_outbound_with_release_on_channel(",
        "static int mesh_send_outbound_with_release(",
    )

    assert "mesh_send_outbound_with_release_on_channel(" in c5_send
    assert "UWB_CHANNEL_WAKE_CONTACT" in c5_send
    assert "out->radio_channel =" not in c5_send
    assert "*tx = *out;" in low_level
    assert "tx->radio_channel = radio_channel;" in low_level
    assert low_level.index("*tx = *out;") < low_level.index(
        "tx->radio_channel = radio_channel;"
    )


if __name__ == "__main__":
    main()
