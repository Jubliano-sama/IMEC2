#!/usr/bin/env python3
"""Source guards for full-width transaction equality commitments."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
NODE_HEADER = (ROOT / "include" / "node_transaction.h").read_text()
NODE_SOURCE = (ROOT / "src" / "node_transaction.c").read_text()
SURVEY_HEADER = (ROOT / "include" / "survey_gateway_transaction.h").read_text()
SURVEY_SOURCE = (ROOT / "src" / "survey_gateway_transaction.c").read_text()
APP_STATE = (ROOT / "app" / "src" / "app_anchor.c").read_text()
APP_GLUE = (
    ROOT / "app" / "src" / "app_anchor_gateway_survey.inc"
).read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end()
    depth = 1
    index = start
    while index < len(source) and depth != 0:
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
        index += 1
    if depth != 0:
        raise AssertionError(f"unterminated function {name}")
    return source[start : index - 1]

ALL_TRANSACTION_SOURCES = "\n".join(
    (NODE_HEADER, NODE_SOURCE, SURVEY_HEADER, SURVEY_SOURCE, APP_STATE, APP_GLUE)
)

# A bounded scalar hash can collide and must never regain equality authority.
for retired_name in (
    "node_transaction_fingerprint_bytes",
    "node_transaction_fingerprint_packet",
    "request_fingerprint",
    "result_fingerprint",
    "accepted_result_fingerprint",
    "survey_gateway_transaction_request_fingerprint",
):
    assert retired_name not in ALL_TRANSACTION_SOURCES, retired_name

assert "uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN]" in NODE_HEADER
assert "uint8_t accepted_result_digest[SEMANTIC_DIGEST_SHA256_LEN]" in NODE_HEADER
assert "uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN]" in NODE_HEADER
assert NODE_SOURCE.count("semantic_digest_equal(") >= 5
assert "memcpy(transaction->accepted_result_digest" in NODE_SOURCE
assert "memcpy(record->request_digest" in NODE_SOURCE
assert "memcpy(record->result_digest" in NODE_SOURCE

# Delayed survey results retain both full commitments, including after the
# active phase has moved into the bounded recent-history cache.
assert "uint8_t request_digest[SEMANTIC_DIGEST_SHA256_LEN]" in SURVEY_HEADER
assert "uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN]" in SURVEY_HEADER
assert SURVEY_SOURCE.count("semantic_digest_equal(") >= 2
assert "memcpy(recent->request_digest" in SURVEY_SOURCE
assert "memcpy(recent->result_digest" in SURVEY_SOURCE
assert "survey_gateway_transaction_request_digest" in SURVEY_SOURCE

# The composed gateway hashes the exact outbound packet and the complete
# received result packet.  The latter is the same semantic commitment carried
# by ACK_CONFIRM, so payload-only hashing cannot promote a different packet.
assert "node_transaction_digest_packet(&outbound->packet" in APP_GLUE
PREFLIGHT_RESULT = function_body(
    APP_GLUE, "gateway_survey_preflight_result"
)
packet_digest = PREFLIGHT_RESULT.index("mesh_packet_semantic_digest(packet,")
reconcile = PREFLIGHT_RESULT.index("survey_gateway_transaction_reconcile_result(")
assert re.search(
    r"mesh_packet_semantic_digest\(\s*packet,\s*payload,\s*"
    r"payload_len,\s*result_digest\s*\)",
    PREFLIGHT_RESULT,
)
assert "node_transaction_digest_bytes(payload" not in PREFLIGHT_RESULT
assert packet_digest < reconcile
assert APP_GLUE.count("survey_gateway_transaction_request_digest(") >= 2
assert "result_token = packet->seq;" in APP_GLUE
assert "uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN]" in APP_STATE

print("transaction digest source invariants passed")
