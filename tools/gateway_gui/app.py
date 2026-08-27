"""Tkinter desktop GUI for the IMEC gateway BLE test workflow."""

from __future__ import annotations

from datetime import datetime
import queue
import time
import tkinter as tk
from tkinter import ttk
from typing import Any, Literal

from .ble_transport import BLEAK_IMPORT_ERROR, BleDeviceInfo, BleTransport
from .cir_reassembly import CirAssemblyKey, CirReassembler, CirSample
from .command_orchestration import (
    GATEWAY_COMMAND_COMPLETION_GUARD_S,
    ROUTE_REFRESH_DEFAULT_BUDGET_MS,
    GatewayAssignmentReplayBarrier,
    GatewayAssignmentReplayReceipt,
    GatewayCommandDispatch,
    GatewayCommandPlan,
    GatewayCommandTransition,
)
from .command_telemetry import (
    CommandTelemetryDecodeError,
    decode_gateway_command_event,
    is_enumeration_count_mismatch,
)
from .delivery_dedup import (
    CommandEventIdentity,
    GatewayPacketDeduplicator,
    PacketDisposition,
    is_host_delivery_packet,
)
from .diagnostics_integration import GatewayDiagnosticsMixin
from .operation_policy import (
    ASSIGNMENT_DEFAULT_BUDGET_MS,
    ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
    ASSIGNMENT_RESPONSE_SPREAD_MIN_MS,
    ASSIGNMENT_RESPONSE_SPREAD_MAX_MS,
    EXPECTED_ANCHOR_COUNT_MAX,
    COMMAND_BUDGET_MAX_MS as OPERATION_POLICY_COMMAND_BUDGET_MAX_MS,
    AssignmentOperationPolicy,
    OperationPolicyProfile,
    assignment_required_budget_ms,
)
from .protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    CMD_SURVEY_CANCEL,
    CMD_SURVEY_GET_STATUS,
    CMD_SURVEY_PLAN,
    CMD_SURVEY_START,
    COMMAND_NAMES,
    COMMAND_STATUS_NAMES,
    DEFAULT_HOST_ID,
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    GATEWAY_COMMAND_BUDGET_MIN_MS,
    GATEWAY_STREAM_FLAG_TRUNCATED,
    MSG_CLICK_REPORT,
    MSG_SELF_TEST_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    MSG_SURVEY_EVENT,
    Packet,
    SURVEY_EVENT_NEIGHBOR_GRAPH,
    SURVEY_EVENT_PLAN_ACCEPTED,
    SURVEY_EVENT_RANGE_PROGRESS,
    SURVEY_EVENT_TERMINAL,
    SURVEY_TERMINAL_ABORTED,
    SURVEY_TERMINAL_COMPLETE,
    SURVEY_TERMINAL_PARTIAL,
    SurveyAssignmentIdentity,
    SurveyEvent,
    STREAM_CLASS_NAMES,
    TLV_ANCHOR_DIAG_BYTES,
    TLV_ANCHOR_ID,
    TLV_BATTERY_MV,
    TLV_BURST_DURATION_MS,
    TLV_BURST_ID,
    TLV_CHANNEL9_REPORT_LATENCY_MS,
    TLV_CLICKER_CLOCK_OFFSET_RAW,
    TLV_CLICKER_DIAG_BYTES,
    TLV_CLICKER_ID,
    TLV_CLICK_LATENCY_MS,
    TLV_COMMAND_ID,
    TLV_COMMAND_STATUS,
    TLV_DIAG_BYTES_CAPTURED,
    TLV_DIAG_BYTES_TRANSMITTED,
    TLV_DIAG_BYTES_TRUNCATED,
    TLV_DIAG_FRAGMENT_COUNT,
    TLV_DIAG_FRAGMENT_INDEX,
    TLV_DIAG_FRAMES_DROPPED,
    TLV_DIAG_SOURCE,
    TLV_DIAG_STATUS_FLAGS,
    TLV_DISCOVERY_ASSIGNMENT_EPOCH,
    TLV_DISCOVERY_ASSIGNMENT_HASH,
    TLV_DISCOVERY_ASSIGNMENT_PHASE,
    TLV_DISCOVERY_ASSIGNMENT_TABLE,
    TLV_DISTANCE_MM,
    TLV_EVENT_SEQ,
    TLV_ERROR_CODE,
    TLV_EXCHANGE_STRIDE_US,
    TLV_GATEWAY_ACK_LATENCY_MS,
    TLV_MESH_CH9_REPORT_LATENCY_MS,
    TLV_PHY_CONFIG_ID,
    TLV_QUALITY,
    TLV_RANGE_STATUS,
    TLV_REASON,
    TLV_REPORT_FRAGMENT_COUNT,
    TLV_SAMPLE_COUNT,
    TLV_SAMPLE_INDEX,
    TLV_SEQUENCE_START_TIMESTAMPS_MS,
    TLV_TIMESTAMP_MS,
    TLV_UWB_AWAKE_TIME_US,
    TLV_UWB_CARRIER_INTEGRATOR,
    TLV_UWB_CIR_BYTE_OFFSET,
    TLV_UWB_CIR_FIRST_PATH_INDEX,
    TLV_UWB_CIR_FULL_CHUNK,
    TLV_UWB_CIR_SAMPLE,
    TLV_UWB_CIR_START_INDEX,
    TLV_UWB_CIR_TOTAL_BYTES,
    TLV_UWB_CLOCK_OFFSET_RAW,
    TLV_UWB_RAW_TIMESTAMPS,
    TLV_UWB_RSL_DBM,
    TLV_UWB_RX_DIAG_BYTES,
    build_assign_discovery_slots_command,
    build_gateway_host_receipt,
    build_here_i_am_command,
    build_reboot_command,
    build_survey_cancel_command,
    build_survey_plan_command,
    build_survey_start_command,
    click_samples,
    decode_cir_sample,
    decode_survey_event,
    format_device_id,
    hex_dump,
    is_gateway_assignment_publisher_event,
    select_survey_pairs,
)
from .survey_runtime import (
    StaleSurveyEvent,
    SurveyEventNotReady,
    SurveyStateError,
)


APP_BG = "#f2f4f5"
PANEL_BG = "#ffffff"
INK = "#20262b"
MUTED = "#667079"
ACCENT = "#126b5b"
ACCENT_DARK = "#0d5548"
AMBER = "#a56200"
ERROR = "#a72b2b"
ERROR_BG = "#fbe9e8"
HEADER = "#252b30"
SELECTION = "#d8ebe6"
DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT = "3"
GUI_PROTOCOL_REVISION = "survey-event-v1"

class Tooltip:
    def __init__(self, widget: tk.Widget, text: str) -> None:
        self.widget = widget
        self.text = text
        self.window: tk.Toplevel | None = None
        widget.bind("<Enter>", self._show, add=True)
        widget.bind("<Leave>", self._hide, add=True)

    def _show(self, _event: tk.Event[Any]) -> None:
        if self.window is not None:
            return
        x = self.widget.winfo_rootx() + 12
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 6
        self.window = tk.Toplevel(self.widget)
        self.window.wm_overrideredirect(True)
        self.window.wm_geometry(f"+{x}+{y}")
        label = tk.Label(
            self.window,
            text=self.text,
            background="#fffbd8",
            foreground=INK,
            relief="solid",
            borderwidth=1,
            padx=7,
            pady=4,
            wraplength=360,
            justify="left",
        )
        label.pack()

    def _hide(self, _event: tk.Event[Any] | None = None) -> None:
        if self.window is not None:
            self.window.destroy()
            self.window = None


