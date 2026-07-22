# Repository Guidelines

## Project Structure & Module Organization

This workspace is a Zephyr/west firmware tree for the IMEC clicker, anchor, and gateway system. Project-specific code lives under `firmware/`; imported SDKs and platform dependencies live in `zephyr/`, `nrf/`, `modules/`, `nrfxlib/`, `bootloader/`, and `dwm3000 examples and sdk/`.

- `firmware/include/`: shared protocol, report, route, status, survey, and UWB headers.
- `firmware/src/`: platform-independent C modules with native unit tests.
- `firmware/app/`: Zephyr application, board overlay, DWM3000 port, role-specific runtime.
- `firmware/tests/`: native C tests.
- `Documentation/`: architecture, protocols, implementation task list.
- `archive/old-dw1000-impl/`: historical reference implementation (old DW1000-era code); do not modify.

## Build, Test, and Development Commands

Use the local uv-managed Python environment:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt
```

Native library tests:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

### Firmware Lines and Role Meaning

The connected-routing mesh line is the production successor and will become
the main firmware after the migration is complete. Treat its role presets as
the default target for new product behavior even while their names still carry
the `mesh_` prefix:

- `mesh_clicker`: normal battery clicker behavior. It sleeps normally, wakes
  for a physical click/range sequence, and uses the connected-routing mesh path
  for delivery. It is not a continuously active test transmitter.
- `mesh_anchor`: the single connected-routing anchor image for every production
  anchor. It derives a stable node ID from the nRF FICR hardware identity;
  logical discovery/ranging order is assigned by the gateway and persisted.
  An anchor ranges local clicks, relays mesh work, and prioritizes its own click
  reports over transit traffic.
- `mesh_gateway`: gateway role for the same connected-routing firmware,
  including gateway BLE ingress/egress and highest-priority gateway commands.

The remaining build lines are not alternative production architectures:

- `mesh_transmitter` and `mesh_transmitter_forcedhop` are synthetic traffic
  generators used to load and regression-test the production-successor mesh
  path. They must not be treated as deployable anchor firmware.
- `ml_clicker` and `ml_anchor_1` through `ml_anchor_8` are demo/data-collection
  images for gathering training and validation data for a distance-offset
  compensation model. They are not production clicker or anchor builds.
- Direct `FIRMWARE_ROLE=clicker|anchor|gateway` builds and staged high-debug
  presets are legacy or bring-up compatibility images. Maintain them for
  bounded regression coverage, but do not add new mainline behavior there
  unless the task explicitly targets a legacy build.

Until the production-successor presets are renamed, always state and verify the
exact preset and probe-to-board mapping before flashing; do not infer behavior
from the generic role name alone.

Production-candidate mesh role builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-clicker -- -DIMEC_BUILD_PRESET=mesh_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-anchor -- -DIMEC_BUILD_PRESET=mesh_anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-gateway -- -DIMEC_BUILD_PRESET=mesh_gateway
```

Bench traffic-source builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-transmitter -- -DIMEC_BUILD_PRESET=mesh_transmitter
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/mesh-transmitter-forcedhop -- -DIMEC_BUILD_PRESET=mesh_transmitter_forcedhop
```

For anchor-relay regression tests, flash `build/mesh-transmitter-forcedhop`.
Do not substitute `build/mesh-transmitter`: the generic transmitter may select
a direct gateway hop and therefore does not prove the relay path under test.

Legacy regression role builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway
```

### Firmware role meaning

- `mesh_clicker`, `mesh_anchor`, and `mesh_gateway` are the active
  production-candidate firmware line and the source of truth for behavior that
  will become the main firmware.
- Plain `clicker`, `anchor`, and `gateway` role builds are legacy compatibility
  and regression images. Keep them building, but do not use them to infer the
  current mesh runtime contract.
- `mesh_transmitter` and `mesh_transmitter_forcedhop` are powered bench traffic
  sources for route, retry, and preemption regression tests; they are not a
  deployed product role.
