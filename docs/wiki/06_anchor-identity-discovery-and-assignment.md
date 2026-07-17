<!-- PAGE_ID: imec2-06-anchor-identity-and-assignment -->

[Start Here](README.md) › [Product Roles](01_product-roles-and-firmware-lines.md) › [One Click, End to End](02_one-click-end-to-end.md) › [UWB Wake and Ranging](03_uwb-wake-ranging-and-power.md) › [Protocol and Data Contracts](04_protocol-packets-and-data-contracts.md) › [Connected Routing](05_connected-routing-and-reliable-delivery.md) › **Anchor Identity and Assignment**

<details>
<summary>📚 Relevant source files</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:31-71](../../AGENTS.md#L31-L71)
- [device_identity.h:11-18](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/include/device_identity.h#L11-L18)
- [device_identity.c:5-29](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/device_identity.c#L5-L29)
- [app_device_identity.c:9-49](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_device_identity.c#L9-L49)
- [discovery_assignment.h:14-41](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/include/discovery_assignment.h#L14-L41)
- [discovery_assignment.c:96-572](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L96-L572)
- [app_discovery_assignment_policy.h:37-195](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_discovery_assignment_policy.h#L37-L195)
- [app_anchor_commands.inc:408-1385](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L408-L1385)
- [app_anchor_gateway_control.inc:137-1052](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L137-L1052)
- [app_anchor_init.inc:122-173](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_init.inc#L122-L173)
- [app_gateway_assignment_publisher.c:47-399](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_assignment_publisher.c#L47-L399)
- [provision_mesh_anchor.py:184-740](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/provision_mesh_anchor.py#L184-L740)
- [Gateway Here-I-Am Stress Coverage.md:3-37](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Here-I-Am%20Stress%20Coverage.md#L3-L37)

</details>

# Anchor Identity, Discovery, and Assignment

> **Related pages:** [Connected Routing](05_connected-routing-and-reliable-delivery.md) · [Anchor Self-Setup](07_anchor-self-setup-survey-and-geometry.md) · [Persistence and Recovery](08_data-custody-persistence-and-recovery.md) · [Verified Deployment](11_verified-deployment-and-qualification.md)

This chapter follows the infrastructure behind the participant story: before an anchor can attach a trustworthy location to a click, the system must know which physical anchor spoke and which logical order it currently owns.

---

<!-- BEGIN:AUTOGEN imec2-06-anchor-identity-and-assignment-stable-hardware-identity -->
## Stable Hardware Identity

Every production anchor runs the same `mesh_anchor` artifact. The common image derives a stable node ID from that board's nRF FICR identity, while the gateway assigns and persists the logical discovery and ranging order; a production anchor's slot is therefore runtime state, not a value compiled into a board-specific image ([AGENTS.md:31-47](../../AGENTS.md#L31-L47)). This distinction matters operationally: replacing a board changes the physical identity, but reordering anchors does not require rebuilding firmware.

At startup, the Zephyr adapter reads both 32-bit FICR device-ID words and hands them to the platform-independent identity mapper ([app_device_identity.c:17-30](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_device_identity.c#L17-L30)). The mapper joins those words into one 64-bit hardware value, XORs it with an anchor-domain constant, and rejects zero or all-ones values before exposing the mapped anchor ID ([device_identity.c:5-29](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/device_identity.c#L5-L29)). The application returns zero until initialization succeeds and panics if lazy initialization cannot establish a valid identity, so an anchor does not continue under an invented fallback ID ([app_device_identity.c:39-49](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_device_identity.c#L39-L49)).

Do not transfer the per-slot mental model from the ML collection line to production. The `ml_anchor_1` through `ml_anchor_8` presets deliberately compile deterministic IDs and discovery slots for data collection, but those images are outside the production contract ([AGENTS.md:104-116](../../AGENTS.md#L104-L116)).

Sources: [AGENTS.md:31-47](../../AGENTS.md#L31-L47), [AGENTS.md:104-116](../../AGENTS.md#L104-L116), [app_device_identity.c:17-49](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_device_identity.c#L17-L49), [device_identity.c:5-29](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/device_identity.c#L5-L29)
<!-- END:AUTOGEN imec2-06-anchor-identity-and-assignment-stable-hardware-identity -->

---

<!-- BEGIN:AUTOGEN imec2-06-anchor-identity-and-assignment-discovery-and-claims -->
## Discovery and Here-I-Am Claims

Anchor setup has two related but separate radio lifecycles. A **Here-I-Am** request asks the gateway to publish a current route advertisement over the production channel-5 flood path, giving anchors a way to install a route candidate before enumeration ([Gateway Here-I-Am Stress Coverage.md:3-8](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Here-I-Am%20Stress%20Coverage.md#L3-L8)). It is an announcement without per-anchor acknowledgements: host `COMMAND_OK` proves gateway acceptance and scheduling, not reception by every anchor ([Gateway Here-I-Am Stress Coverage.md:33-37](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Here-I-Am%20Stress%20Coverage.md#L33-L37)).

The assignment transaction then creates a fresh nonzero epoch, a guarded runtime generation, and a claim command sequence before opening the collection stage ([app_anchor_gateway_control.inc:500-533](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L500-L533)). Each anchor accepts only a well-formed claim whose flood epoch and command sequence are correlated, ignores a retired or stale epoch, and schedules a response from its stable device ID ([app_anchor_commands.inc:1209-1241](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L1209-L1241)).

Claims are spread across the collection window instead of being sent simultaneously. The first response slot is `hash(anchor_id) % slot_count`; the delay also includes 20 ms slot spacing, farthest-first hop staggering, retry backoff, and jitter ([app_anchor_commands.inc:408-470](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L408-L470), [discovery_assignment.c:527-572](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L527-L572)). The gateway accepts a response only while the transaction is active, before its deadline, after a real RF attempt has started, with the current epoch and the correct hash for the packet's source ID; duplicates are counted idempotently and capacity exhaustion is explicit ([app_anchor_gateway_control.inc:560-688](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L560-L688)).

That correlation boundary is what turns “an anchor was heard” into an assignment claim belonging to this run. The [connected-routing chapter](05_connected-routing-and-reliable-delivery.md) explains how the channel-5 flood and return path carry these controls.

Sources: [Gateway Here-I-Am Stress Coverage.md:3-37](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Gateway%20Here-I-Am%20Stress%20Coverage.md#L3-L37), [app_anchor_gateway_control.inc:500-688](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L500-L688), [app_anchor_commands.inc:408-470](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L408-L470), [app_anchor_commands.inc:1209-1241](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L1209-L1241), [discovery_assignment.c:527-572](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L527-L572)
<!-- END:AUTOGEN imec2-06-anchor-identity-and-assignment-discovery-and-claims -->

---

<!-- BEGIN:AUTOGEN imec2-06-anchor-identity-and-assignment-table-publication -->
## Assignment Table Publication

Once claim collection closes, the gateway hashes and deterministically sorts the distinct physical anchor IDs, breaking a hash tie with the full ID. It then assigns contiguous logical slots `0..N-1` in that order ([discovery_assignment.c:96-115](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L96-L115), [discovery_assignment.c:184-206](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L184-L206)). Each wire entry contains the 64-bit anchor ID, its 64-bit hash, and one byte of logical slot; the table also declares the slot capacity and expected node count, and malformed ordering, hashes, duplicates, or counts are rejected during parsing ([discovery_assignment.h:14-24](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/include/discovery_assignment.h#L14-L24), [discovery_assignment.c:281-341](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L281-L341), [discovery_assignment.c:404-491](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L404-L491)).

The table's command sequence is its publication generation within the assignment epoch. Anchors retain the committed epoch, generation, and table fingerprint, accept an exact replay so a lost ACK can be regenerated, and reject conflicting or older publications ([app_discovery_assignment_policy.h:37-81](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_discovery_assignment_policy.h#L37-L81), [app_discovery_assignment_policy.h:133-195](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_discovery_assignment_policy.h#L133-L195)). If a previously unseen valid claim arrives while the gateway is waiting for table ACKs, the gateway abandons that in-flight publication, advances the table sequence, clears the ACK mask, and returns to claim collection so the late anchor cannot be silently omitted ([app_anchor_gateway_control.inc:705-719](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L705-L719)).

Publication success is stricter than successful transmit admission. The gateway records ACKs only for anchors in the current table, with the current epoch and exact table command sequence; a repeated ACK is accepted as a duplicate without increasing completion twice ([app_anchor_gateway_control.inc:643-670](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L643-L670)). It publishes success and terminal telemetry only after the table delivery itself succeeded and the missing-ACK count reached zero ([app_anchor_gateway_control.inc:137-206](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L137-L206)). Missing ACKs drive bounded, jittered table retries and eventually an explicit timeout instead of a false-success result ([app_anchor_gateway_control.inc:997-1052](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L997-L1052)).

The telemetry publisher is statically required to cover 50 anchors and emits mappings, collection completion, table readiness, and terminal counters in order, retaining one in-flight event until the host stream confirms it was sent ([app_gateway_assignment_publisher.c:47-51](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_assignment_publisher.c#L47-L51), [app_gateway_assignment_publisher.c:115-175](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_assignment_publisher.c#L115-L175), [app_gateway_assignment_publisher.c:346-399](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_assignment_publisher.c#L346-L399)).

Sources: [discovery_assignment.c:96-206](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L96-L206), [discovery_assignment.c:281-491](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/src/discovery_assignment.c#L281-L491), [app_discovery_assignment_policy.h:37-195](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_discovery_assignment_policy.h#L37-L195), [app_anchor_gateway_control.inc:137-206](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L137-L206), [app_anchor_gateway_control.inc:643-719](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L643-L719), [app_anchor_gateway_control.inc:997-1052](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_gateway_control.inc#L997-L1052), [app_gateway_assignment_publisher.c:47-51](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_assignment_publisher.c#L47-L51), [app_gateway_assignment_publisher.c:115-175](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_gateway_assignment_publisher.c#L115-L175)
<!-- END:AUTOGEN imec2-06-anchor-identity-and-assignment-table-publication -->

---

<!-- BEGIN:AUTOGEN imec2-06-anchor-identity-and-assignment-persistence-and-provisioning -->
## Persistence and Provisioning Evidence

An anchor does not ACK first and hope persistence works later. When its ID appears in the table, it writes a snapshot containing the epoch, table generation, table fingerprint, local and gateway identities, assigned slot, slot count, and provisioned state; only after that save succeeds does it commit the assignment in RAM and schedule the table ACK ([app_anchor_commands.inc:1286-1335](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L1286-L1335)). If the anchor is absent from the table, it persists an explicit unprovisioned record instead of retaining an obsolete slot, then continues claiming ([app_anchor_commands.inc:1350-1385](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L1350-L1385)).

On reboot, the anchor restores the record only when it is valid and bound to the current local and gateway IDs. Invalid records are cleared, while a missing, disabled, or failed NVS restore leaves the anchor explicitly unprovisioned rather than manufacturing a slot ([app_anchor_init.inc:122-173](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_init.inc#L122-L173)). The [persistence and recovery chapter](08_data-custody-persistence-and-recovery.md) follows this fail-closed pattern across the wider system.

The headless provisioning tool verifies the same lifecycle visible to an operator. It correlates events by command kind, correlation ID, host session, and host sequence; requires the expected unique claims and published mappings; checks that slots are contiguous; requires collection-complete and table-publication stages; and rejects terminal counters unless every expected table ACK succeeded with zero failures or lost events ([provision_mesh_anchor.py:184-220](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/provision_mesh_anchor.py#L184-L220), [provision_mesh_anchor.py:238-346](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/provision_mesh_anchor.py#L238-L346)). Its reachability qualification deliberately runs Here-I-Am first and then a separately correlated assignment, preserving the boundary between a successful local announcement flood and proved per-anchor assignment completion ([provision_mesh_anchor.py:674-740](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/provision_mesh_anchor.py#L674-L740)).

This is the lifecycle proof to look for before moving into [anchor self-setup](07_anchor-self-setup-survey-and-geometry.md): stable hardware identities were claimed, one generation-specific table was published, every listed anchor durably committed its logical slot, all ACKs completed, and the host received correlated terminal evidence.

Sources: [app_anchor_commands.inc:1286-1385](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_commands.inc#L1286-L1385), [app_anchor_init.inc:122-173](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/app/src/app_anchor_init.inc#L122-L173), [provision_mesh_anchor.py:184-346](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/provision_mesh_anchor.py#L184-L346), [provision_mesh_anchor.py:674-740](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/scripts/provision_mesh_anchor.py#L674-L740)
<!-- END:AUTOGEN imec2-06-anchor-identity-and-assignment-persistence-and-provisioning -->

---

**Previous:** [Connected Routing, Priority, and Reliable Delivery](05_connected-routing-and-reliable-delivery.md)

**Next:** [Anchor Self-Setup: Survey and Geometry](07_anchor-self-setup-survey-and-geometry.md)

**Related:** [Data Custody, Persistence, and Recovery](08_data-custody-persistence-and-recovery.md) · [Verified Mesh Deployment and Hardware Qualification](11_verified-deployment-and-qualification.md)
