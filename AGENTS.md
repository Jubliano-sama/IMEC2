# Repository Agent Rules

This is a Zephyr/west firmware workspace for a research experience-sampling
system. Robustness is the governing requirement: the clicker, anchors, and
gateway must not stall silently, lose accepted data, release custody early, or
report success for work that did not complete, including during multi-month
operation.

## Mandatory preflight

Before mutation, file operations, refactoring, builds, or hardware work:

1. Inspect `git status` and preserve all existing or concurrent work. Never
   discard, rewrite, or stage another agent's changes.
2. Run the indexed preflight with every planned path and operation, for example:

   ```sh
   python3 firmware/scripts/agent_preflight.py \
     --paths firmware/src/mesh_relay.c firmware/app/src/app_mesh_report.c \
     --operations refactor
   ```

3. Read the emitted global rules, current active/unqualified/environment
   entries, and required references. Use `--all` for repository-wide work and
   `--list-topics` for routing help. Rerun when the scope changes.

`AGENT_CURRENT_ISSUES.json` is the curated present-tense overlay.
`AGENT_KNOWN_ISSUES.md` is append-only history; use the indexed matches as
non-authoritative context and never infer current truth from a superseded fix.
Append one concise line for a new annoying tool behavior, active gap,
correction, or fix. Add or update the curated overlay when the finding remains
active, unqualified, or environment-specific. The executable critical rule
IDs are `RULE-ROBUSTNESS-001`, `RULE-DEPLOYMENT-001`, and
`RULE-CONCURRENCY-001`.

## Protected scope and code placement

- Project code is under `firmware/`, documentation under `Documentation/`,
  repository automation under `firmware/scripts/`, and operator/GUI host tools
  under `tools/`.
- Do not edit `zephyr/`, `nrf/`, `modules/`, `nrfxlib/`, `bootloader/`, the
  DWM3000 vendor submodule, or `archive/old-dw1000-impl/` unless the task
  explicitly targets that dependency or historical reference.
- Put hardware-independent protocol and state logic in `firmware/src`; keep
  Zephyr, GPIO, BLE, settings, SPI, and DWM3000 integration in `firmware/app`.
- Use C11, four-space indentation, same-line braces, `snake_case`, and existing
  module naming. Do not add a project-wide identifier prefix.

## Firmware roles and hardware safety

The only production-candidate presets are `mesh_clicker`, `mesh_anchor`, and
`mesh_gateway`; they define current product behavior. `mesh_transmitter*`,
`ml_*`, generic `FIRMWARE_ROLE=*`, staged high-debug, and other bring-up images
are test, collection, or compatibility builds. Relay qualification specifically
requires `mesh_transmitter_forcedhop`, because the generic transmitter may take
a direct gateway path.

Before hardware work, state and verify the exact preset, artifact, probe ID, and
probe-to-board mapping. Recheck after any cable or board change. Never infer a
role from a generic build-directory name.

Production mesh deployment is transactional and wrapper-only:
verified `--stage-only`, qualification capture from that exact running
artifact, then promotion with the exact capture. Direct `west flash`, policy
bypasses, weakened evidence, or a second programming step are forbidden for the
three production presets. Keep the proven 4 MHz rate. Do not describe local
capture provenance as cryptographic or remote attestation.

For permitted bench/legacy west flashes, the probe selector is
`-- --dev-id <probe-id>`. Direct pyOCD uses `-u <probe-id>`. RTT needs a TTY;
startup evidence uses the prescribed pre-reset connection mode, but that mode
does not itself guarantee a target reset. Hardware commands require direct USB
access. Exact commands and the complete stage/capture/promote sequence live in
`Documentation/Development and Deployment Guide.md`.

## Verification contract

Use the single fresh-build entrypoint:

```sh
python3 firmware/scripts/verify_changes.py
```

It runs source-of-truth, architecture, agent-guidance, and deployment-policy
checks; configures a fresh native build; runs the complete CTest suite; and runs
the deterministic 500-seed mesh stress merge gate. Use `--sanitizers` for ASan
and UBSan. ThreadSanitizer and LeakSanitizer are not valid evidence on this host.

Zephyr-facing changes must also pass:

```sh
python3 firmware/scripts/verify_changes.py \
  --exact-roles --compatibility-builds
```

That builds fresh `mesh_clicker`, `mesh_anchor`, and `mesh_gateway` artifacts,
runs their static stack gates, executes the real Zephyr NVS persistence test on
`native_sim/native/64`, and compiles the supported legacy, bench-traffic, and
representative first/last ML collection lines. The Zephyr matrix requires a
tracked-clean checkout with no untracked files in its build-input directories,
so embedded Git identity remains truthful; unrelated local logs and artifacts
do not block it. Verification executes from an immutable temporary snapshot of
the application tree; symlink escapes and any source write, including an
ignored or restored write, invalidate the run. Every active west dependency
must match `firmware/west_projects.lock.json`, be clean at the locked commit,
use no concealing Git index flag, contain no unapproved ignored input or
symlink outside the guarded project set, and remain write-guarded with the
pinned `.west/config` for the matrix. The live `manifest/west.yml` must be
byte-identical to the frozen source snapshot and remain write-guarded from
project resolution through the matrix, and the locked project set is resolved
again before verification succeeds. Ambient Zephyr, CMake, module, toolchain,
and compiler-search overrides are forbidden. The default Zephyr build root is
temporary; pass an explicit exclusive root only when its artifacts must be
retained. During focused iteration,
routing, scheduling, click priority, retries, BLE, watchdog, radio sleep/wake,
SPI, airtime, or stack changes must run the relevant test plus both
`mesh_integration` and `hardware_models`; the complete final entrypoint remains
mandatory.