- `ml_clicker` and `ml_anchor_<slot>` are demo/data-collection images for a
  distance-offset compensation model. ML-specific behavior is not the main
  product contract.

ML collection builds:

```sh
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-clicker -- -DIMEC_BUILD_PRESET=ml_clicker
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-1 -- -DIMEC_BUILD_PRESET=ml_anchor_1
.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/ml-anchor-2 -- -DIMEC_BUILD_PRESET=ml_anchor_2
```

For deterministic ML anchors, replace `build/ml-anchor-2` and `ml_anchor_2` with the anchor slot being programmed, such as `build/ml-anchor-3` / `ml_anchor_3` through `build/ml-anchor-8` / `ml_anchor_8`. Each preset assigns a unique `DEVICE_ID` and deterministic discovery slot.

Direct `west flash` is not a deployment path for `mesh_clicker`,
`mesh_anchor`, or `mesh_gateway`. The only supported
deployment path is the repository-owned verified wrapper below. It fixes pyOCD
at 4 MHz, fixes the policy header to this checkout, accepts only those three
presets, and consumes each trusted capture once.

First create a qualification capture for the exact artifact already loaded on
the qualification target. The capture tool itself runs the required TTY-backed
`pyocd rtt -t nrf52833 -M pre-reset -u <probe-id>` command; it does not flash:

```sh
.venv/bin/python firmware/scripts/capture_stack_evidence.py \
  --build-dir build/mesh-anchor \
  --probe-id E46070D247233537 \
  --output-dir logs/stack-evidence
```

Then deploy only through the verified entrypoint:

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-anchor \
  --hardware-manifest logs/stack-evidence/mesh-anchor-<capture-id>.json \
  --probe-id E46070D247233537
