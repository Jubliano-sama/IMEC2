# MeshSpec Worktree State (Context Checkpoint)

Timestamp: 2026-07-02 (UTC-07 from workspace date context; local CET zone is Europe/Amsterdam)
Location: `/home/tommie/Projects/IMEC2`

## Active objective

Implement `Documentation/MeshSpec.md` as closely as possible without modifying the
spec itself, using separable subagents for implementation work. The final
Markdown artifact must describe the mesh protocol that is actually implemented
in the current firmware tree, with partial or missing MeshSpec behavior called
out explicitly. It must not be an idealized protocol proposal, a restatement of
the spec, or an explanation of the agent's intended design.

## Goal state

Current `get_goal()`:
- Thread: `019f140e-48d6-70f3-8ad1-2381b8c06b67`
- Objective: implement MeshSpec + final protocol markdown
- Status: active

## Repo snapshot

- Branch: `master`
- HEAD: current MeshSpec implementation commits through the app preemption
  side-effect test slice
- `git status --short` after this checkpoint edit is expected to show:
  - No tracked modifications after the current slice is committed.
  - Untracked files only:
    - `Documentation/MeshSpec Addendum.md`
    - `logs/` directory with historical test logs and RTT sessions.

## Verified tracking source

- `Documentation/Firmware Implementation Tasklist.md` line 57 onward is the authoritative
  task tracker for MeshSpec.
- Current MeshSpec tracker line statuses relevant to work completion:
  - Completed: lines 61-73
  - Partial: line 74 (collection result source persistence/retry schedule)
  - Partial: line 75 (result offer/grant large-result flow)
  - Partial: line 77 (app-integrated failure-mode tests)

## Confirmed behavior implemented (already in code/tests)

- `mesh_relay` has in-memory `persistent_outbox_record` and collection EACK/retry logic.
- Route reply ACK/retry reverse-path behavior and backup metadata exists.
- Bounded flood controls, duplicate suppression, flood identity handling, and no recursive child
  route discovery for the same request.
- Gateway route advertisement, command flood scopes, collection EACK +
  missing-list support, and gateway app missing-list EACK selection when an
  explicit count-matched command roster is present.
- Relay capacity states and busy responses with `retry_after`.
- C5 contact-state model and channel-9 finite event state semantics.

## Explicit remaining gaps

1. Source/retry persistence now has relay-level snapshot/restore APIs plus
   anchor-role Zephyr NVS save/restore for active relay outbox snapshots and
   pre-relay scheduled command results. Outbox snapshots carry export uptime
   and preserve the remaining retry-backoff delay for restored collection
   retry-round state. Active command-result expiry stops retrying and marks the
   delivery state expired. A `native_sim/native/64` Zephyr test now covers
   scheduled collection-result snapshot save/restore/clear and invalid-save
   rejection against real NVS. `mesh_preemption` native coverage now covers the
   accepted-click preemption decision used by `mesh_preempt_for_click_event()`,
   and `firmware/app/tests/mesh_preemption` covers the app-used Zephyr
   preemption side-effect helper for queue purge/requeue plus delayable timeout
   schedule/cancel.
2. Parent-side result-offer reservations, queued child result bundles,
   in-flight `MSG_RESULT_BUNDLE` outbox state, and forwarded non-bundled child
   `MSG_COMMAND_RESULT` offer/payload custody-retry state now have relay-level
   snapshot/restore coverage. Anchor-role Zephyr NVS save/restore uses the
   existing outbox persistence path for active relay outbox snapshots. The
   remaining gap is broader app-integrated recovery coverage across every radio
   handoff and preemption path.
3. Gateway app collection EACKs now select explicit missing-list format when an
   active all-registered command supplied a count-matched `TLV_EXPECTED_NODE_ID`
   roster. Without that roster, the app still sends explicit received-list EACKs
   because no persistent membership table is wired into collection state. It
   also falls back to received-list if the explicit missing-list payload does
   not fit.
4. App-level failure-mode coverage still needs broader integration tests for
   full radio handoff/retry behavior across real-time preemption points.

## Latest verified change

- `cdd1863`: `struct mesh_relay_child_custody_snapshot`
  exports/restores parent-side result-offer reservation state and queued child
  result bundle state. Restore validates role/local/gateway identity, gateway
  epoch, result identity, collection epoch, payload length, and payload CRC.
  Bundle restore folds pre-reset queued time into `message_age_ms` and resumes
  the remaining hold delay on current boot uptime.
- Anchor-role NVS now stores child-custody snapshots under a separate record,
  restores after `mesh_relay_init()`, saves before result grants and custody
  hop ACKs, and clears/updates after outbound handoff. If anchor NVS child
  custody save fails, the app skips the result grant or hop ACK for that
  exchange.
- `557febd`: outbox snapshot validation/export now accepts
  in-flight gateway-bound `MSG_RESULT_BUNDLE` pending TX state, validating the
  bundle header, gateway epoch, record count, and bundle CRC. Native tests cover
  restore after bundle forward handoff and corrupt bundle-payload rejection.
