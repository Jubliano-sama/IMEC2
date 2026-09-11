"""Execute production preset configuration before the Zephyr dependency import."""

from pathlib import Path
import subprocess
import tempfile
import unittest


APP = Path(__file__).resolve().parents[2] / "app" / "CMakeLists.txt"


class PresetCacheTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="imec-preset-cache-")
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "source"
        self.source.mkdir()
        # Execute the complete real preset selection, including all generated
        # fragments. Only the downstream SDK discovery/build is excluded.
        prefix = APP.read_text().split("find_package(Zephyr REQUIRED", 1)[0]
        (self.source / "CMakeLists.txt").write_text(
            prefix + '\nproject(preset_cache_test NONE)\n'
        )

    def configure(self, build, preset, *options, success=True):
        result = subprocess.run(
            ["cmake", "-S", str(self.source), "-B", str(build),
             f"-DIMEC_BUILD_PRESET={preset}", *options],
            capture_output=True, text=True, check=False,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("requires a pristine build directory",
                          " ".join(result.stderr.split()))
        return (build / "CMakeCache.txt").read_text()

    def test_role_changes_require_pristine_cache(self):
        for first, second in (("mesh_transmitter", "mesh_gateway"),
                              ("mesh_gateway", "mesh_anchor"),
                              ("mesh_anchor", "mesh_clicker")):
            with self.subTest(first=first, second=second):
                build = Path(self.directory.name) / first
                self.configure(build, first)
                self.configure(build, second, success=False)
                self.configure(build, second, success=False)
                fresh = self.configure(build.with_name(first + "-fresh"), second)
                if second == "mesh_gateway":
                    self.assertIn("IMEC_DEVICE_ID:STRING=\n", fresh)
                else:
                    self.assertNotIn("CONF_FILE:STRING=", fresh)

    def test_legacy_build_directory_also_rejects_role_change(self):
        build = Path(self.directory.name) / "legacy"
        self.configure(build, "mesh_transmitter")
        cache = build / "CMakeCache.txt"
        cache.write_text("\n".join(
            line for line in cache.read_text().splitlines()
            if not line.startswith("IMEC_CONFIGURED_PRESET:")
        ) + "\n")
        self.configure(build, "mesh_gateway", success=False)
        self.configure(build, "mesh_gateway", success=False)

    def test_same_preset_reconfiguration_honors_diagnostic_override(self):
        for preset in ("mesh_clicker", "mesh_anchor", "mesh_gateway"):
            with self.subTest(preset=preset):
                build = Path(self.directory.name) / preset
                initial = self.configure(build, preset)
                self.assertIn("IMEC_STACK_DIAGNOSTICS_BUILD:BOOL=ON\n", initial)
                changed = self.configure(
                    build, preset, "-DIMEC_STACK_DIAGNOSTICS_BUILD=OFF"
                )
                self.assertIn("IMEC_STACK_DIAGNOSTICS_BUILD:BOOL=OFF\n", changed)
                repeated = self.configure(build, preset)
                self.assertIn("IMEC_STACK_DIAGNOSTICS_BUILD:BOOL=OFF\n", repeated)


if __name__ == "__main__":
    unittest.main()
