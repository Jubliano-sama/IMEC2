# Mesh Integration Coverage Matrix - 2026-07-12

## Scope

This is the final pragmatic alpha stabilization matrix for the connected-routing
mesh line. It separates functional evidence from deployment qualification. This
pass did not flash hardware. The forced-relay hardware closure cited below is
the already-recorded exact-artifact evidence in `Mesh Integration Simulator
Adversarial Review 2026-07-11.md`.

Fresh functional gates completed from isolated native build directories:

- full native CTest: 79/79;
- `mesh_integration`: 28/28;
- `hardware_models`: 21/21;
- Python capture/evidence/deployment-policy suite: 20/20;
- whole-tree ASan/UBSan CTest: 79/79.

`firmware/CMakeLists.txt` contains 79 `add_test()` registrations and the fresh
CTest manifest contains 79 tests. A no-op rebuild followed the fresh test run;
the newest test inputs predate the produced test executables. `git diff --check`
also passed.

## Functional Coverage

Score method: each requested behavior row is worth one point when Covered,
one-half point when Partial, and zero when Deferred. The score covers the 14
behavior rows below; the explicit deferred qualification dimensions are listed
separately and do not inflate the score.

| Behavior | Status | Concrete evidence |
| --- | --- | --- |
| Complete RX containment, airtime, and collision rejection | Covered | `test_mesh_integration`, `test_dwm3000_models`, and `test_mesh_sim_trace_scheduler` require complete matching RX windows and reject partial airtime/collisions. |
| Low-duty wake and listener windows | Covered | `test_mesh_wake_scenarios` plus the stable low-duty anchor hardware capture path. |
| DS-TWR wake claim/claim handoff | Covered | `test_mesh_wake_scenarios`, `test_app_wake_train_politeness`, and the forced-hop connection scenario cover claim admission and handoff. |
| Click, local, transit, and gateway-command priorities | Covered | `test_mesh_runtime_priority_scenarios`, `test_mesh_app_arbitration_scenarios`, `test_app_mesh_c5_priority`, and gateway-command tests. |
| Retry and custody identities | Covered | `test_mesh_relay`, `test_mesh_forcedhop_connection_scenarios`, and `test_app_mesh_preemption` check same-identity retry, duplicate suppression, custody transfer, and release. |
| No-route retention, route formation, and TTL ladder | Partial | Route request/reply and forward/reverse TTL decrements are covered by `test_mesh_route_formation_scenarios`; exhaustive originate-before-route wait/resume is still a review gap. |
| Multi-hop and four-anchor relay contention | Partial | `test_mesh_network_stress_scenarios` retains four reports and checks delivery/custody, but automatic downstream contention under one continuously live upstream connection remains incomplete. |
| Propose, accept, and repair | Covered | `test_mesh_forcedhop_connection_scenarios` exercises over-air propose/accept and repair; recorded hardware evidence has eight successful ACCEPT realignments, no ACCEPT abort/failure, and no reset. |
| Watchdog expiry and reset invalidation | Covered | `test_mesh_watchdog_lease_scenarios` and scheduler/reset scenarios invalidate role-owned work and prevent stale completions. |
| BLE backpressure and reconnect | Covered | `test_mesh_gateway_ble_scenarios`, `test_gateway_ble_transport_model`, and gateway stream tests cover bounded backpressure and reconnect behavior. |
| SPI, stack, and airtime models | Partial | `test_dwm3000_models`, `test_stack_budget_model`, and exact builds cover modeled timing and capacity; exact generated-config equivalence and full compiler attribution remain qualification work. |
| Discovery and assignment | Partial | Production-helper integration and exact RF are split across `test_discovery_assignment`, `test_app_discovery_assignment_policy`, `test_mesh_discovery_assignment_adversarial_scenarios`, and `test_mesh_discovery_scenarios`. The adversarial matrix runs 2, 6, 16, 32, and 50 anchors through `0x0104` claim/table/ACK identities with loss, duplication, stale epochs, priority deferral, persistence failure, reset, capacity, and deterministic-order faults. The RF scenario separately proves complete-airtime reception and collisions. Neither target executes the static Zephyr gateway coordinator, its workqueue scheduling, nor real NVS, so full coordinator end-to-end coverage remains deferred and hash fallback is never accepted as assignment evidence. |
| Forced-hop contact-only replies and stable relay | Covered | `test_mesh_forcedhop_connection_scenarios` is labeled `mesh_integration` and `hardware_models`; final hardware proof recorded 69 Channel-9 events, contact-only gateway replies, and zero direct-route markers. |
| Second-probe stack regression | Covered | The captured two-probe forced-hop/anchor closure uses transmitter probe `E46070D247394D36` and anchor probe `E46070D247233537`, with no reset during the stable relay segment. |

