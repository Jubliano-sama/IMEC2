"""GUI controls for the enumerated anchor roster and its two actions."""

from __future__ import annotations

import tkinter as tk
import time
from tkinter import ttk

from .compact_dialog import show_dialog
from .anchor_actions import ANCHOR_ACTION_COMMANDS, anchor_action_timeout_s
from .command_orchestration import GatewayCommandDispatch, GatewayCommandPlan
from .protocol import CMD_IDENTIFY_ANCHOR, CMD_READ_ANCHOR_BATTERY, TLV_COMMAND_ID


class GatewayAnchorActionsMixin:
    def _build_anchor_action_controls(self, parent):
        frame = ttk.LabelFrame(parent, text="Anchor controls", padding=6)
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
        self.anchor_battery_button = ttk.Button(frame, text="Read all batteries",
            command=self._read_all_batteries)
        self.anchor_battery_button.grid(row=0, column=2)
        ttk.Label(frame, textvariable=self.anchor_action_text).grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(4, 0))
        ttk.Button(frame, text="Show voltages", command=self._show_battery_readings).grid(row=0, column=3, padx=(6, 0))
        self.battery_window = tk.Toplevel(frame)
        self.battery_window.title("Anchor battery voltages")
        self.battery_window.geometry("660x300")
        self.battery_window.protocol("WM_DELETE_WINDOW", self.battery_window.withdraw)
        self.battery_window.withdraw()
        self.battery_window.grid_columnconfigure(0, weight=1)
        self.battery_window.grid_rowconfigure(1, weight=1)
        self.battery_progress_text = tk.StringVar(value="Read all batteries to refresh the enumerated anchors.")
        ttk.Label(self.battery_window, textvariable=self.battery_progress_text).grid(row=0, column=0, sticky="w", padx=8, pady=8)
        table = ttk.Frame(self.battery_window, padding=8)
        table.grid(row=1, column=0, sticky="nsew")
        table.grid_columnconfigure(0, weight=1)
        table.grid_rowconfigure(0, weight=1)
        self.battery_tree = ttk.Treeview(table, columns=("anchor", "voltage", "status"), show="headings", height=6)
        for name, title, width in (("anchor", "Anchor", 220), ("voltage", "Latest voltage", 110),
                                   ("status", "Latest battery request", 150)):
            self.battery_tree.heading(name, text=title)
            self.battery_tree.column(name, width=width)
        self.battery_tree.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(table, orient="vertical", command=self.battery_tree.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.battery_tree.configure(yscrollcommand=scroll.set)
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
        enabled = (command_state == "normal" and model.gateway_id == self.gateway_id
                   and anchor_id is not None and not model.battery_batch_active)
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

        if hasattr(self, "battery_tree"):
            rows = {str(a.node_id) for a in model.anchors.values()}
            for item in self.battery_tree.get_children():
                if item not in rows:
                    self.battery_tree.delete(item)
            for anchor in sorted(model.anchors.values(), key=lambda a: a.slot):
                node_id = anchor.node_id
                reply = model.batteries.get(node_id)
                state = model.battery_outcomes.get(node_id, "Not read")
                latest = model.replies.get(node_id)
                if state == "failed" and latest is not None and latest.stale_assignment:
                    state = "Failed: enumerate again"
                if model.battery_queue and node_id in model.battery_queue:
                    state = "Reading…" if model.pending and model.pending.anchor.node_id == node_id else "Queued"
                values = (f"slot {anchor.slot} · {node_id:016x}",
                          f"{reply.battery_mv / 1000:.3f} V" if reply else "—", state)
                item = str(node_id)
                if self.battery_tree.exists(item):
                    self.battery_tree.item(item, values=values)
                else:
                    self.battery_tree.insert("", "end", iid=item, values=values)
            completed = len(model.battery_outcomes)
            if model.battery_batch_active:
                text = f"Reading batteries: {completed}/{completed + len(model.battery_queue)} complete"
            elif completed:
                successes = sum(outcome == "complete" for outcome in model.battery_outcomes.values())
                text = f"Batteries: {successes}/{completed} read" + (f" · {completed - successes} failed" if successes != completed else "")
            else:
                text = "Read all batteries to refresh voltages; hover an anchor for its latest reading."
            self.battery_progress_text.set(text)

    def _anchor_can_identify(self, anchor_id):
        try:
            node_id = int(anchor_id, 16) if isinstance(anchor_id, str) else int(anchor_id)
        except (ValueError, TypeError):
            return False
        return (self.connected and self._survey_phase == "idle"
                and not self.command_orchestrator.active and self.command_request_tracker.pending is None
                and not self.anchor_actions.battery_batch_active
                and self.anchor_actions.gateway_id == self.gateway_id
                and node_id in self.anchor_actions.anchors)

    def _anchor_hover_text(self, anchor_id):
        node_id = int(anchor_id, 16) if isinstance(anchor_id, str) else int(anchor_id)
        return f"Anchor {node_id:016x}\n{self.anchor_actions.battery_text(node_id)}"

    def _identify_map_anchor(self, anchor_id):
        if not self._anchor_can_identify(anchor_id):
            return
        node_id = int(anchor_id, 16) if isinstance(anchor_id, str) else int(anchor_id)
        self._select_anchor_action(anchor_id)
        self._send_anchor_action(CMD_IDENTIFY_ANCHOR, anchor_id=node_id)

    def _show_battery_readings(self):
        show_dialog(self.battery_window)

    def _read_all_batteries(self):
        try:
            if (not self.connected or self._survey_phase != "idle" or self.command_orchestrator.active
                or self.command_request_tracker.pending is not None):
                raise ValueError("Wait until the current command or survey has finished.")
            if self.anchor_actions.gateway_id != self.gateway_id:
                raise ValueError("Enumerate anchors on this connection first.")
            self.anchor_actions.begin_battery_batch()
        except ValueError as exc:
            self._show_error(str(exc))
            return
        if hasattr(self, "battery_window"):
            self._show_battery_readings()
        self._advance_battery_batch()

    def _advance_battery_batch(self):
        model = self.anchor_actions
        if not model.battery_batch_active or model.pending is not None:
            return
        if (not self.connected or self._survey_phase != "idle" or model.gateway_id != self.gateway_id
            or self.command_orchestrator.active or self.command_request_tracker.pending is not None):
            model.cancel_battery_batch()
            self._update_command_state()
            return
        anchor_id = model.battery_queue[0]
        if not self._send_anchor_action(CMD_READ_ANCHOR_BATTERY, anchor_id=anchor_id):
            model.finish_battery_target(anchor_id, "failed")
            self.root.after_idle(self._advance_battery_batch)

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

    def _send_anchor_action(self, command_id, *, anchor_id=None):
        sent = False
        try:
            if (not self.connected or self._survey_phase != "idle"
                or self.command_orchestrator.active or self.command_request_tracker.pending is not None):
                raise ValueError("Wait until the current command or survey has finished.")
            if anchor_id is None:
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
                raise ValueError("The gateway command could not be submitted.")
            sent = True
        except ValueError as exc:
            self._show_error(str(exc))
        self._update_command_state()
        return sent

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
                # Retire command/batch ownership before rendering feedback.
                # A widget failure must not strand an already-completed target.
                self._apply_gateway_command_transition(transition)
                self.status_text.set(reply.text)
                self._append_log("event" if reply.status == 0 else "error",
                                 f"Anchor {packet.src_id:016x}: {reply.text}")
        return True

    def _finish_anchor_action(self, transition):
        model = getattr(self, "anchor_actions", None)
        if model is not None and model.pending is not None and transition.completed:
            request = model.pending
            model.pending = None
            if request.command_id == CMD_READ_ANCHOR_BATTERY and model.battery_batch_active:
                if transition.outcome == "disconnected":
                    model.cancel_battery_batch()
                else:
                    model.finish_battery_target(request.anchor.node_id, transition.outcome or "failed")
                    self.root.after_idle(self._advance_battery_batch)
            if transition.outcome in ("timeout", "disconnected"):
                self._show_error(f"Anchor command {transition.outcome}; its outcome is unknown.")
