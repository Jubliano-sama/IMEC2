"""Audit saved Claude results, preserving failures and separating group/global error."""

import argparse
import csv
import io
import json
from pathlib import Path
import statistics
import tarfile

from . import recovered_nlos_probe as probe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    summaries = []
    failures = []
    with tarfile.open(args.archive) as archive:
        for filename in ("drawing_hetero.csv", "drawing_scan.csv", "family.csv", "family2.csv"):
            entry = archive.extractfile(f"prototype/{filename}")
            assert entry is not None
            groups = {}
            for row in csv.DictReader(io.TextIOWrapper(entry)):
                if row["method"] not in ("gui_intervals", "gui_vis_nbaware", "p_af_c16_d1"):
                    continue
                key = (row["method"], row.get("meta_cap", "0"))
                metrics = groups.setdefault(key, [])
                if row["status"] != "ok":
                    metrics.append({"status": "error"})
                    continue
                truth, positions = json.loads(row["truth"]), json.loads(row["positions"])
                group = [a for a, (_, y) in truth.items() if y < 6]
                aligned = probe.align_offsets(truth, positions, group)
                loo = [probe.align_offsets(truth, positions, [b for b in group if a != b])[a] for a in group]
                full = probe.align_offsets(truth, positions)
                failed = max(loo) > 1
                metrics.append({
                    "status": "ok", "group_over_1m": failed,
                    "group_rms_m": statistics.mean(aligned[a] ** 2 for a in group) ** 0.5,
                    "full_rms_m": statistics.mean(v*v for v in full.values()) ** 0.5,
                    "target_flip": int(row["orient_flip"]),
                })
                if failed and row["method"] == "p_af_c16_d1":
                    failures.append({"file": filename, "scene": row["scene"],
                                     "group_loo_max_m": max(loo), "meta": {k: v for k, v in row.items() if k.startswith("meta_")}})
            for (method, cap), metrics in sorted(groups.items()):
                ok = [r for r in metrics if r["status"] == "ok"]
                summaries.append({
                    "file": filename, "method": method, "cap": cap, "cases": len(metrics),
                    "errors": len(metrics) - len(ok), "group_over_1m": sum(r["group_over_1m"] for r in ok),
                    "target_flip_cases": sum(r["target_flip"] >= 0 for r in ok),
                    "target_flips": sum(r["target_flip"] == 1 for r in ok)
                    if any(r["target_flip"] >= 0 for r in ok) else None,
                    "median_group_rms_m": statistics.median(r["group_rms_m"] for r in ok) if ok else None,
                    "median_full_rms_m": statistics.median(r["full_rms_m"] for r in ok) if ok else None,
                })
    args.out.write_text(json.dumps({"summaries": summaries, "candidate_failures": failures}, indent=2) + "\n")
    print(json.dumps(summaries, indent=2))


if __name__ == "__main__":
    main()
