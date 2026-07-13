# AGENT_KNOWN_ISSUES.md

This is a **historical record** of tool issues, environment gotchas, bugs encountered, and lessons learned while working in this codebase.

Entries do **not** need to be contemporary. Many issues listed here may already have been fixed. The value of the file is for agents (and humans) to study past mistakes and avoid repeating similar patterns in the future.

**Recommendation**: Read (or at least skim) this file before starting significant new work. It is a record of past problems so that similar patterns can be recognized and avoided.

---

## Annoying Tool / Environment Issues

- PIL `ImageGrab` can create Tk windows on `DISPLAY=:1` but fail with `X get_image failed: error 8`; use a Wayland/portal screenshot path instead of treating the GUI as non-rendering.
- `capture_stack_evidence.py` can reject an otherwise successful exact mesh-gateway build before RTT because static RAM headroom and compiler-owner attribution are deployment-policy gates; preserve the rejection and do not bypass it with direct flashing.
- Context-mode JavaScript execution can fail before running commands with Bun `Expected CommonJS module to have a function wrapper`; rerun verification directly and do not treat the wrapper failure as test evidence.

- The Grok delegate rejected the requested `sol` model ID as unknown; continue locally or select a model returned by the installed backend rather than treating the failed launch as review evidence.
- A focused provisioning script that imported an in-progress GUI decoder broke when another agent removed that symbol concurrently; keep hardware probes independent of files owned by concurrent agents.

- Fixed gateway survey discovery missing a reachable anchor by extending the control follow-up RX floor through the protocol wake-train bound plus TX-transition guard; root cause was the anchor window closing before the first post-train flood frame on hardware.
- Fixed the gateway lifecycle invariant target link failure by adding its ingress implementation dependency; root cause was lifecycle identity validation moving to the shared ingress helper without updating that target.

- Large monolithic source files (e.g. `app_mesh_report.c` at 14k+ lines, `app_anchor.c` at 6k+, `mesh_relay.c` at 7k+) became extremely difficult to navigate, understand, and modify safely. Past work suffered when files grew this large without being split.
- Using `git submodule deinit -f` on a path would clear the working tree contents (not just metadata), requiring an explicit `git submodule update --init` recovery afterward.
- Running `rm -rf` on gitignored build trees could surface historical "D" entries and "M" noise in `git status --porcelain` even when no real source change was intended.
- After destructive or move operations, `git status` and directory listings often needed to be re-run immediately because state drift was common.
- The `list_dir` tool respected .gitignore and did not show dot-directories, leading to incomplete pictures of the tree.
- Submodule trees with internal `.git` directories triggered "absorbgitdirs" warnings and could leave inconsistent checkout state.
- Long tool outputs were frequently truncated, requiring follow-up with `head`, `tail`, `grep -A/-B`, or `read_file` with limits.
- `search_replace` would fail if the `old_string` was not unique; multi-occurrence cases required `replace_all` or more context.
- Background `run_terminal_command` tasks required explicit tracking of `task_id` for later `get_command_or_subagent_output` or kill.
- User mid-operation corrections ("don't remove X") required immediate state re-inspection because prior tool calls may have already mutated things irreversibly.
- Build directories (`firmware/build/`, top-level `build/`) frequently re-appeared or contained stale artifacts despite being listed in `.gitignore`.

## Bugs Fixed & Root Causes (one line per entry)

- Fixed the click-collision complete-airtime boundary mutation by including link propagation in the receiver-side RX close; root cause was comparing sender TX end directly with a receiver RX window.
- Fixed gateway anchor-survey rejection before its first radio attempt by applying the required channel-5 broadcast envelope; root cause was leaving `mesh_outbound.radio_channel` zero before `mesh_send_c5_flood()` validation.
- Fixed gateway GUI command-status labels by matching the firmware enum values; root cause was stale mappings for busy, denied, radio error, and invalid state.
- Fixed successful enumeration baseline rejection by measuring telemetry-loss deltas per correlated run; root cause was treating the gateway's cumulative lifetime loss counter as run-local.

- Fixed permanently busy anchor enumeration by enforcing an absolute operation deadline before retrying priority-work handoff; root cause was rescheduling a failed safe-boundary submission forever while retaining active command ownership.
- Fixed control-followup wake session rejection by aligning the UWB session and frame flag validators; root cause was duplicating packet-flag validation across two modules without a shared acceptance test.
- Fixed blank-anchor provisioning wake ownership by marking gateway-command wake claims as control follow-ups; root cause was routing every non-click wake claim into the route-reply listener, which discarded assignment commands as unrelated frames.