class GatewayGui(GatewayDiagnosticsMixin):
    MAX_PACKET_ROWS = 1000
    MAX_LOG_LINES = 2500

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(
            f"IMEC2 Gateway BLE Console — {GUI_PROTOCOL_REVISION}"
        )
        self.root.geometry("1420x900")
        self.root.minsize(1080, 700)
        self.root.configure(background=APP_BG)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

        self.events: queue.Queue[dict[str, Any]] = queue.Queue()
        self.transport = BleTransport(self.events.put)
        self.packet_by_iid: dict[str, Packet] = {}
        # Keep reliable host records in a bounded RAM cache across BLE
        # reconnects so an exact replay cannot become a second visible/model
        # record. The cache is scoped when the GATT gateway identity arrives;
        # a new GUI process starts a fresh host session.
        self.delivery_dedup = GatewayPacketDeduplicator()
        self.cir_reassembler = CirReassembler()
        self.cir_key_by_packet_id: dict[int, CirAssemblyKey] = {}
        self.cir_errors_by_packet_id: dict[int, tuple[str, ...]] = {}
        self.cir_plot_samples: tuple[CirSample, ...] = ()
        self.cir_plot_start_index: int | None = None
        self.cir_plot_first_path_index: int | None = None
        self.device_by_display: dict[str, BleDeviceInfo] = {}
        self.packet_counter = 0
        self.sequence = 0
        self._last_command_session_id = 0
        self.connected = False
        self.connection_state = "disconnected"
        self.scanning = False
        self.gateway_id: int | None = None
        self._command_progress_text = ""
        self._topology_gateway_id: int | None = None
        self._topology_slot_span: int | None = None
        self._topology_deepest_hop = 0
        self._topology_timing_summary = "Topology estimate pending enumeration."
        self._survey_chain_pending = False
        self._survey_phase = "idle"
        self._survey_generation: int | None = None
        self._survey_assignment: SurveyAssignmentIdentity | None = None
        self._survey_pairs: tuple[tuple[int, int], ...] = ()
        self._survey_results: dict[int, Any] = {}
        self._survey_pending_dispatch: GatewayCommandDispatch | None = None
        self._survey_deferred_dispatch: GatewayCommandDispatch | None = None
        self._initialize_gateway_diagnostics()

        self.connection_text = tk.StringVar(value="Disconnected")
        self.device_text = tk.StringVar()
        self.status_text = tk.StringVar(value="Ready")
        self.error_text = tk.StringVar()
        self.host_id_text = tk.StringVar(value=f"0x{DEFAULT_HOST_ID:016x}")
        self.assignment_expected_anchors_text = tk.StringVar(
            value=DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT
        )
        self.assignment_budget_text = tk.StringVar(
            value=str(ASSIGNMENT_DEFAULT_BUDGET_MS)
        )
        self.assignment_response_spread_text = tk.StringVar(
            value=str(ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS)
        )
        self.assignment_expected_anchors_text.trace_add(
            "write", self._on_assignment_parameters_changed
        )
        self.assignment_response_spread_text.trace_add(
            "write", self._on_assignment_parameters_changed
        )
        self.gateway_id_text = tk.StringVar(value="Unavailable")
        self.gateway_id_source = tk.StringVar(value="Connect to read the gateway firmware DEVICE_ID.")
        self.operation_estimate_text = tk.StringVar(value="")
        self._on_assignment_parameters_changed()
        self.sample_warning_text = tk.StringVar(value="Select a click report to inspect aligned samples.")
        self.cir_state_text = tk.StringVar(value="Select a CIR diagnostic fragment to inspect its assembly.")

        self._configure_styles()
        self._build_ui()
        self._set_connection_state("disconnected")
        if BLEAK_IMPORT_ERROR is not None:
            self._show_error(f"Bleak import failed: {BLEAK_IMPORT_ERROR}")
        self.root.after(50, self._drain_events)

    def _configure_styles(self) -> None:
        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure(".", font=("TkDefaultFont", 10), background=APP_BG, foreground=INK)
        style.configure("TFrame", background=APP_BG)
        style.configure("Panel.TFrame", background=PANEL_BG)
        style.configure("Header.TFrame", background=HEADER)
        style.configure("Header.TLabel", background=HEADER, foreground="#ffffff", font=("TkDefaultFont", 14, "bold"))
        style.configure("HeaderMeta.TLabel", background=HEADER, foreground="#cbd1d5")
        style.configure("Status.TLabel", background=HEADER, foreground="#ffca67", font=("TkDefaultFont", 10, "bold"))
        style.configure("Connected.Status.TLabel", background=HEADER, foreground="#75d2b5")
        style.configure("Error.TFrame", background=ERROR_BG)
        style.configure("Error.TLabel", background=ERROR_BG, foreground=ERROR)
        style.configure("Muted.TLabel", foreground=MUTED, background=APP_BG)
        style.configure("PanelMuted.TLabel", foreground=MUTED, background=PANEL_BG)
        style.configure("Section.TLabel", font=("TkDefaultFont", 11, "bold"), foreground=INK)
        style.configure("Primary.TButton", background=ACCENT, foreground="#ffffff", borderwidth=0, padding=(10, 7))
        style.map("Primary.TButton", background=[("active", ACCENT_DARK), ("disabled", "#a8b6b2")])
        style.configure("Tool.TButton", padding=(8, 6))
        style.configure("Danger.TButton", foreground=ERROR, padding=(8, 6))
        style.configure("TLabelframe", background=APP_BG, borderwidth=1, relief="solid")
        style.configure("TLabelframe.Label", background=APP_BG, foreground=INK, font=("TkDefaultFont", 10, "bold"))
        style.configure("Treeview", background=PANEL_BG, fieldbackground=PANEL_BG, rowheight=26, borderwidth=0)
        style.configure("Treeview.Heading", background="#e3e7e8", foreground=INK, font=("TkDefaultFont", 9, "bold"), relief="flat")
        style.map("Treeview", background=[("selected", SELECTION)], foreground=[("selected", INK)])
        style.configure("TNotebook", background=APP_BG, borderwidth=0)
        style.configure("TNotebook.Tab", padding=(12, 7))

    def _build_ui(self) -> None:
        self.root.grid_rowconfigure(3, weight=1)
        self.root.grid_columnconfigure(0, weight=1)

        header = ttk.Frame(self.root, style="Header.TFrame", padding=(16, 10))
        header.grid(row=0, column=0, sticky="ew")
        header.grid_columnconfigure(1, weight=1)
        ttk.Label(header, text="IMEC2 Gateway BLE Console", style="Header.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(
            header,
            text="Live packets, gateway control, and click diagnostics",
            style="HeaderMeta.TLabel",
        ).grid(row=0, column=1, padx=(18, 0), sticky="w")
        self.connection_label = ttk.Label(header, textvariable=self.connection_text, style="Status.TLabel")
        self.connection_label.grid(row=0, column=2, sticky="e")

        self.error_frame = ttk.Frame(self.root, style="Error.TFrame", padding=(12, 8))
        self.error_frame.grid(row=1, column=0, sticky="ew")
        self.error_frame.grid_columnconfigure(0, weight=1)
        ttk.Label(self.error_frame, textvariable=self.error_text, style="Error.TLabel", wraplength=1100).grid(
            row=0, column=0, sticky="w"
        )
        dismiss = ttk.Button(self.error_frame, text="×", width=3, command=self.error_frame.grid_remove)
        dismiss.grid(row=0, column=1, sticky="e")
        Tooltip(dismiss, "Dismiss this error. The event remains in the activity log.")
        self.error_frame.grid_remove()

        connection = ttk.Frame(self.root, padding=(12, 9))
        connection.grid(row=2, column=0, sticky="ew")
        connection.grid_columnconfigure(2, weight=1)
        self.scan_button = ttk.Button(connection, text="↻ Scan", style="Tool.TButton", command=self._scan)
        self.scan_button.grid(row=0, column=0, padx=(0, 8))
        Tooltip(self.scan_button, "Scan for IMEC devices advertising the gateway service or an IMEC device name.")
        ttk.Label(connection, text="Gateway").grid(row=0, column=1, padx=(0, 6))
        self.device_combo = ttk.Combobox(connection, textvariable=self.device_text, state="normal")
        self.device_combo.grid(row=0, column=2, sticky="ew", padx=(0, 8))
        Tooltip(self.device_combo, "Select a scan result or enter a BLE address manually.")
        self.connect_button = ttk.Button(connection, text="⛓ Connect", style="Primary.TButton", command=self._connect)
        self.connect_button.grid(row=0, column=3, padx=(0, 6))
        Tooltip(self.connect_button, "Connect and subscribe to gateway packet notifications.")
        self.disconnect_button = ttk.Button(connection, text="⏻ Disconnect", style="Danger.TButton", command=self.transport.disconnect)
        self.disconnect_button.grid(row=0, column=4)
        Tooltip(self.disconnect_button, "Disconnect the current BLE gateway link.")

        main = ttk.Panedwindow(self.root, orient="horizontal")
        main.grid(row=3, column=0, sticky="nsew", padx=12, pady=(0, 8))
        controls = ttk.Frame(main, padding=(0, 0, 10, 0))
        workspace = ttk.Frame(main, style="Panel.TFrame")
        main.add(controls, weight=0)
        main.add(workspace, weight=1)
        self._build_controls(controls)
        self._build_workspace(workspace)

        status = ttk.Frame(self.root, padding=(12, 5))
        status.grid(row=4, column=0, sticky="ew")
        status.grid_columnconfigure(0, weight=1)
        ttk.Label(status, textvariable=self.status_text, style="Muted.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(status, text="No simulated transport results", style="Muted.TLabel").grid(row=0, column=1, sticky="e")

    def _build_controls(self, parent: ttk.Frame) -> None:
        parent.configure(width=340)
        parent.grid_propagate(False)
        parent.grid_columnconfigure(0, weight=1)

        # Primary Mesh Network Actions
        operations = ttk.LabelFrame(parent, text="Mesh Network Operations", padding=10)
        operations.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        operations.grid_columnconfigure(0, weight=1)
        operations.grid_columnconfigure(1, weight=1)

        ttk.Label(operations, text="Expected Anchors").grid(row=0, column=0, sticky="w", padx=(0, 8))
        anchors_entry = ttk.Entry(operations, textvariable=self.assignment_expected_anchors_text)
        anchors_entry.grid(row=0, column=1, sticky="ew")
        Tooltip(anchors_entry, "Number of anchors in the fleet (default 3). Enables right-sized slot allocation and fast completion.")

        self.assignment_button = ttk.Button(
            operations,
            text="1. Enumerate & Assign Slots",
            style="Primary.TButton",
            command=self._send_assign_discovery_slots,
        )
        self.assignment_button.grid(
            row=1, column=0, columnspan=2, sticky="ew", pady=(8, 0)
        )
        Tooltip(
            self.assignment_button,
            "Discovers all network anchors, refreshes routing trees, and assigns unique, collision-free discovery slots.",
        )

        self.survey_button = ttk.Button(
            operations,
            text="Run Survey",
            style="Primary.TButton",
            command=self._run_survey,
        )
        self.survey_button.grid(
            row=2, column=0, columnspan=2, sticky="ew", pady=(6, 0)
        )
        Tooltip(
            self.survey_button,
            "Runs a fresh RAM-only enumeration, collects mutual neighbors, then ranges a degree-balanced plan.",
        )

        self.survey_cancel_button = ttk.Button(
            operations,
            text="Abort Active Survey",
            command=self._cancel_survey,
        )
        self.survey_cancel_button.grid(
            row=3, column=0, columnspan=2, sticky="ew", pady=(6, 0)
        )
        Tooltip(
            self.survey_cancel_button,
            "Floods an explicit abort for the active survey generation.",
        )

        self.refresh_button = ttk.Button(
            operations,
            text="Refresh Routes (Here I Am)",
            command=self._send_here_i_am,
        )
        self.refresh_button.grid(
            row=4, column=0, columnspan=2, sticky="ew", pady=(6, 0)
        )
        Tooltip(
            self.refresh_button,
            "Broadcasts a route advertisement flood to rebuild reverse routing paths to the gateway.",
        )

        self.clear_memory_button = ttk.Button(
            operations,
            text="Clear Host Memory / Reboot Gateway",
            style="Danger.TButton",
            command=self._clear_gateway_memory,
        )
        self.clear_memory_button.grid(
            row=5, column=0, columnspan=2, sticky="ew", pady=(6, 0)
        )
        Tooltip(
            self.clear_memory_button,
            "Clears host caches and models; when connected, it also sends a command that reboots the gateway board. Disabled while a command or reconnect is active.",
        )

        self.command_availability_text = tk.StringVar(value="Connect gateway to run a command.")
        ttk.Label(
            operations,
            textvariable=self.command_availability_text,
            style="Muted.TLabel",
            wraplength=295,
            justify="left",
        ).grid(row=6, column=0, columnspan=2, sticky="w", pady=(8, 0))
        ttk.Label(
            operations,
            textvariable=self.operation_estimate_text,
            style="Muted.TLabel",
            wraplength=295,
            justify="left",
        ).grid(row=7, column=0, columnspan=2, sticky="w", pady=(4, 0))

        advanced = ttk.LabelFrame(parent, text="Enumeration Timing", padding=10)
        advanced.grid(row=1, column=0, sticky="ew", pady=(0, 10))
        advanced.grid_columnconfigure(0, weight=1)
        advanced.grid_columnconfigure(1, weight=1)

        self._labeled_spin(advanced, 0, "Response spread (ms)", self.assignment_response_spread_text, 20, 10000)
    def _labeled_spin(
        self,
        parent: ttk.LabelFrame,
        row: int,
        label: str,
        variable: tk.StringVar,
        minimum: int,
        maximum: int,
    ) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)
        widget = ttk.Spinbox(parent, textvariable=variable, from_=minimum, to=maximum, increment=1)
        widget.grid(row=row, column=1, sticky="ew", pady=3)

    def _build_workspace(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(0, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        split = ttk.Panedwindow(parent, orient="vertical")
        split.grid(row=0, column=0, sticky="nsew")

        activity = ttk.Notebook(split)
        inspector = ttk.Notebook(split)
        split.add(activity, weight=3)
        split.add(inspector, weight=2)

        packet_tab = ttk.Frame(activity, style="Panel.TFrame", padding=8)
        log_tab = ttk.Frame(activity, style="Panel.TFrame", padding=8)
        activity.add(packet_tab, text="Packets")
        activity.add(log_tab, text="Activity")
        self._build_gateway_diagnostic_tabs(activity)
        self._build_packet_table(packet_tab)
        self._build_log(log_tab)

        overview = ttk.Frame(inspector, style="Panel.TFrame", padding=8)
        samples = ttk.Frame(inspector, style="Panel.TFrame", padding=8)
        cir_window = ttk.Frame(inspector, style="Panel.TFrame", padding=8)
        diagnostics = ttk.Frame(inspector, style="Panel.TFrame", padding=8)
        tlvs = ttk.Frame(inspector, style="Panel.TFrame", padding=8)
        raw = ttk.Frame(inspector, style="Panel.TFrame", padding=8)
        inspector.add(overview, text="Overview")
        inspector.add(samples, text="Samples")
        inspector.add(cir_window, text="CIR Window")
        inspector.add(diagnostics, text="Diagnostics & CIR")
        inspector.add(tlvs, text="All TLVs")
        inspector.add(raw, text="Raw Bytes")
        self._build_overview(overview)
        self._build_samples(samples)
        self._build_cir_window(cir_window)
        self._build_diagnostics(diagnostics)
        self._build_tlvs(tlvs)
        self._build_raw(raw)

    def _build_packet_table(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(1, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        bar = ttk.Frame(parent, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        bar.grid_columnconfigure(0, weight=1)
        ttk.Label(bar, text="Live packet stream", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        clear = ttk.Button(bar, text="⌫ Clear", style="Tool.TButton", command=self._clear_packets)
        clear.grid(row=0, column=1)
        Tooltip(clear, "Clear the packet list and current inspector selection.")

        columns = ("time", "message", "source", "seq", "flags", "summary")
        self.packet_tree = ttk.Treeview(parent, columns=columns, show="headings", selectmode="browse")
        headings = {
            "time": "Host time",
            "message": "Message",
            "source": "Source",
            "seq": "Seq",
            "flags": "Flags",
            "summary": "Summary",
        }
        widths = {"time": 90, "message": 180, "source": 150, "seq": 60, "flags": 170, "summary": 420}
        for column in columns:
            self.packet_tree.heading(column, text=headings[column])
            self.packet_tree.column(column, width=widths[column], minwidth=50, stretch=column == "summary")
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=self.packet_tree.yview)
        self.packet_tree.configure(yscrollcommand=scrollbar.set)
        self.packet_tree.grid(row=1, column=0, sticky="nsew")
        scrollbar.grid(row=1, column=1, sticky="ns")
        self.packet_tree.bind("<<TreeviewSelect>>", self._packet_selected)
        self._configure_diagnostic_packet_tags()

    def _build_log(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(1, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        bar = ttk.Frame(parent, style="Panel.TFrame")
        bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        bar.grid_columnconfigure(0, weight=1)
        ttk.Label(bar, text="GUI transport events", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        clear = ttk.Button(bar, text="⌫ Clear", style="Tool.TButton", command=lambda: self._set_text(self.log_text, ""))
        clear.grid(row=0, column=1)
        self.log_text = tk.Text(
            parent,
            background=PANEL_BG,
            foreground=INK,
            insertbackground=INK,
            relief="flat",
            wrap="none",
            font=("TkFixedFont", 9),
            state="disabled",
        )
        self.log_text.tag_configure("error", foreground=ERROR)
        self.log_text.tag_configure("tx", foreground=ACCENT_DARK)
        self.log_text.tag_configure("event", foreground=AMBER)
        yscroll = ttk.Scrollbar(parent, orient="vertical", command=self.log_text.yview)
        xscroll = ttk.Scrollbar(parent, orient="horizontal", command=self.log_text.xview)
        self.log_text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.log_text.grid(row=1, column=0, sticky="nsew")
        yscroll.grid(row=1, column=1, sticky="ns")
        xscroll.grid(row=2, column=0, sticky="ew")

    def _build_overview(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(0, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        self.overview_tree = ttk.Treeview(parent, columns=("field", "value"), show="headings")
        self.overview_tree.heading("field", text="Field")
        self.overview_tree.heading("value", text="Value")
        self.overview_tree.column("field", width=240, stretch=False)
        self.overview_tree.column("value", width=720, stretch=True)
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=self.overview_tree.yview)
        self.overview_tree.configure(yscrollcommand=scrollbar.set)
        self.overview_tree.grid(row=0, column=0, sticky="nsew")
        scrollbar.grid(row=0, column=1, sticky="ns")

    def _build_samples(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(1, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        ttk.Label(parent, textvariable=self.sample_warning_text, style="PanelMuted.TLabel").grid(
            row=0, column=0, sticky="ew", pady=(0, 5)
        )
        self.sample_tree = ttk.Treeview(
            parent,
            columns=("index", "distance", "round", "timestamp"),
            show="headings",
        )
        for column, title, width in (
            ("index", "Global sample", 120),
            ("distance", "Distance (mm)", 160),
            ("round", "Round index", 130),
            ("timestamp", "Sequence start timestamp (ms)", 280),
        ):
            self.sample_tree.heading(column, text=title)
            self.sample_tree.column(column, width=width, stretch=column == "timestamp")
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=self.sample_tree.yview)
        self.sample_tree.configure(yscrollcommand=scrollbar.set)
        self.sample_tree.grid(row=1, column=0, sticky="nsew")
        scrollbar.grid(row=1, column=1, sticky="ns")

    def _build_cir_window(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(2, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        ttk.Label(
            parent,
            textvariable=self.cir_state_text,
            style="PanelMuted.TLabel",
            justify="left",
            wraplength=1260,
        ).grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 5))
        self.cir_canvas = tk.Canvas(
            parent,
            background=PANEL_BG,
            height=190,
            highlightthickness=1,
            highlightbackground="#d6dcdf",
        )
        self.cir_canvas.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(0, 6))
        self.cir_canvas.bind("<Configure>", lambda _event: self._redraw_cir_plot())

        columns = (
            "marker",
            "window",
            "absolute",
            "offset",
            "real",
            "imaginary",
            "magnitude",
            "raw",
        )
        self.cir_tree = ttk.Treeview(parent, columns=columns, show="headings")
        for column, title, width in (
            ("marker", "Marker", 115),
            ("window", "Window index", 105),
            ("absolute", "Accumulator index", 130),
            ("offset", "Byte offset", 95),
            ("real", "Raw real (s24)", 125),
            ("imaginary", "Raw imaginary (s24)", 145),
            ("magnitude", "Magnitude", 125),
            ("raw", "Raw 6 bytes", 190),
        ):
            self.cir_tree.heading(column, text=title)
            self.cir_tree.column(column, width=width, stretch=column == "raw")
        yscroll = ttk.Scrollbar(parent, orient="vertical", command=self.cir_tree.yview)
        xscroll = ttk.Scrollbar(parent, orient="horizontal", command=self.cir_tree.xview)
        self.cir_tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.cir_tree.grid(row=2, column=0, sticky="nsew")
        yscroll.grid(row=2, column=1, sticky="ns")
        xscroll.grid(row=3, column=0, sticky="ew")

    def _build_diagnostics(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(0, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        self.diagnostics_text = tk.Text(
            parent,
            background=PANEL_BG,
            foreground=INK,
            relief="flat",
            wrap="word",
            font=("TkFixedFont", 9),
            state="disabled",
        )
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=self.diagnostics_text.yview)
        self.diagnostics_text.configure(yscrollcommand=scrollbar.set)
        self.diagnostics_text.grid(row=0, column=0, sticky="nsew")
        scrollbar.grid(row=0, column=1, sticky="ns")

    def _build_tlvs(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(0, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        columns = ("type", "name", "length", "decoded", "raw")
        self.tlv_tree = ttk.Treeview(parent, columns=columns, show="headings")
        for column, title, width in (
            ("type", "Type", 70),
            ("name", "Name", 230),
            ("length", "Len", 55),
            ("decoded", "Decoded", 430),
            ("raw", "Raw value", 330),
        ):
            self.tlv_tree.heading(column, text=title)
            self.tlv_tree.column(column, width=width, stretch=column in ("decoded", "raw"))
        yscroll = ttk.Scrollbar(parent, orient="vertical", command=self.tlv_tree.yview)
        xscroll = ttk.Scrollbar(parent, orient="horizontal", command=self.tlv_tree.xview)
        self.tlv_tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.tlv_tree.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")

    def _build_raw(self, parent: ttk.Frame) -> None:
        parent.grid_rowconfigure(0, weight=1)
        parent.grid_columnconfigure(0, weight=1)
        self.raw_text = tk.Text(
            parent,
            background=PANEL_BG,
            foreground=INK,
            relief="flat",
            wrap="none",
            font=("TkFixedFont", 9),
            state="disabled",
        )
        yscroll = ttk.Scrollbar(parent, orient="vertical", command=self.raw_text.yview)
        xscroll = ttk.Scrollbar(parent, orient="horizontal", command=self.raw_text.xview)
        self.raw_text.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.raw_text.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")

    def _scan(self) -> None:
        self.status_text.set("Scanning for IMEC BLE devices...")
        self.transport.scan()

    def _connect(self) -> None:
        selected = self.device_text.get().strip()
        device = self.device_by_display.get(selected)
        target = device.address if device is not None else selected
        if not target:
            self._show_error("Select a scan result or enter a BLE address before connecting.")
            return
        self.transport.connect(target)

    def _parse_int(self, label: str, value: str) -> int:
        try:
            return int(value.strip(), 0)
        except ValueError as exc:
            raise ValueError(f"{label} must be a decimal or 0x-prefixed integer") from exc

    def _next_identity(self) -> tuple[int, int]:
        self.sequence = (self.sequence + 1) & 0xFFFF
        if self.sequence == 0:
            self.sequence = 1
        session_id = (time.monotonic_ns() // 1_000_000) & 0xFFFFFFFF
        session_id = session_id or 1
        previous = getattr(self, "_last_command_session_id", 0)
        if previous != 0:
            session_id = (previous + 1) & 0xFFFFFFFF or 1
        self._last_command_session_id = session_id
        return session_id, self.sequence

    def _on_assignment_parameters_changed(self, *args: object) -> None:
        try:
            expected_raw = self.assignment_expected_anchors_text.get().strip()
            expected = int(expected_raw) if expected_raw else 0
            spread_raw = self.assignment_response_spread_text.get().strip()
            spread = (
                int(spread_raw)
                if spread_raw
                else ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS
            )
            if (
                0 <= expected <= EXPECTED_ANCHOR_COUNT_MAX
                and ASSIGNMENT_RESPONSE_SPREAD_MIN_MS
                <= spread
                <= ASSIGNMENT_RESPONSE_SPREAD_MAX_MS
            ):
                budget = assignment_required_budget_ms(
                    spread, expected
                )
                self.assignment_budget_text.set(str(budget))
                estimate_var = getattr(self, "operation_estimate_text", None)
                if estimate_var is not None:
                    estimate_var.set(
                        f"{self._topology_timing_summary} Enumeration up to "
                        f"{self._format_duration_ms(budget)}."
                    )
        except (ValueError, TypeError):
            pass

    @staticmethod
    def _format_duration_ms(duration_ms: int) -> str:
        seconds = max(0, (duration_ms + 999) // 1000)
        hours, remainder = divmod(seconds, 3600)
        minutes, seconds = divmod(remainder, 60)
        return (
            f"{hours:d}:{minutes:02d}:{seconds:02d}"
            if hours else f"{minutes:d}:{seconds:02d}"
        )

    def _operation_policy_profile(
        self, *, ram_only_iteration: bool = False
    ) -> OperationPolicyProfile:
        expected_raw = self.assignment_expected_anchors_text.get().strip()
        expected_anchor_count = (
            self._parse_int("Expected anchors", expected_raw)
            if expected_raw else 0
        )
        return OperationPolicyProfile(
            assignment=AssignmentOperationPolicy(
                expected_anchor_count=expected_anchor_count,
                operation_budget_ms=self._parse_int(
                    "Assignment budget", self.assignment_budget_text.get()
                ),
                response_spread_ms=self._parse_int(
                    "Assignment response spread",
                    self.assignment_response_spread_text.get(),
                ),
                ram_only_iteration=ram_only_iteration,
            ),
        )

    @staticmethod
    def _command_timeout_s(
        command_budget_ms: int | None,
        default_budget_ms: int = GATEWAY_COMMAND_BUDGET_MAX_MS,
    ) -> float:
        effective_budget_ms = (
            default_budget_ms
            if command_budget_ms is None
            else command_budget_ms
        )
        return (
            effective_budget_ms / 1000.0
            + GATEWAY_COMMAND_COMPLETION_GUARD_S
        )

    def _require_gateway_identity(self) -> int:
        if not self.connected:
            raise ValueError("Connect to a gateway before sending commands")
        if self.gateway_id is None:
            raise ValueError("Connected gateway identity is unavailable; reconnect before sending commands")
        return self.gateway_id

    def _submit_gateway_command(self, plan: GatewayCommandPlan) -> bool:
        barrier = getattr(self, "assignment_replay_barrier", None)
        if isinstance(barrier, GatewayAssignmentReplayBarrier) and not barrier.active:
            barrier.reset()
        dispatch = self.command_orchestrator.begin(plan)
        if dispatch is None:
            self.status_text.set("A gateway command is already active")
            return False
        self._update_command_state()
        self._dispatch_gateway_command(dispatch)
        return True

    def _dispatch_gateway_command(self, dispatch: GatewayCommandDispatch) -> None:
        if dispatch.on_dispatch is not None:
            dispatch.on_dispatch()
        if getattr(self.command_orchestrator, "phase", None) == "preflight":
            self._command_progress_text = "Refreshing routes"
        elif dispatch.command_kind == 1:
            self._command_progress_text = "Collecting enumeration responses"
        else:
            self._command_progress_text = "Running gateway command"
        self.status_text.set(dispatch.status_text)
        self.transport.send_frame(dispatch.frame, dispatch.label)

    def _apply_gateway_command_transition(
        self, transition: GatewayCommandTransition
    ) -> None:
        if not transition.matched:
            return
        if transition.dispatch is not None:
            self._dispatch_gateway_command(transition.dispatch)
        elif (
            transition.matched
            and self.command_orchestrator.phase == "target_wait"
        ):
            self.status_text.set(
                "Waiting for restored assignment telemetry to be receipted..."
            )
        elif transition.completed and transition.phase == "preflight":
            self.status_text.set(
                "Here I Am preflight failed; requested command was not sent"
            )
            if self._survey_chain_pending:
                self._survey_chain_pending = False
                self._survey_phase = "idle"
        if transition.completed:
            self._command_progress_text = ""
        self._refresh_survey_view()
        self._update_command_state()

    def _observe_operation_progress(self, event: Any) -> None:
        pending = self.command_request_tracker.pending
        if pending is None or (
            event.command_kind,
            event.host_session_id,
            event.host_sequence,
        ) != (
            pending.command_kind,
            pending.host_session_id,
            pending.host_sequence,
        ):
            return
        if self._survey_chain_pending or self._survey_phase == "enumerating":
            try:
                self.survey_model.observe_command_event(event)
            except SurveyStateError as exc:
                self.survey_model.fail("enumeration", str(exc))
                self._survey_chain_pending = False
                self._survey_phase = "idle"
                self._show_error(f"Survey enumeration telemetry conflict: {exc}")
            self._refresh_survey_view()
        if event.command_kind == 1:
            anchors = self.command_timeline_model.enumerated_anchors.get(
                event.correlation_key, {}
            )
            if event.stage == 6:
                expected_raw = self.assignment_expected_anchors_text.get().strip()
                expected = int(expected_raw) if expected_raw else event.total_count
                if event.discovery_slot == 255:
                    suffix = f"/{expected}" if expected else ""
                    self._command_progress_text = (
                        f"Collecting responses {len(anchors)}{suffix}"
                    )
                else:
                    total = event.total_count or len(anchors)
                    self._command_progress_text = (
                        f"Publishing slot map {event.progress_count}/{total}"
                    )
            elif event.stage == 7:
                self._command_progress_text = "Publishing exact slot table"
            elif event.stage == 8:
                total = event.total_count or len(anchors)
                self._command_progress_text = (
                    f"Confirmations {event.progress_count}/{total}"
                )
            if event.terminal:
                self._store_enumeration_timing(event, anchors)
                if self._survey_chain_pending:
                    count_mismatch = (
                        is_enumeration_count_mismatch(event)
                        and len(anchors) == event.success_count
                    )
                    enumeration_ok = (
                        not (event.flags & 0x04)
                        and event.command_status == 0
                        and (
                            (event.reason == 0 and event.failure_count == 0)
                            or count_mismatch
                        )
                        and len(anchors) > 0
                        and len(anchors) == (
                            event.success_count
                            if count_mismatch
                            else event.total_count
                        )
                        and all(
                            detail.discovery_slot != 255
                            and detail.hop_count != 0
                            for detail in anchors.values()
                        )
                    )
                    if enumeration_ok:
                        if count_mismatch:
                            self._append_log(
                                "warning",
                                f"Survey expected {event.total_count} anchors "
                                f"but found {event.success_count}; continuing "
                                "with the complete discovered slot table.",
                            )
                        self._command_progress_text = (
                            "Survey: starting neighbor collection"
                        )
                        self.root.after_idle(
                            self._start_survey_after_enumeration
                        )
                    else:
                        self._survey_chain_pending = False
                        self._survey_phase = "idle"
                        self._append_log(
                            "error",
                            "Survey stopped because its fresh enumeration "
                            "did not complete with one exact slot/hop record "
                            "for every anchor.",
                        )
        self._update_command_state()

    def _start_survey_after_enumeration(self) -> None:
        if not self._survey_chain_pending:
            return
        if self.command_orchestrator.active:
            self.root.after(25, self._start_survey_after_enumeration)
            return
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            session_id, seq = self._next_identity()
            command = build_survey_start_command(
                host_id=host_id,
                gateway_id=gateway_id,
                session_id=session_id,
                seq=seq,
            )
        except ValueError as exc:
            self._survey_chain_pending = False
            self._survey_phase = "idle"
            self._show_error(str(exc))
            return
        self._survey_chain_pending = False
        self._survey_phase = "neighbors"
        self._submit_survey_dispatch(
            GatewayCommandDispatch(
                command_kind=2,
                command_id=command.command_id,
                session_id=session_id,
                sequence=seq,
                frame=command.frame,
                label=command.label,
                timeout_s=60.0,
                status_text=(
                    "Enumeration complete; starting scheduled neighbor "
                    "collection..."
                ),
            )
        )

    def _submit_survey_plan(self, event: SurveyEvent) -> None:
        if (
            self._survey_generation != event.generation
            or self._survey_assignment != event.assignment
            or self._survey_phase != "planning"
        ):
            return
        try:
            pairs = select_survey_pairs(event)
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            session_id, seq = self._next_identity()
            command = build_survey_plan_command(
                host_id=host_id,
                gateway_id=gateway_id,
                session_id=session_id,
                seq=seq,
                generation=event.generation,
                assignment=event.assignment,
                pairs=pairs,
            )
        except ValueError as exc:
            self._survey_phase = "idle"
            self.survey_model.fail("plan", str(exc))
            self._show_error(str(exc))
            self._refresh_survey_view()
            return
        self._survey_pairs = pairs
        try:
            self.survey_model.set_requested_pairs(pairs)
        except SurveyStateError as exc:
            self._survey_phase = "idle"
            self.survey_model.fail("plan", str(exc))
            self._show_error(str(exc))
            self._refresh_survey_view()
            return
        self._survey_phase = "submitting-plan"
        self._submit_survey_dispatch(
            GatewayCommandDispatch(
                command_kind=2,
                command_id=command.command_id,
                session_id=session_id,
                sequence=seq,
                frame=command.frame,
                label=command.label,
                timeout_s=60.0,
                status_text=(
                    f"Submitting {len(pairs)} mutual pair"
                    f"{'s' if len(pairs) != 1 else ''} for ranging..."
                ),
            )
        )

    def _submit_survey_dispatch(self, dispatch: GatewayCommandDispatch) -> bool:
        if self.survey_command_owner.pending is not None:
            if self._survey_deferred_dispatch is not None:
                self.survey_model.fail(
                    "plan", "A second survey command was queued before the first completed"
                )
                self._show_error(self.survey_model.error or "Survey command queue conflict")
                self._refresh_survey_view()
                return False
            self._survey_deferred_dispatch = dispatch
            self.status_text.set(
                f"{dispatch.label} is queued behind the current survey command result"
            )
            self._refresh_survey_view()
            return True
        if not self.survey_command_owner.begin(
            dispatch.command_id,
            dispatch.session_id,
            dispatch.sequence,
            dispatch.label,
            timeout_s=dispatch.timeout_s,
        ):
            return False
        self._survey_pending_dispatch = dispatch
        self.survey_model.note_command_dispatched(dispatch.command_id)
        self._dispatch_gateway_command(dispatch)
        self._refresh_survey_view()
        self._update_command_state()
        return True

    def _dispatch_deferred_survey_command(self) -> None:
        dispatch = self._survey_deferred_dispatch
        self._survey_deferred_dispatch = None
        if dispatch is not None:
            self._submit_survey_dispatch(dispatch)

    def _observe_survey_event_packet(
        self,
        packet: Packet,
        *,
        received_at: float | None = None,
        allow_buffer: bool = True,
    ) -> bool:
        try:
            event = decode_survey_event(packet)
        except Exception as exc:
            self._show_error(f"Malformed survey event: {exc}")
            return True
        observed_at = time.monotonic() if received_at is None else received_at
        created_at = observed_at - max(packet.age_ms, 0) / 1000.0
        try:
            self.survey_model.observe_survey_event(
                event,
                created_at=created_at,
            )
        except SurveyEventNotReady as exc:
            if not allow_buffer:
                self._append_log("error", f"Ignored survey event: {exc}")
                return False
            controlling_command = (
                "START" if not self.survey_model.start_accepted else "PLAN"
            )
            controlling_step = (
                "neighbors" if controlling_command == "START" else "plan"
            )
            duplicate = any(
                (
                    buffered.src_id,
                    buffered.dst_id,
                    buffered.session_id,
                    buffered.seq,
                    buffered.flags,
                    buffered.payload,
                )
                == (
                    packet.src_id,
                    packet.dst_id,
                    packet.session_id,
                    packet.seq,
                    packet.flags,
                    packet.payload,
                )
                for buffered, _ in self._survey_event_buffer
            )
            if duplicate:
                self.status_text.set(
                    "Repeated early survey event is still waiting for "
                    f"{controlling_command} acceptance"
                )
            elif len(self._survey_event_buffer) >= 16:
                self.survey_model.fail(
                    controlling_step,
                    "Survey event buffer exceeded 16 reliable records before "
                    f"{controlling_command} acceptance",
                )
                self._survey_phase = "idle"
                self._show_error(self.survey_model.error or str(exc))
            else:
                self._survey_event_buffer.append((packet, received_at))
                self.status_text.set(
                    "Survey event received early; waiting for the exact "
                    f"{controlling_command} result"
                )
            self._refresh_survey_view()
            return False
        except StaleSurveyEvent as exc:
            self._append_log(
                "error",
                f"Ignored stale survey generation {event.generation}: {exc}",
            )
            return True
        except SurveyStateError as exc:
            step = {
                SURVEY_EVENT_NEIGHBOR_GRAPH: "neighbors",
                SURVEY_EVENT_PLAN_ACCEPTED: "plan",
            }.get(event.kind, "ranging")
            self.survey_model.fail(step, str(exc))
            self._survey_phase = "idle"
            self._show_error(f"Survey state conflict: {exc}")
            self._refresh_survey_view()
            return True

        self._survey_generation = self.survey_model.generation
        self._survey_assignment = self.survey_model.assignment
        self._survey_results = dict(self.survey_model.results)
        if event.kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
            self._survey_phase = "planning"
            self.status_text.set(
                f"Neighbor graph received from {len(event.neighbor_reports)} "
                "anchors; building the degree-4 mutual-edge plan..."
            )
            self.root.after_idle(lambda: self._submit_survey_plan(event))
        elif event.kind == SURVEY_EVENT_PLAN_ACCEPTED:
            self._survey_phase = "ranging"
            self._survey_pairs = tuple(
                (pair.initiator_slot, pair.responder_slot)
                for pair in self.survey_model.plan_pairs
            )
            self.status_text.set(
                f"Survey plan accepted: {len(self._survey_pairs)} pairs in "
                f"{event.wave_count} fixed wave"
                f"{'s' if event.wave_count != 1 else ''}."
            )
        elif event.kind == SURVEY_EVENT_RANGE_PROGRESS:
            self._survey_phase = "ranging"
            self.status_text.set(
                f"Survey ranging: {len(self._survey_results)}/"
                f"{len(self._survey_pairs)} pair results received."
            )
        elif event.kind == SURVEY_EVENT_TERMINAL:
            usable = sum(
                result.usable for result in self._survey_results.values()
            )
            self._survey_phase = "idle"
            outcome = {
                SURVEY_TERMINAL_COMPLETE: "complete",
                SURVEY_TERMINAL_PARTIAL: "partial",
                SURVEY_TERMINAL_ABORTED: "aborted",
            }.get(event.status, f"status {event.status}")
            message = (
                f"Survey {outcome}: {usable}/{len(self._survey_pairs)} "
                f"pairs have usable median ranges"
            )
            if event.partial_reasons:
                message += f"; partial flags=0x{event.partial_reasons:04x}"
            self.status_text.set(message)
            self._append_log("event", message)
        self._schedule_survey_geometry_solve()
        self._refresh_survey_view()
        self._update_command_state()
        return True

    def _commit_buffered_survey_packet(self, packet: Packet) -> None:
        """Commit and receipt an early event only after its semantic apply."""
        dedup = self.delivery_dedup
        gateway_scope = getattr(self, "gateway_id", None)
        if gateway_scope is None:
            gateway_scope = packet.dst_id
        if gateway_scope != packet.dst_id:
            self._log_gateway_receipt(
                "error",
                "Buffered survey event was applied after the gateway scope "
                "changed; no host receipt was sent",
            )
            return
        delivery = dedup.observe(packet, commit=False)
        if delivery.disposition is PacketDisposition.CONFLICT:
            self._log_gateway_receipt(
                "error",
                "Buffered survey event conflicted with retained host state; "
                "no host receipt was sent",
            )
            return
        if delivery.disposition is PacketDisposition.NEW:
            try:
                committed = dedup.commit(packet, delivery)
            except Exception as exc:
                self._log_gateway_receipt(
                    "error", f"Buffered survey delivery commit failed: {exc}"
                )
                return
            if not committed:
                self._log_gateway_receipt(
                    "error",
                    "Buffered survey event could not be committed to the "
                    "active RAM scope; no host receipt was sent",
                )
                return
        receipt_frame = self._maybe_send_gateway_host_receipt(
            packet,
            delivery,
            gateway_scope=gateway_scope,
        )
        if receipt_frame is None and delivery.cached:
            self._log_gateway_receipt(
                "error",
                "Buffered survey event remains retained upstream because its "
                "host receipt was not written",
            )

    def _drain_buffered_survey_events(self) -> None:
        buffered = tuple(self._survey_event_buffer)
        self._survey_event_buffer.clear()
        for buffered_packet, buffered_at in buffered:
            applied = self._observe_survey_event_packet(
                buffered_packet,
                received_at=buffered_at,
                allow_buffer=False,
            )
            if applied:
                self._commit_buffered_survey_packet(buffered_packet)

    def _observe_survey_command_result(self, packet: Packet) -> None:
        command_id = packet.value(TLV_COMMAND_ID)
        status = packet.value(TLV_COMMAND_STATUS)
        if command_id not in {
            CMD_SURVEY_START,
            CMD_SURVEY_PLAN,
            CMD_SURVEY_CANCEL,
            CMD_SURVEY_GET_STATUS,
        } or not isinstance(status, int):
            return
        transition = self.survey_command_owner.observe_result(
            command_id,
            packet.session_id,
            packet.seq,
            status,
        )
        if not transition.matched or transition.request is None:
            return
        self._survey_pending_dispatch = None
        if transition.outcome == "accepted":
            self.survey_model.note_command_accepted(command_id)
            if command_id in (CMD_SURVEY_START, CMD_SURVEY_PLAN):
                self._drain_buffered_survey_events()
            self._dispatch_deferred_survey_command()
        else:
            status_name = COMMAND_STATUS_NAMES.get(status, str(status))
            self.survey_model.note_command_rejected(command_id, status_name)
            if command_id == CMD_SURVEY_CANCEL:
                self._survey_phase = "ranging"
            else:
                self._survey_chain_pending = False
                self._survey_phase = "idle"
                self._survey_deferred_dispatch = None
            self._show_error(
                f"{COMMAND_NAMES.get(command_id, 'Survey command')} was "
                f"rejected with {status_name}"
            )
        self._refresh_survey_view()
        self._update_command_state()

    def _expire_survey_command(self) -> None:
        transition = self.survey_command_owner.expire()
        if not transition.matched or transition.request is None:
            return
        command_id = transition.request.command_id
        self._survey_pending_dispatch = None
        self.survey_model.note_command_timeout(command_id)
        if command_id == CMD_SURVEY_CANCEL:
            self._survey_phase = "ranging"
        else:
            self._survey_chain_pending = False
            self._survey_phase = "idle"
            self._survey_deferred_dispatch = None
        self._show_error(f"{transition.request.label} timed out waiting for COMMAND_RESULT")
        self._refresh_survey_view()
        self._update_command_state()

    def _store_enumeration_timing(
        self, event: Any, anchors: dict[int, Any]
    ) -> None:
        gateway_id = getattr(self, "gateway_id", None)
        count_mismatch = (
            is_enumeration_count_mismatch(event)
            and len(anchors) == event.success_count
        )
        if (
            not isinstance(gateway_id, int)
            or gateway_id == 0
            or event.flags & 0x04
            or event.command_status != 0
            or (event.reason != 0 and not count_mismatch)
            or (event.failure_count != 0 and not count_mismatch)
            or len(anchors) == 0
            or len(anchors) != (
                event.success_count if count_mismatch else event.total_count
            )
        ):
            return
        details = tuple(anchors.values())
        if any(
            detail.discovery_slot == 255 or detail.hop_count == 0
            for detail in details
        ):
            return
        anchor_count = len(details)
        deepest_hop = max(detail.hop_count for detail in details)
        slot_span = max(detail.discovery_slot for detail in details) + 1
        previous_hop = self._topology_deepest_hop
        if previous_hop and deepest_hop > previous_hop:
            self._append_log(
                "warning",
                f"Enumeration increased known route depth from {previous_hop} "
                f"to {deepest_hop}; future enumeration timing was enlarged.",
            )
        self._topology_gateway_id = gateway_id
        self._topology_slot_span = slot_span
        self._topology_deepest_hop = deepest_hop
        self.assignment_expected_anchors_text.set(str(anchor_count))
        self._topology_timing_summary = (
            f"Topology: {anchor_count} anchors, max hop {deepest_hop}, "
            f"slot span {slot_span}."
        )
        self._on_assignment_parameters_changed()

    def _reset_topology_timing(self) -> None:
        self._topology_gateway_id = None
        self._topology_slot_span = None
        self._topology_deepest_hop = 0
        self._topology_timing_summary = "Topology estimate pending enumeration."
        if hasattr(self, "assignment_expected_anchors_text"):
            self.assignment_expected_anchors_text.set(
                DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT
            )

    def _here_i_am_dispatch(
        self,
        *,
        host_id: int,
        gateway_id: int,
        operation_policy: OperationPolicyProfile,
        status_text: str,
    ) -> GatewayCommandDispatch:
        session_id, seq = self._next_identity()
        command = build_here_i_am_command(
            host_id=host_id,
            gateway_id=gateway_id,
            session_id=session_id,
            seq=seq,
            operation_policy=operation_policy,
        )
        return GatewayCommandDispatch(
            command_kind=3,
            command_id=command.command_id,
            session_id=session_id,
            sequence=seq,
            frame=command.frame,
            label=command.label,
            timeout_s=self._command_timeout_s(
                None, ROUTE_REFRESH_DEFAULT_BUDGET_MS
            ),
            status_text=status_text,
        )

    def _send_here_i_am(self) -> None:
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            operation_policy = self._operation_policy_profile()
            target = self._here_i_am_dispatch(
                host_id=host_id,
                gateway_id=gateway_id,
                operation_policy=operation_policy,
                status_text="Writing Here I Am route-refresh request over BLE...",
            )
            plan = GatewayCommandPlan.user_triggered(target)
        except ValueError as exc:
            self._show_error(str(exc))
            return
        self._submit_gateway_command(plan)

    def _send_assign_discovery_slots(
        self, *, ram_only_iteration: bool = False
    ) -> bool:
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            session_id, seq = self._next_identity()
            operation_policy = self._operation_policy_profile(
                ram_only_iteration=ram_only_iteration
            )
            assignment_policy = operation_policy.assignment
            command_budget_ms = assignment_policy.operation_budget_ms
            expected_anchor_count = assignment_policy.expected_anchor_count or None
            command = build_assign_discovery_slots_command(
                host_id=host_id,
                gateway_id=gateway_id,
                session_id=session_id,
                seq=seq,
                operation_policy=operation_policy,
            )
            target = GatewayCommandDispatch(
                command_kind=1,
                command_id=command.command_id,
                session_id=session_id,
                sequence=seq,
                frame=command.frame,
                label=command.label,
                timeout_s=self._command_timeout_s(
                    command_budget_ms,
                    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
                ),
                status_text=(
                    (
                        f"Enumerating {expected_anchor_count} expected "
                        f"anchor{'s' if expected_anchor_count != 1 else ''} "
                        "and assigning discovery slots..."
                    )
                    if expected_anchor_count is not None
                    else (
                        "Enumerating an unknown anchor roster across the full "
                        "8-hop horizon; set Expected anchors for fast completion..."
                    )
                ),
            )
            preflight = self._here_i_am_dispatch(
                host_id=host_id,
                gateway_id=gateway_id,
                operation_policy=operation_policy,
                status_text=(
                    "Refreshing mesh routes before anchor enumeration and "
                    "discovery-slot assignment..."
                ),
            )
            plan = GatewayCommandPlan.user_triggered(
                target, preflight=preflight
            )
        except ValueError as exc:
            self._show_error(str(exc))
            return False
        return self._submit_gateway_command(plan)

    def _run_survey(self) -> None:
        self._survey_chain_pending = True
        self._survey_phase = "enumerating"
        self._survey_generation = None
        self._survey_assignment = None
        self._survey_pairs = ()
        self._survey_results.clear()
        self.survey_command_owner.reset()
        self._survey_pending_dispatch = None
        self._survey_deferred_dispatch = None
        self._survey_event_buffer.clear()
        if not self._send_assign_discovery_slots(ram_only_iteration=True):
            self._survey_chain_pending = False
            self._survey_phase = "idle"
        else:
            expected_raw = self.assignment_expected_anchors_text.get().strip()
            expected = int(expected_raw) if expected_raw else 0
            self.survey_model.begin(expected_anchor_count=expected)
            self._command_progress_text = "Survey: fresh RAM-only enumeration"
            self._show_survey_tab()
            self._refresh_survey_view()
            self._update_command_state()

    def _cancel_survey(self) -> None:
        if self._survey_generation is None or self._survey_phase == "idle":
            self._show_error("There is no active survey generation to abort")
            return
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            session_id, seq = self._next_identity()
            command = build_survey_cancel_command(
                host_id=host_id,
                gateway_id=gateway_id,
                session_id=session_id,
                seq=seq,
                generation=self._survey_generation,
            )
        except ValueError as exc:
            self._show_error(str(exc))
            return
        self._survey_phase = "aborting"
        self._submit_survey_dispatch(
            GatewayCommandDispatch(
                command_kind=2,
                command_id=command.command_id,
                session_id=session_id,
                sequence=seq,
                frame=command.frame,
                label=command.label,
                timeout_s=60.0,
                status_text="Flooding an explicit survey abort...",
            )
        )

    def _drain_events(self) -> None:
        try:
            while True:
                event = self.events.get_nowait()
                self._handle_event(event)
        except queue.Empty:
            pass
        # A packet completed by the BLE worker before its deadline remains
        # eligible even if Tk did not service the queue until just after it.
        # Each packet carries its immutable worker-side receive timestamp; only
        # expire against wall time after all already-received events are seen.
        self._expire_gateway_command()
        self._expire_survey_command()
        self._update_command_state()
        if self.root.winfo_exists():
            self.root.after(50, self._drain_events)

    def _handle_event(self, event: dict[str, Any]) -> None:
        if self._handle_diagnostic_event(event):
            return
        kind = event.get("kind")
        if kind == "scan_state":
            self.scanning = bool(event.get("active"))
            self.scan_button.configure(state="disabled" if self.scanning else "normal")
            if not self.scanning and not self.connected:
                self.status_text.set("Scan complete")
        elif kind == "scan_result":
            devices: list[BleDeviceInfo] = event.get("devices", [])
            self.device_by_display = {device.display: device for device in devices}
            values = list(self.device_by_display)
            self.device_combo.configure(values=values)
            if values:
                self.device_text.set(values[0])
                self.status_text.set(f"Found {len(values)} IMEC BLE device(s)")
            else:
                self.status_text.set("No IMEC BLE devices found")
                self._append_log("event", "Scan completed with no IMEC BLE advertisements")
        elif kind == "connection_state":
            state = str(event.get("state", "disconnected"))
            identity_error = None
            if state == "connected":
                identity_error = self._accept_gateway_identity(
                    event.get("gateway_id"),
                    "Read from the gateway identity characteristic.",
                )
            self._set_connection_state(state)
            target = event.get("target") or ""
            self._append_log("event", f"BLE state: {event.get('state')} {target}".rstrip())
            if identity_error is not None:
                self._show_error(identity_error)
        elif kind == "gateway_identity":
            identity_error = self._accept_gateway_identity(
                event.get("gateway_id"),
                "Read from the gateway identity characteristic.",
            )
            if identity_error is not None:
                self._show_error(identity_error)
        elif kind == "transport_error":
            message = str(event.get("message", "Unknown transport error"))
            self._show_error(message)
        elif kind == "packet":
            packet = event.get("packet")
            if isinstance(packet, Packet):
                received_at = event.get("received_at")
                self._add_packet(
                    packet,
                    received_at=(
                        float(received_at)
                        if isinstance(received_at, (int, float))
                        else None
                    ),
                )
        elif kind == "tx_written":
            label = str(event.get("label", "command"))
            byte_count = int(event.get("byte_count", 0))
            chunks = int(event.get("chunks", 0))
            if label == "gateway host receipt":
                message = (
                    f"BLE write complete for {label}: {byte_count} bytes in "
                    f"{chunks} ATT chunk(s); gateway will release custody after validation"
                )
            else:
                message = (
                    f"BLE write complete for {label}: {byte_count} bytes in {chunks} ATT chunk(s); "
                    "command outcome is pending COMMAND_RESULT"
                )
            self.status_text.set(message)
            self._append_log("tx", message)
            if label == "gateway host receipt":
                raw = event.get("raw")
                replay_receipts = getattr(
                    self, "_assignment_replay_receipts", {}
                )
                token = (
                    replay_receipts.pop(bytes(raw), None)
                    if isinstance(raw, (bytes, bytearray))
                    else None
                )
                barrier = getattr(self, "assignment_replay_barrier", None)
                if (
                    isinstance(token, GatewayAssignmentReplayReceipt)
                    and isinstance(barrier, GatewayAssignmentReplayBarrier)
                    and barrier.receipt_written(token)
                    and not barrier.active
                ):
                    self._apply_gateway_command_transition(
                        self.command_orchestrator.release_waiting_target(
                            now=(
                                float(event["received_at"])
                                if isinstance(
                                    event.get("received_at"), (int, float)
                                )
                                else None
                            )
                        )
                    )

    def _set_connection_state(self, state: str) -> None:
        self.connection_state = state
        self.connected = state == "connected"
        if (
            not self.connected
            and state not in ("connecting", "reconnecting")
            and hasattr(self, "command_orchestrator")
        ):
            self._apply_gateway_command_transition(
                self.command_orchestrator.disconnect()
            )
            barrier = getattr(self, "assignment_replay_barrier", None)
            if isinstance(barrier, GatewayAssignmentReplayBarrier):
                barrier.reset()
            getattr(self, "_assignment_replay_receipts", {}).clear()
            survey_owner = getattr(self, "survey_command_owner", None)
            if survey_owner is not None:
                survey_owner.reset()
            self._survey_pending_dispatch = None
            self._survey_deferred_dispatch = None
            getattr(self, "_survey_event_buffer", []).clear()
            survey_model = getattr(self, "survey_model", None)
            if survey_model is not None and survey_model.active:
                running_step = next(
                    (
                        step.key
                        for step in survey_model.steps.values()
                        if step.state == "running"
                    ),
                    "ranging",
                )
                survey_model.fail(
                    running_step,
                    "BLE disconnected before the survey reached a terminal event",
                )
                self._survey_phase = "idle"
                self._refresh_survey_view()
        if not self.connected and state != "connecting":
            self._clear_gateway_identity("Connect to read the gateway firmware DEVICE_ID.")
        elif state == "connecting":
            self._clear_gateway_identity("Reading the gateway firmware DEVICE_ID...")
        names = {
            "connecting": "Connecting...",
            "reconnecting": "Reconnecting...",
            "connected": "Connected",
            "disconnecting": "Disconnecting...",
            "disconnected": "Disconnected",
        }
        self.connection_text.set(names.get(state, state.title()))
        self.connection_label.configure(
            style="Connected.Status.TLabel" if self.connected else "Status.TLabel"
        )
        busy = state in ("connecting", "reconnecting", "disconnecting")
        self.connect_button.configure(state="disabled" if self.connected or busy else "normal")
        self.disconnect_button.configure(state="normal" if self.connected or state == "reconnecting" else "disabled")
        self._update_command_state()
        if self.connected and self.gateway_id is not None:
            self.status_text.set(
                f"Connected to gateway {format_device_id(self.gateway_id)}; packet notifications active"
            )
        elif self.connected:
            self.status_text.set("Connected, but gateway identity is unavailable; commands are disabled")
        elif state == "disconnected":
            self.status_text.set("Disconnected")

    def _accept_gateway_identity(self, value: Any, source: str) -> str | None:
        if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 0xFFFFFFFFFFFFFFFF:
            self._clear_gateway_identity("Gateway identity unavailable; commands are disabled.")
            return "Connected gateway did not provide a valid 64-bit DEVICE_ID"
        if self.gateway_id is not None and self.gateway_id != value:
            previous = self.gateway_id
            self._clear_gateway_identity("Gateway identity conflict; reconnect before sending commands.")
            return (
                f"Gateway identity contradiction: expected {format_device_id(previous)}, "
                f"observed {format_device_id(value)}"
            )
        topology_gateway_id = getattr(self, "_topology_gateway_id", None)
        if topology_gateway_id is not None and topology_gateway_id != value:
            self._reset_topology_timing()
        self.gateway_id = value
        delivery_dedup = getattr(self, "delivery_dedup", None)
        if delivery_dedup is not None:
            delivery_dedup.set_gateway_id(value)
        self.gateway_id_text.set(format_device_id(value))
        self.gateway_id_source.set(source)
        self._update_command_state()
        return None

    def _clear_gateway_identity(self, source: str) -> None:
        self.gateway_id = None
        self.gateway_id_text.set("Unavailable")
        self.gateway_id_source.set(source)
        self._update_command_state()

    def _update_command_state(self) -> None:
        tracker = getattr(self, "command_request_tracker", None)
        orchestrator = getattr(self, "command_orchestrator", None)
        survey_owner = getattr(self, "survey_command_owner", None)
        survey_pending = (
            survey_owner.pending if survey_owner is not None else None
        )
        command_active = bool(
            getattr(orchestrator, "active", False)
            or (tracker is not None and tracker.pending is not None)
            or survey_pending is not None
        )
        survey_active = getattr(self, "_survey_phase", "idle") != "idle"
        if command_active:
            pending = (
                tracker.pending
                if tracker is not None and tracker.pending is not None
                else survey_pending
            )
            now = time.monotonic()
            remaining_s = 0.0
            if pending is not None:
                remaining_s = max(
                    0.0,
                    pending.started_at + pending.timeout_s - now,
                )
            if (
                orchestrator is not None
                and orchestrator.phase == "preflight"
                and orchestrator.plan is not None
            ):
                remaining_s += orchestrator.plan.target.timeout_s
            progress = (
                self.survey_model.headline
                if survey_pending is not None
                else self._command_progress_text or "Command running"
            )
            reason = (
                f"{progress}. Deadline countdown: "
                f"{self._format_duration_ms(int(remaining_s * 1000))}."
            )
        elif not self.connected:
            reason = "Connect gateway to run a command."
        elif self.gateway_id is None:
            reason = "Waiting for a valid gateway identity."
        else:
            reason = "Ready for a gateway command."
        if survey_active and not command_active:
            reason = (
                "Survey "
                f"{self._survey_phase.replace('-', ' ')}; fixed radio plan active."
            )
        command_state = (
            "normal"
            if reason.startswith("Ready") and not survey_active
            else "disabled"
        )
        self.refresh_button.configure(state=command_state)
        self.assignment_button.configure(state=command_state)
        if hasattr(self, "survey_button"):
            self.survey_button.configure(state=command_state)
        if hasattr(self, "survey_cancel_button"):
            can_abort = (
                survey_active
                and self._survey_generation is not None
                and self.connected
                and self.gateway_id is not None
                and survey_pending is None
                and getattr(self, "_survey_deferred_dispatch", None) is None
                and not getattr(orchestrator, "active", False)
                and self._survey_phase != "aborting"
            )
            self.survey_cancel_button.configure(
                state="normal" if can_abort else "disabled"
            )
        if hasattr(self, "clear_memory_button"):
            clear_blocked = command_active or survey_active or getattr(
                self, "connection_state", "disconnected"
            ) in ("connecting", "reconnecting", "disconnecting")
            self.clear_memory_button.configure(
                state="disabled" if clear_blocked else "normal"
            )
        if hasattr(self, "command_availability_text"):
            self.command_availability_text.set(reason)

    def _show_error(self, message: str) -> None:
        self.error_text.set(message)
        self.error_frame.grid()
        self.status_text.set("Transport or protocol error")
        self._append_log("error", message)

    def _append_log(self, tag: str, message: str) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"{timestamp}  {message}\n", tag)
        line_count = int(self.log_text.index("end-1c").split(".")[0])
        if line_count > self.MAX_LOG_LINES:
            self.log_text.delete("1.0", f"{line_count - self.MAX_LOG_LINES}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    @staticmethod
    def _is_receiptable_gateway_packet(packet: Packet) -> bool:
        """Return whether a packet is a gateway stream record we can receipt."""
        return (
            packet.transport == "gateway-stream-v1"
            and is_host_delivery_packet(packet)
        )

    def _observe_assignment_replay(
        self, packet: Packet
    ) -> GatewayAssignmentReplayReceipt | None:
        if (
            packet.msg_type != MSG_GATEWAY_COMMAND_EVENT
            or not self._is_receiptable_gateway_packet(packet)
        ):
            return None
        try:
            event = decode_gateway_command_event(
                packet.payload,
                valid_statuses=set(COMMAND_STATUS_NAMES),
            )
        except CommandTelemetryDecodeError:
            return None
        if not is_gateway_assignment_publisher_event(event):
            return None
        barrier = getattr(self, "assignment_replay_barrier", None)
        if not isinstance(barrier, GatewayAssignmentReplayBarrier):
            barrier = GatewayAssignmentReplayBarrier()
            self.assignment_replay_barrier = barrier
        return barrier.observe(event)

    def _track_assignment_replay_receipt(
        self,
        frame: bytes | None,
        token: GatewayAssignmentReplayReceipt | None,
    ) -> None:
        if frame is None or token is None:
            return
        receipts = getattr(self, "_assignment_replay_receipts", None)
        if receipts is None:
            receipts = {}
            self._assignment_replay_receipts = receipts
        receipts[bytes(frame)] = token

    def _log_gateway_receipt(self, tag: str, message: str) -> None:
        """Keep receipt failures non-fatal for headless/model receive paths."""
        try:
            self._append_log(tag, message)
        except Exception:
            # Logging must not turn a custody-preserving transport failure into
            # a receive-loop failure, especially for model-only fixtures.
            pass

    def _maybe_send_gateway_host_receipt(
        self,
        packet: Packet,
        delivery: Any,
        *,
        gateway_scope: int | None = None,
    ) -> bytes | None:
        """Receipt a cached stream record while leaving custody upstream on error."""
        if delivery.disposition not in (
            PacketDisposition.NEW,
            PacketDisposition.DUPLICATE,
        ):
            return None
        if not delivery.cached or not self._is_receiptable_gateway_packet(packet):
            return None

        if gateway_scope is None:
            configured_gateway_id = getattr(self, "gateway_id", None)
            gateway_id = (
                packet.dst_id
                if configured_gateway_id is None
                else configured_gateway_id
            )
        else:
            gateway_id = gateway_scope
        if gateway_id != packet.dst_id:
            self._log_gateway_receipt(
                "error",
                "Not sending host receipt: stream destination "
                f"{format_device_id(packet.dst_id)} does not match gateway "
                f"scope {format_device_id(gateway_id)}",
            )
            return None
        if gateway_id == 0:
            # Before the GATT identity event arrives, the stream destination is
            # the only safe gateway scope available to the GUI.
            self._log_gateway_receipt(
                "error",
                "Not sending host receipt: gateway identity is unavailable "
                "and the stream record has no destination",
            )
            return None

        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
        except (AttributeError, ValueError) as exc:
            self._log_gateway_receipt("error", f"Not sending host receipt: {exc}")
            return None

        try:
            receipt = build_gateway_host_receipt(
                packet,
                host_id=host_id,
                gateway_id=gateway_id,
            )
            self.transport.send_frame(receipt.frame, "gateway host receipt")
            return bytes(receipt.frame)
        except Exception as exc:
            # A failed write must not alter the cache or semantic models; the
            # gateway will retain source custody and replay the stream record.
            self._log_gateway_receipt(
                "error",
                f"Gateway host receipt was not sent; upstream custody remains: "
                f"{type(exc).__name__}: {exc}",
            )
            return None

    def _add_packet(
        self, packet: Packet, *, received_at: float | None = None
    ) -> None:
        dedup = getattr(self, "delivery_dedup", None)
        if dedup is None:
            # Keep headless/model fixtures that construct GatewayGui via
            # ``__new__`` on the same safe receive path as the real app.
            dedup = GatewayPacketDeduplicator(
                gateway_id=getattr(self, "gateway_id", None)
            )
            self.delivery_dedup = dedup
        if (
            self._is_receiptable_gateway_packet(packet)
            and getattr(self, "gateway_id", None) is None
            and packet.dst_id != 0
        ):
            # Scope the first stream record to its destination before caching
            # it, so the later GATT identity event preserves the entry across
            # the reconnect/identity handoff.
            try:
                dedup.set_gateway_id(packet.dst_id)
            except ValueError as exc:
                self._log_gateway_receipt(
                    "error", f"Not sending host receipt: {exc}"
                )
        receipt_gateway_scope = None
        if self._is_receiptable_gateway_packet(packet):
            receipt_gateway_scope = getattr(self, "gateway_id", None)
            if receipt_gateway_scope is None:
                receipt_gateway_scope = packet.dst_id
        delivery = dedup.observe(packet, commit=False)
        packet_label = packet.message_name.lower().replace("_", " ")
        if delivery.disposition is PacketDisposition.DUPLICATE:
            # Exact replays still need a fresh transport attempt after a
            # reconnect, but only after the already-committed RAM record is
            # recognized. Conflicts never receive a receipt.
            replay_receipt = self._observe_assignment_replay(packet)
            receipt_frame = self._maybe_send_gateway_host_receipt(
                packet, delivery, gateway_scope=receipt_gateway_scope
            )
            self._track_assignment_replay_receipt(
                receipt_frame, replay_receipt
            )
            identity = delivery.identity
            if isinstance(identity, CommandEventIdentity):
                self._append_log(
                    "event",
                    "Suppressed semantic replay of gateway command event "
                    f"kind={identity.command_kind} stage={identity.stage} "
                    f"anchor={format_device_id(identity.anchor_id) if identity.anchor_id else '-'} "
                    f"slot={identity.discovery_slot}; host delivery is at-least-once",
                )
            elif identity is not None:
                self._append_log(
                    "event",
                    f"Suppressed exact replay of {packet_label} "
                    f"src={format_device_id(identity.src_id)} "
                    f"session={identity.session_id} seq={identity.seq}; "
                    "host delivery is at-least-once",
                )
            return
        if delivery.disposition is PacketDisposition.CONFLICT:
            identity = delivery.identity
            if identity is None:
                self._append_log(
                    "error",
                    f"Rejected malformed {packet_label}; no host receipt was sent",
                )
            elif isinstance(identity, CommandEventIdentity):
                self._append_log(
                    "error",
                    "Conflicting gateway command event reused semantic identity "
                    f"kind={identity.command_kind} stage={identity.stage} "
                    f"anchor={format_device_id(identity.anchor_id) if identity.anchor_id else '-'} "
                    f"slot={identity.discovery_slot}; no host receipt was sent",
                )
            else:
                self._append_log(
                    "error",
                    f"Conflicting {packet_label} reused packet identity "
                    f"src={format_device_id(identity.src_id)} "
                    f"session={identity.session_id} seq={identity.seq}; "
                    "showing the forensic record without applying a second "
                    "semantic mutation",
                )
        canonical_delivery = delivery.disposition is PacketDisposition.NEW
        semantic_applied = True
        replay_receipt = (
            self._observe_assignment_replay(packet)
            if canonical_delivery
            else None
        )
        if canonical_delivery:
            if packet.msg_type == MSG_SURVEY_EVENT:
                semantic_applied = self._observe_survey_event_packet(
                    packet,
                    received_at=received_at,
                )
            elif packet.msg_type == MSG_COMMAND_RESULT:
                self._observe_survey_command_result(packet)
            self._observe_diagnostic_packet(packet, received_at=received_at)
        self.packet_counter += 1
        iid = f"packet-{self.packet_counter}"
        self.packet_by_iid[iid] = packet
        cir_result = (
            self.cir_reassembler.ingest(packet)
            if canonical_delivery
            else None
        )
        if cir_result is not None:
            if cir_result.key is not None:
                self.cir_key_by_packet_id[id(packet)] = cir_result.key
            if cir_result.errors:
                self.cir_errors_by_packet_id[id(packet)] = cir_result.errors
                for error in cir_result.errors:
                    self._append_log("error", f"CIR reassembly: {error}")
        summary = self._packet_summary(packet)
        flags = ",".join(packet.flag_names) or "none"
        self.packet_tree.insert(
            "",
            "end",
            iid=iid,
            values=(
                datetime.now().strftime("%H:%M:%S.%f")[:-3],
                packet.message_name,
                format_device_id(packet.src_id),
                packet.seq,
                flags,
                summary,
            ),
            tags=self._diagnostic_packet_tags(packet),
        )
        self._register_diagnostic_packet_row(packet, iid)
        rows = self.packet_tree.get_children()
        while len(rows) > self.MAX_PACKET_ROWS:
            oldest = rows[0]
            self.packet_tree.delete(oldest)
            removed = self.packet_by_iid.pop(oldest, None)
            if removed is not None:
                self._forget_diagnostic_packet_row(removed)
                self.cir_key_by_packet_id.pop(id(removed), None)
                self.cir_errors_by_packet_id.pop(id(removed), None)
            rows = self.packet_tree.get_children()
        self.packet_tree.see(iid)
        self.status_text.set(f"Received {packet.message_name} seq={packet.seq}")
        self._observe_gateway_id(packet)
        if cir_result is not None and cir_result.key is not None:
            self._refresh_selected_cir(cir_result.key)
        if canonical_delivery and delivery.cached and semantic_applied:
            try:
                committed = dedup.commit(packet, delivery)
            except Exception as exc:
                committed = False
                self._log_gateway_receipt(
                    "error", f"Gateway delivery commit failed: {exc}"
                )
            if committed:
                # The semantic/model path completed, so this RAM entry now
                # represents data the GUI can replay without reapplying.
                receipt_frame = self._maybe_send_gateway_host_receipt(
                    packet, delivery, gateway_scope=receipt_gateway_scope
                )
                self._track_assignment_replay_receipt(
                    receipt_frame, replay_receipt
                )
            else:
                self._log_gateway_receipt(
                    "error",
                    "Gateway delivery was applied but not committed to the "
                    "active RAM scope; no host receipt was sent",
                )

    def _packet_summary(self, packet: Packet) -> str:
        cir_key = self.cir_key_by_packet_id.get(id(packet))
        if cir_key is not None:
            view = self.cir_reassembler.view(cir_key)
            fragment_index = packet.value(TLV_DIAG_FRAGMENT_INDEX)
            byte_offset = packet.value(TLV_UWB_CIR_BYTE_OFFSET)
            chunks = [tlv.raw for tlv in packet.tlvs if tlv.type_id == TLV_UWB_CIR_FULL_CHUNK]
            chunk_bytes = sum(len(chunk) for chunk in chunks)
            if view is not None:
                fragment_label = (
                    f"{fragment_index + 1}/{view.fragment_count}"
                    if isinstance(fragment_index, int)
                    else f"-/{view.fragment_count}"
                )
                return (
                    f"CIR fragment={fragment_label} offset={byte_offset if isinstance(byte_offset, int) else '-'} "
                    f"chunks={len(chunks)} bytes={chunk_bytes} assembly={view.state} "
                    f"fragments={len(view.received_fragment_indices)}/{view.fragment_count} "
                    f"coverage={view.bytes_received}/{view.total_bytes} bytes"
                )
        cir_errors = self.cir_errors_by_packet_id.get(id(packet))
        if cir_errors:
            return f"CIR malformed: {cir_errors[0]}"
        if packet.msg_type == MSG_CLICK_REPORT:
            anchor = packet.value(TLV_ANCHOR_ID)
            clicker = packet.value(TLV_CLICKER_ID)
            event_seq = packet.value(TLV_EVENT_SEQ)
            distance = packet.value(TLV_DISTANCE_MM)
            chunk_rows, _ = click_samples(packet)
            return (
                f"{self._diagnostic_packet_label(packet)} anchor={format_device_id(anchor) if isinstance(anchor, int) else '-'} "
                f"clicker={format_device_id(clicker) if isinstance(clicker, int) else '-'} "
                f"event={event_seq if event_seq is not None else '-'} "
                f"distance={distance if distance is not None else '-'} mm samples={len(chunk_rows)}"
            )
        if packet.msg_type == MSG_SELF_TEST_REPORT:
            clicker = packet.value(TLV_CLICKER_ID)
            event_seq = packet.value(TLV_EVENT_SEQ)
            failure = packet.value(TLV_ERROR_CODE)
            battery_mv = packet.value(TLV_BATTERY_MV)
            return (
                f"clicker={format_device_id(clicker) if isinstance(clicker, int) else '-'} "
                f"event={event_seq if event_seq is not None else '-'} "
                f"failure={failure if failure is not None else '-'} "
                f"battery={battery_mv if battery_mv is not None else '-'} mV"
            )
        if packet.msg_type == MSG_COMMAND_RESULT:
            command_id = packet.value(TLV_COMMAND_ID)
            status = packet.first_tlv(TLV_COMMAND_STATUS)
            command_name = COMMAND_NAMES.get(command_id, "UNKNOWN") if isinstance(command_id, int) else "UNKNOWN"
            reason = packet.value(TLV_REASON)
            assignment_phase = packet.first_tlv(TLV_DISCOVERY_ASSIGNMENT_PHASE)
            if command_id == CMD_ASSIGN_DISCOVERY_SLOTS and assignment_phase is not None:
                epoch = packet.value(TLV_DISCOVERY_ASSIGNMENT_EPOCH, "-")
                return (
                    f"command={command_name} phase={assignment_phase.display} "
                    f"epoch={epoch} status={status.display if status else '-'}"
                )
            if (
                command_id == CMD_ASSIGN_DISCOVERY_SLOTS
                and status is not None
                and status.decoded == 0
                and isinstance(reason, int)
            ):
                return (
                    f"command={command_name} status={status.display} "
                    f"assigned_anchors={reason}"
                )
            return f"command={command_name} status={status.display if status else '-'} reason={packet.value(TLV_REASON, '-')}"
        if packet.msg_type == MSG_SURVEY_EVENT:
            try:
                event = decode_survey_event(packet)
            except Exception as exc:
                return f"malformed survey event: {exc}"
            if event.kind == SURVEY_EVENT_NEIGHBOR_GRAPH:
                return (
                    f"survey={event.generation} neighbor_reports="
                    f"{len(event.neighbor_reports)} partial="
                    f"0x{event.partial_reasons:04x}"
                )
            if event.kind == SURVEY_EVENT_PLAN_ACCEPTED:
                return (
                    f"survey={event.generation} pairs={len(event.plan_pairs)} "
                    f"waves={event.wave_count} skipped={len(event.skipped_pairs)}"
                )
            usable = sum(result.usable for result in event.range_results)
            return (
                f"survey={event.generation} results={len(event.range_results)} "
                f"usable={usable} partial=0x{event.partial_reasons:04x}"
            )
        return f"dst={format_device_id(packet.dst_id)} payload={len(packet.payload)} bytes"

    def _observe_gateway_id(self, packet: Packet) -> None:
        observed: int | None = None
        source = ""
        if packet.dst_id != 0 and packet.msg_type in (MSG_CLICK_REPORT, 0x21, 0x22, 0x51, 0x53, 0x55):
            observed = packet.dst_id
            source = f"Observed as {packet.message_name} destination."
        elif packet.msg_type == MSG_COMMAND_RESULT:
            try:
                host_id = self._parse_int("Host ID", self.host_id_text.get())
            except ValueError:
                host_id = -1
            if packet.dst_id == host_id and packet.src_id != 0:
                observed = packet.src_id
                source = "Observed as local COMMAND_RESULT source."
        if observed is None or self.gateway_id is None or observed == self.gateway_id:
            return
        expected = self.gateway_id
        self._clear_gateway_identity("Gateway identity conflict; reconnect before sending commands.")
        self._show_error(
            f"Gateway identity contradiction: GATT reported {format_device_id(expected)}, "
            f"but packet identity was {format_device_id(observed)} ({source})"
        )

    def _packet_selected(self, _event: tk.Event[Any]) -> None:
        selection = self.packet_tree.selection()
        if not selection:
            return
        packet = self.packet_by_iid.get(selection[0])
        if packet is not None:
            self._populate_inspector(packet)

    def _populate_inspector(self, packet: Packet) -> None:
        self._clear_tree(self.overview_tree)
        rows: list[tuple[str, str]] = [
            ("Transport format", packet.transport),
            ("Message", f"{packet.message_name} (0x{packet.msg_type:02x})"),
            ("Packet flags", f"0x{packet.flags:02x} ({', '.join(packet.flag_names) or 'none'})"),
            ("Source ID", format_device_id(packet.src_id)),
            ("Destination ID", format_device_id(packet.dst_id)),
            ("Session ID", f"{packet.session_id} (0x{packet.session_id:08x})"),
            ("Sequence", str(packet.seq)),
            ("TTL", "not carried by gateway stream record" if packet.ttl is None else str(packet.ttl)),
            (packet.age_kind, str(packet.age_ms)),
            ("Payload length", str(len(packet.payload))),
            ("Raw transport length", str(len(packet.raw_transport))),
            ("Raw shared packet length", "unavailable" if packet.raw_packet is None else str(len(packet.raw_packet))),
        ]
        if packet.stream_class is not None:
            rows.extend(
                (
                    ("Stream class", f"{packet.stream_class} ({STREAM_CLASS_NAMES.get(packet.stream_class, 'UNKNOWN')})"),
                    ("Stream priority", str(packet.stream_priority)),
                    ("Stream flags", f"0x{packet.stream_flags:02x}"),
                    ("Stream payload truncated", "yes" if packet.stream_flags & GATEWAY_STREAM_FLAG_TRUNCATED else "no"),
                )
            )
        if packet.is_click_report:
            for type_id, label in (
                (TLV_CLICKER_ID, "Clicker ID"),
                (TLV_ANCHOR_ID, "Anchor ID"),
                (TLV_EVENT_SEQ, "Event sequence"),
                (TLV_TIMESTAMP_MS, "Packet timestamp ms"),
                (TLV_DISTANCE_MM, "Aggregate distance mm"),
                (TLV_QUALITY, "Quality"),
                (TLV_UWB_RSL_DBM, "UWB RSL dBm"),
                (TLV_UWB_CLOCK_OFFSET_RAW, "Anchor clock offset raw (0x4D)"),
                (TLV_CLICKER_CLOCK_OFFSET_RAW, "Clicker clock offset raw (0xA8)"),
                (TLV_RANGE_STATUS, "Range status"),
                (TLV_SAMPLE_INDEX, "Chunk sample index"),
                (TLV_SAMPLE_COUNT, "Total sample count"),
                (TLV_BURST_ID, "Burst ID"),
            ):
                tlv = packet.first_tlv(type_id)
                rows.append((label, "absent" if tlv is None else tlv.display))
        if packet.msg_type == MSG_COMMAND_RESULT:
            command_id = packet.value(TLV_COMMAND_ID)
            status = packet.first_tlv(TLV_COMMAND_STATUS)
            reason = packet.value(TLV_REASON)
            rows.extend(
                (
                    (
                        "Command",
                        COMMAND_NAMES.get(command_id, "UNKNOWN")
                        if isinstance(command_id, int)
                        else "absent",
                    ),
                    ("Command status", "absent" if status is None else status.display),
                    ("Result reason", "absent" if reason is None else str(reason)),
                )
            )
            if command_id == CMD_ASSIGN_DISCOVERY_SLOTS:
                assigned_count = (
                    str(reason)
                    if packet.first_tlv(TLV_DISCOVERY_ASSIGNMENT_PHASE) is None
                    and status is not None
                    and status.decoded == 0
                    and isinstance(reason, int)
                    else "not available unless command status is OK"
                )
                rows.append(("Assigned anchor count", assigned_count))
        for field_name, value in rows:
            self.overview_tree.insert("", "end", values=(field_name, value))

        self._clear_tree(self.sample_tree)
        samples, warnings = click_samples(packet)
        if packet.is_click_report:
            declared = packet.value(TLV_SAMPLE_COUNT)
            self.sample_warning_text.set(
                "; ".join(warnings)
                if warnings
                else f"{len(samples)} sample row(s) in this packet; declared total={declared if declared is not None else 'absent'}."
            )
            for sample in samples:
                self.sample_tree.insert(
                    "",
                    "end",
                    values=(
                        sample.sample_index,
                        "missing" if sample.distance_mm is None else sample.distance_mm,
                        "missing" if sample.round_index is None else sample.round_index,
                        "missing" if sample.timestamp_ms is None else sample.timestamp_ms,
                    ),
                )
        else:
            self.sample_warning_text.set("Selected packet is not a click report.")

        self._clear_tree(self.tlv_tree)
        for tlv in packet.tlvs:
            self.tlv_tree.insert(
                "",
                "end",
                values=(
                    f"0x{tlv.type_id:02x}",
                    tlv.name
                    + (" (unknown)" if not tlv.known else "")
                    + (" (partial truncated tail)" if tlv.truncated else ""),
                    len(tlv.raw),
                    tlv.display,
                    tlv.raw.hex(" "),
                ),
            )

        self._set_text(self.diagnostics_text, self._diagnostics_text(packet))
        self._set_text(self.raw_text, self._raw_text(packet))
        self._populate_cir_window(packet)

    @staticmethod
    def _cir_ranges(ranges: tuple[tuple[int, int], ...]) -> str:
        return ", ".join(f"[{start},{end})" for start, end in ranges) or "none"

    def _populate_cir_window(self, packet: Packet) -> None:
        self._clear_tree(self.cir_tree)
        key = self.cir_key_by_packet_id.get(id(packet))
        packet_errors = self.cir_errors_by_packet_id.get(id(packet), ())
        if key is None:
            self._set_cir_plot((), None, None)
            if packet_errors:
                self.cir_state_text.set("MALFORMED CIR fragment: " + "; ".join(packet_errors))
            else:
                self.cir_state_text.set("Selected packet is not a CIR diagnostic fragment.")
            return

        view = self.cir_reassembler.view(key)
        if view is None:
            self._set_cir_plot((), None, None)
            self.cir_state_text.set("CIR assembly state is unavailable.")
            return

        missing = ", ".join(str(index) for index in view.missing_fragment_indices) or "none"
        errors = "; ".join(view.errors) or "none"
        sample_note = (
            f"decoded samples={len(view.samples)}"
            if view.complete
            else "samples unavailable until byte coverage is complete and valid"
        )
        self.cir_state_text.set(
            f"{view.state.upper()} | clicker={format_device_id(view.key.clicker_id)} "
            f"anchor={format_device_id(view.key.anchor_id)} event={view.key.event_seq} | "
            f"fragments={len(view.received_fragment_indices)}/{view.fragment_count} missing={missing} | "
            f"bytes={view.bytes_received}/{view.total_bytes} gaps={self._cir_ranges(view.gaps)} | "
            f"start={view.start_index} first-path={view.first_path_index} | {sample_note} | errors={errors}"
        )
        self._set_cir_plot(view.samples, view.start_index, view.first_path_index)
        for sample in view.samples:
            markers: list[str] = []
            if sample.window_index == 0:
                markers.append("START")
            if sample.absolute_index == view.first_path_index:
                markers.append("FIRST PATH")
            self.cir_tree.insert(
                "",
                "end",
                values=(
                    " / ".join(markers),
                    sample.window_index,
                    sample.absolute_index,
                    sample.byte_offset,
                    sample.real,
                    sample.imaginary,
                    f"{sample.magnitude:.3f}",
                    sample.raw.hex(" "),
                ),
            )

    def _set_cir_plot(
        self,
        samples: tuple[CirSample, ...],
        start_index: int | None,
        first_path_index: int | None,
    ) -> None:
        self.cir_plot_samples = samples
        self.cir_plot_start_index = start_index
        self.cir_plot_first_path_index = first_path_index
        self._redraw_cir_plot()

    def _redraw_cir_plot(self) -> None:
        canvas = self.cir_canvas
        canvas.delete("all")
        width = max(canvas.winfo_width(), 360)
        height = max(canvas.winfo_height(), 150)
        left = 66
        right = 18
        top = 18
        bottom = 34
        plot_right = width - right
        plot_bottom = height - bottom
        if not self.cir_plot_samples:
            canvas.create_text(
                width / 2,
                height / 2,
                text="CIR magnitude unavailable until the assembly is complete and valid.",
                fill=MUTED,
            )
            return

        samples = self.cir_plot_samples
        maximum = max(sample.magnitude for sample in samples)
        scale_max = maximum if maximum > 0.0 else 1.0
        canvas.create_line(left, top, left, plot_bottom, fill=MUTED)
        canvas.create_line(left, plot_bottom, plot_right, plot_bottom, fill=MUTED)
        for fraction in (0.0, 0.5, 1.0):
            y = plot_bottom - fraction * (plot_bottom - top)
            value = fraction * scale_max
            canvas.create_line(left - 4, y, plot_right, y, fill="#e2e6e8", dash=(2, 3))
            canvas.create_text(left - 7, y, text=f"{value:.0f}", anchor="e", fill=MUTED)

        x_span = max(len(samples) - 1, 1)
        points: list[float] = []
        for index, sample in enumerate(samples):
            x = left + index * (plot_right - left) / x_span
            y = plot_bottom - sample.magnitude * (plot_bottom - top) / scale_max
            points.extend((x, y))
        if len(points) >= 4:
            canvas.create_line(*points, fill=ACCENT, width=2)

        start_index = self.cir_plot_start_index
        if start_index is not None:
            canvas.create_line(left, top, left, plot_bottom, fill=ACCENT_DARK, width=2)
            canvas.create_text(
                left + 4,
                plot_bottom + 8,
                text=f"start {start_index}",
                anchor="nw",
                fill=ACCENT_DARK,
            )
            canvas.create_text(
                plot_right,
                plot_bottom + 8,
                text=str(start_index + len(samples) - 1),
                anchor="ne",
                fill=MUTED,
            )

        first_path_index = self.cir_plot_first_path_index
        if start_index is not None and first_path_index is not None:
            relative = first_path_index - start_index
            if 0 <= relative < len(samples):
                x = left + relative * (plot_right - left) / x_span
                canvas.create_line(x, top, x, plot_bottom, fill=AMBER, width=2, dash=(5, 3))
                anchor: Literal["nw", "ne"] = (
                    "nw" if relative < len(samples) - 45 else "ne"
                )
                canvas.create_text(
                    x + (4 if anchor == "nw" else -4),
                    top + 2,
                    text=f"first path {first_path_index}",
                    anchor=anchor,
                    fill=AMBER,
                )

    def _refresh_selected_cir(self, key: CirAssemblyKey) -> None:
        selection = self.packet_tree.selection()
        if not selection:
            return
        selected = self.packet_by_iid.get(selection[0])
        if selected is not None and self.cir_key_by_packet_id.get(id(selected)) == key:
            self._populate_cir_window(selected)

    def _diagnostics_text(self, packet: Packet) -> str:
        lines = ["Click/report diagnostics", "=" * 72]
        diagnostic_types = (
            TLV_DIAG_STATUS_FLAGS,
            TLV_BURST_ID,
            TLV_EXCHANGE_STRIDE_US,
            TLV_BURST_DURATION_MS,
            TLV_CLICK_LATENCY_MS,
            TLV_UWB_AWAKE_TIME_US,
            TLV_DIAG_BYTES_CAPTURED,
            TLV_DIAG_BYTES_TRANSMITTED,
            TLV_DIAG_BYTES_TRUNCATED,
            TLV_DIAG_FRAMES_DROPPED,
            TLV_REPORT_FRAGMENT_COUNT,
            TLV_CHANNEL9_REPORT_LATENCY_MS,
            TLV_GATEWAY_ACK_LATENCY_MS,
            TLV_MESH_CH9_REPORT_LATENCY_MS,
            TLV_PHY_CONFIG_ID,
            TLV_UWB_CARRIER_INTEGRATOR,
            TLV_DIAG_FRAGMENT_INDEX,
            TLV_DIAG_FRAGMENT_COUNT,
            TLV_DIAG_SOURCE,
            TLV_UWB_CIR_BYTE_OFFSET,
            TLV_UWB_CIR_TOTAL_BYTES,
            TLV_UWB_CIR_FIRST_PATH_INDEX,
            TLV_UWB_CIR_START_INDEX,
            TLV_UWB_RAW_TIMESTAMPS,
            TLV_DISCOVERY_ASSIGNMENT_PHASE,
            TLV_DISCOVERY_ASSIGNMENT_EPOCH,
            TLV_DISCOVERY_ASSIGNMENT_HASH,
        )
        present = False
        for type_id in diagnostic_types:
            tlv = packet.first_tlv(type_id)
            if tlv is not None:
                lines.append(f"{tlv.name:<36} {tlv.display}")
                present = True
        if not present:
            lines.append("No scalar diagnostic TLVs in this packet.")

        lines.extend(("", "Clock offsets", "-" * 72))
        for type_id, label in (
            (TLV_UWB_CLOCK_OFFSET_RAW, "Anchor clock offset raw (0x4D)"),
            (TLV_CLICKER_CLOCK_OFFSET_RAW, "Clicker clock offset raw (0xA8)"),
        ):
            tlv = packet.first_tlv(type_id)
            lines.append(f"{label:<40} {'absent' if tlv is None else tlv.display}")

        assignment_tables = [
            tlv for tlv in packet.tlvs if tlv.type_id == TLV_DISCOVERY_ASSIGNMENT_TABLE
        ]
        lines.extend(("", "Discovery assignment table", "-" * 72))
        if not assignment_tables:
            lines.append("Absent in this packet.")
        else:
            for index, tlv in enumerate(assignment_tables):
                lines.append(f"Table TLV {index}: {tlv.display}")

        lines.extend(("", "UWB_CIR_SAMPLE", "-" * 72))
        cir_raw = packet.raw_value(TLV_UWB_CIR_SAMPLE)
        cir = decode_cir_sample(cir_raw)
        if cir is None:
            lines.append("Absent in this packet.")
        elif "error" in cir:
            lines.append(str(cir["error"]))
            lines.append(f"Raw: {cir_raw.hex(' ') if cir_raw is not None else '<none>'}")
        else:
            lines.append(f"Raw bytes:       {cir['raw']}")
            lines.append(f"Real signed24:   {cir['real_signed24']}")
            lines.append(f"Imag signed24:   {cir['imag_signed24']}")
            lines.append(f"Magnitude:       {cir['magnitude']:.3f}")
            lines.append("")
            lines.append(
                "This is one DW3000 first-path complex accumulator sample (3-byte real + "
                "3-byte imaginary), not a CIR trace. A waveform plot would invent missing samples."
            )

        for type_id, title in (
            (TLV_CLICKER_DIAG_BYTES, "CLICKER_DIAG_BYTES"),
            (TLV_ANCHOR_DIAG_BYTES, "ANCHOR_DIAG_BYTES"),
            (TLV_UWB_RX_DIAG_BYTES, "UWB_RX_DIAG_BYTES"),
        ):
            raw = packet.raw_value(type_id)
            lines.extend(("", title, "-" * 72))
            lines.append("Absent in this packet." if raw is None else hex_dump(raw))
        chunks = [tlv.raw for tlv in packet.tlvs if tlv.type_id == TLV_UWB_CIR_FULL_CHUNK]
        lines.extend(("", "UWB_CIR_FULL_CHUNK values", "-" * 72))
        if not chunks:
            lines.append("Absent in this packet.")
        else:
            lines.append(
                f"{len(chunks)} value(s), {sum(len(chunk) for chunk in chunks)} contiguous CIR bytes"
            )
            for index, chunk in enumerate(chunks):
                lines.extend(("", f"Chunk TLV {index} ({len(chunk)} bytes)", hex_dump(chunk)))
        return "\n".join(lines)

    def _raw_text(self, packet: Packet) -> str:
        lines = [
            f"Transport bytes ({packet.transport}, {len(packet.raw_transport)} bytes)",
            "=" * 72,
            hex_dump(packet.raw_transport),
            "",
            "Decoded shared packet bytes",
            "=" * 72,
        ]
        if packet.raw_packet is None:
            lines.append(
                "Unavailable: gateway stream v1 carries selected envelope fields plus payload, "
                "not the original shared-packet header, TTL, or packet CRC."
            )
        else:
            lines.append(hex_dump(packet.raw_packet))
        lines.extend(("", f"TLV payload ({len(packet.payload)} bytes)", "=" * 72, hex_dump(packet.payload)))
        cir = packet.raw_value(TLV_UWB_CIR_SAMPLE)
        lines.extend(("", "CIR bytes", "=" * 72, "absent" if cir is None else hex_dump(cir)))
        return "\n".join(lines)

    def _clear_packets(self) -> None:
        self._clear_tree(self.packet_tree)
        self.packet_by_iid.clear()
        self._wake_row_iids.clear()
        self.cir_reassembler.clear()
        self.cir_key_by_packet_id.clear()
        self.cir_errors_by_packet_id.clear()
        self._clear_tree(self.overview_tree)
        self._clear_tree(self.sample_tree)
        self._clear_tree(self.cir_tree)
        self._set_cir_plot((), None, None)
        self._clear_tree(self.tlv_tree)
        self._set_text(self.diagnostics_text, "")
        self._set_text(self.raw_text, "")
        self.sample_warning_text.set("Select a click report to inspect aligned samples.")
        self.cir_state_text.set("Select a CIR diagnostic fragment to inspect its assembly.")

    def _clear_gateway_memory(self) -> None:
        """Clear external gateway host RAM (dedup, geometry, history, CIR) and reset connected board RAM."""
        tracker = getattr(self, "command_request_tracker", None)
        orchestrator = getattr(self, "command_orchestrator", None)
        survey_owner = getattr(self, "survey_command_owner", None)
        survey_model = getattr(self, "survey_model", None)
        if (
            getattr(orchestrator, "active", False)
            or (tracker is not None and tracker.pending is not None)
            or (survey_owner is not None and survey_owner.pending is not None)
            or (survey_model is not None and survey_model.active)
            or getattr(self, "connection_state", "disconnected")
            in ("connecting", "reconnecting", "disconnecting")
        ):
            self._show_error(
                "Wait for the active command or BLE reconnect to finish before "
                "clearing host memory or rebooting the gateway."
            )
            return
        self._clear_packets()
        self.delivery_dedup.clear()
        getattr(self, "_assignment_replay_receipts", {}).clear()
        if hasattr(self, "assignment_replay_barrier"):
            self.assignment_replay_barrier.reset()
        if hasattr(self, "command_request_tracker"):
            self.command_request_tracker.reset()
        if hasattr(self, "command_orchestrator"):
            self.command_orchestrator.reset()
        if survey_owner is not None:
            survey_owner.reset()
        if survey_model is not None:
            survey_model.clear()
        self._survey_pending_dispatch = None
        self._survey_deferred_dispatch = None
        getattr(self, "_survey_event_buffer", []).clear()
        if hasattr(self, "geometry_model"):
            self.geometry_model.reset()
        if hasattr(self, "click_location_model"):
            self.click_location_model.reset()
        if hasattr(self, "wake_monitor"):
            self.wake_monitor.reset()
        if hasattr(self, "command_timeline_model"):
            self.command_timeline_model.reset()
        if hasattr(self, "topology_model"):
            self.topology_model.reset()
        self._survey_chain_pending = False
        self._survey_phase = "idle"
        self._survey_generation = None
        self._survey_assignment = None
        self._survey_pairs = ()
        self._survey_results = {}
        self._reset_topology_timing()
        if hasattr(self, "click_diagnostics_view"):
            self.click_diagnostics_view.show(self.click_location_model.state, {})
        if hasattr(self, "mesh_diagnostics_view"):
            self.mesh_diagnostics_view.show_timeline(self.command_timeline_model)
            self.mesh_diagnostics_view.show_topology(None, {})
        self._refresh_survey_view()

        # If connected to physical gateway board, submit CMD_REBOOT to clear board RAM
        connected_gateway_id = getattr(self, "gateway_id", None)
        if (
            getattr(self, "connected", False)
            and isinstance(connected_gateway_id, int)
        ):
            try:
                host_id = self._parse_int("Host ID", self.host_id_text.get())
                session_id, seq = self._next_identity()
                cmd = build_reboot_command(
                    host_id=host_id,
                    gateway_id=connected_gateway_id,
                    session_id=session_id,
                    seq=seq,
                )
                target = GatewayCommandDispatch(
                    command_kind=3,
                    command_id=cmd.command_id,
                    session_id=session_id,
                    sequence=seq,
                    frame=cmd.frame,
                    label=cmd.label,
                    timeout_s=5.0,
                    status_text="Rebooting gateway board to clear board RAM...",
                )
                self._submit_gateway_command(GatewayCommandPlan.user_triggered(target))
                self._append_log("info", "Sent reboot command to gateway board; board RAM cleared and reconnecting...")
                self.status_text.set("Cleared host RAM and sent reboot command to gateway board.")
                return
            except Exception as exc:
                self._append_log("error", f"Could not send reboot to gateway board: {exc}")

        self._append_log("info", "Cleared gateway external RAM: dedup state, geometry models, and packet history reset.")
        self.status_text.set("Gateway external RAM and deduplication state cleared.")
    @staticmethod
    def _clear_tree(tree: ttk.Treeview) -> None:
        children = tree.get_children()
        if children:
            tree.delete(*children)

    @staticmethod
    def _set_text(widget: tk.Text, value: str) -> None:
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", value)
        widget.configure(state="disabled")

    def _close(self) -> None:
        self.status_text.set("Stopping BLE transport...")
        self._shutdown_gateway_diagnostics()
        self.transport.shutdown()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    GatewayGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
