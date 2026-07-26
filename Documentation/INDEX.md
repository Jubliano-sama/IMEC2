# Documentation Index

`CURRENT.json` is the machine-readable current-document registry. The
repository verification gate rejects navigation drift instead of asking
readers to infer authority from version numbers.

- [Mesh Connected Routing Contract.md](Mesh%20Connected%20Routing%20Contract.md) — Binding behavior for routing, channel scheduling, preemption, ACKs, and click versus transit priority.
- [UWB+BLE Architecture 0.6.6.2.md](<UWB+BLE Architecture 0.6.6.2.md>) — Current system architecture and design decisions.
- [UWB+BLE Protocols and Strategies 0.3.12.4.md](<UWB+BLE Protocols and Strategies 0.3.12.4.md>) — Current wire formats, strategies, and timing details.
- [Mesh Connected Routing Walkthrough.md](Mesh%20Connected%20Routing%20Walkthrough.md) — Current runtime flow replacement for the retired state-machine series.
- [Development and Deployment Guide.md](Development%20and%20Deployment%20Guide.md) — Build, verification, transactional deployment, and RTT procedures.
- [Architecture Reset Plan.md](Architecture%20Reset%20Plan.md) — Accepted staged replacement of orchestration ownership.

The immediate predecessor of each current versioned document remains in the
worktree as its `Previous version` anchor. Older revisions remain available in
Git history instead of being duplicated.

## Other Useful Documents

- `narrative(user story).md` — The project story and research goals (Living Vitality Hub, experience sampling, evolution from mechanical to current UWB mesh system, including recent anchor self-setup work). Primary reference for intent.
- `HARDWARE_BRINGUP_DEBUG.md` — Staged bring-up process and debug techniques.
- `Gateway BLE Streaming.md` — Details of the connected GATT packet service.
- `Mesh Integration Simulator Adversarial Review 2026-07-11.md` — Audit of the simulator and test coverage.
- Various requirements, constraints, and historical notes (see file list).

## Navigation Tips

- For code changes that affect mesh or channel behavior → start with the **Contract**.
- For the overall system picture → start with **Architecture 0.6.6.2**.
- For protocol wire format details → **Protocols 0.3.12.4**.
- For project intent, research goals, and why certain robustness rules exist → start with the **narrative(user story).md** (and the short version in the root README.md).
- Cross-reference with `../CODEMAP.md` and `../AGENTS.md`.

The root `archive/` is reserved for the protected DW1000-era implementation reference.
