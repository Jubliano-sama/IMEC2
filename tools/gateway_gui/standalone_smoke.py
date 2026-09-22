"""Exercise the bundled GUI, numerical libraries, image support and BLE backend."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
import sys
import traceback


def run(report_path: Path, *, scan: bool = False) -> int:
    report: dict[str, object] = {"frozen": bool(getattr(sys, "frozen", False)), "ok": False}
    app = None
    try:
        import tkinter as tk
        from PIL import Image, ImageTk
        from bleak import BleakScanner
        from bleak.backends.winrt.client import BleakClientWinRT
        from tools.gateway_gui.app import GatewayGui
        from tools.gateway_gui.anchor_geometry import AnchorPairDistance
        from tools.gateway_gui.diagnostic_models import solve_geometry
        from tools.gateway_gui.survey_view import SOLVER_CHOICES

        report["ble_backend"] = BleakClientWinRT.__name__
        root = tk.Tk()
        root.withdraw()
        callback_errors = []
        root.report_callback_exception = lambda *error: callback_errors.append(str(error))
        app = GatewayGui(root)
        photo = ImageTk.PhotoImage(Image.new("RGB", (16, 16), "white"), master=root)
        assert photo.width() == 16
        root.after(300, root.quit)
        root.mainloop()
        if callback_errors:
            raise RuntimeError("; ".join(callback_errors))
        report["gui"] = "startup and event loop passed"
        if scan:
            # Use the application's BLE thread: Tk owns a Windows STA thread.
            devices = asyncio.run_coroutine_threadsafe(
                BleakScanner.discover(timeout=5.0), app.transport._loop
            ).result(timeout=15.0)
            report["ble_scan_device_count"] = len(devices)
        app._close()
        app = None

        pairs = (AnchorPairDistance("A", "B", 3), AnchorPairDistance("B", "C", 4))
        results = {}
        for solver in SOLVER_CHOICES:
            layout = solve_geometry(pairs, solver=solver)
            assert set(layout.positions_m) == {"A", "B", "C"}
            assert any("underconstrained" in warning for warning in layout.warnings)
            results[solver] = {"rmse_m": layout.rmse_m, "warnings": layout.warnings}
        report["solvers"] = results
        report["ok"] = True
    except Exception:
        report["error"] = traceback.format_exc()
    finally:
        if app is not None:
            app._close()
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["ok"] else 1