The simulator preserves hardware constraints: a frame decodes only when its
complete airtime fits a matching RX window; partial overlap fails; overlapping
transmissions collide; and SPI, BLE credit, watchdog, stack, and cleanup delays
are nonzero. Unsupported relay actions, malformed frames, capacity exhaustion,
missing routes, stale work, and terminal leaks fail explicitly. Never add a
direct-delivery, seeded-route, or success fallback that hides a broken
production path.

Simulation supplements native core tests and never replaces exact-preset
builds, probe-role verification, RTT, BLE observation, or requested multi-board
evidence.

## Mesh and state-ownership changes

Before changing routing, Channel 5/9 scheduling, mesh-role DWM3000 sleep/idle,
ACK retry, discovery, wake trains, flooding, or click preemption, read
`Documentation/Mesh Connected Routing Contract.md`. State which invariants the
change preserves. A contradiction requires explicit user permission plus a
list of changed behavior, affected roles, removed invariants, compatibility,
tests, and hardware checks before implementation.

Every new or migrated long-running path must have one serialized owner,
immutable identity, generation, 64-bit absolute deadline, actual RF-attempt
accounting, independent liveness wake, and exactly one terminal result.
Callbacks carry the generation and cannot mutate a later operation. One
logical packet has one custody owner; policy callers cannot start parallel
retry or terminal state machines. Frozen legacy paths remain architecture debt
until their ownership is explicitly audited or migrated; do not describe this
target model as already universal.

Timing, radio ownership, routes, queues, capacity, persistence, and
success/failure accounting need a worst-case test or build-time guard before
use. When a bug escapes existing tests, understand the cause first, delegate a
bounded regression task when an agent is available, and prefer a broad
invariant or adversarial scenario over one trace-shaped assertion. Keep code,
tests, contract, comments, and documentation aligned in the same change.

When repeated defects in one subsystem share a phase, token, deadline, retry,
cleanup, or terminal boundary, stop adding leaf-level patches. Map every writer
and wake edge, choose the smallest complete operation slice, move that slice
behind one pure state owner, and delete or delegate the retired state in the
same stage. Preserve wire, timing, power, role, and telemetry behavior unless a
separate decision explicitly changes them.

Source-shape tests may enforce static boundaries that the compiler cannot, such
as forbidding Zephyr or direct radio calls in a pure module. They must not make
function names, private fields, or statement ordering the primary proof of
runtime behavior when a native transition test can express the invariant.

For difficult DWM3000 bring-up, audit slow/fast SPI transitions and retained
sleep configuration before blaming RF or protocol behavior.

## Architecture and file health

`Documentation/Architecture Reset Plan.md` is the accepted migration decision.
Do not add new `.inc` composition or new behavior to the frozen orchestration
fragments. Replace one owner at a time behind tested interfaces; do not run a
big-bang product rewrite or combine an ownership migration with a wire-format,
timing, or power-policy change.

Check file and function size before and after significant work. A `.c` file
over 3000 lines, a core module over 2500, a function over roughly 150–200, or a
file mixing policy, persistence, transport, and coordination requires a
concrete extraction proposal. Name the target `.c/.h` modules and ownership
boundary. Do not perform a structural refactor without explicit permission in
the current conversation. `firmware/architecture_boundaries.json` is a
no-growth debt ceiling, not approval for the existing monoliths. Do not bypass
it with implementation-bearing headers, source inclusion, symlinks, alternate
fragment extensions, or production C files outside the declared roots.

The immutable architecture debt baseline is commit
`4b29225ce1efa4e1731887ab4df806434b63edca`, and published history must
retain that exact policy object. Do not squash, rebase, or prune it from a
release branch; a missing object is a verification failure, not permission to
weaken or reconstruct the baseline from the mutable checkout. An intentional
rebaseline is a two-commit review: first create and preserve the standalone
approved source/manifest baseline commit, then update the checker pin and its
guidance/tests in a separate commit. Preserve both commits in the merge.

## Documentation and delivery discipline

`Documentation/CURRENT.json` is the machine-readable current-document source.
Do not hardcode another current version elsewhere. Cross-reference-only edits
may update the current file in place. Content changes to a versioned document
create the next patch file and add a dated changelog entry; for example,
`1.2.3` advances to `1.2.4`. A larger major/minor change is permitted only when
the user explicitly requests it.

State-machine diagrams stay reader-level: use behavioral labels, keep
implementation names in prose, and make bounded loops clear about whether they
continue or restart a time window.

Commit bug fixes even when partial and state the remaining gap. Use imperative
subjects. A PR or handoff names affected roles, commands run, hardware
assumptions, and missing evidence. A wiki synchronization commit pins its
immediate source commit; generated citations, validation reports, and context
files bind to that pin and the current wiki-state digest. Preserve the source
commit in published history, or regenerate and repin the wiki after any
squash. Subagents use the default service tier unless the user explicitly
requests otherwise.

Write to the user like a senior engineer in chat: lead with the verdict, keep
causal reasoning together, use lists only for genuinely parallel facts or a
real sequence, avoid theatrical language, and cut material that does not change
the next decision.

## Authoritative navigation

- Current documents: `Documentation/CURRENT.json` and `Documentation/INDEX.md`
- Runtime contract: `Documentation/Mesh Connected Routing Contract.md`
- Runtime flow: `Documentation/Mesh Connected Routing Walkthrough.md`
- Build/deployment procedure: `Documentation/Development and Deployment Guide.md`
- Architecture migration: `Documentation/Architecture Reset Plan.md`
- Code navigation: `CODEMAP.md`
- Test model and stress profiles: `firmware/tests/mesh_integration/README.md`
