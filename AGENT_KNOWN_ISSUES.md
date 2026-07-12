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
- Context-mode shell injection can make a command beginning directly with `if` syntactically invalid; prefix shell conditionals with `true;` (similar to the existing `for` issue).

- A repeated TTY-backed pyOCD `pre-reset` RTT capture can find the RTT control block but emit no target boot output; preserve the empty capture and use an explicit reset followed by RTT attach to distinguish capture failure from a dead target.
- Fixed intermittent mesh-gateway illegal-EPSR resets by replacing a full `struct mesh_relay_result` retransmit-repair local with a scalar action/status API; root cause was an 8904-byte dormant branch frame overflowing the 8192-byte `mesh_route` workqueue stack on every handled RX packet and corrupting the adjacent retained-fatal block.
- Fixed an observability edit landing `duplicate_report` in an earlier unrelated `entry_count` block by reapplying it with function-scoped context; root cause was a broad patch anchor inside the 7k-line `app_anchor.c`.
- A bounded read-only Grok review can consume both four and eight allowed turns without returning findings; treat `max turns reached` as no review evidence and continue the review locally.
- Fixed the mesh-anchor preset test loader on Python 3.12 by registering the dynamically loaded verifier in `sys.modules` before execution; root cause was `dataclasses` resolving annotations through a module entry that the ad hoc loader had not installed.
- The installed pyOCD rejects `pyocd list --json`, while `capture_stack_evidence.py` currently requires that option; use plain `pyocd list` for bench probe verification and treat trusted qualification capture as unavailable until enumeration has a compatible fallback.
- Fixed qualification probe enumeration on older pyOCD by falling back from rejected `pyocd list --json` to exact-token parsing of plain `pyocd list`; root cause was assuming the installed CLI exposed the newer JSON option.
- `pyocd commander` can print a command-level error such as an unaligned `read32` length yet exit with status zero; validate the requested output itself instead of trusting only the process status.
