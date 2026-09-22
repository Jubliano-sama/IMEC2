"""Entry point for the standalone Windows application."""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.gateway_gui.app import main


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="IMEC2 Gateway BLE Console")
    parser.add_argument("--smoke-test", type=Path, metavar="REPORT_JSON")
    parser.add_argument("--scan", action="store_true", help="Include BLE scanning in the smoke test")
    args = parser.parse_args()
    if args.smoke_test:
        from tools.gateway_gui.standalone_smoke import run

        raise SystemExit(run(args.smoke_test, scan=args.scan))
    main()
