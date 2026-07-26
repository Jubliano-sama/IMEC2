# IMEC2 — UWB Clicker / Anchor / Gateway Firmware

## Project Narrative & Goals

This project is a **research tool for experience sampling** in real office environments.

The Living Vitality Hub is a test facility instrumented with dense environmental sensors (CO₂, PM₂.₅, light, occupancy, temperature, etc.). The goal is to understand how the physical environment affects human behavior and cognitive performance by collecting **subjective, in-the-moment feedback** from people and correlating it with the objective sensor data.

People use a portable "clicker" to report things like loss of concentration, thermal discomfort, or social interactions. The system must deliver:
- Precise timestamps
- Automatic, passive spatial location (via UWB)
- High reliability over multi-month deployments

Previous approaches (mechanical clickers or early connected prototypes) suffered from missing timestamps, user error in localization, poor battery life, and lack of scalability.

**Current scope** includes a full UWB mesh architecture (clickers, anchors, gateway) plus a recently completed **anchor self-setup protocol**. The self-setup algorithm automatically measures anchor-to-anchor distances and solves the 3D geometry of the network given only those distances plus an approximate minimum radio radius.

**Core non-negotiable**: The system must be *robust*. It must never stall silently, drop data, or return false results (no bad fallbacks). Everything is designed for long-term, trustworthy operation in real deployments.

The technical complexity (UWB wake/discovery/ranging, channel 5 vs 9 separation, click preemption, reliable mesh routing, low-duty anchors, etc.) all serves the goal of collecting clean, contextual behavioral data without burdening users.

---

This is a Zephyr/west workspace for a battery-powered UWB (DWM3000) + nRF52833 system consisting of:

- **Clickers**: handheld/button devices that wake on press, perform multi-anchor UWB ranging, and report results over a connected mesh.
- **Anchors**: fixed nodes that participate in ranging, relay mesh traffic, and prioritize local click reports.
- **Gateway**: mesh root that delivers data and accepts commands over a connected Bluetooth GATT link to a PC.

The design is **UWB-first**. Channel 5 handles wake, discovery, and click/ranging preemption. Channel 9 carries the reliable connected-routing mesh for reports, transit traffic, and acknowledgements. BLE is used only for clicker courtesy hints (non-connected) and the gateway-to-PC edge (connected GATT).

## Quick Links (Start Here)

- **[AGENTS.md](AGENTS.md)** — Concise repository safety, ownership, and verification rules. **Read this first for any code change**, then run its indexed issue preflight for the planned paths and operations.
- **[CODEMAP.md](CODEMAP.md)** — Detailed navigation guide for the codebase (this is the map you're looking for).
- **[firmware/README.md](firmware/README.md)** — Long-form technical description of the firmware implementation, hardware assumptions, and bring-up checklist.
- **[Documentation/Mesh Connected Routing Contract.md](<Documentation/Mesh Connected Routing Contract.md>)** — The high-level behavioral contract. Changes that contradict it require explicit permission.
- **[Documentation/UWB+BLE Architecture 0.6.6.2.md](<Documentation/UWB+BLE Architecture 0.6.6.2.md>)** — Current system architecture.
- **[Documentation/UWB+BLE Protocols and Strategies 0.3.12.4.md](<Documentation/UWB+BLE Protocols and Strategies 0.3.12.4.md>)** — Current protocol details.
- **[Documentation/Development and Deployment Guide.md](<Documentation/Development and Deployment Guide.md>)** — Executable build, verification, flashing, and RTT workflow.
- **[Documentation/Architecture Reset Plan.md](<Documentation/Architecture Reset Plan.md>)** — Accepted staged orchestration replacement and simplification decision.

## Important Notes on Current State

- **Production line**: Use the `mesh_clicker`, `mesh_anchor`, and `mesh_gateway` build presets. These are the connected-routing successor; every production anchor uses the same `mesh_anchor` artifact.
- Legacy role builds (`FIRMWARE_ROLE=clicker|anchor|gateway`) and various high-debug/ML/transmitter presets exist for regression and data collection only.
- All native protocol and mesh logic is in `firmware/src/` and is unit-testable without Zephyr.
- The Zephyr application lives in `firmware/app/`.
- Historical DW1000 reference material is isolated under `archive/`; superseded documentation remains available in Git history instead of being duplicated in the working tree.

## Getting Started

See the detailed build and test instructions in `AGENTS.md` (preferred) or `firmware/README.md`.

Typical first steps for an agent or contributor:

1. Read `AGENTS.md`.
2. Consult `CODEMAP.md` for where specific behavior lives.
3. Run the fresh repository gate: `python3 firmware/scripts/verify_changes.py`.
4. For Zephyr-facing work, add `--exact-roles` and review the Mesh Contract.

## License & Status

This is internal/project firmware. See individual files for any vendor licenses (DWM3000 decadriver, nRF SDK components, etc.).

Hardware bring-up and full validation are ongoing (see the checklist in `firmware/README.md`).

---

For navigation help, open **[CODEMAP.md](CODEMAP.md)**. For rules and exact commands, open **[AGENTS.md](AGENTS.md)**.
