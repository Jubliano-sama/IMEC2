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
- HEAD: current MeshSpec implementation commits through the strict
  all-registered collection roster test slice
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
  missing-list support, strict `CMD_SCOPE_ALL_REGISTERED` collection rejection
  without a full explicit roster, and gateway app missing-list EACK selection
  when a count-matched command roster is present.
- Relay capacity states and busy responses with `retry_after`.
- C5 contact-state model and channel-9 finite event state semantics.

## Explicit remaining gaps

1. Source/retry persistence now has relay-level snapshot/restore APIs plus
   anchor-role Zephyr NVS save/restore for active relay outbox snapshots and
   pre-relay scheduled command results. Outbox snapshots carry export uptime
   and preserve the remaining retry-backoff delay for restored collection
   retry-round state. Active command-result expiry stops retrying and marks the
   delivery state expired. `native_sim/native/64` Zephyr tests now cover
   scheduled collection-result snapshot save/restore/clear, invalid-save
   rejection, and active collection-result outbox restore while already waiting
   in retry-backoff against real NVS. `mesh_preemption` native coverage now
   covers the accepted-click preemption decision used by
   `mesh_preempt_for_click_event()`, and `firmware/app/tests/mesh_preemption`
   covers the app-used Zephyr preemption side-effect helper for queue
   purge/requeue plus delayable timeout schedule/cancel. `firmware/app/tests/mesh_persistence`
   also covers accepted-click preemption of an active collection-result outbox
   through the app helper, real NVS save/restore, preserved retry-backoff,
   same-payload retransmit, and no route failure/hold-down from the preemption.
2. Parent-side result-offer reservations, queued child result bundles,
   in-flight `MSG_RESULT_BUNDLE` outbox state, and forwarded non-bundled child
   `MSG_COMMAND_RESULT` offer/payload custody-retry state now have relay-level
   snapshot/restore coverage. Anchor-role Zephyr NVS save/restore uses the
   existing outbox persistence path for active relay outbox snapshots, and
   `firmware/app/tests/mesh_persistence` verifies parent-side result-offer
   reservation save/restore/clear and queued child result bundle
   save/restore/flush through the app NVS child-custody path, parent reservation
   restore after `RESULT_GRANT` accepted-C5 send failure and identical retried
   offer, plus after-grant forwarded child payload restore through the app NVS
   outbox path.
   `firmware/app/tests/mesh_result_handoff` verifies the app bridge that saves
   child custody before result grants, suppresses grants on save failure, reports
   grant-send failure without marking TX sent, notes forwarded bundles before
   saving custody, and updates custody after forwarded child-result handoff. The
   remaining gap is broader app-integrated recovery coverage across every radio
   handoff and preemption path.
3. Gateway app collection EACKs now select explicit missing-list format when the
   active collection state has a count-matched `TLV_EXPECTED_NODE_ID` roster.
   `CMD_SCOPE_ALL_REGISTERED` collection commands without that full roster are
   rejected instead of being downgraded to best-effort membership. The gateway
   still has no persistent membership table; `CMD_SCOPE_ALL_HEARD` remains the
   best-effort collection mode and received-list fallback is still used when a
   missing-list payload does not fit.
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
  selection when the roster is count-matched, and relay-side confirmation when
  an explicit missing-list EACK omits the local pending result node. The newer
  strict collection slice rejects `CMD_SCOPE_ALL_REGISTERED` collection commands
  when that full roster is not supplied.
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
- Latest child-custody persistence test slice: `firmware/app/tests/mesh_persistence`
  verifies parent-side `RESULT_OFFER` reservation save/restore/clear and queued
  child `MSG_COMMAND_RESULT` bundle save/restore/flush through the app NVS
  child-custody path under `native_sim/native/64`.
- Latest result-grant failure persistence test slice:
  `firmware/app/tests/mesh_persistence` verifies that if a parent saves a child
  `RESULT_OFFER` reservation but the `RESULT_GRANT` accepted-C5 send returns
  `-ENOTCONN`, the grant is not marked TX-sent, the reservation restores from
  NVS after `mesh_relay_init()`, and an identical retried offer receives another
  `RESULT_GRANT` instead of `RESULT_BUSY` or drop.
- Latest result-handoff app test slice: `firmware/app/src/app_mesh_result_handoff.*`
  applies the app bridge around child-custody persistence and result-grant
  sends, and `firmware/app/tests/mesh_result_handoff` verifies save-before-grant,
  grant suppression on save failure, send-failure reporting without TX-sent
  notation, child-custody update after forwarded child-result handoff,
  bundle-forward notation before custody save, and save-failure reporting after
  forwarded handoff. The app dispatcher now also uses the helper to gate
  hop/custody ACKs on child-custody save, and the same test target verifies ACK
  allowed after save success/forward success and suppressed after save failure
  under `native_sim/native/64`.
- Latest channel-9 ACK matching slice: `firmware/app/src/app_mesh_ch9_ack.*`
  factors route-test channel-9 batch ACK matching and partial-ACK requeue
  behavior into caller-owned helpers.
  `firmware/app/tests/mesh_ch9_ack_handoff` verifies partial ACKs mark only the
  matching pending entry, complete ACKs use session lists to disambiguate equal
  sequence numbers, legacy requested-sequence ACKs still require matching ACK
  packet session, malformed ACK lists are rejected, partial recovery requeues
  only unACKed packets ahead of pre-existing queued work, and a full queue
  reports the retry drop path. Full radio/runtime integration coverage around
  route-test partial ACK recovery remains a gap.
- Latest active collection retry persistence test slice:
  `firmware/app/tests/mesh_persistence` verifies a real active
  `MSG_COMMAND_RESULT` outbox already waiting in collection retry-backoff can be
  saved to Zephyr NVS, restored after `mesh_relay_init()`, keep retry round 1,
  preserve the remaining retry delay, avoid retransmit before the restored
  deadline, and retransmit the same payload to the gateway at the deadline.
- Latest active collection click-preemption persistence test slice:
  `firmware/app/tests/mesh_persistence` verifies accepted-click preemption of a
  live `MSG_COMMAND_RESULT` collection outbox through
  `mesh_prepare_click_preemption()` and `app_mesh_apply_click_preempt_plan()`;
  the path purges RX, saves the outbox to Zephyr NVS, schedules the retry
  timeout, restores after `mesh_relay_init()`, retransmits the same payload only
  after the restored retry deadline, and leaves the selected route without
  failure count or hold-down.
- Latest forwarded child payload persistence test slice:
  `firmware/app/tests/mesh_persistence` verifies a forwarded child
  `MSG_COMMAND_RESULT` that has received `RESULT_GRANT` can be saved through the
  app outbox NVS path, restored after `mesh_relay_init()`, remain gateway-ACK
  tracked, resume in retry-backoff instead of a stale radio wait, and
  retransmit the original child payload to the gateway rather than re-sending
  `MSG_RESULT_OFFER`.
- Latest strict all-registered collection slice:
  `gateway_command_extract_options()` rejects collection-required
  `CMD_SCOPE_ALL_REGISTERED` commands unless `TLV_EXPECTED_NODE_ID` entries
  exactly match `TLV_EXPECTED_NODE_COUNT`. `CMD_SCOPE_ALL_HEARD` is the
  best-effort collection mode without a strict roster. The gateway app keeps
  the command-supplied explicit roster only for the active collection and uses
  it for missing-list EACKs when the payload fits.
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
- Verification run after the strict all-registered collection slice:
  `cmake --build firmware/build` passed and
  `ctest --test-dir firmware/build --output-on-failure` passed 14/14.
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
