"""Run production GET_STATUS and result admission against publication pressure.

The radio/survey snapshot and BLE publication are fault-injection boundaries;
generation parsing, handler decisions, BUSY suppression, and result reservation
conversion execute the production C implementation.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def function(source: str, name: str) -> str:
    match = re.search(
        rf"^(?:static )?(?:int|void|bool) {name}\s*\([^;]*?\)\s*\{{",
        source,
        re.M | re.S,
    )
    if match is None:
        raise AssertionError(f"missing production function {name}")
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


native = r'''
#include "protocol.h"
#include "survey_protocol.h"
#include "app_gateway_command_result.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct app_gateway_command_result_queue results;
static uint32_t gateway_command_result_dispatch_token;
static struct proto_packet command;
static const uint8_t unscoped[] = {
    TLV_COMMAND_ID, 2u,
    CMD_SURVEY_GET_STATUS & 0xffu, CMD_SURVEY_GET_STATUS >> 8,
};
static struct survey_event current;
static int status_error, acceptance_error, publish_error;
static unsigned snapshots, acceptances, publications, terminals;
static enum command_status terminal_status;
static uint32_t published_generation;
static bool plan_available;
static enum survey_event_kind blocked_kind, published_kinds[128];

static int app_survey_gateway_acceptance(enum survey_event_kind kind,
    struct survey_event *event)
{
    acceptances++;
    assert(kind == SURVEY_EVENT_STARTED || kind == SURVEY_EVENT_PLAN_ACCEPTED);
    if (acceptance_error != 0) return acceptance_error;
    if (kind == SURVEY_EVENT_PLAN_ACCEPTED && !plan_available) return -ENOENT;
    memset(event, 0, sizeof(*event));
    event->identity = current.identity;
    event->kind = kind;
    event->host_session_id = UINT32_C(0x10203040);
    event->host_sequence = kind == SURVEY_EVENT_STARTED ? 31u : 32u;
    return 0;
}

static int app_survey_gateway_status(struct survey_event *event)
{
    snapshots++;
    if (status_error == 0) {
        *event = current;
    }
    return status_error;
}

static int gateway_survey_emit_event(const struct survey_event *event)
{
    assert(publications < sizeof(published_kinds) / sizeof(published_kinds[0]));
    published_kinds[publications] = event->kind;
    publications++;
    published_generation = event->identity.generation;
    if (event->kind == SURVEY_EVENT_STARTED || event->kind == SURVEY_EVENT_PLAN_ACCEPTED) {
        assert(event->host_session_id == UINT32_C(0x10203040));
        assert(event->host_sequence == (event->kind == SURVEY_EVENT_STARTED ? 31u : 32u));
    }
    return blocked_kind == 0 || blocked_kind == event->kind ? publish_error : 0;
}

static void gateway_emit_host_command_result_reserved(
    uint32_t token, const struct proto_packet *request,
    enum command_id id, enum command_status status, uint8_t reason)
{
    struct proto_packet terminal, result;
    uint8_t payload[APP_GATEWAY_COMMAND_RESULT_PAYLOAD_LEN];
    size_t len = 0u;

    /* Consume the real reservation, so an erroneous early terminal cannot be
     * hidden by a permissive mock when the worker retries the command. */
    assert(app_gateway_command_result_terminal_command(
        &results, token, request, id, &terminal) == 0);
    result = terminal;
    result.msg_type = MSG_COMMAND_RESULT;
    assert(tlv_append_u16(payload, sizeof(payload), &len,
        TLV_COMMAND_ID, (uint16_t)id) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &len,
        TLV_COMMAND_STATUS, (uint16_t)status) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &len,
        TLV_REASON, reason) == PROTO_OK);
    result.payload_len = (uint16_t)len;
    assert(app_gateway_command_result_commit(
        &results, token, &terminal, id, &result, payload, len) == 0);
    terminal_status = status;
    terminals++;
}
'''

for path, name in (
    ("app/src/app_gateway_command_ingress.c",
     "app_gateway_command_ingress_contention_retryable"),
    ("app/src/app_gateway_result_runtime.inc",
     "gateway_emit_host_command_result"),
    ("app/src/app_anchor_gateway_control.inc", "gateway_get_survey_status"),
):
    native += function((ROOT / path).read_text(), name)

native += r'''
static void reset(void)
{
    app_gateway_command_result_queue_init(&results);
    command = (struct proto_packet) {
        .msg_type = MSG_COMMAND, .src_id = 1u, .dst_id = 2u,
        .session_id = 19u, .seq = 23u,
    };
    memset(&current, 0, sizeof(current));
    current.identity.generation = UINT32_C(0x82135791);
    current.kind = SURVEY_EVENT_STARTED;
    status_error = acceptance_error = publish_error = 0;
    snapshots = acceptances = publications = terminals = 0u;
    plan_available = false;
    blocked_kind = 0;
    published_generation = 0u;
    assert(app_gateway_command_result_reserve(
        &results, &gateway_command_result_dispatch_token) == 0);
    assert(app_gateway_command_result_bind(&results,
        gateway_command_result_dispatch_token, &command,
        CMD_SURVEY_GET_STATUS) == 0);
}

static size_t generation_payload(uint8_t *payload, uint32_t generation)
{
    size_t len = 0u;
    assert(tlv_append_u32(payload, 32u, &len,
        TLV_SURVEY_GENERATION, generation) == PROTO_OK);
    return len;
}

static void expect_terminal(enum command_status status)
{
    assert(terminals == 1u && terminal_status == status);
    assert(app_gateway_command_result_reservation_depth(&results) == 0u);
    assert(app_gateway_command_result_queue_depth(&results) == 1u);
}

static void generation_validation(void)
{
    uint8_t payload[32];
    size_t len;
    reset();
    len = generation_payload(payload, current.identity.generation);
    assert(gateway_get_survey_status(&command, payload, len) == 0);
    assert(snapshots == 1u && publications == 1u);
    assert(published_generation == current.identity.generation);
    expect_terminal(COMMAND_OK);

    /* An unscoped diagnostic query remains supported. */
    reset();
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == 0);
    assert(publications == 1u);
    expect_terminal(COMMAND_OK);

    /* Full-width equality, including same low bits and generation wrap. */
    const uint32_t stale[] = {1u, UINT32_MAX, UINT32_C(0x03135791)};
    for (size_t i = 0u; i < sizeof(stale) / sizeof(stale[0]); i++) {
        reset();
        len = generation_payload(payload, stale[i]);
        assert(gateway_get_survey_status(&command, payload, len) == -ESTALE);
        assert(publications == 0u);
        expect_terminal(COMMAND_INVALID_STATE);
    }

    /* Malformed requests cannot even read the active snapshot. */
    for (unsigned malformed = 0u; malformed < 5u; malformed++) {
        reset();
        len = generation_payload(payload, current.identity.generation);
        switch (malformed) {
        case 0u: len = generation_payload(payload, 0u); break;
        case 1u: payload[1] = 3u; len--; break;
        case 2u: len--; break; /* Truncated value. */
        case 3u:
            assert(tlv_append_u32(payload, sizeof(payload), &len,
                TLV_SURVEY_GENERATION, current.identity.generation) == PROTO_OK);
            break;
        case 4u: payload[len++] = 0xeeu; break; /* Truncated trailing TLV. */
        }
        assert(gateway_get_survey_status(&command, payload, len) == -EINVAL);
        assert(snapshots == 0u && acceptances == 0u && publications == 0u);
        expect_terminal(COMMAND_MALFORMED_PAYLOAD);
    }
}

static void publication_backpressure(void)
{
    uint8_t payload[32];
    const int retryable[] = {-EBUSY, -EAGAIN, -ENOSPC};
    reset();
    size_t len = generation_payload(payload, current.identity.generation);
    uint32_t token = gateway_command_result_dispatch_token;
    for (unsigned round = 0u; round < 10u; round++) {
        for (size_t i = 0u; i < sizeof(retryable) / sizeof(retryable[0]); i++) {
            publish_error = retryable[i];
            assert(gateway_get_survey_status(&command, payload, len) == retryable[i]);
            assert(gateway_command_result_dispatch_token == token);
            assert(terminals == 0u);
            assert(app_gateway_command_result_reservation_depth(&results) == 1u);
            assert(app_gateway_command_result_queue_depth(&results) == 0u);
        }
    }
    publish_error = 0;
    assert(gateway_get_survey_status(&command, payload, len) == 0);
    assert(publications == 31u && snapshots == 1u);
    expect_terminal(COMMAND_OK);

    /* If a newer operation replaces the snapshot between retries, the old
     * query terminalizes stale without publishing that replacement. */
    reset();
    len = generation_payload(payload, current.identity.generation);
    publish_error = -EBUSY;
    assert(gateway_get_survey_status(&command, payload, len) == -EBUSY);
    current.identity.generation++;
    publish_error = 0;
    assert(gateway_get_survey_status(&command, payload, len) == -ESTALE);
    assert(publications == 1u);
    expect_terminal(COMMAND_INVALID_STATE);

    reset();
    publish_error = -EAGAIN;
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == -EAGAIN);
    /* The worker's bounded expiry can still convert the retained credit. */
    gateway_emit_host_command_result(&command, CMD_SURVEY_GET_STATUS,
        COMMAND_TIMEOUT, 0u);
    expect_terminal(COMMAND_TIMEOUT);
}

static void terminal_failures(void)
{
    reset();
    acceptance_error = -ENOENT;
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == -ENOENT);
    assert(publications == 0u);
    expect_terminal(COMMAND_INVALID_STATE);

    reset();
    publish_error = -EIO;
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == -EIO);
    expect_terminal(COMMAND_INTERNAL_ERROR);
}

static void acceptance_replay_precedes_latest_status(void)
{
    reset();
    plan_available = true;
    current.kind = SURVEY_EVENT_RANGE_PROGRESS;
    blocked_kind = SURVEY_EVENT_PLAN_ACCEPTED;
    publish_error = -ENOSPC;
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == -ENOSPC);
    assert(publications == 2u && snapshots == 0u && terminals == 0u);
    assert(published_kinds[0] == SURVEY_EVENT_STARTED);
    assert(published_kinds[1] == SURVEY_EVENT_PLAN_ACCEPTED);
    assert(app_gateway_command_result_reservation_depth(&results) == 1u);

    publish_error = 0;
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == 0);
    assert(publications == 5u && snapshots == 1u);
    assert(published_kinds[2] == SURVEY_EVENT_STARTED);
    assert(published_kinds[3] == SURVEY_EVENT_PLAN_ACCEPTED);
    assert(published_kinds[4] == SURVEY_EVENT_RANGE_PROGRESS);
    expect_terminal(COMMAND_OK);

    /* An acceptance that is also the latest event is emitted once; retaining
     * its host command identity makes replay safe after lost notifications. */
    reset();
    plan_available = true;
    current.kind = SURVEY_EVENT_PLAN_ACCEPTED;
    assert(gateway_get_survey_status(&command, unscoped, sizeof(unscoped)) == 0);
    assert(publications == 2u && snapshots == 1u);
    expect_terminal(COMMAND_OK);
}

int main(void)
{
    generation_validation();
    publication_backpressure();
    terminal_failures();
    acceptance_replay_precedes_latest_status();
    puts("survey GET_STATUS: exact identity, malformed rejection, retry custody and terminal recovery passed");
}
'''

with tempfile.TemporaryDirectory(prefix="gateway-survey-status-") as directory:
    path = Path(directory)
    source = path / "test.c"
    source.write_text(native)
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")),
        "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "include"), "-I", str(ROOT / "app/src"),
        str(source), str(ROOT / "src/protocol.c"),
        str(ROOT / "app/src/app_gateway_command_result.c"),
        "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)
