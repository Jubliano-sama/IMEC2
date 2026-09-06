"""GUI controls for the enumerated anchor roster and its two actions."""

from __future__ import annotations

import tkinter as tk
import time
from tkinter import ttk

from .anchor_actions import ANCHOR_ACTION_COMMANDS, anchor_action_timeout_s
from .command_orchestration import GatewayCommandDispatch, GatewayCommandPlan
from .protocol import CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY, TLV_COMMAND_ID


class GatewayAnchorActionsMixin:
    def _build_anchor_action_controls(self, parent):
        frame = ttk.LabelFrame(parent, text="Selected anchor", padding=6)
        frame.grid_columnconfigure(0, weight=1)
        self.anchor_selection_text = tk.StringVar(value="Enumerate anchors first")
        self.anchor_action_text = tk.StringVar(value="A successful enumeration enables these commands.")
        self.anchor_selection = ttk.Combobox(frame, textvariable=self.anchor_selection_text,
                                             state="disabled", width=38)
        self.anchor_selection.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        self.anchor_selection.bind("<<ComboboxSelected>>", lambda _event: self._update_command_state())
        self.anchor_identify_button = ttk.Button(frame, text="Identify RGB (10 s)",
            command=lambda: self._send_anchor_action(CMD_IDENTIFY_ANCHOR))
        self.anchor_identify_button.grid(row=0, column=1, padx=(0, 6))
        self.anchor_battery_button = ttk.Button(frame, text="Read battery",
            command=lambda: self._send_anchor_action(CMD_READ_ANCHOR_BATTERY))
        self.anchor_battery_button.grid(row=0, column=2)
        ttk.Label(frame, textvariable=self.anchor_action_text).grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(4, 0))
        self._anchor_choice_ids = {}
        return frame

    def _update_anchor_action_controls(self, command_state):
        if not hasattr(self, "anchor_selection"):
            return
        model = self.anchor_actions
        choices = {
            f"{a.node_id:016x} · slot {a.slot} · {a.hop_count} hop{'s' if a.hop_count != 1 else ''}": a.node_id
            for a in sorted(model.anchors.values(), key=lambda a: a.slot)
        }
        self._anchor_choice_ids = choices
        self.anchor_selection.configure(values=tuple(choices),
            state="readonly" if choices else "disabled")
        if self.anchor_selection_text.get() not in choices:
            self.anchor_selection_text.set(next(iter(choices), "Enumerate anchors first"))
        anchor_id = choices.get(self.anchor_selection_text.get())
        enabled = command_state == "normal" and model.gateway_id == self.gateway_id and anchor_id is not None
        for button in (self.anchor_identify_button, self.anchor_battery_button):
            button.configure(state="normal" if enabled else "disabled")
        if model.pending is not None:
            self.anchor_action_text.set(f"Waiting for {model.pending.anchor.node_id:016x}...")
        elif anchor_id in model.replies:
            reply = model.replies[anchor_id]
            if reply.battery_mv is not None:
                elapsed_s = max(0, time.monotonic() - reply.received_at)
                freshness = (f"received {elapsed_s:.1f} s ago" if reply.age_ms is None else
                             f"sample age {reply.age_ms / 1000 + elapsed_s:.1f} s")
                self.anchor_action_text.set(f"Battery: {reply.battery_mv / 1000:.3f} V · {freshness}")
            else:
                self.anchor_action_text.set(reply.text)
        elif enabled:
            self.anchor_action_text.set(
                f"Enumerated route: {model.anchors[anchor_id].hop_count} hops · "
                f"reply timeout {anchor_action_timeout_s(model.anchors[anchor_id].hop_count):g} s")
        elif not choices:
            self.anchor_action_text.set("A successful enumeration enables these commands.")

    def _select_anchor_action(self, anchor_id):
        if anchor_id is None:
            return
        try:
            node_id = int(anchor_id, 16) if isinstance(anchor_id, str) else int(anchor_id)
        except ValueError:
            return
        for choice, identity in self._anchor_choice_ids.items():
            if identity == node_id:
                self.anchor_selection_text.set(choice)
                self._update_command_state()
                break

    def _send_anchor_action(self, command_id):
        try:
            if (not self.connected or self._survey_phase != "idle"
                or self.command_orchestrator.active or self.command_request_tracker.pending is not None):
                raise ValueError("Wait until the current command or survey has finished.")
            anchor_id = self._anchor_choice_ids.get(self.anchor_selection_text.get())
            session_id, seq = self._next_identity()
            command = self.anchor_actions.prepare(gateway_id=self._require_gateway_identity(),
                host_id=self._parse_int("Host ID", self.host_id_text.get()), anchor_id=anchor_id,
                command_id=command_id, session_id=session_id, sequence=seq)
            pending = self.anchor_actions.pending
            dispatch = GatewayCommandDispatch(command_kind=2, command_id=command_id,
                session_id=session_id, sequence=seq, frame=command.frame, label=command.label,
                timeout_s=anchor_action_timeout_s(pending.anchor.hop_count),
                status_text=f"{command.label}: {anchor_id:016x}, {pending.anchor.hop_count} hops")
            if not self._submit_gateway_command(GatewayCommandPlan.user_triggered(dispatch)):
                self.anchor_actions.pending = None
        except ValueError as exc:
            self._show_error(str(exc))
        self._update_command_state()

    def _observe_anchor_action_result(self, packet, *, received_at=None):
        if packet.value(TLV_COMMAND_ID) not in ANCHOR_ACTION_COMMANDS:
            return False
        transition = self.command_orchestrator.expire(now=received_at)
        if transition.matched:
            self._apply_gateway_command_transition(transition)
            return True
        try:
            reply = self.anchor_actions.observe(packet)
        except ValueError as exc:
            self._show_error(str(exc))
            return True
        if reply is not None:
            transition = self.command_orchestrator.observe_command_result(
                command_id=packet.value(TLV_COMMAND_ID), host_session_id=packet.session_id,
                host_sequence=packet.seq, command_status=reply.status, anchor_result_validated=True,
                received_at=received_at)
            if transition.matched:
                self.status_text.set(reply.text)
                self._append_log("event" if reply.status == 0 else "error",
                                 f"Anchor {packet.src_id:016x}: {reply.text}")
                self._apply_gateway_command_transition(transition)
        return True

    def _finish_anchor_action(self, transition):
        model = getattr(self, "anchor_actions", None)
        if model is not None and model.pending is not None and transition.completed:
            if transition.outcome in ("timeout", "disconnected"):
                self._show_error(f"Anchor command {transition.outcome}; its outcome is unknown.")
            model.pending = None
