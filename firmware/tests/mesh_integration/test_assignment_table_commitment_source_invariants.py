#!/usr/bin/env python3

from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
PROTOCOL = (ROOT / "include/protocol.h").read_text(encoding="utf-8")
ASSIGNMENT_HEADER = (
    ROOT / "include/discovery_assignment.h"
).read_text(encoding="utf-8")
ASSIGNMENT = (ROOT / "src/discovery_assignment.c").read_text(encoding="utf-8")
MEMBERSHIP_HEADER = (
    ROOT / "include/gateway_membership.h"
).read_text(encoding="utf-8")
MEMBERSHIP = (ROOT / "src/gateway_membership.c").read_text(encoding="utf-8")
ANCHOR = read_composed_source(ROOT / "app/src/app_anchor.c")
APP_STATE = (ROOT / "app/src/app_state.c").read_text(encoding="utf-8")
APP_STATE_HEADER = (ROOT / "app/src/app_state.h").read_text(encoding="utf-8")
POLICY = (
    ROOT / "app/src/app_discovery_assignment_policy.h"
).read_text(encoding="utf-8")
PERSISTENCE_HEADER = (
    ROOT / "app/src/app_mesh_persistence.h"
).read_text(encoding="utf-8")
PERSISTENCE = (
    ROOT / "app/src/app_mesh_persistence.c"
).read_text(encoding="utf-8")


assert re.search(
    r"#define\s+DISCOVERY_ASSIGNMENT_SCHEME_VERSION\s+2u",
    ASSIGNMENT_HEADER,
)
assert "TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT = 0xB2" in PROTOCOL
assert re.search(
    r"struct\s+discovery_assignment_table_commitment\s*\{\s*"
    r"uint8_t\s+bytes\[SEMANTIC_DIGEST_SHA256_LEN\];\s*\}",
    ASSIGNMENT_HEADER,
)
assert "assignment TABLE commitment must be one SHA-256 digest" in (
    ASSIGNMENT_HEADER
)

# The semantic digest is domain-separated, includes the slot/count envelope,
# and walks explicit slot order. Arrival order cannot change the proof.
for required in (
    "'I', 'M', 'E', 'C'",
    "DISCOVERY_ASSIGNMENT_SCHEME_VERSION,",
    "header[0] = slot_count",
    "header[1] = (uint8_t)entry_count",
    "for (uint8_t slot = 0u; slot < slot_count; slot++)",
    "proto_put_u64_le(encoded, entry->anchor_id)",
    "proto_put_u64_le(&encoded[8], entry->hash)",
    "encoded[16] = entry->slot",
    "semantic_digest_sha256_final(&context, commitment->bytes)",
):
    assert required in ASSIGNMENT, (
        f"assignment TABLE commitment omits canonical input: {required}"
    )
assert "semantic_digest_equal(" in ASSIGNMENT
assert "raw_len != sizeof(parsed.table_commitment.bytes)" in ASSIGNMENT
assert "TLV_DISCOVERY_ASSIGNMENT_TABLE_COMMITMENT" in ASSIGNMENT
assert "app_discovery_assignment_response_identity_matches(" in POLICY
assert "app_discovery_assignment_response_identity_matches(" in ANCHOR

# Every live ACK/state/durable authority surface carries the complete digest.
# The old 32-bit name may exist only in byte-exact migration fixtures.
legacy_name = "table_" + "fingerprint"
current_authority = "\n".join(
    (
        PROTOCOL,
        ASSIGNMENT_HEADER,
        ASSIGNMENT,
        MEMBERSHIP_HEADER,
        MEMBERSHIP,
        ANCHOR,
        APP_STATE,
        APP_STATE_HEADER,
        POLICY,
        PERSISTENCE_HEADER,
    )
)
assert legacy_name not in current_authority
assert "struct discovery_assignment_table_commitment table_commitment" in (
    PERSISTENCE_HEADER
)
assert "struct discovery_assignment_table_commitment pending_table_commitment" in (
    PERSISTENCE_HEADER
)
assert "APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION 8u" in (
    PERSISTENCE_HEADER
)
assert "GATEWAY_MEMBERSHIP_SNAPSHOT_VERSION 4u" in MEMBERSHIP_HEADER
assert "sizeof(struct gateway_membership_snapshot) == 896u" in (
    MEMBERSHIP_HEADER
)

# Installed 32-bit records are recognized only to preserve their roster or
# retire the record. They never prove a schema-2 ACK or synthesize a digest.
assert PERSISTENCE.count(
    "A legacy 32-bit proof can never authorize a schema-2 ACK."
) == 2
for version in range(2, 8):
    assert (
        f"discovery_assignment_retire_legacy_snapshot(snapshot, {version}u)"
        in PERSISTENCE
    )
assert "fresh schema-2 TABLE required" in PERSISTENCE
assert "gateway_membership_migrate_roster_only(roster, 2u)" in PERSISTENCE
assert "gateway_membership_migrate_roster_only(roster, 3u)" in PERSISTENCE

allowed_legacy_files = {
    ROOT / "app/src/app_mesh_persistence.c",
    ROOT / "app/tests/mesh_persistence/src/main.c",
}
for suffix in ("*.c", "*.h", "*.py"):
    for path in ROOT.rglob(suffix):
        if path == Path(__file__) or path in allowed_legacy_files:
            continue
        assert legacy_name not in path.read_text(
            encoding="utf-8", errors="ignore"
        ), f"32-bit assignment authority escaped legacy fixture: {path}"

print("assignment TABLE commitment source invariants passed")