- `6feaf4c`: outbox snapshot validation/export
  now accepts forwarded non-bundled child `MSG_COMMAND_RESULT` pending TX state
  when the payload has a valid command-result identity for the child source.
  Restore no longer requires the pending packet source to equal the local relay
  ID. Native tests cover restore before `RESULT_GRANT` as a pending
  `MSG_RESULT_OFFER` retry and restore after `RESULT_GRANT` as a pending child
  `MSG_COMMAND_RESULT` retry.
- `69d7547`: `mesh_relay_outbox_snapshot`
  version is bumped and now carries `snapshot_at_ms`; restore preserves the
  remaining retry-backoff delay when the saved pending TX was already waiting
  for a collection retry round. Native coverage:
  `test_collection_outbox_snapshot_preserves_retry_round_delay`.
- `4cea71d`: `mesh_prepare_click_preemption()`
  extracts the app-used accepted-click preemption decision into a
  platform-independent helper. `mesh_preempt_for_click_event()` now applies the
  helper's plan for NVS save/clear, delayable timeout schedule/cancel, RX queue
  purge, and click-report requeue. Native coverage:
  `test_mesh_preemption` verifies collection-result deferral, non-collection TX
  cancel, and local click-report requeue. Zephyr runtime side effects are still
  build-proven rather than runtime-tested.
- Current `Add gateway collection roster EACK` commit: `TLV_EXPECTED_NODE_ID`
  command roster parsing,
  gateway app roster storage for active collection, explicit missing-list EACK
  selection when the roster is count-matched, received-list fallback when no
  roster is supplied, and relay-side confirmation when an explicit missing-list
  EACK omits the local pending result node.
- Latest expiry slice: active pending `MSG_COMMAND_RESULT` outbox records
  with expired `TLV_COMMAND_EXPIRY_S` stop retrying and mark
  `MESH_RELAY_DELIVERY_EXPIRED`; native coverage:
  `test_collection_result_expires_without_retrying_forever`.
- Latest mesh persistence test slice: `firmware/app/tests/mesh_persistence` adds a
  Zephyr `native_sim/native/64` NVS test for scheduled collection-result
  snapshot round-trip, clear, and invalid-save rejection.
- Latest app preemption test slice: `firmware/app/src/app_mesh_preemption.*`
  applies the app-used Zephyr side effects for a precomputed click-preemption
  plan, and `firmware/app/tests/mesh_preemption` verifies queue purge/requeue,
  delayable timeout schedule/cancel, and save/clear callback dispatch using
  real Zephyr objects under `native_sim/native/64`.
- Local collection-result gateway ACK/EACK timeout now schedules a collection retry round with
  deterministic jitter instead of immediately counting the missing EACK as a route failure.
- Relay outbox snapshot/restore now preserves local collection command results with payload
  CRC/identity validation after `mesh_relay_init()` and restores them into retry-backoff state
  instead of stale pre-reboot radio wait deadlines.
- Anchor-role app/NVS integration now saves active relay outbox snapshots on
  TX/defer/retry/busy/progress transitions, clears them on gateway confirmation,
  collection close, or true cancel, and restores valid snapshots after
  `mesh_relay_init()`.
- Anchor-role app/NVS integration also saves scheduled command results before
  relay handoff, restores them with the original collection result identity, and
  clears the scheduled record when an active relay outbox snapshot supersedes it.
- Native regression coverage: `test_collection_result_timeout_uses_collection_retry_round`,
  outbox snapshot/restore tests, child-custody bundle/reservation snapshot
  tests, no-state export coverage, corrupt bundle snapshot rejection, and
  updated route-loss preservation expectations in `test_mesh_relay`.
- Verification run after the pending forwarded-child slice:
  `cmake --build firmware/build` passed and
  `ctest --test-dir firmware/build --output-on-failure` passed 13/13.
- Zephyr role builds also passed after the change:
  - `.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker`
  - `.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor`
  - `.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway`
- Verification run after the pending retry-delay slice:
  `cmake --build firmware/build` passed,
  `ctest --test-dir firmware/build --output-on-failure` passed 13/13, and
  Zephyr clicker/anchor/gateway role builds passed.
- Verification run after the accepted-click preemption decision slice:
  `cmake --build firmware/build` passed,
  `ctest --test-dir firmware/build --output-on-failure` passed 14/14, and
  Zephyr clicker/anchor/gateway role builds passed.

## Documentation rule after user correction

`Documentation/Mesh Protocol Detailed Flow.md` is an implementation snapshot.
It should answer "what does the code do right now?" If a MeshSpec feature is
only partially implemented or unimplemented, document it as a gap. Do not write
forward-looking behavior as if it exists.

## Next actions

1. Add/finalize broader app-integrated failure-mode test cases around full
   radio handoff/retry behavior during real-time preemption.
2. Keep `Documentation/Mesh Protocol Detailed Flow.md` aligned with what is
   actually implemented right now, including partial and missing behavior.
