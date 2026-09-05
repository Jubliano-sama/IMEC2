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
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_SURVEY_EVENT,
    SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_BATCH_COMPLETE,
    SURVEY_EVENT_PLAN_ACCEPTED,
    SURVEY_EVENT_RANGE_PROGRESS,
    SURVEY_EVENT_SIGNALS,
    SURVEY_EVENT_TERMINAL,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_REASON,
    Packet,
    decode_survey_event,
    validate_gateway_command_event_packet,
)


@dataclass
class SurveyHilEvidence:
    """Protocol evidence required before one GUI survey may pass."""

    command_status: dict[int, int] = field(default_factory=dict)
    plan_command_successes: int = 0
    neighbor_reports: int | None = None
    planned_pairs_by_batch: dict[int, int] = field(default_factory=dict)
    completed_batches: set[int] = field(default_factory=set)
    terminal_results: int | None = None
    terminal_usable: int | None = None
    partial_reasons: int = 0
    terminal_seen: bool = False
    signal_measurements: int = 0

    def qualifies(
        self,
        *,
        expected_anchors: int,
        expected_pairs: int,
        expected_samples: int,
        batch_pairs: int,
        gui_pairs: tuple[tuple[int, int], ...],
        gui_results: dict[int, Any],
        gui_error: str,
    ) -> bool:
        expected_batches = (expected_pairs + batch_pairs - 1) // batch_pairs
        expected_batch_sizes = {
            index: min(batch_pairs, expected_pairs - index * batch_pairs)
            for index in range(expected_batches)
        }
        return (
            self.command_status.get(CMD_SURVEY_START) == 0
            and self.command_status.get(CMD_SURVEY_PLAN) == 0
            and self.plan_command_successes == expected_batches
            and self.neighbor_reports == expected_anchors
            and self.signal_measurements >= expected_pairs
            and self.planned_pairs_by_batch == expected_batch_sizes
            and self.completed_batches == set(range(expected_batches - 1))
            and self.terminal_seen
            and self.terminal_results == expected_batch_sizes[expected_batches - 1]
            and self.terminal_usable == expected_batch_sizes[expected_batches - 1]
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
        forwarded = False
        try:
            if packet.msg_type == MSG_COMMAND_RESULT:
                command_id = packet.value(TLV_COMMAND_ID)
                status = packet.value(TLV_COMMAND_STATUS)
                reason = packet.value(TLV_REASON)
                if isinstance(command_id, int) and isinstance(status, int):
                    self.hil_evidence.command_status[command_id] = status
                    if command_id == CMD_SURVEY_PLAN and status == 0:
                        self.hil_evidence.plan_command_successes += 1
                print(
                    f"HOST_PACKET command={COMMAND_NAMES.get(command_id, command_id)} "
                    f"status={status} ({COMMAND_STATUS_NAMES.get(status, status)}) "
                    f"reason={reason}",
                    flush=True,
                )
            elif packet.msg_type == MSG_GATEWAY_COMMAND_EVENT:
                gateway_event = validate_gateway_command_event_packet(packet)
                print(
                    f"HOST_PACKET gateway_event stage={gateway_event.stage} "
                    f"status={gateway_event.command_status} reason={gateway_event.reason} "
                    f"anchor=0x{gateway_event.anchor_id:016x} "
                    f"previous=0x{gateway_event.previous_hop_id:016x} "
                    f"hop={gateway_event.hop_count} slot={gateway_event.discovery_slot} "
                    f"progress={gateway_event.progress_count}/{gateway_event.total_count} "
                    f"success={gateway_event.success_count} "
                    f"failure={gateway_event.failure_count} "
                    f"duplicates={gateway_event.duplicate_count}",
                    flush=True,
                )
            elif packet.msg_type == MSG_SURVEY_EVENT:
                event = decode_survey_event(packet)
                # Let the production GUI apply its generation and dispatch-time
                # gates first.  A reconnect can replay the abandoned survey's
                # terminal packet after the next run has already started; that
                # packet is useful to print, but it is not evidence for the new
                # run.
                forwarded = True
                super()._add_packet(packet, received_at=received_at)
                if not _survey_event_is_current(
                    event_generation=event.generation,
                    current_generation=self._survey_generation,
                ):
                    print(
                        f"HOST_PACKET stale_survey={event.generation} "
                        f"current={self._survey_generation}",
                        flush=True,
                    )
                    return
                self.hil_evidence.partial_reasons |= event.partial_reasons
                if event.kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
                    self.hil_evidence.neighbor_reports = len(
                        event.neighbor_reports
                    )
                    occupied_slots = ",".join(
                        str(slot) for slot in sorted(event.occupied_slots)
                    ) or "none"
                    report_slots = ",".join(
                        str(report.own_slot)
                        for report in sorted(
                            event.neighbor_reports,
                            key=lambda report: report.own_slot,
                        )
                    ) or "none"
                    heard_by_slot = ";".join(
                        f"{report.own_slot}:" + (
                            ",".join(
                                str(slot)
                                for slot in sorted(report.heard_slots)
                            ) or "none"
                        )
                        for report in sorted(
                            event.neighbor_reports,
                            key=lambda report: report.own_slot,
                        )
                    ) or "none"
                    print(
                        f"HOST_PACKET survey={event.generation} "
                        f"neighbor_reports={len(event.neighbor_reports)} "
                        f"occupied_slots={occupied_slots} "
                        f"report_slots={report_slots} "
                        f"heard_by_slot={heard_by_slot} "
                        f"partial=0x{event.partial_reasons:04x}",
                        flush=True,
                    )
                elif event.kind == SURVEY_EVENT_PLAN_ACCEPTED:
                    self.hil_evidence.planned_pairs_by_batch[event.batch_index] = len(
                        event.plan_pairs
                    )
                    print(
                        f"HOST_PACKET survey={event.generation} "
                        f"pairs={len(event.plan_pairs)} waves={event.wave_count} "
                        f"skipped={len(event.skipped_pairs)}",
                        flush=True,
                    )
                elif event.kind == SURVEY_EVENT_SIGNALS:
                    self.hil_evidence.signal_measurements = len(
                        event.signal_measurements
                    )
                    print(
                        f"HOST_PACKET survey={event.generation} "
                        f"signal_pairs={len(event.signal_measurements)}",
                        flush=True,
                    )
                elif event.kind == SURVEY_EVENT_BATCH_COMPLETE:
                    self.hil_evidence.completed_batches.add(event.batch_index)
                    usable = sum(result.usable for result in event.range_results)
                    print(
                        f"HOST_PACKET survey={event.generation} "
                        f"batch_complete={event.batch_index} "
                        f"results={len(event.range_results)} usable={usable}",
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
        if not forwarded:
            super()._add_packet(packet, received_at=received_at)


def _emit(kind: str, **fields: object) -> None:
    print(kind, json.dumps(fields, sort_keys=True), flush=True)


def _configure_expected_anchors(
    gui: SurveyHilGui, expected_anchors: int
) -> None:
    """Show the qualification roster assertion as the GUI's warning hint."""
    gui.assignment_expected_anchors_text.set(str(expected_anchors))


def _disconnect_injection_due(
    *, delay_s: float | None, accepted_at: float | None, now: float
) -> bool:
    return (
        delay_s is not None
        and accepted_at is not None
        and now - accepted_at >= delay_s
    )


def _survey_event_is_current(
    *, event_generation: int, current_generation: int | None
) -> bool:
    return (
        current_generation is not None
        and event_generation == current_generation
    )


def run(args: argparse.Namespace) -> int:
    root = tk.Tk()
    if not args.show_window:
        root.withdraw()
    evidence = SurveyHilEvidence()
    gui = SurveyHilGui(root, evidence)
    gui._survey_batch_pair_limit = args.batch_pairs
    _configure_expected_anchors(gui, args.expected_anchors)
    started_at = time.monotonic()
    run_started_at: float | None = None
    terminal_seen_at: float | None = None
    last_state: tuple[object, ...] | None = None
    reboot_requested = False
    reboot_disconnect_seen = False
    started = False
    survey_start_accepted_at: float | None = None
    exit_code = 1

    def finish(reason: str) -> None:
        nonlocal exit_code
        pairs = tuple(gui._survey_pairs)
        results = dict(gui._survey_results)
        error = gui.error_text.get()
        if reason == "disconnect-injected":
            success = evidence.command_status.get(CMD_SURVEY_START) == 0
        else:
            success = reason == "survey-complete" and evidence.qualifies(
                expected_anchors=args.expected_anchors,
                expected_pairs=args.expected_pairs,
                expected_samples=args.expected_samples,
                batch_pairs=args.batch_pairs,
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
            plan_command_successes=evidence.plan_command_successes,
            planned_pairs_by_batch=evidence.planned_pairs_by_batch,
            completed_batches=sorted(evidence.completed_batches),
            partial_reasons=evidence.partial_reasons,
            signal_measurements=evidence.signal_measurements,
            status=gui.status_text.get(),
            error=error,
            success=success,
        )
        # This is the same graceful BLE teardown used by the real window-close
        # path.  In disconnect-injection mode the firmware must finish and
        # unwind the already accepted survey without any more host traffic.
        gui.transport.shutdown()
        root.destroy()

    def poll() -> None:
        nonlocal last_state, reboot_requested, reboot_disconnect_seen
        nonlocal started, run_started_at, terminal_seen_at
        nonlocal survey_start_accepted_at
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
        if (
            args.reboot_before_survey
            and reboot_requested
            and not reboot_disconnect_seen
            and gui.connection_state in ("reconnecting", "disconnected")
        ):
            reboot_disconnect_seen = True
            print("GUI_REBOOT_DISCONNECT_OBSERVED", flush=True)
        if not started and gui.connection_state == "connected":
            if args.reboot_before_survey and not reboot_requested:
                reboot_requested = True
                print(
                    f"GUI_REBOOT_REQUESTED gateway=0x{gui.gateway_id:016x}",
                    flush=True,
                )
                gui._clear_gateway_memory()
            elif (
                not args.reboot_before_survey
                or (
                    reboot_disconnect_seen
                    and gui._expected_gateway_reboot_identity is None
                )
            ):
                if args.reboot_before_survey:
                    print("GUI_REBOOT_RECONNECTED", flush=True)
                started = True
                run_started_at = time.monotonic()
                print(
                    f"GUI_ONE_SURVEY_STARTED gateway=0x{gui.gateway_id:016x}",
                    flush=True,
                )
                gui._run_survey()
        if (
            args.disconnect_after_start is not None
            and started
            and evidence.command_status.get(CMD_SURVEY_START) == 0
        ):
            if survey_start_accepted_at is None:
                survey_start_accepted_at = time.monotonic()
                print("GUI_SURVEY_START_ACCEPTED_FOR_DISCONNECT", flush=True)
            elif _disconnect_injection_due(
                delay_s=args.disconnect_after_start,
                accepted_at=survey_start_accepted_at,
                now=time.monotonic(),
            ):
                finish("disconnect-injected")
                return
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
        if (
            args.reboot_before_survey
            and reboot_requested
            and not started
            and elapsed >= args.reboot_timeout
        ):
            finish("reboot-timeout")
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
    parser.add_argument(
        "--batch-pairs",
        type=int,
        default=100,
        help=(
            "test-only maximum pairs per PLAN; use 1 to exercise repeated "
            "PLAN/range/offload handoffs"
        ),
    )
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument(
        "--reboot-before-survey",
        action="store_true",
        help=(
            "invoke the production GUI gateway-reboot action and require its "
            "BLE disconnect/reconnect before starting the survey"
        ),
    )
    parser.add_argument("--reboot-timeout", type=float, default=30.0)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument(
        "--disconnect-after-start",
        type=float,
        metavar="SECONDS",
        help=(
            "close the production GUI this many seconds after SURVEY_START is "
            "accepted; this deliberately leaves the gateway to time out and "
            "unwind the survey without further host traffic"
        ),
    )
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
    if (
        args.timeout <= 0
        or args.reboot_timeout <= 0
        or args.settle < 0
        or (
            args.disconnect_after_start is not None
            and args.disconnect_after_start < 0
        )
    ):
        parser.error(
            "timeouts must be positive and delays must be nonnegative"
        )
    if not 1 <= args.batch_pairs <= 100:
        parser.error("batch-pairs must be in 1..100")
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
