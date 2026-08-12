#!/usr/bin/env python3
from pathlib import Path
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
APP = FIRMWARE / "app"
CMAKE = (APP / "CMakeLists.txt").read_text()
CONFIG = (APP / "src/app_config.h").read_text()
APP_IDENTITY = (APP / "src/app_device_identity.c").read_text()
MAIN = (APP / "src/main.c").read_text()


def preset_branch(name: str) -> str:
    marker = f'IMEC_BUILD_PRESET STREQUAL "{name}"'
    start = CMAKE.index(marker)
    end = CMAKE.find("\n    elseif(IMEC_BUILD_PRESET", start + len(marker))
    if end < 0:
        end = CMAKE.index(
            '\n    else()\n        message(FATAL_ERROR "Unknown IMEC_BUILD_PRESET',
            start + len(marker),
        )
    return CMAKE[start:end]


class ProductionDeviceIdentitySourceInvariants(unittest.TestCase):
    def test_clicker_and_anchor_presets_force_hardware_identity(self) -> None:
        for preset in ("mesh_clicker", "mesh_anchor"):
            branch = preset_branch(preset)
            clear = branch.index('set(IMEC_DEVICE_ID "" CACHE STRING')
            force = branch.index("FORCE)", clear)
            hardware = branch.index("set(IMEC_USE_HARDWARE_DEVICE_ID ON)")

            self.assertLess(clear, force)
            self.assertLess(force, hardware)
            self.assertNotIn("0x1111111111111111", branch)

    def test_device_id_macro_uses_initialized_hardware_node(self) -> None:
        hardware = CONFIG.index("#if IMEC_USE_HARDWARE_DEVICE_ID")
        runtime = CONFIG.index("#define DEVICE_ID app_device_id()", hardware)
        anchor_fallback = CONFIG.index("#elif DEVICE_ROLE == ROLE_ANCHOR", runtime)

        self.assertLess(hardware, runtime)
        self.assertLess(runtime, anchor_fallback)
        self.assertIn("device_identity_node_from_ficr(", APP_IDENTITY)
        self.assertNotIn("device_identity_anchor_from_ficr(", APP_IDENTITY)
        self.assertIn("#define LEGACY_FIXED_CLICKER_ID", CONFIG)
        self.assertIn("#define DEVICE_ID LEGACY_FIXED_CLICKER_ID", CONFIG)
        self.assertIn("network_id == GATEWAY_ID", APP_IDENTITY)
        self.assertIn("network_id == LEGACY_FIXED_CLICKER_ID", APP_IDENTITY)

    def test_hardware_identity_precedes_durable_and_click_admission(self) -> None:
        identity = MAIN.index("app_device_identity_init()")
        identity_record = MAIN.index("mesh_node_identity_print();", identity)
        durable = MAIN.index("app_durable_state_init(")
        click_sequence = MAIN.index("app_click_event_sequence_init()")
        click_runtime = MAIN.index("app_clicker_start_work_queue()")

        self.assertLess(identity, durable)
        self.assertLess(identity, identity_record)
        self.assertLess(identity_record, durable)
        self.assertIn("ficr=0x%016llx node=0x%016llx preset=%s", MAIN)
        self.assertLess(durable, click_sequence)
        self.assertLess(click_sequence, click_runtime)


if __name__ == "__main__":
    unittest.main()