- Fixed "SDK directory cleared" by running `git submodule update --init -- "dwm3000 examples and sdk"` after deinit; root cause was `deinit -f` aggressively removing the worktree.
- Fixed apparent loss of `app_anchor_low_power_policy.h` by `git checkout HEAD -- path`; root cause was pre-existing tracked deletion status from uncommitted prior refactor surfacing after fs ops.
- Fixed incomplete tree view during cleanup by always cross-checking with `ls`, `find`, and explicit `git ls-files` instead of relying only on `list_dir`.
- Fixed proceeding with moves after user said "don't remove" by immediately restoring submodule and re-listing; root cause was acting on prior authorization without re-confirmation.
- Fixed build dir pollution by adding explicit `rm -rf` + re-`ls` before any west/cmake commands; root cause was assuming gitignore + "clean tree" snapshot meant zero on-disk build state.
- Fixed wrong path updates during archive moves by grepping for hardcoded references first (CMakeLists, AGENTS.md); root cause was mechanical mv without searching for string references.
- Large files like `app_mesh_report.c` (14k lines) mix too many concerns and slow down all work; root cause of many past difficulties was lack of splitting before the files became unwieldy.
- Fixed gateway command work running before a cooperative RX boundary by making admission wait for explicit RX-abort observation; failed scheduling now cancels admitted command identities before error reporting.

---

## Architectural & Test Gaps (from past audits)

These are one-line distillations of findings from the 2026-07-11 Simulator Adversarial Review and the Mesh Deployment Hardening Audit. They are retained here as historical reference. Some of these gaps may have been addressed since the audits were written; the purpose is to understand the kinds of problems that have arisen before.

- Simulator Channel 9 paths did not enforce full DWM3000 runtime prepare/write/start timing inside the slot (F1).
- Connection repair/establishment used direct state updates instead of real over-the-air propose/accept exchanges that could be collided or preempted (F2).
- No-route originated packets and route-wait resume were not handled fully end-to-end in the simulator (F3).
- Link quality only affected route cost metrics, not actual frame decode success rates (F4).
- Gateway BLE and UWB timelines were completely separate; backpressure on one could not affect the other (F5).
- Missed connection events still fed the simulated watchdog (F6).
- Many production priority and coordinator decisions lived outside the native mesh_runtime used by the simulator (C1).
- Route recovery tests often relied on fixture-installed routes instead of simulating real parent loss and recovery (C3).
- Persistence, retained sleep, and ACK retry had documented false-success / silent-loss paths (noted in Hardening audit P0/P1).
- Several subsystems lacked autonomous progress heartbeats or bounded recovery at the time of the audits, creating risk of permanent stalls.
- Simulator and stack models were not tied to exact app build presets and full workqueue paths.
- Simulator watchdog expiry used to leave started RX/TX completion events live; a reset must invalidate role-owned scheduled work so stale completions cannot feed a lease, deliver data, or fail event ordering.
- A green `ctest` run can execute stale binaries when test sources are newer than the build tree; adversarial evidence must configure and build a fresh isolated directory before running CTest.
- Audit references written as `tmp/...` may refer to host `/tmp/...`; check both repository-relative and absolute paths before declaring an external review artifact absent.

## How to Use This File

- Before starting any significant work, read the relevant sections of this file to learn from past issues.
- When you encounter a new annoying tool behavior or have to work around a problem, append a one-line entry describing it.
- A resumed subagent can fail immediately with quota exhaustion; reassign the bounded work or continue it locally, and do not treat the failure as code or test evidence.
- Pure native-testable app helpers must include their own standard definitions such as `<stddef.h>` for `NULL`; do not rely on transitive Zephyr headers that the native target does not include.
- After connection repair became an over-the-air propose/accept exchange, tests that count all TX starts can mistake valid control repair traffic for duplicate payload delivery; assert data and control message classes separately.
- When you fix a bug (whether in code or process), add a short "Bugs Fixed" entry explaining what went wrong and how it was resolved. Use the format: "Fixed X by doing Y; root cause was Z."
- Prefer one line per item. Date or short context in brackets is optional but helpful.
- This file is append-only for lessons learned. Do not rewrite or remove historical entries.