Result: 10 Covered + 4 Partial x 0.5 = 12 / 14 = **85.7%**.

## Exact Preset Capacity

All images below were freshly built with `--pristine=always` for
`nrf52833dk/nrf52833`. Free capacity is calculated from 512 KiB FLASH and
128 KiB RAM linker regions. These are functional build artifacts, not a claim
that the artifacts have been newly hardware-qualified.

| Preset | Artifact directory | FLASH used / free | RAM used / free | Tight margin |
| --- | --- | --- | --- | --- |
| `mesh_clicker` | final exact-role build | 257,288 / 267,000 B (49.07%) | 99,600 / 31,472 B (75.99%) | Stack policy needs 32,768 B static RAM headroom; short by 1,296 B. |
| `mesh_anchor` | final exact-role build | 293,184 / 231,104 B (55.92%) | 125,408 / 5,664 B (95.68%) | RAM margin is 4.32%; exact stack watermark qualification remains required. |
| `mesh_gateway` | final exact-role build | 345,680 / 178,608 B (65.93%) | 126,652 / 4,420 B (96.63%) | Tightest RAM margin: 3.37%; exact stack watermark qualification remains required. |
| `mesh_transmitter_forcedhop` | `build/alpha-mesh-transmitter-forcedhop` | 223,932 / 300,356 B (42.71%) | 123,296 / 7,776 B (94.07%) | Bench-only source; RAM margin is 5.93%. |

The three deployable role builds have `CONFIG_IMEC_STACK_DIAGNOSTICS=y`; the
forced-hop bench image has `CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ=y`.
All listed images fit their linker regions. `verify_stack_evidence.py` rejects
the fresh deployable directories because its conservative static headroom
thresholds are not met and it cannot attribute every linked application
function to compiler stack-use evidence. That is a deployment-qualification
blocker, not a compile, link, sanitizer, or demonstrated relay-functional
blocker. A fresh schema-3 RTT capture bound to each exact ELF/HEX is still
required. The native 50-claim/50-entry assignment-publisher guard is tied to
the same production local-object types and catches capacity growth beyond
4 KiB, but it is not a compiler frame measurement or a runtime stack watermark.

## Deferred Qualification Dimensions

| Dimension | Status | Reason |
| --- | --- | --- |
| Exhaustive Zephyr callback/workqueue equivalence | Deferred | Native seams and models do not prove every vendor-driver, ISR, and workqueue interleaving. |
| Malicious local flash bypass | Deferred | The repository policy prevents eligible-path bypasses; it cannot prevent a hostile local host owner programming a probe directly. |
| Perfect compiler IPA attribution | Deferred | Current verifier conservatively rejects unattributed linked application functions. |
| Every NVS power-loss phase | Deferred | Current persistence tests do not exhaust every write/interruption phase. |
| Probabilistic RF realism | Deferred | Models are deterministic and do not claim calibrated fading, interference, or loss distributions. |
| All-three-role runtime captures | Deferred | Existing hardware proof covers the forced-hop transmitter and mesh anchor; each deployable role still needs its own exact-artifact capture. |

## Release Interpretation

Functional alpha stabilization passes: the fresh native, model, policy,
sanitizer, registration, hygiene, exact-build, and observed forced-relay gates
are green. Verified deployment qualification does not pass until the conservative
stack verifier and fresh exact-artifact schema-3 runtime captures pass for each
deployable preset. The gateway and anchor RAM margins should be treated as
release-monitoring risks even though their binaries fit.
