# Documentation Index

**Current authoritative documents** (always use the highest version number of each series):

- **Mesh Connected Routing Contract.md** — The binding high-level behavioral contract for routing, channel scheduling, preemption, ACKs, and click vs. transit priority. Do not contradict without explicit permission.
- **UWB+BLE Architecture 0.6.6.md** — Current system architecture and major design decisions.
- **UWB+BLE Protocols and Strategies 0.3.12.2.md** — Protocol formats, strategies, and timing details.

Superseded versions remain available in Git history and are not duplicated in the working tree.

## Other Useful Documents

- `narrative(user story).md` — The project story and research goals (Living Vitality Hub, experience sampling, evolution from mechanical to current UWB mesh system, including recent anchor self-setup work). Primary reference for intent.
- `HARDWARE_BRINGUP_DEBUG.md` — Staged bring-up process and debug techniques.
- `Gateway BLE Streaming.md` — Details of the connected GATT packet service.
- `Mesh Integration Simulator Adversarial Review 2026-07-11.md` — Audit of the simulator and test coverage.
- Various requirements, constraints, and historical notes (see file list).

## Navigation Tips

- For code changes that affect mesh or channel behavior → start with the **Contract**.
- For overall system picture → start with **Architecture 0.6.6**.
- For protocol wire format details → **Protocols 0.3.12.2**.
- For project intent, research goals, and why certain robustness rules exist → start with the **narrative(user story).md** (and the short version in the root README.md).
- Cross-reference with `../CODEMAP.md` and `../AGENTS.md`.

The root `archive/` is reserved for the protected DW1000-era implementation reference.
