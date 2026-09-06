# IMEC2 documentation

Updated against the working implementation on 2026-09-06. Explicit user requirements take precedence over code; code in the active checkout establishes what is implemented. A dated report proves only the checkout, build and experiment it records.

| Read when | Source | Status |
|---|---|---|
| Starting work or using the bench | [AGENTS.md](../AGENTS.md) | Working instructions and explicit survey ownership requirement |
| Changing radio ownership, routing, wake timing, enumeration or survey | [Mesh Connected Routing Contract](<Mesh Connected Routing Contract.md>) | Current behavior and explicit requirements |
| Changing report custody, batching or ACKs | [Channel 5 Delivery Protocol](<Channel 5 Delivery Protocol.md>) | Current delivery implementation and remaining limits |
| Finding firmware/host responsibilities | [UWB+BLE Architecture](<UWB+BLE Architecture.md>) | Current implementation map |
| Building firmware | [Firmware README](../firmware/README.md) | Build and hardware verification workflow |
| Running or changing the host | [Gateway GUI README](../tools/gateway_gui/README.md) | Current GUI workflows |
| Understanding the product | [Narrative](<narrative(user story).md>) and the requirements tables below | Product objectives, not implementation or qualification claims |
| Considering identification/battery actions | [Anchor identification and battery commands](<Anchor identification and battery commands.md>) | Implemented; current-session enumeration required; direct/software-forced-hop action evidence in the dated review |
| Reviewing the September scan/SPI and survey bench work | [Scan, power and survey qualification](Reviews/scan-power-survey-2026-09-06.md) | Dated measurements, regression evidence and qualification limits |
| Considering a state-machine rewrite | [State-machine design](IMEC2_Firmware_State_Machine_Design.md) | Historical proposal; not the runtime architecture |
| Investigating a past experiment | `Reviews/`, [protocol timelines](protocol_timelines/README.md), `../docs/superpowers/`, `../logs/` | Historical evidence and plans; revalidate before reuse |

The requirements tables are [customer needs](<Customer Needs.md>), [user requirements](<user requirements.md>), [stakeholder requirements](<Stakeholder Requirements.md>), [functional requirements](<Functional Requirements.md>), [performance requirements](<Performance Requirements.md>), [constraints](Constraints.md) and [physical domain](<Physical Domain.md>). Their original IDs and traceability are retained. Their Obsidian links to dated internship notes and pasted images refer to an external notebook, not missing firmware dependencies.

`AGENT_KNOWN_ISSUES.md` and its curated summary are historical bug evidence, never protocol requirements. `COMPILED_DOCUMENTATION.md` now links to these source documents instead of retaining its stale generated snapshot. Version-suffixed architecture documents mentioned in old logs have been replaced by the unversioned sources above.

Current production candidates are `mesh_clicker`, `mesh_anchor` and `mesh_gateway`. Production report delivery uses Channel 5. Legacy Channel-9 modules and tests remain in the tree, but their presence does not establish a second production data lane. Survey uses START/PLAN/CANCEL and the compact response lane; old PREPARE/START pair-control descriptions do not describe it.
