<!-- PAGE_ID: imec2-12-testing-and-simulation -->

[Wiki home](README.md) / [From the user story to the system](README.md#follow-the-story) / Testing, simulation, and release evidence

<details>
<summary>📚 Relevant source files</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:23-28](../../AGENTS.md#L23-L28)
- [AGENTS.md:62-71](../../AGENTS.md#L62-L71)
- [AGENTS.md:124-150](../../AGENTS.md#L124-L150)
- [AGENTS.md:204-206](../../AGENTS.md#L204-L206)
- [AGENTS.md:251-274](../../AGENTS.md#L251-L274)
- [AGENT_KNOWN_ISSUES.md:73-75](../../AGENT_KNOWN_ISSUES.md#L73-L75)
- [CMakeLists.txt:698-771](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L698-L771)
- [CMakeLists.txt:773-803](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L773-L803)
- [README.md:1-49](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L1-L49)
- [README.md:51-142](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L51-L142)
- [test_mesh_production_scenarios.c:382-589](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_production_scenarios.c#L382-L589)
- [test_mesh_click_report_delivery_sweep.c:398-441](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_click_report_delivery_sweep.c#L398-L441)
- [test_mesh_survey_topology_scenarios.c:879-888](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c#L879-L888)
- [test_mesh_survey_topology_scenarios.c:1337-1360](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c#L1337-L1360)
- [test_gateway_ble_transport_model.c:177-300](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_gateway_ble_transport_model.c#L177-L300)
- [test_dwm3000_models.c:167-342](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_dwm3000_models.c#L167-L342)
- [Mesh Integration Coverage Matrix 2026-07-12.md:50-94](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Mesh%20Integration%20Coverage%20Matrix%202026-07-12.md#L50-L94)

</details>

# Testing, Simulation, and Release Evidence

Testing follows the same story as a participant click: prove each decision locally, prove the complete [delivery path](02_one-click-end-to-end.md) under hostile timing, prove that the exact role artifacts still build, then prove the remaining hardware behavior on the boards. No single green layer is a release verdict.

> **Story path:** [← Previous: Verified Mesh Deployment](11_verified-deployment-and-qualification.md) · [Next: Hardware Bring-Up →](13_hardware-bring-up-and-troubleshooting.md)
>
> **Related:** [Connected routing](05_connected-routing-and-reliable-delivery.md) · [Anchor self-setup](07_anchor-self-setup-survey-and-geometry.md)

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-test-layers -->
## The Evidence Ladder

Each layer answers a different question, so later layers complement rather than erase earlier evidence.

| Layer | What it proves | What remains unproved |
| --- | --- | --- |
| Focused native tests | A platform-independent protocol, state machine, policy, or data contract behaves correctly in isolation. The repository requires native protocol/state tests before hardware and a full native CTest run before submission ([AGENTS.md:204-206](../../AGENTS.md#L204-L206)). | Cross-module timing, Zephyr scheduling, and physical devices. |
| Application seams and source invariants | Focused C targets exercise application helpers, while source-invariant tests reject forbidden ownership and API shapes. The enumeration-stall guards are registered across the communication, survey, enumeration, BLE, and static-analysis labels, with the focused Channel-5 priority helper in the full native suite ([CMakeLists.txt:321-325](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L321-L325), [CMakeLists.txt:384-405](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L384-L405)). | The real kernel, controller, vendor driver, scheduling interleavings, and board. |
| Mesh integration scenarios | Production-independent protocol, route, relay, runtime, report, and UWB-session modules interact inside a deterministic discrete-event radio world ([README.md:3-25](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L3-L25)). | Unmodeled silicon, analog RF, and complete Zephyr workqueue equivalence. |
| Focused hardware models | DWM3000 runtime order and duration, BLE framing and credits, stack budgets, and deployment tooling are checked without flashing ([README.md:89-108](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L89-L108)). | Vendor implementation details, calibrated RF behavior, compiler-complete stack proof, and runtime watermarks. |
| Exact preset builds | The current source, Kconfig, overlays, and role composition produce the three production-candidate artifacts. Zephyr-facing changes must build all three roles ([AGENTS.md:62-71](../../AGENTS.md#L62-L71), [AGENTS.md:204-206](../../AGENTS.md#L204-L206)). | That those exact bytes behave correctly on the mapped physical boards. |
| Bench and qualification evidence | Probe identity, exact artifact identity, RTT behavior, BLE observation, and requested multi-board interactions close the physical gaps ([AGENTS.md:269-274](../../AGENTS.md#L269-L274)). | Nothing outside the stated capture scope; each claim must stay bound to its artifact, probes, roles, and observation window. |

This structure matters to the user story because silent loss can occur at several ownership boundaries: a correct encoder does not prove a route, a correct route does not prove a BLE stream, and a linked binary does not prove the loaded board. The release record should name the layer that produced each fact and carry open gaps forward.

Sources: [AGENTS.md:62-71](../../AGENTS.md#L62-L71), [AGENTS.md:204-206](../../AGENTS.md#L204-L206), [AGENTS.md:269-274](../../AGENTS.md#L269-L274), [CMakeLists.txt:321-325](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L321-L325), [CMakeLists.txt:384-405](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L384-L405), [README.md:3-25](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L3-L25), [README.md:89-108](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L89-L108)
<!-- END:AUTOGEN imec2-12-testing-and-simulation-test-layers -->

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-simulator-contract -->
## What the Simulator Models

The simulator uses integer-microsecond discrete events and fixed-point DW3000 arrival intervals. A frame decodes only when its complete propagated airtime lies inside one RX window with a matching channel and PHY; a partial preamble, SFD, or frame times out, while overlapping transmissions collide instead of being delivered ([README.md:27-45](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L27-L45)). Capacity exhaustion, radio conflict, malformed frames, unsupported relay actions, and unexpected route discovery fail explicitly, with no direct-delivery fallback to rescue a broken path ([README.md:47-49](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L47-L49)).

Time is also charged where firmware must spend it. The DWM3000 model checks slow and fast SPI transitions, reset and wake, retained restore, PHY and PLL preparation, RX/TX work, status reads, frame transfers, and CIR access; illegal radio/SPI overlap fails instead of completing instantaneously ([README.md:89-102](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L89-L102)). Its tests reject illegal sequencing and radio overlap, and retain the legal retained-sleep/wake path as an explicit state contract ([test_dwm3000_models.c:167-342](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_dwm3000_models.c#L167-L342)).

At the gateway edge, the BLE model preserves a transmit cursor across rejected completions, limits notification payloads by negotiated ATT MTU, exhausts credits when the central stalls, restores them only at modeled connection events, and retries the same cursor chunk after disconnect ([test_gateway_ble_transport_model.c:177-300](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_gateway_ble_transport_model.c#L177-L300)). Watchdog progress, route state, relay queues, and stack-budget costs are likewise finite rather than zero-time assumptions ([AGENTS.md:261-267](../../AGENTS.md#L261-L267)).

The boundary is deliberate: the models do not emulate analog UWB path loss, hardware DS-TWR timestamps, the vendor driver, real NVS power-loss phases, SoftDevice RF behavior, or every Zephyr scheduling interleaving. Compiler call-chain analysis and physical stack high-water measurements remain separate evidence ([README.md:110-136](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L110-L136)).

Sources: [README.md:27-49](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L27-L49), [README.md:89-136](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L89-L136), [test_dwm3000_models.c:167-342](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_dwm3000_models.c#L167-L342), [test_gateway_ble_transport_model.c:177-300](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_gateway_ble_transport_model.c#L177-L300)
<!-- END:AUTOGEN imec2-12-testing-and-simulation-simulator-contract -->

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-scenarios-and-invariants -->
## Scenario Sweeps and Source Invariants

Scenario tests are broad contracts, not reenactments of one convenient log. The production suite drives one-to-six-relay lines, click preemption, same-route retry, empty receive-slot expiry, connection repair, and watchdog failure; it checks delivery identity, gateway ACK completion, retry timing, and the absence of hidden route discovery ([README.md:66-87](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L66-L87), [test_mesh_production_scenarios.c:382-589](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_production_scenarios.c#L382-L589)).

The click-report sweep runs 128 deterministic direct and relayed cases, requires real attempts, retries, deferrals, and a bounded maximum latency, then prints `reliability_claim=none` and names the unmodeled end-to-end seam. That output prevents an empirical model percentage from being promoted into a product reliability claim ([test_mesh_click_report_delivery_sweep.c:398-441](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_click_report_delivery_sweep.c#L398-L441)).

[Anchor self-setup](07_anchor-self-setup-survey-and-geometry.md) coverage scales through 2, 6, 16, 32, and 50 anchors, then runs explicit over-capacity, missing-route, direct-gateway custody, semantic rejection, duplicate redelivery, short-RX, mismatched-ACK, twenty-anchor contention, route-loss recovery, reset recovery, and TTL-exhaustion cases ([test_mesh_survey_topology_scenarios.c:1337-1360](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c#L1337-L1360)). One of those scenarios deliberately collides twenty RF-direct survey reports, injects ACK loss, and requires retry to exactly-once completion rather than treating scheduled transmission as delivery ([test_mesh_survey_topology_scenarios.c:879-888](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c#L879-L888)).

The enumeration-stall regression adds a structural guard at each ownership boundary that failed. Survey-pair results must enter bounded asynchronous communication custody without falling back to the legacy report queue or direct mesh calls, and verified gateway command or survey traffic must preempt an unrelated route-reply listener ([test_node_comm_protocol_callers.py:89-110](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_node_comm_protocol_callers.py#L89-L110)). The BLE source invariant requires retry custody after notification refusal, forbids forced disconnect in that path, and checks capped exponential retry ([test_gateway_observability_source_invariants.py:48-60](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_gateway_observability_source_invariants.py#L48-L60)). The focused Channel-5 helper test separately proves that gateway-control priority keeps the packet relevant even when ordinary control-followup state is false ([test_app_mesh_c5_priority.c:160-180](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/test_app_mesh_c5_priority.c#L160-L180)).

Those checks encode the diagnosed cause—short BLE retry bursts, an unbounded legacy report queue, and gateway controls losing to unrelated route listening—but they prove source shape and the focused helper, not end-to-end progress on a loaded gateway and anchors ([AGENT_KNOWN_ISSUES.md:316-317](../../AGENT_KNOWN_ISSUES.md#L316-L317)). Keep them paired with labeled pressure and timing scenarios, exact role builds, and bench observation.

Sources: [README.md:66-87](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/README.md#L66-L87), [test_mesh_production_scenarios.c:382-589](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_production_scenarios.c#L382-L589), [test_mesh_click_report_delivery_sweep.c:398-441](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_click_report_delivery_sweep.c#L398-L441), [test_mesh_survey_topology_scenarios.c:879-888](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c#L879-L888), [test_mesh_survey_topology_scenarios.c:1337-1360](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c#L1337-L1360), [test_node_comm_protocol_callers.py:89-110](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_node_comm_protocol_callers.py#L89-L110), [test_gateway_observability_source_invariants.py:48-60](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/mesh_integration/test_gateway_observability_source_invariants.py#L48-L60), [test_app_mesh_c5_priority.c:160-180](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/tests/test_app_mesh_c5_priority.c#L160-L180), [AGENT_KNOWN_ISSUES.md:316-317](../../AGENT_KNOWN_ISSUES.md#L316-L317)
<!-- END:AUTOGEN imec2-12-testing-and-simulation-scenarios-and-invariants -->

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-required-gates -->
## Required Gates Before Hardware

Start from a freshly configured and built native tree. Running CTest alone is insufficient because a green invocation can execute binaries older than their test sources ([AGENT_KNOWN_ISSUES.md:73-75](../../AGENT_KNOWN_ISSUES.md#L73-L75)).

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
ctest --test-dir firmware/build -L mesh_integration --output-on-failure
ctest --test-dir firmware/build -L hardware_models --output-on-failure
```

The full native sequence is the repository baseline ([AGENTS.md:23-28](../../AGENTS.md#L23-L28)); mesh-affecting changes must additionally run their focused test plus both labeled suites before flashing ([AGENTS.md:251-259](../../AGENTS.md#L251-L259)). For the enumeration-stall regression, the communication-caller and gateway-observability source invariants belong to both labeled suites, while the focused Channel-5 priority target remains part of the complete native run ([CMakeLists.txt:321-325](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L321-L325), [CMakeLists.txt:384-405](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L384-L405)). Record the build command, resulting test counts, failures, source revision, and any skipped seam rather than copying an older green count forward.

Next, build the exact `mesh_clicker`, `mesh_anchor`, and `mesh_gateway` presets. A successful link proves role composition and capacity only; the dated coverage matrix explicitly distinguishes fresh exact builds from fresh hardware qualification and shows that stack-policy verification can still reject artifacts that fit their linker regions ([Mesh Integration Coverage Matrix 2026-07-12.md:50-74](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Mesh%20Integration%20Coverage%20Matrix%202026-07-12.md#L50-L74)).

For deployable roles, [verified qualification](11_verified-deployment-and-qualification.md) binds runtime stack evidence to the exact probe, ELF and HEX hashes, target-reported preset/build identity, transcript hash, capture command, and bounded UTC window. Typed `RUN_BEGIN`, `SAMPLE_BEGIN`, and `RUN_END` records must exercise click sequences, CIR handling, relay retry/custody, and BLE backpressure; marker-only logs and ISR-only output do not count as runtime stack watermarks ([AGENTS.md:124-150](../../AGENTS.md#L124-L150)).

Finish with the requested physical proof: verify the exact preset and probe-to-board map, observe RTT and BLE, and run the required multi-board smoke path. Simulation remains a pre-flash gate and regression net; it never substitutes for evidence from the exact bytes loaded on the exact boards ([AGENTS.md:62-64](../../AGENTS.md#L62-L64), [AGENTS.md:269-274](../../AGENTS.md#L269-L274)). Continue with [Hardware Bring-Up and Troubleshooting](13_hardware-bring-up-and-troubleshooting.md) when the remaining question is physical rather than algorithmic.

Sources: [AGENT_KNOWN_ISSUES.md:73-75](../../AGENT_KNOWN_ISSUES.md#L73-L75), [AGENTS.md:23-28](../../AGENTS.md#L23-L28), [AGENTS.md:62-71](../../AGENTS.md#L62-L71), [AGENTS.md:124-150](../../AGENTS.md#L124-L150), [AGENTS.md:251-274](../../AGENTS.md#L251-L274), [CMakeLists.txt:321-325](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L321-L325), [CMakeLists.txt:384-405](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/firmware/CMakeLists.txt#L384-L405), [Mesh Integration Coverage Matrix 2026-07-12.md:50-94](https://github.com/Jubliano-sama/IMEC2/blob/f6594e41b57f5fd612aba182e0bd13cbbdd0c621/Documentation/Mesh%20Integration%20Coverage%20Matrix%202026-07-12.md#L50-L94)
<!-- END:AUTOGEN imec2-12-testing-and-simulation-required-gates -->

---

## Continue the story

[← Previous: Verified Mesh Deployment](11_verified-deployment-and-qualification.md) · [Wiki home](README.md) · [Next: Hardware Bring-Up →](13_hardware-bring-up-and-troubleshooting.md)

Related deep dives: [Connected Routing, Priority, and Reliable Delivery](05_connected-routing-and-reliable-delivery.md) · [Anchor Self-Setup: Survey and Geometry](07_anchor-self-setup-survey-and-geometry.md)