```

Schema-3 capture evidence binds the probe ID, exact ELF and HEX SHA-256 hashes,
target-reported preset/build identity, raw transcript SHA-256, fixed capture
command, and bounded UTC capture window. It requires typed real-operation
`RUN_BEGIN`/`SAMPLE_BEGIN`/`RUN_END` records for click sequences, CIR handling,
relay retry/custody, and BLE backpressure. Queue and custody state are recorded
at run completion. ISR output is configuration-only; it is not a runtime stack
watermark. Marker-only logs and user-authored manifests are rejected.

This is the strongest local provenance available, not cryptographic probe
attestation. A user able to modify the checkout, artifacts, transcript, tool,
and local replay ledger can still forge local state; do not represent it as a
remote attestation service. A malicious local host owner can always bypass
repository policy or program a probe directly; this gate makes repository and
CI release/deployment eligibility fail, not host-owner prevention.

Bench and legacy images remain non-deployment paths. For example, a relay bench
may flash the forced-hop traffic source directly:

```sh
.venv/bin/west flash --runner pyocd --build-dir build/mesh-transmitter-forcedhop -- --frequency 4000000
```

The following flash commands are only for legacy regression and ML collection
images; do not use them for connected-routing hardware tests:

```sh
.venv/bin/west flash --runner pyocd --build-dir build/firmware-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/firmware-anchor -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/firmware-gateway -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/ml-clicker -- --frequency 4000000
.venv/bin/west flash --runner pyocd --build-dir build/ml-anchor-1 -- --frequency 4000000
```

With more than one probe attached, pass the probe ID to west after the runner
separator: `-- --dev-id <probe-id> --frequency 4000000`. The shorter
`-u <probe-id>` form belongs to direct `pyocd` commands such as RTT and must not
be used as a west-flash argument.

For deterministic ML anchors, replace `build/ml-anchor-1` with the anchor image being programmed, such as `build/ml-anchor-2` through `build/ml-anchor-8`.

For legacy and bench-only direct west flashes, select a specific probe with
`-- --dev-id <probe-id> --frequency 4000000`. Do not pass `-u <probe-id>` to
`west flash`; `-u` is only for direct `pyocd rtt`. Deployable mesh presets do
not use this bypass: pass `--probe-id` only to `flash_verified_mesh.py`.

Flashing at 4MHz has been proven to work, if a flash fails, assume the cabling is at fault and do not reduce flash speed.

When capturing RTT logs for startup or boot behavior, use pyOCD's `pre-reset` connect mode so the capture includes reset-time output, for example `pyocd rtt -t nrf52833 -M pre-reset -u <probe-id>`. `pyocd rtt` needs a TTY; run it interactively or under `script`, and do not redirect its stdout directly to a file because that can fail with `Inappropriate ioctl for device`.

Hardware RTT and flash commands must run with direct USB device access. A
sandboxed command can misleadingly show both probes in `pyocd list` yet leave
`pyocd rtt` waiting forever for the same probe ID because the RTT subprocess
cannot access the USB device. If that exact mismatch occurs, rerun the hardware
command with full host/USB permissions; do not treat it as a disconnected probe
or as firmware evidence.

## Coding Style & Naming Conventions

Code is C using Zephyr conventions: 4-space indentation, braces on the same line for functions/control blocks, and `snake_case` for functions and variables. Do not add project-wide prefixes such as `IMEC_` to new identifiers; use descriptive module-scoped names and the existing protocol/route/status naming style. Keep hardware-independent logic in `firmware/src`; keep Zephyr, GPIO, BLE, and SPI code in `firmware/app`.

## Testing Guidelines

Add native tests for protocol/state behavior before relying on hardware. Keep tests focused and named after the module under test, such as `test_protocol.c` or `test_survey.c`. Run `ctest --test-dir firmware/build --output-on-failure` before submitting changes. For Zephyr-facing changes, build all three roles.

## Commit & Pull Request Guidelines

Use clear imperative commit subjects, for example `Add BLE-gated anchor ranging MVP`. PRs should include a short summary, affected roles (`clicker`, `anchor`, `gateway`), test/build commands run, and any hardware assumptions or smoke-test gaps. After a bug is fixed, always commit your work, even if the bug is only partially fixed.

## Agent-Specific Instructions

When spawning subagents, use the default service tier. Do not request a
priority/fast tier unless the user explicitly asks for it; fast-tier agents
consume limited credits.

**Mandatory pre-step**: Before starting any new work, adding code, refactoring, performing file operations, structural changes, or build modifications, read the entire `AGENT_KNOWN_ISSUES.md` and review its lists for similar past tool or state issues. Append one-line entries for new annoying tool behaviors or bugs you encounter/fix.

**Project context for agents**: This is a research experience-sampling system for correlating subjective user clicks with environmental sensors in a real office testbed. A major new capability is automated anchor self-setup (solving network geometry from anchor-to-anchor distances). The single most important property is **robustness** — the system must not stall, lose data, or return incorrect results under any circumstances, including multi-month operation. See the "Project Narrative & Goals" section in the root README.md for details.

Do not edit imported dependency trees unless the task explicitly targets them. Prefer documenting protocol changes in `Documentation/` alongside code changes.

### Proactive Refactoring and Monitoring File Health

You are expected to **autonomously monitor** code size and complexity during your work and proactively suggest refactors or splits when files are becoming unwieldy — even if the user has not asked.

**Thresholds that should trigger a suggestion** (use `wc -l` and inspect structure):
- Any `.c` file exceeding ~3000 lines.
- Core modules (especially in `firmware/src/` or `firmware/app/src/`) exceeding ~2500 lines.
- Any single function exceeding ~150-200 lines.
- A file that clearly mixes multiple responsibilities (e.g. report generation + coordinator logic + persistence + policy in one file).

When you notice these conditions (or when editing a large file makes navigation difficult), pause and suggest a refactor to the user.

Suggestions should be concrete:
- Propose specific new file names (follow existing patterns like `app_mesh_*.c`).
- Describe what to extract and why.
- Reference successful past splits (the original monolithic `main.c` was broken into many focused `app_*.c` modules; `app_mesh_coordinator.c` was extracted from reporting logic).

**Important**: For unwieldy files, your role is to *suggest* the refactor to the user. Provide a clear plan (what to extract, suggested file names, rationale). Do **not** perform the actual refactoring edits yourself unless the user explicitly tells you to proceed in this conversation.

Prioritize small, focused modules with clear ownership.

Always check current file sizes at the start of significant work on a module, and again after adding substantial code.

When you identify opportunities to refactor in order to prevent recurring bugs or improve clarity, suggest the change to the user with a clear rationale and proposed approach. Do not make structural changes without explicit confirmation in the current conversation. Prefer explicit state ownership and timing-phase boundaries over accumulating local patches, while preserving contract behavior, role behavior, LEDs, tests, and required hardware checks.

For timing, radio state, routing, queues, packet capacity, or success/failure accounting, add a worst-case test or build-time guard before relying on the path. Keep hardware assumptions aligned across code, docs, and this file.

Treat `firmware/tests/mesh_integration/` as a mandatory pre-flash gate for the
production-candidate mesh line. Changes to routing, channel scheduling, click
priority, retries, BLE transport, watchdog behavior, radio sleep/wake, SPI
timing, airtime, or stack budgets must run both the focused native test and:

```sh
ctest --test-dir firmware/build -L mesh_integration --output-on-failure
ctest --test-dir firmware/build -L hardware_models --output-on-failure
```

The simulator must preserve the hardware constraints it models: a UWB frame is
decodable only when its complete airtime is contained in a matching RX window;
partial overlap is a timeout or decode failure; overlapping transmissions
collide; SPI, BLE credit, watchdog, and stack-budget delays are not zero-time.
Unsupported relay actions, capacity exhaustion, malformed frames, and missing
routes must fail the scenario explicitly. Do not add direct-delivery or seeded
route fallbacks that can hide a broken production path.

After diagnosing a firmware bug that escaped all existing tests, delegate a
bounded test task to a subagent when available. Add the regression only after
the cause is understood, and prefer a broader invariant or scenario test over
an assertion that reproduces only one observed trace. The simulator supplements
but does not replace exact-preset builds, probe-role verification, RTT capture,
BLE observation, and requested multi-board smoke tests.

Before changing mesh routing, channel 5/channel 9 scheduling, DWM3000 sleep/idle behavior in mesh roles, ACK retry, route discovery, wake-train semantics, blind flooding, or click preemption, read `Documentation/Mesh Connected Routing Contract.md`. Treat it as the high-level design contract, state which invariants the change preserves, and update it alongside any intentional design change. Do not push through a change that contradicts the contract without explicit user permission; first generate a clear list of the new behavior, affected roles, changed or removed invariants, and required tests or hardware checks.

For difficult DWM3000 bring-up failures, explicitly audit SPI speed transitions and sleep/wake configuration retention before assuming the protocol or RF path is at fault. Both fast/slow SPI ordering and retained sleep configuration have caused hard-to-find behavior where a path works once after reset but fails after sleep or wake.

When modifying versioned documentation in `Documentation/`, increment only the patch component unless the user requests a larger version change. For example, the next edit to `Documentation/UWB+BLE Architecture 0.4.md` becomes `Documentation/UWB+BLE Architecture 0.4.1.md`; do not change `0.4` to `0.5` unless explicitly requested. Cross-reference-only edits do not need a version bump or changelog entry; update the existing document in place unless the content itself changes. Every documentation version bump must also add a short dated changelog entry inside the modified file. This rule applies to all versioned docs:

- `UWB+BLE Architecture X.Y.Z.md`
- `UWB+BLE Protocols and Strategies X.Y.md`
- `Firmware State Machines and Status Report X.Y.md`

For `Documentation/Firmware State Machines and Status Report X.Y.Z.md`, keep diagrams as a high-level reader overview. Mermaid flowchart and state labels should describe system behavior in plain language, not function names, enum/action constants, internal variable names, or low-level implementation shorthand. When a loop represents a bounded time window, state that it is the same continuous window and not a restarted window. Prefer labels such as "Run UWB range with next anchor", "Send result through mesh", and "Mark anchor idle and resume scan" over labels such as `dwm3000_driver_*`, `mesh_start_tracked_tx`, `RANGE_OK`, `success_count++`, or "poll once until deadline". Keep precise source references in the prose above each chart, not inside the chart labels.

# Writing style

Write in flowing technical prose, the way a sharp senior engineer talks in chat - direct, conversational, and confident. Not documentation, not a report, not a slide deck.

Rules:

1. **Answer exactly what was asked, at the length it deserves - err short.** A yes/no or confirmation question gets 2-4 sentences. A "which one should I pick" gets a few paragraphs. Only a genuinely multi-part design question earns a long answer. Before sending, cut any paragraph that doesn't change what the reader does next: background they didn't ask for, restating their situation back to them, generic advice ("monitor it", "measure first") they'd already know. Seven paragraphs where three would do is a style failure even if every paragraph is well-written.
2. **Every paragraph and every bullet carries a complete argument** - claim, mechanism, and consequence together. Never state a fact without saying why it matters in the same breath. Not "MoR increases scan cost, latency, and metadata overhead" but "MoR is cheap to write, but every read has to reconcile delete files against data files, so scans get slower and flakier until something compacts them - and now that's your problem to operate."
3. **Match the form to the content - and vary it.** A long answer whose every block has the same shape (all paragraphs, all bold-lead paragraphs, all bullets) is monotonous and hard to scan; real explanations mix forms because the content mixes kinds. Pick per part:
 - **Distinct sections or comparison axes** (cost vs ops, "how generation works" vs "conventions") -> short bold headings on their own line, like "**The API reference is generated, not hand-written**" or "**Cost:**". A multi-axis comparison in undifferentiated paragraphs is a style failure just like a fragmented list is.
 - **A genuine sequence** (pipeline stages, diagnostic steps, ranked guesses) -> a numbered list, each item opening with a short bolded lead phrase and continuing in full sentences (1-4 of them).
 - **Genuinely parallel, enumerable facts** (the four config files involved, the three limits that apply) -> a plain bullet list; items may be a single full sentence when the facts are simple, and that's fine.
 - **Reasoning, causality, narrative** -> paragraphs.
 Shortening never means flattening: when rule 1 says cut, cut sentences within the structure - don't collapse headings, lists, and sections into uniform paragraphs.
4. **Don't shred connected reasoning into bullets.** If items connect with "because"/"so"/"but", those connections are the content - write prose. And never a bolded label followed by a clipped noun phrase posing as a bullet.
5. **Open with the verdict and its central caveat in one or two plain sentences.** Not a bolded headline.
6. **Conversational but not dramatic.** Use contractions (it's, you'd, don't). Say "so" and "but", not "therefore" and "however". Never write scaffolding like "The deciding mechanism is", "It is worth noting", "Importantly". No theatrical labels or hype adjectives: no "**The poison**", "the trap", "brutally expensive", "the killer feature", "sharp edge", "absurdly cheap". State the actual problem in plain words - "this rewrites gigabytes to change megabytes" beats any dramatic framing.
 - No staccato, short dramatic sentences. Let sentences breathe with commas, dependent clauses, and ideas linked together.
 - No cheesy setup phrases that introduce a point instead of stating it. Never write "here's the thing", "here's the kicker", "the part nobody warns you about", "what nobody tells you", "the dirty secret", "the truth is", "plot twist", "the reality is", "here's what's wild". State the claim directly.
 - No contrastive "not just X, but Y" structure or its variants ("it's not just X, it's Y", "not only X but also Y"). State the point directly instead of negating one framing to elevate another.
7. **No compression.** No dropped articles, no strings of abstract nouns where one concrete mechanism explains more. Shortness comes from cutting low-value content (rule 1), never from clipping sentences.
8. **End with a bottom line only when the answer weighed a real decision.** One plain-prose sentence: the call plus the condition that would flip it. Short factual or confirmation answers just end - no formulaic closer.
