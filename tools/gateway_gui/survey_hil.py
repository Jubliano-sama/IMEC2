"""Run one strict survey through the production GatewayGui and real BLE link."""

from __future__ import annotations

import argparse
import json
import time
import tkinter as tk
from dataclasses import dataclass, field
from typing import Any

from .app import GatewayGui
from .protocol import (
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    COMMAND_NAMES,
    COMMAND_STATUS_NAMES,
    MSG_COMMAND_RESULT,
    MSG_SURVEY_EVENT,
    SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_PLAN_ACCEPTED,
    SURVEY_EVENT_RANGE_PROGRESS,
    SURVEY_EVENT_TERMINAL,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_REASON,
    Packet,
    decode_survey_event,
)


@dataclass
class SurveyHilEvidence:
    """Protocol evidence required before one GUI survey may pass."""

    command_status: dict[int, int] = field(default_factory=dict)
    neighbor_reports: int | None = None
    planned_pairs: int | None = None
    terminal_results: int | None = None
    terminal_usable: int | None = None
    partial_reasons: int = 0
    terminal_seen: bool = False

    def qualifies(
        self,
        *,
        expected_anchors: int,
        expected_pairs: int,
        expected_samples: int,
        gui_pairs: tuple[tuple[int, int], ...],
        gui_results: dict[int, Any],
        gui_error: str,
    ) -> bool:
        return (
            self.command_status.get(CMD_SURVEY_START) == 0
            and self.command_status.get(CMD_SURVEY_PLAN) == 0
            and self.neighbor_reports == expected_anchors
            and self.planned_pairs == expected_pairs
            and self.terminal_seen
            and self.terminal_results == expected_pairs
            and self.terminal_usable == expected_pairs
            and self.partial_reasons == 0
            and len(gui_pairs) == expected_pairs
            and len(gui_results) == expected_pairs
            and all(
                result.usable and result.success_count == expected_samples
                for result in gui_results.values()
            )
            and not gui_error
        )


class SurveyHilGui(GatewayGui):
    """Production GUI with read-only terminal evidence hooks."""

    def __init__(self, root: tk.Tk, evidence: SurveyHilEvidence) -> None:
        self.hil_evidence = evidence
        super().__init__(root)

    def _append_log(self, tag: str, message: str) -> None:
        print(f"GUI_LOG level={tag} message={message}", flush=True)
        super()._append_log(tag, message)

    def _add_packet(
        self, packet: Packet, *, received_at: float | None = None
    ) -> None:
        try:
            if packet.msg_type == MSG_COMMAND_RESULT:
                command_id = packet.value(TLV_COMMAND_ID)
                status = packet.value(TLV_COMMAND_STATUS)
                reason = packet.value(TLV_REASON)
                if isinstance(command_id, int) and isinstance(status, int):
                    self.hil_evidence.command_status[command_id] = status
                print(
                    f"HOST_PACKET command={COMMAND_NAMES.get(command_id, command_id)} "
                    f"status={status} ({COMMAND_STATUS_NAMES.get(status, status)}) "
                    f"reason={reason}",
                    flush=True,
                )
            elif packet.msg_type == MSG_SURVEY_EVENT:
                event = decode_survey_event(packet)
                self.hil_evidence.partial_reasons |= event.partial_reasons
                if event.kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
                    self.hil_evidence.neighbor_reports = len(
                        event.neighbor_reports
                    )
                    print(
                        f"HOST_PACKET survey={event.generation} "
                        f"neighbor_reports={len(event.neighbor_reports)} "
                        f"partial=0x{event.partial_reasons:04x}",
                        flush=True,
                    )
                elif event.kind == SURVEY_EVENT_PLAN_ACCEPTED:
                    self.hil_evidence.planned_pairs = len(event.plan_pairs)
                    print(
                        f"HOST_PACKET survey={event.generation} "
                        f"pairs={len(event.plan_pairs)} waves={event.wave_count} "
                        f"skipped={len(event.skipped_pairs)}",
                        flush=True,
                    )
                elif event.kind in (
                    SURVEY_EVENT_RANGE_PROGRESS,
                    SURVEY_EVENT_TERMINAL,
                ):
                    usable = sum(result.usable for result in event.range_results)
                    if event.kind == SURVEY_EVENT_TERMINAL:
                        self.hil_evidence.terminal_seen = True
                        self.hil_evidence.terminal_results = len(
                            event.range_results
                        )
                        self.hil_evidence.terminal_usable = usable
                    print(
                        f"HOST_PACKET survey={event.generation} kind={event.kind} "
                        f"results={len(event.range_results)} usable={usable} "
                        f"partial=0x{event.partial_reasons:04x}",
                        flush=True,
                    )
        except Exception as exc:
            print(f"HOST_PACKET malformed_survey={exc}", flush=True)
        super()._add_packet(packet, received_at=received_at)


