#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app"
SEQUENCE = (APP / "src/app_click_event_sequence.c").read_text()
SEQUENCE_H = (APP / "src/app_click_event_sequence.h").read_text()
STORAGE = (APP / "src/app_nvs_storage.c").read_text()
STORAGE_H = (APP / "src/app_nvs_storage.h").read_text()
MAIN = (APP / "src/main.c").read_text()
STATE = (APP / "src/app_state.c").read_text()
STATE_H = (APP / "src/app_state.h").read_text()
CLICKER_CONF = (APP / "conf/mesh-clicker.conf").read_text()
GENERIC_CLICKER_CONF = (APP / "prj-clicker.conf").read_text()
KCONFIG = (APP / "Kconfig").read_text()
PERSISTENCE = (APP / "src/app_mesh_persistence.c").read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = source.index("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


for symbol in (
    "CONFIG_FLASH=y",
    "CONFIG_FLASH_MAP=y",
    "CONFIG_FLASH_PAGE_LAYOUT=y",
    "CONFIG_NVS=y",
    "CONFIG_NVS_DATA_CRC=y",
    "CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE=y",
):
    assert symbol in CLICKER_CONF
    assert symbol in GENERIC_CLICKER_CONF
assert (
    "depends on FLASH && FLASH_MAP && FLASH_PAGE_LAYOUT && NVS && NVS_DATA_CRC"
    in KCONFIG
)
assert "IS_ENABLED(CONFIG_NVS_DATA_CRC)" in SEQUENCE
assert "app_nvs_storage_init()" in SEQUENCE
assert "static struct nvs_fs" not in SEQUENCE
assert "nvs_mount(" not in SEQUENCE
assert "static struct nvs_fs app_storage_nvs" in STORAGE
assert STORAGE.count("nvs_mount(") == 1
assert "APP_NVS_STORAGE_SECTOR_SIZE 4096u" in STORAGE_H

main = function_body(MAIN, "main")
sequence_init = main.index("app_click_event_sequence_init()")
clicker_init = main.index("app_clicker_init(", sequence_init)
startup_input = main.index("app_clicker_prepare_startup_idle(", clicker_init)
button_init = main.index("app_clicker_button_init()", startup_input)
assert sequence_init < clicker_init < startup_input < button_init
assert "k_panic()" in main[sequence_init:clicker_init]

reserve = function_body(SEQUENCE, "click_event_sequence_reserve_block")
write = reserve.index("nvs_write(")
readback = reserve.index("nvs_read(", write)
publish = reserve.index("*reserved_through =", readback)
assert write < readback < publish
assert "nvs_delete" not in SEQUENCE
assert "nvs_clear" not in SEQUENCE

allocate = function_body(SEQUENCE, "app_click_event_sequence_next")
durable_reserve = allocate.index("click_event_sequence_reserve_block(")
expose = allocate.index("*event_seq = click_event_next", durable_reserve)
assert durable_reserve < expose
assert "ret = -EACCES" in allocate
assert "ret = -EOVERFLOW" in allocate

assert "APP_CLICK_EVENT_SEQUENCE_BLOCK_SIZE 256u" in SEQUENCE_H
assert "APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR UINT32_C(0x01000000)" in SEQUENCE_H
assert "previous_limit =\n                APP_CLICK_EVENT_SEQUENCE_FIRST_INSTALL_FLOOR" in SEQUENCE
assert "16711679u" in SEQUENCE
click_id_match = re.search(
    r"APP_NVS_ID_CLICK_EVENT_SEQUENCE\s+(0x[0-9A-Fa-f]+)u", STORAGE_H
)
assert click_id_match is not None
click_id = int(click_id_match.group(1), 16)
mesh_ids = {
    int(value, 16)
    for value in re.findall(
        r"#define\s+APP_MESH_NVS_[A-Z0-9_]+_ID\s+(0x[0-9A-Fa-f]+)u",
        PERSISTENCE,
    )
}
mesh_ids.update(
    int(value, 16)
    for value in re.findall(
        r"#define\s+APP_NVS_ID_MESH_[A-Z0-9_]+\s+(0x[0-9A-Fa-f]+)u",
        STORAGE_H,
    )
)
assert click_id not in mesh_ids

assert "next_click_event_seq" not in STATE
assert "next_click_event_seq" not in STATE_H
assert re.search(r"\bnext_event_seq\b", STATE) is None

for source_path in (
    APP / "src/app_clicker.c",
    APP / "src/app_ml.c",
    APP / "src/app_high_debug.c",
):
    source = source_path.read_text()
    for call in re.finditer(r"app_click_event_sequence_next\([^;]+;", source):
        statement = call.group(0)
        preceding = source[max(0, call.start() - 24) : call.start()]
        assert re.search(r"\bret\s*=\s*$", preceding)
        following = source[call.end() : call.end() + 240]
        assert re.search(r"if\s*\(\s*ret\s*<\s*0\s*\)", following), (
            f"unchecked identity allocation in {source_path}: {statement}"
        )

fixture = APP / "tests/click_event_sequence_persistence"
for relative in (
    "CMakeLists.txt",
    "prj.conf",
    "boards/native_sim.overlay",
    "boards/native_sim_native_64.overlay",
    "src/main.c",
):
    assert (fixture / relative).is_file()

print("click event sequence source invariants passed")
