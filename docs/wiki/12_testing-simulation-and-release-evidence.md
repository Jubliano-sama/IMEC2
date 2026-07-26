<!-- PAGE_ID: imec2-12-testing-and-simulation -->

[Wiki home](README.md) / [From the user story to the system](README.md#follow-the-story) / Testing, simulation, and release evidence

<details>
<summary>📚 Relevant source files</summary>

The following files were used as context for generating this wiki page:

- [AGENTS.md:76-127](../../AGENTS.md#L76-L127)
- [AGENTS.md:129-152](../../AGENTS.md#L129-L152)
- [AGENTS.md:188-193](../../AGENTS.md#L188-L193)
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

Each layer answers a narrower question than the one after it. A release record is credible only when it says which layer produced each fact and carries every remaining gap forward.

| Layer | What it proves | What it does not prove |
| --- | --- | --- |
| Executable source checks | Repository truth, architecture limits, agent guidance, and deployment policy still agree, including their negative self-tests ([verify_changes.py:34-63](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L34-L63)). | Runtime behavior or a successful compilation. |
| Fresh native suite and source invariants | Platform-independent protocol/state behavior and structural ownership rules pass from a newly configured binary, followed by the deterministic 500-seed busy-line merge gate ([verify_changes.py:198-268](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L198-L268), [verify_changes.py:509-535](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L509-L535)). | Zephyr scheduling, the controller, the vendor driver, or a board. |
| Integration simulator and focused hardware models | Production protocol, route, relay, custody, UWB timing, BLE credit, watchdog, persistence, and stack-budget contracts interact under deterministic failures and nonzero costs ([README.md:3-31](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L3-L31), [README.md:314-359](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L314-L359)). | Analog RF, silicon behavior, and complete Zephyr workqueue equivalence. |
| Exact production-role and persistence gates | Fresh `mesh_clicker`, `mesh_anchor`, and `mesh_gateway` artifacts build, each passes the static stack policy, and the real Zephyr NVS persistence test runs on `native_sim/native/64` ([verify_changes.py:284-352](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L284-L352)). | That those exact bytes operate correctly on the mapped boards. |
| Compatibility builds | Bench transmitters, representative ML endpoints, and plain legacy roles still compile without being mistaken for production candidates ([verify_changes.py:355-404](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L355-L404)). | Deployment eligibility or current product semantics. |
| Candidate hardware and qualification | The exact artifact, probe, role, RTT workloads, BLE behavior, and requested multi-board RF path work in the stated observation window ([README.md:370-413](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L370-L413)). | Any workload or environment outside that recorded scope. |

CI mirrors the first five layers: separate normal and ASan/UBSan native jobs retain deterministic replay artifacts on failure, while the role job runs checks-only plus exact and compatibility builds ([firmware-verification.yml:9-38](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/.github/workflows/firmware-verification.yml#L9-L38), [firmware-verification.yml:40-68](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/.github/workflows/firmware-verification.yml#L40-L68)). A correct encoder does not prove a route, a correct route does not prove BLE custody, and a linked artifact does not prove the board, so no green layer substitutes for the next one.
<!-- END:AUTOGEN imec2-12-testing-and-simulation-test-layers -->

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-simulator-contract -->
## What the Simulator Models

The simulator calls the production protocol, connection, route, relay, event-owner, survey, gateway-collection, UWB-session, and report modules directly. Roles are explicit instances, and a scenario may seed a route only when that route is a declared precondition; route-formation scenarios start empty and exchange real requests, replies, and reply ACKs through the normal dispatcher ([README.md:7-31](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L7-L31)).

The radio world is strict. Discrete events use integer microseconds, arrival intervals use fixed-point DW3000 time, and a packet decodes only when its complete propagated airtime fits one matching RX window. A partial preamble, SFD, or frame fails; overlapping transmissions collide; capacity exhaustion, malformed frames, unsupported relay actions, radio conflicts, and missing routes fail the scenario rather than falling back to direct delivery ([README.md:33-55](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L33-L55)).

Time also advances where hardware would spend it. The focused models charge deterministic costs for slow/fast SPI transitions, reset, retained wake/restore, PHY/PLL setup, RX/TX, frame and CIR access, BLE notification credits, watchdog progress, stack use, and cleanup; illegal overlap and terminal leaks fail explicitly ([README.md:314-359](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L314-L359), [AGENTS.md:117-123](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/AGENTS.md#L117-L123)).

The boundary stays explicit: this is not an analog RF emulator, a vendor-driver replacement, or proof of every Zephyr/controller interleaving. It cannot supply calibrated path loss, silicon DS-TWR timestamps, exact embedded call-frame evidence, or physical thread high-water marks, which remain exact-build and board evidence ([README.md:370-413](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L370-L413)).
<!-- END:AUTOGEN imec2-12-testing-and-simulation-simulator-contract -->

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-scenarios-and-invariants -->
## Scenario Sweeps and Source Invariants

Scenario tests encode product invariants, not one successful trace. The current matrix covers direct and one-to-six-relay delivery, click preemption with same-route retry, finite low-duty receive windows, collision behavior, route formation, 50-anchor assignment, BLE queue and credit pressure, watchdog/reset behavior, CIR-sized records, and stack bounds ([README.md:57-99](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L57-L99)).

The deterministic stress runner separates protocol scheduling randomness from loss, ACK-loss, duplication, delay, and reset faults. Every boundary checks queue capacity, ownership, semantic delivery, duplicates, retries, released work, and a settled terminal world; a self-rescheduling zero-time loop returns a liveness error instead of hanging the host ([README.md:101-136](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L101-L136)). Campaigns run one immutable hashed binary, retain exact failing seeds and replay scripts, and make the merge profile reproducible rather than anecdotal ([README.md:258-287](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L258-L287)).

Lifecycle scenarios keep operations connected across their real custody boundaries: click wake/discovery/ranging and multihop report delivery, route formation, PROPOSE/ACCEPT/UPDATE/END, assignment publication and persistence, survey pair timing, result EACK, reset recovery, and post-operation liveness ([README.md:138-180](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/README.md#L138-L180)). That breadth matters because a local ACK can be correct while later gateway or host custody is still wrong.

Source-invariant tests guard boundaries that are difficult to exercise through a host harness. The click path must reserve BLE capacity before semantic acceptance, persist the click journal before stream/relay commit, send the gateway ACK only after those commits, restore the journal before transport startup, and clear it only after notification completion ([test_click_ble_custody_source_invariants.py:51-150](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/test_click_ble_custody_source_invariants.py#L51-L150)). The ML boundary test separately keeps the bounded BLE host path on `ml_clicker` while proving that `ml_anchor` remains UWB-only ([test_ml_preset_boundaries.py:28-52](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/tests/mesh_integration/test_ml_preset_boundaries.py#L28-L52)). These tests prove source structure; they still need scenario, build, and hardware evidence for end-to-end progress.
<!-- END:AUTOGEN imec2-12-testing-and-simulation-scenarios-and-invariants -->

---

<!-- BEGIN:AUTOGEN imec2-12-testing-and-simulation-required-gates -->
## Required Gates Before Hardware

Use the repository entrypoint, because it always performs the executable source checks and creates a fresh temporary native build unless an explicit build directory is requested ([verify_changes.py:470-535](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L470-L535)).

```sh
python3 firmware/scripts/verify_changes.py
python3 firmware/scripts/verify_changes.py --sanitizers
python3 firmware/scripts/verify_changes.py \
  --exact-roles --compatibility-builds
```

The first command runs source checks, the complete fresh native suite, and the 500-seed stress merge gate. The second repeats that behavior under ASan and UBSan. The third adds pristine production-role builds, static stack verification, the real Zephyr NVS persistence test, and all supported compatibility representatives ([AGENTS.md:76-115](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/AGENTS.md#L76-L115), [verify_changes.py:493-578](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L493-L578)).

The result is bound to one stable input graph. The verifier fingerprints the full application state, runs from an immutable snapshot, requires all 52 west projects at their locked commits, and write-guards both source and dependencies across the matrix; a dirty dependency, hidden index flag, influential ignored file, symlink escape, ambient build override, or concurrent mutation fails the run ([AGENTS.md:96-110](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/AGENTS.md#L96-L110), [verification_inputs.py:946-999](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verification_inputs.py#L946-L999), [verification_inputs.py:1000-1170](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verification_inputs.py#L1000-L1170)). It does not content-attest host tools selected through `PATH` or the host CMake package registry, so container-grade reproducibility still requires a pinned toolchain image.

During focused iteration on routing, scheduling, priority, retries, BLE, watchdogs, radio sleep/wake, SPI, airtime, or stack budgets, run the relevant target plus the `mesh_integration` and `hardware_models` labels. Those shorter loops help locate a failure, but the complete final entrypoint remains mandatory ([AGENTS.md:111-115](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/AGENTS.md#L111-L115)). `--checks-only` deliberately skips native compilation and tests, so it is suitable for documentation/source-policy validation or CI composition, never as behavioral release evidence ([verify_changes.py:413-421](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L413-L421), [verify_changes.py:508-535](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/firmware/scripts/verify_changes.py#L508-L535)).

After these gates pass, bind the exact artifact to the exact probe with [verified staging, qualification, and promotion](11_verified-deployment-and-qualification.md), then complete the requested RTT, BLE, and multi-board smoke evidence. The contract explicitly keeps simulator results separate from probe-role verification and physical behavior ([AGENTS.md:117-127](https://github.com/Jubliano-sama/IMEC2/blob/c9e8e2fe4a450a8d65f697ce026f8524c81b105f/AGENTS.md#L117-L127)).
<!-- END:AUTOGEN imec2-12-testing-and-simulation-required-gates -->

---

## Continue the story

[← Previous: Verified Mesh Deployment](11_verified-deployment-and-qualification.md) · [Wiki home](README.md) · [Next: Hardware Bring-Up →](13_hardware-bring-up-and-troubleshooting.md)

Related deep dives: [Connected Routing, Priority, and Reliable Delivery](05_connected-routing-and-reliable-delivery.md) · [Anchor Self-Setup: Survey and Geometry](07_anchor-self-setup-survey-and-geometry.md)