def _emit(kind: str, **fields: object) -> None:
    print(kind, json.dumps(fields, sort_keys=True), flush=True)


def _configure_expected_anchors(
    gui: SurveyHilGui, expected_anchors: int
) -> None:
    """Apply the asserted roster size to the production enumeration input."""
    gui.assignment_expected_anchors_text.set(str(expected_anchors))


def run(args: argparse.Namespace) -> int:
    root = tk.Tk()
    if not args.show_window:
        root.withdraw()
    evidence = SurveyHilEvidence()
    gui = SurveyHilGui(root, evidence)
    _configure_expected_anchors(gui, args.expected_anchors)
    started_at = time.monotonic()
    run_started_at: float | None = None
    terminal_seen_at: float | None = None
    last_state: tuple[object, ...] | None = None
    started = False
    exit_code = 1

    def finish(reason: str) -> None:
        nonlocal exit_code
        pairs = tuple(gui._survey_pairs)
        results = dict(gui._survey_results)
        error = gui.error_text.get()
        success = reason == "survey-complete" and evidence.qualifies(
            expected_anchors=args.expected_anchors,
            expected_pairs=args.expected_pairs,
            expected_samples=args.expected_samples,
            gui_pairs=pairs,
            gui_results=results,
            gui_error=error,
        )
        exit_code = 0 if success else 1
        _emit(
            "GUI_FINAL",
            exit_reason=reason,
            elapsed_s=round(time.monotonic() - started_at, 3),
            connection=gui.connection_state,
            generation=gui._survey_generation,
            pairs=pairs,
            results={
                str(index): {
                    "median_mm": value.median_mm,
                    "success_count": value.success_count,
                    "usable": value.usable,
                }
                for index, value in results.items()
            },
            command_status=evidence.command_status,
            neighbor_reports=evidence.neighbor_reports,
            partial_reasons=evidence.partial_reasons,
            status=gui.status_text.get(),
            error=error,
            success=success,
        )
        gui.transport.shutdown()
        root.destroy()

    def poll() -> None:
        nonlocal last_state, started, run_started_at, terminal_seen_at
        elapsed = time.monotonic() - started_at
        command_active = gui.command_orchestrator.active
        state = (
            gui.connection_state,
            command_active,
            gui._survey_phase,
            gui.status_text.get(),
            gui.error_text.get(),
        )
        if state != last_state:
            _emit(
                "GUI_STATE",
                elapsed_s=round(elapsed, 3),
                connection=state[0],
                command_active=state[1],
                survey_phase=state[2],
                status=state[3],
                error=state[4],
            )
            last_state = state
        if not started and gui.connection_state == "connected":
            started = True
            run_started_at = time.monotonic()
            print(
                f"GUI_ONE_SURVEY_STARTED gateway=0x{gui.gateway_id:016x}",
                flush=True,
            )
            gui._run_survey()
        if (
            started
            and evidence.terminal_seen
            and gui._survey_generation is not None
            and gui._survey_phase == "idle"
        ):
            if terminal_seen_at is None:
                terminal_seen_at = time.monotonic()
            elif time.monotonic() - terminal_seen_at >= args.settle:
                finish("survey-complete")
                return
        elif started:
            terminal_seen_at = None
        if run_started_at is not None and (
            time.monotonic() - run_started_at >= args.timeout
        ):
            finish("run-timeout")
            return
        root.after(100, poll)

    gui.device_text.set(args.target)
    gui._connect()
    root.after(100, poll)
    root.mainloop()
    return exit_code


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Run one real survey through GatewayGui and require exact terminal "
            "command, anchor, pair, sample, and receipt-backed GUI evidence."
        )
    )
    parser.add_argument(
        "target",
        help="BLE address selected by the production GUI, for example E0:85:31:10:C4:17",
    )
    parser.add_argument("--expected-anchors", type=int, default=3)
    parser.add_argument("--expected-pairs", type=int, default=3)
    parser.add_argument("--expected-samples", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument(
        "--show-window",
        action="store_true",
        help="show the production GUI while the automated survey runs",
    )
    args = parser.parse_args()
    if min(
        args.expected_anchors,
        args.expected_pairs,
        args.expected_samples,
    ) <= 0:
        parser.error("expected counts must be positive")
    if args.timeout <= 0 or args.settle < 0:
        parser.error("timeout must be positive and settle must be nonnegative")
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
