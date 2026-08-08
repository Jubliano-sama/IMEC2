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
    ROUTE_REFRESH_DEFAULT_BUDGET_MS,
    GatewayCommandDispatch,
    GatewayCommandPlan,
    GatewayCommandTransition,
)
from .delivery_dedup import (
    GatewayPacketDeduplicator,
    PacketDisposition,
    is_host_delivery_packet,
)
from .diagnostics_integration import GatewayDiagnosticsMixin
from .operation_policy import (
    ASSIGNMENT_DEFAULT_BUDGET_MS,
    ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS,
    DISCOVERY_DEFAULT_BUDGET_MS,
    DISCOVERY_DEFAULT_REPORT_GRACE_MS,
    DISCOVERY_DEFAULT_ROUND_COUNT,
    DISCOVERY_DEFAULT_SLOT_COUNT,
    DISCOVERY_DEFAULT_SLOT_MS,
    DISCOVERY_DEFAULT_START_DELAY_MS,
    PAIR_AUTO_MAX_PARALLEL_PAIRS,
    PAIR_DEFAULT_MAX_RERUNS,
    AssignmentOperationPolicy,
    DiscoveryOperationPolicy,
    OperationPolicyProfile,
    PairOperationPolicy,
)
from .protocol import (
    CMD_ASSIGN_DISCOVERY_SLOTS,
    CMD_FORCE_REDISCOVERY,
    COMMAND_NAMES,
    DEFAULT_HOST_ID,
    DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
    GATEWAY_COMMAND_BUDGET_MAX_MS,
    GATEWAY_COMMAND_BUDGET_MIN_MS,
    GATEWAY_STREAM_FLAG_TRUNCATED,
    MSG_CLICK_REPORT,
    MSG_COMMAND_RESULT,
    MSG_GATEWAY_COMMAND_EVENT,
    Packet,
    STREAM_CLASS_NAMES,
    TLV_ANCHOR_DIAG_BYTES,
    TLV_ANCHOR_ID,
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
    build_anchor_discovery_command,
    build_assign_discovery_slots_command,
    build_gateway_host_receipt,
    build_here_i_am_command,
    click_samples,
    decode_cir_sample,
    format_device_id,
    hex_dump,
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
DEFAULT_COMMAND_BUDGET_TEXT = ""
DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT = ""


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
        self.root.title("IMEC2 Gateway BLE Console")
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
        self.scanning = False
        self.gateway_id: int | None = None
        self._initialize_gateway_diagnostics()

        self.connection_text = tk.StringVar(value="Disconnected")
        self.device_text = tk.StringVar()
        self.status_text = tk.StringVar(value="Ready")
        self.error_text = tk.StringVar()
        self.host_id_text = tk.StringVar(value=f"0x{DEFAULT_HOST_ID:016x}")
        self.command_budget_text = tk.StringVar(value=DEFAULT_COMMAND_BUDGET_TEXT)
        self.assignment_expected_anchors_text = tk.StringVar(
            value=DEFAULT_ASSIGNMENT_EXPECTED_ANCHORS_TEXT
        )
        self.assignment_budget_text = tk.StringVar(
            value=str(ASSIGNMENT_DEFAULT_BUDGET_MS)
        )
        self.assignment_response_spread_text = tk.StringVar(
            value=str(ASSIGNMENT_DEFAULT_RESPONSE_SPREAD_MS)
        )
        self.gateway_id_text = tk.StringVar(value="Unavailable")
        self.gateway_id_source = tk.StringVar(value="Connect to read the gateway firmware DEVICE_ID.")
        survey_id_seed = (time.time_ns() // 1_000_000) & 0xFFFFFFFF or 1
        self._survey_id_counter = survey_id_seed
        self._used_survey_ids: set[int] = set()
        self.survey_id_text = tk.StringVar(value=str(survey_id_seed))
        self.survey_id_auto = tk.BooleanVar(value=True)
        self.duration_text = tk.StringVar(
            value=str(DISCOVERY_DEFAULT_REPORT_GRACE_MS)
        )
        self.discovery_start_delay_text = tk.StringVar(
            value=str(DISCOVERY_DEFAULT_START_DELAY_MS)
        )
        self.discovery_slot_ms_text = tk.StringVar(
            value=str(DISCOVERY_DEFAULT_SLOT_MS)
        )
        self.discovery_slots_text = tk.StringVar(
            value=str(DISCOVERY_DEFAULT_SLOT_COUNT)
        )
        self.discovery_round_count_text = tk.StringVar(
            value=str(DISCOVERY_DEFAULT_ROUND_COUNT)
        )
        self.discovery_budget_text = tk.StringVar(
            value=str(DISCOVERY_DEFAULT_BUDGET_MS)
        )
        self.pair_max_reruns_text = tk.StringVar(
            value=str(PAIR_DEFAULT_MAX_RERUNS)
        )
        self.pair_max_parallel_text = tk.StringVar(value="auto (25)")
        self.sample_count_text = tk.StringVar(
            value=str(SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT)
        )
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

        identity = ttk.LabelFrame(parent, text="Host Identity", padding=10)
        identity.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        identity.grid_columnconfigure(1, weight=1)
        ttk.Label(identity, text="Host ID").grid(row=0, column=0, sticky="w", padx=(0, 8))
        host_entry = ttk.Entry(identity, textvariable=self.host_id_text)
        host_entry.grid(row=0, column=1, sticky="ew")
        Tooltip(host_entry, "Source ID placed in host command envelopes. Accepts decimal or 0x-prefixed hexadecimal.")
        ttk.Label(identity, text="Command limit (ms)").grid(
            row=1, column=0, sticky="w", padx=(0, 8), pady=(6, 0))
        command_budget_entry = ttk.Entry(
            identity, textvariable=self.command_budget_text)
        command_budget_entry.grid(row=1, column=1, sticky="ew", pady=(6, 0))
        Tooltip(
            command_budget_entry,
            "Optional total firmware deadline. Leave blank for the robust command-specific default; a short limit can intentionally end before all retries finish.",
        )

        discovery = ttk.LabelFrame(parent, text="Anchor-Pair Survey", padding=10)
        discovery.grid(row=1, column=0, sticky="ew", pady=(0, 10))
        discovery.grid_columnconfigure(1, weight=1)
        self._labeled_spin(discovery, 0, "Survey ID", self.survey_id_text, 1, 0xFFFFFFFF)
        auto_survey_id = ttk.Checkbutton(
            discovery,
            text="Generate a fresh ID for every survey",
            variable=self.survey_id_auto,
        )
        auto_survey_id.grid(row=1, column=0, columnspan=2, sticky="w", pady=(3, 3))
        Tooltip(
            auto_survey_id,
            "Enabled by default so delayed packets from an older run cannot match a new one. Clear it to send the exact Survey ID above.",
        )
        self._labeled_spin(
            discovery, 2, "Start delay (ms)",
            self.discovery_start_delay_text, 6000, 60000
        )
        self._labeled_spin(
            discovery, 3, "Discovery slot (ms)",
            self.discovery_slot_ms_text, 30, 1000
        )
        self._labeled_spin(
            discovery, 4, "Discovery slots", self.discovery_slots_text, 1, 50
        )
        self._labeled_spin(
            discovery, 5, "Discovery rounds",
            self.discovery_round_count_text, 1, 4
        )
        self._labeled_spin(
            discovery, 6, "Report grace (ms)", self.duration_text, 1, 60000
        )
        self._labeled_spin(
            discovery, 7, "Survey budget (ms)",
            self.discovery_budget_text, 1000, GATEWAY_COMMAND_BUDGET_MAX_MS
        )
        self._labeled_spin(
            discovery, 8, "Pair reruns", self.pair_max_reruns_text, 0, 2
        )
        ttk.Label(discovery, text="Concurrent pairs").grid(
            row=9, column=0, sticky="w", padx=(0, 8), pady=(4, 0)
        )
        parallel_entry = ttk.Entry(
            discovery, textvariable=self.pair_max_parallel_text
        )
        parallel_entry.grid(row=9, column=1, sticky="ew", pady=(4, 0))
        Tooltip(
            parallel_entry,
            "Use 'auto' to expose all 25 safe lanes; the neighborhood conflict "
            "classifier still serializes pairs that can interfere.",
        )
        self._labeled_spin(
            discovery, 10, "Pair samples", self.sample_count_text, 5, 5
        )
        ttk.Label(
            discovery,
            text="Each pair collects exactly 5 samples in its shared survey round.",
            style="Muted.TLabel",
            wraplength=295,
            justify="left",
        ).grid(row=11, column=0, columnspan=2, sticky="w", pady=(5, 8))
        self.discovery_button = ttk.Button(
            discovery,
            text="Start anchor-pair survey",
            style="Primary.TButton",
            command=self._send_discovery,
        )
        self.discovery_button.grid(row=12, column=0, columnspan=2, sticky="ew")
        Tooltip(
            self.discovery_button,
            "Send gateway-local CMD_SURVEY_REACHABILITY (0x0100). Firmware starts survey discovery and reports COMMAND_RESULT.",
        )

        refresh = ttk.LabelFrame(parent, text="Gateway-Local Commands", padding=10)
        refresh.grid(row=2, column=0, sticky="ew", pady=(0, 10))
        refresh.grid_columnconfigure(0, weight=1)
        refresh.grid_columnconfigure(1, weight=1)
        ttk.Label(refresh, text="Connected gateway DEVICE_ID").grid(row=0, column=0, sticky="w")
        gateway_identity = ttk.Label(refresh, textvariable=self.gateway_id_text)
        gateway_identity.grid(row=1, column=0, sticky="w", pady=(3, 4))
        Tooltip(gateway_identity, "Read directly from the connected gateway identity characteristic.")
        ttk.Label(
            refresh,
            textvariable=self.gateway_id_source,
            style="Muted.TLabel",
            wraplength=295,
            justify="left",
        ).grid(row=2, column=0, sticky="w", pady=(0, 8))
        self._labeled_spin(
            refresh,
            3,
            "Expected anchors (blank = full 8-hop scan)",
            self.assignment_expected_anchors_text,
            1,
            50,
        )
        self._labeled_spin(
            refresh,
            4,
            "Assignment budget (ms)",
            self.assignment_budget_text,
            1000,
            GATEWAY_COMMAND_BUDGET_MAX_MS,
        )
        self._labeled_spin(
            refresh,
            5,
            "Response spread (ms)",
            self.assignment_response_spread_text,
            20,
            10000,
        )
        self.refresh_button = ttk.Button(
            refresh,
            text="Refresh mesh routes (Here I Am)",
            style="Primary.TButton",
            command=self._send_here_i_am,
        )
        self.refresh_button.grid(row=6, column=0, columnspan=2, sticky="ew")
        Tooltip(
            self.refresh_button,
            "Send local CMD_FORCE_REDISCOVERY (0x000c). The gateway responds and schedules a priority GATEWAY_ROUTE_ADV flood.",
        )
        self.assignment_button = ttk.Button(
            refresh,
            text="Enumerate anchors and assign slots",
            style="Primary.TButton",
            command=self._send_assign_discovery_slots,
        )
        self.assignment_button.grid(
            row=7, column=0, columnspan=2, sticky="ew", pady=(6, 0)
        )
        Tooltip(
            self.assignment_button,
            "Send gateway-local CMD_ASSIGN_DISCOVERY_SLOTS (0x0104). Firmware collects anchor claims, floods the assignment table, and returns the assigned-anchor count.",
        )
        self.command_availability_text = tk.StringVar(value="Connect gateway to run a command.")
        ttk.Label(refresh, textvariable=self.command_availability_text, style="Muted.TLabel", wraplength=295, justify="left").grid(row=8, column=0, columnspan=2, sticky="w", pady=(6, 0))

        contract = ttk.LabelFrame(parent, text="Command Surface", padding=10)
        contract.grid(row=3, column=0, sticky="ew")
        ttk.Label(
            contract,
            text="Only the three host workflows proven by the current firmware are exposed. Arbitrary TLVs are intentionally not sent.",
            style="Muted.TLabel",
            wraplength=295,
            justify="left",
        ).grid(row=0, column=0, sticky="w")

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

    def _command_budget_ms(self) -> int | None:
        raw = self.command_budget_text.get().strip()
        if not raw:
            return None
        budget_ms = self._parse_int("Command limit", raw)
        if not (
            GATEWAY_COMMAND_BUDGET_MIN_MS
            <= budget_ms
            <= GATEWAY_COMMAND_BUDGET_MAX_MS
        ):
            raise ValueError(
                f"Command limit must be in {GATEWAY_COMMAND_BUDGET_MIN_MS}.."
                f"{GATEWAY_COMMAND_BUDGET_MAX_MS} ms, or blank"
            )
        return budget_ms

    def _operation_policy_profile(self) -> OperationPolicyProfile:
        expected_raw = self.assignment_expected_anchors_text.get().strip()
        expected_anchor_count = (
            self._parse_int("Expected anchors", expected_raw)
            if expected_raw else 0
        )
        parallel_raw = self.pair_max_parallel_text.get().strip().lower()
        if parallel_raw in {"auto", "auto25", "auto (25)"}:
            max_parallel_pairs = PAIR_AUTO_MAX_PARALLEL_PAIRS
        else:
            max_parallel_pairs = self._parse_int(
                "Concurrent pairs", parallel_raw
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
            ),
            discovery=DiscoveryOperationPolicy(
                start_delay_ms=self._parse_int(
                    "Discovery start delay", self.discovery_start_delay_text.get()
                ),
                slot_ms=self._parse_int(
                    "Discovery slot duration", self.discovery_slot_ms_text.get()
                ),
                slot_count=self._parse_int(
                    "Discovery slots", self.discovery_slots_text.get()
                ),
                round_count=self._parse_int(
                    "Discovery rounds", self.discovery_round_count_text.get()
                ),
                report_grace_ms=self._parse_int(
                    "Report grace", self.duration_text.get()
                ),
                operation_budget_ms=self._parse_int(
                    "Survey budget", self.discovery_budget_text.get()
                ),
            ),
            pair=PairOperationPolicy(
                max_reruns=self._parse_int(
                    "Pair reruns", self.pair_max_reruns_text.get()
                ),
                max_parallel_pairs=max_parallel_pairs,
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
        return effective_budget_ms / 1000.0 + 2.0

    def _survey_id_for_send(self) -> int:
        if not self.survey_id_auto.get():
            survey_id = self._parse_int("Survey ID", self.survey_id_text.get())
            if not 1 <= survey_id <= 0xFFFFFFFF:
                raise ValueError("Survey ID must be in 1..4294967295")
            self._used_survey_ids.add(survey_id)
            return survey_id

        candidate = self._survey_id_counter
        for _ in range(len(self._used_survey_ids) + 1):
            candidate = (candidate + 1) & 0xFFFFFFFF
            if candidate == 0:
                candidate = 1
            if candidate not in self._used_survey_ids:
                self._survey_id_counter = candidate
                self._used_survey_ids.add(candidate)
                self.survey_id_text.set(str(candidate))
                return candidate
        raise ValueError("No unused Survey ID is available")

    def _require_gateway_identity(self) -> int:
        if not self.connected:
            raise ValueError("Connect to a gateway before sending commands")
        if self.gateway_id is None:
            raise ValueError("Connected gateway identity is unavailable; reconnect before sending commands")
        return self.gateway_id

    def _submit_gateway_command(self, plan: GatewayCommandPlan) -> bool:
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
        self.status_text.set(dispatch.status_text)
        self.transport.send_frame(dispatch.frame, dispatch.label)

    def _apply_gateway_command_transition(
        self, transition: GatewayCommandTransition
    ) -> None:
        if not transition.matched:
            return
        if transition.dispatch is not None:
            self._dispatch_gateway_command(transition.dispatch)
        elif transition.completed and transition.phase == "preflight":
            self.status_text.set(
                "Here I Am preflight failed; requested command was not sent"
            )
        self._update_command_state()

    def _here_i_am_dispatch(
        self,
        *,
        host_id: int,
        gateway_id: int,
        command_budget_ms: int | None,
        operation_policy: OperationPolicyProfile,
        status_text: str,
    ) -> GatewayCommandDispatch:
        session_id, seq = self._next_identity()
        command = build_here_i_am_command(
            host_id=host_id,
            gateway_id=gateway_id,
            session_id=session_id,
            seq=seq,
            command_budget_ms=command_budget_ms,
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
                command_budget_ms, ROUTE_REFRESH_DEFAULT_BUDGET_MS
            ),
            status_text=status_text,
        )

    def _send_discovery(self) -> None:
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            session_id, seq = self._next_identity()
            operation_policy = self._operation_policy_profile()
            discovery_policy = operation_policy.discovery
            command_budget_ms = discovery_policy.operation_budget_ms
            survey_id = self._survey_id_for_send()
            command = build_anchor_discovery_command(
                host_id=host_id,
                gateway_id=gateway_id,
                session_id=session_id,
                seq=seq,
                survey_id=survey_id,
                duration_ms=discovery_policy.report_grace_ms,
                discovery_slot_count=discovery_policy.slot_count,
                sample_count=self._parse_int("Pair samples", self.sample_count_text.get()),
                command_budget_ms=command_budget_ms,
                operation_policy=operation_policy,
            )
            target = GatewayCommandDispatch(
                command_kind=2,
                command_id=command.command_id,
                session_id=session_id,
                sequence=seq,
                frame=command.frame,
                label=command.label,
                timeout_s=self._command_timeout_s(command_budget_ms),
                status_text="Writing anchor-pair survey command over BLE...",
                on_dispatch=lambda: self._prepare_anchor_geometry_survey(
                    survey_id, session_id, seq
                ),
            )
            preflight = self._here_i_am_dispatch(
                host_id=host_id,
                gateway_id=gateway_id,
                command_budget_ms=None,
                operation_policy=operation_policy,
                status_text="Refreshing mesh routes before anchor-pair survey...",
            )
            plan = GatewayCommandPlan.user_triggered(
                target, preflight=preflight
            )
        except ValueError as exc:
            self._show_error(str(exc))
            return
        self._submit_gateway_command(plan)

    def _send_here_i_am(self) -> None:
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            command_budget_ms = self._command_budget_ms()
            operation_policy = self._operation_policy_profile()
            target = self._here_i_am_dispatch(
                host_id=host_id,
                gateway_id=gateway_id,
                command_budget_ms=command_budget_ms,
                operation_policy=operation_policy,
                status_text="Writing Here I Am route-refresh request over BLE...",
            )
            plan = GatewayCommandPlan.user_triggered(target)
        except ValueError as exc:
            self._show_error(str(exc))
            return
        self._submit_gateway_command(plan)

    def _send_assign_discovery_slots(self) -> None:
        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
            gateway_id = self._require_gateway_identity()
            session_id, seq = self._next_identity()
            operation_policy = self._operation_policy_profile()
            assignment_policy = operation_policy.assignment
            command_budget_ms = assignment_policy.operation_budget_ms
            expected_anchor_count = assignment_policy.expected_anchor_count or None
            command = build_assign_discovery_slots_command(
                host_id=host_id,
                gateway_id=gateway_id,
                session_id=session_id,
                seq=seq,
                command_budget_ms=command_budget_ms,
                expected_anchor_count=expected_anchor_count,
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
                command_budget_ms=None,
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
            return
        self._submit_gateway_command(plan)

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

    def _set_connection_state(self, state: str) -> None:
        self.connected = state == "connected"
        if (
            not self.connected
            and state != "connecting"
            and hasattr(self, "command_orchestrator")
        ):
            self._apply_gateway_command_transition(
                self.command_orchestrator.disconnect()
            )
        if not self.connected and state != "connecting":
            self._clear_gateway_identity("Connect to read the gateway firmware DEVICE_ID.")
        elif state == "connecting":
            self._clear_gateway_identity("Reading the gateway firmware DEVICE_ID...")
        names = {
            "connecting": "Connecting...",
            "connected": "Connected",
            "disconnecting": "Disconnecting...",
            "disconnected": "Disconnected",
        }
        self.connection_text.set(names.get(state, state.title()))
        self.connection_label.configure(
            style="Connected.Status.TLabel" if self.connected else "Status.TLabel"
        )
        busy = state in ("connecting", "disconnecting")
        self.connect_button.configure(state="disabled" if self.connected or busy else "normal")
        self.disconnect_button.configure(state="normal" if self.connected else "disabled")
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
        if not self.connected:
            reason = "Connect gateway to run a command."
        elif self.gateway_id is None:
            reason = "Waiting for a valid gateway identity."
        elif tracker is not None and tracker.pending is not None:
            reason = "Command already running; waiting for its terminal result."
        else:
            reason = "Ready for a gateway command."
        command_state = "normal" if reason.startswith("Ready") else "disabled"
        self.discovery_button.configure(state=command_state)
        self.refresh_button.configure(state=command_state)
        self.assignment_button.configure(state=command_state)
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
            and packet.msg_type != MSG_GATEWAY_COMMAND_EVENT
            and is_host_delivery_packet(packet)
        )

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
    ) -> None:
        """Receipt a cached stream record while leaving custody upstream on error."""
        if delivery.disposition not in (
            PacketDisposition.NEW,
            PacketDisposition.DUPLICATE,
        ):
            return
        if not delivery.cached or not self._is_receiptable_gateway_packet(packet):
            return

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
            return
        if gateway_id == 0:
            # Before the GATT identity event arrives, the stream destination is
            # the only safe gateway scope available to the GUI.
            self._log_gateway_receipt(
                "error",
                "Not sending host receipt: gateway identity is unavailable "
                "and the stream record has no destination",
            )
            return

        try:
            host_id = self._parse_int("Host ID", self.host_id_text.get())
        except (AttributeError, ValueError) as exc:
            self._log_gateway_receipt("error", f"Not sending host receipt: {exc}")
            return

        try:
            receipt = build_gateway_host_receipt(
                packet,
                host_id=host_id,
                gateway_id=gateway_id,
            )
            self.transport.send_frame(receipt.frame, "gateway host receipt")
        except Exception as exc:
            # A failed write must not alter the cache or semantic models; the
            # gateway will retain source custody and replay the stream record.
            self._log_gateway_receipt(
                "error",
                f"Gateway host receipt was not sent; upstream custody remains: "
                f"{type(exc).__name__}: {exc}",
            )

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
            self._maybe_send_gateway_host_receipt(
                packet, delivery, gateway_scope=receipt_gateway_scope
            )
            identity = delivery.identity
            assert identity is not None
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
            assert identity is not None
            self._append_log(
                "error",
                f"Conflicting {packet_label} reused packet identity "
                f"src={format_device_id(identity.src_id)} "
                f"session={identity.session_id} seq={identity.seq}; "
                "showing the forensic record without applying a second "
                "semantic mutation",
            )
        canonical_delivery = delivery.disposition is PacketDisposition.NEW
        if canonical_delivery:
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
        if canonical_delivery and delivery.cached:
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
                self._maybe_send_gateway_host_receipt(
                    packet, delivery, gateway_scope=receipt_gateway_scope
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
        self.transport.shutdown()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    GatewayGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