When working in areas that have had past problems (e.g. mesh, routing, simulation, persistence, large files), review the historical entries to avoid repeating the same classes of mistakes.

Reviewing the history in this file helps agents avoid repeating problems that have been encountered before.

- Fixed an ASan/UBSan failure in the gateway-command priority retry test by sizing its callback trace for two complete safe-boundary handoffs; root cause was a fixture capacity left at the original one-handoff bound.
- Fixed a strict native build failure after making click-preemption planning non-destructive by explicitly marking the legacy planner timestamp unused; root cause was the removed in-planner relay deferral no longer consuming it.
- Compiler IPA graph dumps use object-relative output paths; run any manual compile probe from its build directory or it will fail to create the paired `.su` output.
- Fixed stack transcript identity reuse acceptance by retaining completed run IDs and requiring active previous-click linkage; root cause was validating only the active-run map.
- Fixed a gateway-only build break after shared BLE ingress routing by compiling the exact mesh gateway preset; root cause was a stale log reference to a removed local decoded item that native seams do not compile.
- A TTY-wrapped `timeout ... script ... pyocd rtt` session can outlive its requested bound; retain the exec session ID and send an explicit interrupt before treating the capture window as closed.
- Diagnosed a false forced-hop firmware regression by comparing target vector words with exact build binaries; root cause was the mapped anchor probe carrying a different artifact while the transmitter artifact matched exactly.
- Fixed a deferred route-request one-millisecond livelock with an expiry-aware due-time helper; root cause was treating `uptime_ms_until_deadline()`'s intentional expired value of one as future work.
- Fixed Channel-5 proposal/ACCEPT ownership by processing queued proposals immediately after releasing route-reply RX ownership and before closing contact; root cause was either closing contact before work ran or attempting nested UWB TX inside the RX owner.
- Fixed repeated ACCEPT realignment aborts by reserving the measured 16-17 ms completion skew with a tested 20 ms bound; root cause was a 15 ms nominal-offset assumption that omitted hardware completion overhead.
- Context-mode shell injection can make a command beginning directly with `for` syntactically invalid (`NODE_OPTIONS=... for`); prefix loop commands with another shell statement.
- Context-mode shell injection can make a command beginning directly with `if` syntactically invalid; prefix shell conditionals with `true;` (similar to the existing `for` issue).
- A repeated TTY-backed pyOCD `pre-reset` RTT capture can find the RTT control block but emit no target boot output; preserve the empty capture and use an explicit reset followed by RTT attach to distinguish capture failure from a dead target.
- Fixed intermittent mesh-gateway illegal-EPSR resets by replacing a full `struct mesh_relay_result` retransmit-repair local with a scalar action/status API; root cause was an 8904-byte dormant branch frame overflowing the 8192-byte `mesh_route` workqueue stack on every handled RX packet and corrupting the adjacent retained-fatal block.
- Fixed an observability edit landing `duplicate_report` in an earlier unrelated `entry_count` block by reapplying it with function-scoped context; root cause was a broad patch anchor inside the 7k-line `app_anchor.c`.
- A bounded read-only Grok review can consume both four and eight allowed turns without returning findings; treat `max turns reached` as no review evidence and continue the review locally.
- Fixed the mesh-anchor preset test loader on Python 3.12 by registering the dynamically loaded verifier in `sys.modules` before execution; root cause was `dataclasses` resolving annotations through a module entry that the ad hoc loader had not installed.
- The installed pyOCD rejects `pyocd list --json`, while `capture_stack_evidence.py` currently requires that option; use plain `pyocd list` for bench probe verification and treat trusted qualification capture as unavailable until enumeration has a compatible fallback.
- Fixed qualification probe enumeration on older pyOCD by falling back from rejected `pyocd list --json` to exact-token parsing of plain `pyocd list`; root cause was assuming the installed CLI exposed the newer JSON option.
- `pyocd commander` can print a command-level error such as an unaligned `read32` length yet exit with status zero; validate the requested output itself instead of trusting only the process status.
- Fixed LED-only unprovisioned click handling by retaining deterministic fallback discovery replies; root cause was provisioning state suppressing local range ownership after a valid wake claim.
- Simultaneous pyOCD RTT attachment can make the loaded gateway BLE service disconnect during GATT discovery; preserve both traces and retry provisioning without RTT instead of treating the anchor as unreachable.
- A loaded gateway with stale BLE stream backpressure can accept a connection but return ATT 0x0e on identity reads after recovery; do not claim provisioning or survey success without complete command lifecycle records.
- Fixed multi-anchor winner-takes-all discovery by replacing 1 ms reply slots with full-airtime 12 ms slots; root cause was adjacent standard-wake discovery replies overlapping on air.
- `git commit --only <paths>` reads the selected paths from the working tree rather than preserving their staged-only hunks; isolate mixed staged/unstaged files before using pathspec commits, then verify the committed stat immediately.
- Fixed survey discovery terminating with `No anchors` while enumeration reached the same devices by admitting `MSG_SURVEY_DISCOVERY_START` in the gateway-command control-followup RX policy; root cause was a message-type allowlist that accepted broadcast `MSG_COMMAND` but rejected the survey flood using the same contact purpose.
- Fixed deterministic survey probe collisions across retries by deriving four bounded exponential-jitter opportunities from anchor, survey, and attempt identity; root cause was a single stable hash slot with no attempt-diversified retry horizon.
- Fixed the Zephyr survey physical-slot validator link by compiling the shared DWM3000 timing model into the app; root cause was native `core` owning `dwm3000_timing.c` while the firmware app source list omitted it.
- Fixed terminal survey discovery-report loss under transient queue pressure by retaining one exact encoded report until queue custody or a bounded deadline; root cause was treating `queue_anchor_report()` admission failure as discovery completion.
- Fixed survey discovery start becoming physically undecodable by classifying `MSG_SURVEY_DISCOVERY_START` as extended-PHR channel-5 control traffic; root cause was transmitting a short `0x54` with standard PHR while the anchor control-follow-up receiver used extended PHR.
- Fixed blocked survey slots silently consuming probe opportunities by separating chronological nominal and reserve horizons and requiring four real send attempts; root cause was advancing the opportunity index whenever a nominal window passed even if no transmission was attempted.
- Concurrent TTY-backed RTT commands launched through one shell can create only one requested transcript despite both subprocesses producing merged terminal output; verify every expected transcript path and recapture missing probes separately.
- The mesh simulator's queued payload/custody path currently requires a scheduled connection even for a direct gateway next hop, while its unscheduled direct-probe radio path does not drive queued payload custody; do not claim unscheduled direct survey-report contention from either seam alone.
- Fixed unscheduled direct-gateway survey custody modeling by composing queued relay custody with exact-runtime raw Channel 9 RX/TX and exact ACK dispatch; root cause was representing every queued direct payload through a persistent gateway connection that production does not own.
- Fixed the 20-anchor direct survey model's retry phase ownership by reserving survey policy for route probing and driving 0x55 loss through mesh-relay ACK timeout/backoff; root cause was reusing discovery retry timing after a direct route was already installed.
- Fixed correlated direct-gateway survey retries and an undersized collection tail by adding four anchor/survey-diversified real attempts and budgeting probe plus full gateway-ACK recovery; root cause was reusing three narrow 30-50 ms route-probe retries while treating a 5 second queue-admission timeout as enough time for final delivery.
- Fixed an immediate mesh-anchor hardware stack-check reset by restoring the survey journal directly into its existing singleton; a 1 KiB startup-local snapshot combined with the nested 2.3 KiB outbox restore and overflowed the 4 KiB main stack despite static RAM and native tests passing.
- Fixed an apparently wide 250/500/1000 ms direct-survey retry model by including the gateway's 20 ms RX re-arm service interval and widening to 2/4/8 seconds; root cause was counting frame airtime alone, which made only 22 of 256 all-20 seeded cases complete.
- Kept simulator Here-I-Am forwarding fail-closed until it models the four-opportunity quiet-checked flood state machine; directly scheduling one `MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV` frame would bypass production click and RF-busy deferral.
- Fixed deterministic survey retry aliases for IDs with equal low32 XOR high32 by mixing the complete 64-bit anchor identity with survey and attempt identity; root cause was folding the ID to 32 bits before hashing.
- Fixed survey retry timing and attempt overclaims by bounding scratch acquisition, separating busy exhaustion from attempted-send exhaustion, and naming the calculated interval a policy horizon; root cause was treating an unbounded mutex wait and eight all-busy deferrals as four completed radio attempts.
- Fixed discovery-assignment coverage being concentrated in one 50-anchor happy-path fixture by adding 2/6/16/32/50 adversarial claim-table-ACK matrices; root cause was not sweeping lost, duplicate, stale, persistence, reset, priority-deferral, and capacity combinations together.
- Native test include order can mask a public header embedding an incomplete type; include the defining header directly and checkpoint an exact Zephyr role build before trusting host compilation.
- Fixed anchor-only survey journal storage consuming 1024 bytes in gateway and clicker builds by compile-time role-gating the singleton; runtime role checks do not remove shared static RAM from non-anchor link maps.
- Fixed the 5.3-second mesh-anchor reboot loop after restored survey delivery by running survey radio work on the existing 12 KiB anchor UWB owner queue; root cause was a hardware-measured 4080-byte journal/direct-ACK/SPI chain exhausting its separate 4096-byte queue while static frame checks attributed that callback to the wrong workqueue.
- Fixed durable survey attempt accounting racing synchronous direct ACK and reset by consuming a persisted token before RF and token-guarding sent, refund, release, and ACK transitions; post-send accounting alone can grant free attempts or report false failure after ACK clear.
- Fixed journal-owned survey reports overwriting the generic single route-wait packet by giving durable-local transport a no-store route-discovery path; route discovery may run while the exact journal remains the sole retry owner.
- Fixed corrupt or old survey journal records stalling anchor startup by quarantining and clearing them with diagnostics while continuing safely even when the cleanup delete fails.
- This checkout's pyOCD does not support `pyocd list --json`, and `read32` takes a byte length aligned to four rather than a word count; use plain `pyocd list` and `read32 <address> 8` for two words.
- Running `.venv/bin/python -m tools.gateway_gui --help` launches the Tk GUI because that entry point does not parse `--help`; inspect source or use the dedicated headless scripts instead.
- A context-mode timeline search can remain pending for more than 90 seconds on a large session index; terminate that cell and use a bounded file-specific query instead of waiting indefinitely.
- `ctx_execute_file` refuses files under `/tmp` even with full host filesystem permission because it is confined to the project root; inspect short external captures with a bounded shell query or place the capture under the workspace.
- Fixed survey discovery losing its scheduled probe horizon by starting one measured-budget PHY preparation early and using same-PHY ensure after probe TX; root cause was a roughly 63 ms full reset at the absolute start and after every probe.
- Fixed deterministic enumeration stage loss by credit-pacing a compact retained assignment publication batch; root cause was three final slot events filling the depth-three BLE stream before stage 7 could be admitted.
- Fixed gateway survey pair commands losing routes after gateway reset by retaining accepted current-survey reverse hints for 50 anchors and reinstalling each target on demand; root cause was ACKing 0x55 local delivery without preserving its immediate reverse hop while the general downlink cache holds only 16 routes.
- Fixed later valid 0x55 packets mutating an anchor's accepted survey graph and reverse route by making report storage first-accepted-wins while still ACKing and counting duplicates; root cause was deduplication affecting observability only before the shared report API overwrote the slot.
- Interrupting the headless BLE client can leave BlueZ reporting the gateway as connected while the gateway no longer advertises; run `bluetoothctl disconnect <gateway-mac>` before treating the gateway as unavailable.
- Fixed three-sample surveys being denied by the gateway by making the four-sample connected-runtime capacity a shared cross-role contract; root cause was deriving the gateway limit from its unrelated one-entry local report queue.
- Fixed survey pair control repeatedly missing powered low-duty anchors by using the bounded priority channel-5 wake-and-flood executor; a reverse route hint identifies the next hop but does not make one unscheduled transmission reliably decodable.
- Fixed nondeterministic relay outbox ages by resetting `message_age_ms` in every fresh mesh and survey packet initializer; root cause was otherwise complete constructors inheriting an uninitialized stack field.
- Fixed cross-role stack attribution charging delayable-work and callback-table handlers to startup main by distinguishing compiler call edges from address references and stopping inherited ownership at reviewed thread roots; root cause was treating registration as synchronous execution.
- Fixed a role-dependent false unowned helper by giving the gateway-command-flow TLV parser a unique static symbol; root cause was linker-name-only evidence conflating three translation-unit-local `find_u16_tlv` functions.
