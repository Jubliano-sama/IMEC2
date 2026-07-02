# MeshSpec Worktree State (Context Checkpoint)

Timestamp: 2026-07-02 (UTC-07 from workspace date context; local CET zone is Europe/Amsterdam)
Location: `/home/tommie/Projects/IMEC2`

## Active objective

Implement `Documentation/MeshSpec.md` as closely as possible without modifying the
spec itself, using separable subagents for implementation work, and finish with a
single full protocol narrative markdown artifact.

## Goal state

Current `get_goal()`:
- Thread: `019f140e-48d6-70f3-8ad1-2381b8c06b67`
- Objective: implement MeshSpec + final protocol markdown
- Status: active

## Repo snapshot

- Branch: `master`
- HEAD: `b0d4c5b`
- `git status --short`:
  - No tracked modifications in code or headers.
  - Untracked files only:
    - `Documentation/MeshSpec Addendum.md`
    - `Documentation/Mesh Protocol Detailed Flow.md` (current implementation snapshot,
      not an idealized spec restatement)
    - `Documentation/MeshSpec_Worktree_State.md` (this file)
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
- Gateway route advertisement, command flood scopes, collection EACK + missing-list support.
- Relay capacity states and busy responses with `retry_after`.
- C5 contact-state model and channel-9 finite event state semantics.

## Explicit remaining gaps

1. Source/retry persistence after reboot is not restart-tolerant (no snapshot/restore path).
2. Large-result durability across restart and some multi-hop custody corner cases are still
   incomplete versus full persistence target.
3. Gateway app collection EACKs still send explicit received-list format because no app-side
   expected-node roster table is currently wired into collection state.
4. App-level failure-mode coverage still needs one or more focused integration tests for
   real-time preemption/retry behavior.

## Latest verified change

- Local collection-result gateway ACK/EACK timeout now schedules a collection retry round with
  deterministic jitter instead of immediately counting the missing EACK as a route failure.
- Native regression coverage: `test_collection_result_timeout_uses_collection_retry_round`
  plus updated route-loss preservation expectations in `test_mesh_relay`.
- Verification run: `cmake --build firmware/build && ctest --test-dir firmware/build --output-on-failure`
  passed 13/13.
- Zephyr role builds also passed after the change:
  - `.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-clicker -- -DFIRMWARE_ROLE=clicker`
  - `.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-anchor -- -DFIRMWARE_ROLE=anchor`
  - `.venv/bin/west build --no-sysbuild -s firmware/app -b nrf52833dk/nrf52833 --build-dir build/firmware-gateway -- -DFIRMWARE_ROLE=gateway`

## Next actions

1. Add explicit restart-safe relay outbox snapshot/restore APIs plus tests.
2. Add/finalize app-integrated failure-mode test cases.
3. Keep `Documentation/Mesh Protocol Detailed Flow.md` aligned with what is actually implemented
   right now, including partial and missing behavior. Do not turn it into an idealized restatement
   of `Documentation/MeshSpec.md`.
