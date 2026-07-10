# Mesh Deployment Hardening Audit

Date: 2026-07-09

Implementation update: 2026-07-10

## Purpose

This document records a read-only audit of the mesh firmware for failure paths
that could stop useful progress, lose data, or hide errors during a multi-month,
maintenance-free deployment.

The audit covered mesh clicker, anchor, gateway, transmitter, and forced-hop
builds; route and ACK state; channel-5/channel-9 scheduling; DWM3000 SPI,
sleep, wake, and recovery; BLE delivery; persistence; workqueues; fatal-error
handling; observability; and build-time resource use.

The primary audit snapshot was commit `b3dbe33647e557889aeb663e41a485c7f92f3830`.
The highest-risk findings were rechecked against the concurrently changing
working tree after the snapshot and were still present at that time. This
document does not claim hardware reproduction of every path; it distinguishes
code-path findings from build and test evidence.

## Implementation update

The audit findings remain the acceptance baseline. The 2026-07-10 hardening
pass implemented the code changes below without reducing packet, queue, CIR, or
diagnostic capacity. Verification evidence is recorded separately so an
implemented path is not mistaken for a hardware-proven one.

| Finding | Current implementation | Remaining proof or deliberate residual |
|---|---|---|
| 1. Route epoch split | Gateway route clears and force-rediscovery preserve the gateway-owned epoch; non-gateway roles cannot mint a replacement epoch. | Multi-hop reset and delayed-command fault injection. |
| 2. Force-rediscovery result stall | Gateway host commands remain ordered and retry transient admission failures with bounded randomized exponential backoff. Route rediscovery also has a bounded terminal result. | Multi-hop command interruption soak. |
| 3. Fatal halt | Hardware watchdog, independent system/radio progress leases, retained fatal breadcrumb, reset-cause diagnostics, and bounded fatal-loop delay are enabled for mesh roles. | Inject thread, SPI, and fatal stalls on hardware and verify reset evidence. |
| 4. Believable SPI failure | The port latches the first SPI error, poisons failed read buffers, and the driver rejects status/config success, invalidates cached radio state, validates device identity after init/wake, and exposes forced recovery counters. | Synchronous controller hangs are recovered by watchdog reset; per-transfer asynchronous timeout and injectable SPIM faults remain future platform work. |
| 5. ACK retry loss | Unacknowledged packets retain ownership until requeue admission succeeds; a full queue cannot displace existing work or create a duplicate owner. Under a live downstream reservation, failed transit custody is visibly dropped instead of starting a blocking route search, and the originator remains responsible for end-to-end retry. | Concurrent RF queue-pressure hardware stress. |
| 6. BLE permanent stall | Stack enable, advertising, disconnect, notification admission, and stream successor failures enter bounded recovery with capped exponential backoff; repeated notify failure resets the link. Continuous gateway RX now refreshes the radio watchdog lease after every bounded driver iteration, so valid long-running mesh work cannot reset an otherwise healthy BLE connection. | Prolonged host reconnect and RF coexistence soak; repeated BLE-only controller failure remains visible and watchdog-independent. |
| 7. Unbounded route reply listener | Wake claims and route replies are clamped to the original cumulative deadline and cannot restart the receive budget. | Inject sustained valid and invalid claim traffic. |
| 8. Gateway receive monopolization | Long gateway channel-9 and disconnected channel-5 receives are split into at most 25 ms slices under immutable cumulative deadlines. Ordinary SFD/CRC errors clear and re-arm RX without a full radio reset, and every completed slice refreshes the radio progress lease. | Measure workqueue latency during sustained gateway traffic. |
| 9. False retained-wake success | Retained wake validates SPI state and device identity; repeated recoverable radio errors force a full reset/configuration scrub. | Repeated sleep/wake deaf-radio fault injection. |
| 10. Stranded report successor | Permanent head failure records packet identity and error, retires only that record, and schedules the next queued report immediately. | Two-record app-level hardware failure injection. |
| 11. Persistence false success | Mount/open and write failures retain dirty in-RAM state, retry with capped backoff, distinguish durable success, and expose total/consecutive failure health. Discovery assignment is persisted before acknowledgement. | Power-cut atomicity tests and a documented flash-endurance budget. |
| Channel-9 EACK fallback | A failed ACK on its selected negotiated channel-9 lane returns failure with the original packet retained; it does not silently switch neighbor or channel. | Alternate-route recovery is allowed only as a later explicit retry. |
| Invalid-channel fallback | Radio send entry points accept only exact channel 5 or channel 9 values. | Inject invalid-channel calls in an app-level test harness. |
| Scheduler failure masking | UWB RX scheduling/start errors propagate to their owner and are logged instead of becoming apparent inactivity. | Inject scheduler admission failures. |
| Production observability | Watchdog, persistence, radio recovery, delivery ownership, BLE queue age, and permanent packet failure have always-on compact health state. | A dedicated host health-report packet remains optional protocol work; local autonomous recovery does not depend on it. |
| Autonomous progress | Independent system and radio leases control watchdog feeding even when periodic network announcements are disabled. | Long-duration accelerated soak. |

Additional robustness added in the same pass includes acknowledged and persisted
normal-click slot assignment, bounded gateway-command retry queues, slot- and
hop-aware reply timing, deterministic survey planning capped at six peers per
anchor, and strict prepare-before-start survey state.

## Outcome

The original audited builds and automated tests passed, but green builds did not
establish field liveness. The paths identified below describe the audit snapshot;
the implementation table above records the current remediation state. Hardware
fault injection and soak gates remain necessary before claiming unattended
multi-month operation.

The critical work is:

1. Make route epoch ownership convergent and remove the force-rediscovery
   pending-result deadlock.
2. Add an independent hardware watchdog and persistent reset diagnostics.
3. Make SPI failures visible and recoverable instead of allowing undefined
   radio state to appear valid.
4. Preserve packet ownership until ACK retry admission is guaranteed.
5. Bound every listener, retry, recovery, and delivery state with an explicit
   progress or retirement action.

## Critical findings

### 1. Route-maintenance commands can split the route epoch

`mesh_relay_invalidate_routes()` increments the local epoch. Anchor maintenance
commands call it, while the gateway remains on its own boot epoch. Collection
results use the anchor's local epoch, and the gateway rejects offers whose epoch
does not match its collection state.

Consequently, `CMD_CLEAR_ROUTE` or `CMD_FORCE_REDISCOVERY` can make subsequent
results undeliverable until the affected anchor reboots. Repeating the command
can increase the divergence.

Relevant code:

- `firmware/src/mesh_relay.c`: `mesh_relay_invalidate_routes()` and offer epoch
  validation.
- `firmware/app/src/app_anchor.c`: maintenance command handling, result identity,
  and route initialization.
- `firmware/app/src/app_gateway_ble.c`: gateway collection epoch state.
- `Documentation/Mesh Connected Routing Contract.md`: gateway route epoch
  advertisement and stale/newer epoch handling.

Required behavior:

- Define one authoritative epoch owner. The contract currently implies the
  gateway advertises the current epoch.
- A local route-table clear must not independently create a network epoch.
- A node that misses a maintenance command must converge from later gateway
  traffic without rebooting.

Required tests:

- Complete a collection, issue clear-route, then complete another collection.
- Repeat with one anchor missing the maintenance command and rejoining later.
- Repeat force-rediscovery several times and prove all nodes converge.
- Exercise epoch rollover explicitly.

### 2. Force-rediscovery can leave a pending result permanently blocking work

A collection result can be stamped with the old epoch before force-rediscovery
increments the local epoch. If `RESULT_GRANT` is missed, the retry observes an
old-result/current-epoch mismatch. The associated tick handler can return
without clearing the pending relay or scheduling another action.

The stale pending relay can then block low-duty scanning, receive work, route
progress, and later reports indefinitely.

Relevant code:

- `firmware/app/src/app_anchor.c`: result identity and force-rediscovery order.
- `firmware/src/mesh_relay.c`: pending-result epoch checks.
- `firmware/app/src/app_mesh_report.c`: relay tick and timeout handling.

Required behavior:

- Do not change the epoch underneath a pending result.
- Every tick outcome must explicitly do exactly one of: make progress, schedule
  a retry, retire the state with a visible terminal error, or trigger recovery.
- Route maintenance must not strand unrelated application work.

Required test:

- Drop `RESULT_GRANT` during force-rediscovery and prove that the result reaches
  a terminal state and scanning, reporting, and routing all resume.

### 3. Fatal faults halt forever

The mesh fatal handler records volatile debug state and calls `k_fatal_halt()`.
No hardware watchdog, reset-on-fatal path, persistent crash counter, or durable
reset-reason breadcrumb is enabled in the audited configurations.

A stack overflow, kernel panic, assertion, or other fatal error therefore turns
into a permanent outage until power is cycled.

Relevant code and configuration:

- `firmware/app/src/main.c`: fatal handler.
- Generated mesh role configurations: stack protection is enabled, but no
  watchdog-based liveness recovery is enabled.

Required behavior:

- Use a hardware watchdog that cannot be satisfied by a single busy or stuck
  worker.
- Feed it only when independent role-specific progress leases are current, such
  as scheduler progress, radio progress, queue progress, and gateway delivery
  progress.
- On fatal error, save a compact retained or durable breadcrumb and stop feeding
  the watchdog rather than halting forever.
- Detect reset loops and enter a bounded degraded/recovery mode instead of
  rebooting continuously without evidence.

Required tests:

- Inject stack-check, assertion, workqueue-stall, and radio-lock faults.
- Verify automatic reset, reset-reason persistence, and useful recovery.
- Verify that a busy loop cannot falsely keep the complete system healthy.

### 4. SPI errors can become believable but invalid radio state

The project SPI port returns errors, but imported DWM3000 SDK routines discard
several read/write return values. Failed register reads can consequently be
consumed as status data. Driver deadlines also cannot help if a synchronous SPI
operation itself never returns.

This can turn a transport failure into false RX/TX completion, false status, or
an indefinitely held radio guard without a clear root-cause signal.

Relevant code:

- `firmware/app/src/dwm3000_port.c`: synchronous Zephyr SPI operations.
- `firmware/app/src/dwm3000_sdk_port.c`: project-owned SDK adapter.
- `dwm3000 examples and sdk/decadriver/deca_device.c`: ignored adapter return
  values and register reads.
- `firmware/app/src/dwm3000_driver.c`: polling deadlines and radio ownership.

Required behavior:

- Implement recovery in the project-owned adapter; do not modify the imported
  dependency tree for this hardening.
- Latch the first SPI error and make it application-visible.
- Give failed reads deterministic poisoned or zeroed data that cannot be
  interpreted as successful completion.
- Validate device ID and essential state after reset and wake.
- Reinitialize SPIM and reset/reconfigure the DWM3000 after bounded failures.
- Escalate repeated failure to the hardware watchdog.
- Audit fast/slow SPI transitions and retained sleep configuration together.

Required tests:

- Return `-EIO` from every individual transfer position.
- Simulate a transfer that never completes.
- Corrupt status reads and prove no success is reported.
- Prove guards and locks are released or reset recovery is entered.

## High-risk liveness and data-loss findings

### 5. ACK retry can silently drop unacknowledged data

ACK retry uses a non-blocking queue insertion. If the report queue is full, the
message is counted as dropped, while the pending ACK state is still cleared.
The message no longer has an owner and cannot be recovered.

This contradicts the connected-routing requirement that missing or
unacknowledged packets remain pending and retry.

Required behavior:

- Retain ownership until requeue admission succeeds, or reserve retry capacity
  atomically before releasing pending state.
- Keep enough identity information to diagnose every terminal drop.
- Expose always-on counters for retry admission failure and oldest pending age.

Required test:

- Fill the application queue while an ACK is pending, drop or partially deliver
  the ACK batch, and prove the original packet is neither lost nor duplicated.

### 6. Gateway BLE delivery can stall permanently

The audited gateway path has several one-shot behaviors:

- BLE initialization and advertising failures are logged but not retried by an
  explicit state machine.
- Advertising restart failures in callbacks are discarded.
- A notification error leaves the oldest stream record at the queue head without
  guaranteeing another attempt.
- The log backend can report a successful length even when BLE delivery failed.

A transient allocation failure, disconnect storm, or advertising error can
therefore stop host delivery indefinitely while appearing healthier than it is.

Required behavior:

- Model disabled, enabling, advertising, connected, backpressured, recovering,
  and terminal states explicitly.
- Retry transient failures with bounded exponential backoff.
- Track oldest queued record age and consecutive delivery failures.
- Trigger BLE subsystem recovery and then system recovery after bounded failure.
- Never report successful delivery for a failed notification.

Required tests:

- Inject `-ENOMEM`, disconnects, advertising-start failures, and repeated notify
  failures with no unrelated new traffic to wake the worker.

### 7. Route reply listening can be extended indefinitely

Every valid non-click wake claim can reset the route reply listener deadline.
Wake claims do not consume the bounded reply capture count. A noisy or faulty
peer can therefore retain radio/workqueue ownership until traffic stops.

This path applies to anchor-role mesh nodes and the transmitter; the gateway
exits this listener path earlier.

Required behavior:

- Use an immutable cumulative deadline for one listener invocation.
- If extension is required, cap both extension count and total elapsed time.
- Ensure cleanup releases radio state on all exits.

Required test:

- Feed hundreds of otherwise valid wake claims and prove the listener exits by
  the original bounded deadline and releases its guard.

### 8. Gateway receive work can monopolize its workqueue

The gateway uses a long channel-9 receive window and polls radio status with
short busy waits. The configured 30-second window can execute roughly hundreds
of thousands of polls and occupy the dedicated route workqueue for the entire
window.

This is bounded, but it can delay lower-priority diagnostics, maintenance, and
future watchdog feeders enough to create secondary failure modes.

Required behavior:

- Prefer IRQ/semaphore-driven receive completion.
- Otherwise split the receive window into yielding slices with a cumulative
  deadline.
- Keep health supervision independent and at a priority that remains runnable.

Required test:

- Run a no-RF receive window while measuring scheduling latency of health,
  maintenance, BLE, and queue-drain canaries.

### 9. Retained-sleep restoration can report false success

Standby records the radio as asleep without functional readback. Wake restoration
can treat cached configuration equivalence as proof that the hardware is usable.
Explicit configuration failures cause reset recovery, but a device that accepts
configuration calls and remains silently deaf has no comparable escalation path.

Some robust connected-radio idle/reset behavior is conditional on
`CONFIG_IMEC_MESH_ROUTE_TEST`; configuration changes must not silently remove
those recovery properties from deployed roles.

Required behavior:

- Validate device identity and essential state after wake.
- Add consecutive no-progress thresholds for RX, TX, and wake restoration.
- Perform a periodic bounded radio scrub/reset even when software state appears
  valid but no useful radio progress is observed.
- Treat connected-role receive availability as a required invariant.

Required tests:

- Make retained restoration return success while RX/TX remains nonfunctional,
  then prove bounded reset and recovery.
- Exercise slow/fast SPI ordering across every sleep/wake cycle.

### 10. Permanent report errors can strand later queue entries

On a permanent TX error, the report worker removes the head record and returns
without scheduling another attempt when more records remain. The tail waits for
an unrelated event to resubmit the worker.

Required behavior:

- After retiring a permanent failure, immediately schedule the next queued
  record if the queue is nonempty.
- Emit a durable terminal reason for the discarded record.

Required test:

- Queue two records, force a permanent error for the first, and prove the second
  progresses without any new enqueue or unrelated radio event.

### 11. Persistence failures can masquerade as success

Persistence initialization marks the attempt before NVS is successfully opened.
A transient failure disables persistence for the rest of that boot. Several save
results are ignored, and membership updates can return success even when durable
storage failed.

The immediate runtime state can look correct while a later reset loses new state
or resurrects old state. Repeated synchronous writes also lack a documented
multi-month endurance budget.

Required behavior:

- Retry transient NVS initialization failures with bounded backoff.
- Retain a dirty record until persistence succeeds or a visible terminal fault
  is raised.
- Distinguish accepted-in-RAM from durably-committed results.
- Coalesce and rate-limit writes, and calculate worst-case flash endurance.
- Expose persistence health in compact role telemetry.

Required tests:

- Inject mount, open, write, delete, full-sector, and power-cut failures.
- Reboot after every failure point and verify state is either old-and-valid or
  new-and-valid, never silently inconsistent.

## Dangerous fallbacks that hide errors

### Direct channel-9 EACK rewritten to channel 5

The gateway EACK policy rewrites a failed direct channel-9 collection EACK as a
channel-5 broadcast and can return success. Existing tests preserve this
behavior, but the routing contract does not clearly establish it as a safe
substitute for a reply on an established channel-9 connection.

Required behavior:

- Replies belonging to an active negotiated channel-9 connection should remain
  pending on that connection unless the contract explicitly defines a lane
  transition and its duplicate/ordering semantics.
- Reserve channel-5 broadcast EACK for an explicitly defined multi-node
  collection mode rather than using it as an invisible delivery fallback.

### Invalid radio channel falls through to channel 5

Channel selection treats values other than the exact channel-9 value as channel
5 and logs the result as legitimate channel-5 operation.

Required behavior:

- Validate the channel enum and return `-EINVAL` for unknown values.
- Do not transmit on any fallback channel after invalid input.

### Scheduler and radio-start failures are converted into inactivity

Some scheduling failures mark RX inactive or increment debug-only counters while
the higher-level caller continues. `mesh_start_uwb_rx()` can report success even
after internal scheduling failure.

Required behavior:

- Propagate admission and scheduling failures to the owning state machine.
- The owner must retry, retire visibly, or escalate; it must not treat inactive
  state as successful work.

### Debug-only accounting hides production failure

Detailed counters are commonly compiled only into test/debug configurations,
while transmitter logging is disabled and anchor roles lack a durable deployed
backend. Queue drops, retry exhaustion, no-progress radio cycles, and scheduler
failures can therefore occur without field-visible evidence.

Required behavior:

- Keep small saturating health counters in all deployed builds.
- Include reset reason, last terminal subsystem, queue high-water marks, oldest
  item ages, radio recovery count, persistence health, and BLE recovery count.
- Expose health through a compact periodic or on-request report that does not
  depend on verbose logging.

### No autonomous progress heartbeat

Anchor heartbeat reporting defaults to disabled and depends on host control.
There is no independent proof that a node is still scanning, routing, draining
queues, and recovering errors.

Required behavior:

- Maintain local progress leases even if external heartbeat transmission is
  disabled for power or airtime reasons.
- Make stale leases drive recovery and expose them in the next available health
  report.

## RAM and stack headroom

Snapshot resource use was:

| Build | Flash | RAM |
|---|---:|---:|
| `mesh_clicker` | 47.07% | 73.99% |
| `mesh_gateway` | 59.96% | 94.40% |
| `mesh_anchor_1` through `mesh_anchor_5` | about 47.6% | 94.46% |
| `mesh_transmitter` | 38.50% | 89.48% |
| `mesh_transmitter_forcedhop` | 38.53% | 89.48% |

After the 2026-07-10 hardening implementation, exact role builds used:

| Build | Flash | RAM | Static RAM reserve |
|---|---:|---:|---:|
| `mesh_clicker` | 248,872 B (47.47%) | 98,456 B (75.12%) | 32,616 B |
| `mesh_gateway` | 333,664 B (63.64%) | 129,140 B (98.53%) | 1,932 B |
| `mesh_anchor_1` | 263,600 B (50.28%) | 127,584 B (97.34%) | 3,488 B |
| `mesh_transmitter` | 213,100 B (40.65%) | 119,008 B (90.80%) | 12,064 B |
| `mesh_transmitter_forcedhop` | 213,276 B (40.68%) | 119,008 B (90.80%) | 12,064 B |

Gateway and anchor RAM are therefore deployment constraints, especially the
gateway's 1,932-byte static reserve. The hardening pass kept all functional
capacity intact; no queue, protocol packet, CIR, stack, or diagnostic buffer was
reduced to improve the percentage. Runtime stack low-water measurement remains
an acceptance gate.

RAM reduction is strictly subordinate to functionality and robustness. It is
acceptable only when tests and measurements prove that it does not reduce or
alter:

- protocol packet capacity or queue capacity required by the worst case;
- radio availability, timing margins, retry coverage, or concurrent operation;
- recovery state, watchdog independence, persistence, or health reporting;
- diagnostics required to identify field failures;
- supported topology, throughput, or application behavior.

Do not reduce buffers, queues, stacks, protocol capacity, or diagnostics merely
to meet a percentage target. Prefer eliminating proven duplicate lifetime
storage, moving mutually exclusive scratch storage into guarded shared storage,
and measuring actual stack low-water marks.

If no behavior-preserving reduction is available, retain the existing RAM use
and treat it as a hardware/resource constraint. Any headroom guard should allow
an explicit reviewed exception when the alternative changes functionality.

Required validation:

- Measure stack low-water marks for every thread under worst-case concurrent RF,
  BLE, persistence, queue, retry, and diagnostic load.
- Add build-time stack and region overflow guards.
- Record the expected static reserve per role and explain any reviewed exception.
- Re-run packet-capacity and simultaneous-event tests after every memory change.

## Other long-duration risks

- Most uptime comparisons are wrap-safe, but end-to-end behavior around the
  32-bit millisecond rollover still needs a forced rollover test and boot ID.
- There is no deployed remote update/rollback path in the audited build. This is
  a maintenance and recovery limitation even though it is not an execution
  stall by itself.
- Several zero-delay resubmission paths need bounded backoff and progress tests.
- Queue/drop counters need saturation behavior so multi-month operation does not
  wrap into misleading values.

## Ordered hardening plan

### P0: eliminate permanent stalls and silent loss

1. Define authoritative route epoch ownership in the connected-routing contract.
2. Fix local invalidation and force-rediscovery so pending results cannot be
   stranded across an epoch change.
3. Add the hardware watchdog, independent progress leases, fatal reset, durable
   reset reason, and reset-loop protection.
4. Add SPI error latching, deterministic failed reads, bounded radio recovery,
   and watchdog escalation in project-owned code.
5. Make ACK retry admission lossless and preserve ownership until admitted.

### P1: make every subsystem self-recovering

1. Add explicit BLE advertising, connection, backpressure, and recovery states.
2. Bound reply listeners and replace long busy-poll ownership with yielding or
   interrupt-driven operation.
3. Ensure every queue retirement schedules its successor.
4. Add retained-wake validation and consecutive radio no-progress recovery.
5. Make persistence retryable, observable, power-cut safe, and endurance-bounded.
6. Validate configuration invariants so deployed roles cannot silently lose
   required recovery behavior.

### P2: prove long-duration behavior

1. Add compact always-on health telemetry and oldest-item/progress-age metrics.
2. Add uptime rollover, counter saturation, reset-loop, and boot-ID tests.
3. Perform accelerated soak testing with repeated resets, RF loss, reconnects,
   queue saturation, retained sleep/wake, and persistence faults.
4. Record stack low-water and RAM reserve without accepting any memory reduction
   that changes functionality or robustness.
5. Establish a recoverable update and rollback process for unattended devices.

## Verification completed during the audit

The following passed against the audit snapshot:

- Native configure and build.
- Native tests: 32 of 32 passed.
- Exact Zephyr builds for `mesh_clicker`, `mesh_gateway`, `mesh_anchor_1`
  through `mesh_anchor_5`, `mesh_transmitter`, and
  `mesh_transmitter_forcedhop`.
- Zephyr `native_sim` application suites for mesh result handoff, gateway EACK
  policy, mesh persistence, mesh flooding, mesh preemption, and channel-9 ACK
  handoff.

The transmitter variants emitted configuration warnings because logging was
disabled while other logging options remained assigned. These should be cleaned
up so meaningful configuration warnings remain visible.

No hardware was flashed or modified during this audit. Hardware fault injection
and long-duration soak evidence remain required before claiming maintenance-free
operation.

## Verification after implementation

The 2026-07-10 implementation pass completed the following checks:

- Native firmware tests: 33 of 33 passed.
- Gateway GUI tests: 34 of 34 passed; `compileall` and mypy with explicit
  package bases passed.
- Exact Zephyr builds passed for `mesh_clicker`, `mesh_gateway`,
  `mesh_anchor_1`, `mesh_transmitter`, and `mesh_transmitter_forcedhop`; exact
  resource use is recorded above.
- Normal `clicker`, `anchor`, and `gateway` role builds also passed after the
  final runtime fixes.
- The focused channel-9 ACK handoff Zephyr suite passed 9 of 9 after the
  ownership fix. A later pristine rebuild was blocked before compilation by the
  host's missing 32-bit libc development files; native ownership tests and all
  target builds still passed.
- Exact final anchor and gateway images were flashed at 4 MHz after confirming
  probe identity from live RTT: anchor `E46070D247233537`, gateway
  `E46070D247394D36`.
- The first assignment hardware run exposed a real 6,144-byte system-workqueue
  stack overflow while an anchor response entered route discovery. The retained
  fatal breadcrumb recorded stack-check reason `2`, PC in
  `spi_nrfx_transceive`, and the system-workqueue stack bounds. Assignment
  responses were moved to the existing 12 KiB mesh route workqueue, preserving
  static RAM.
- After that fix, one anchor completed two claim rounds, durable table install,
  and first-send table acknowledgement. BLE received the gateway's terminal
  `COMMAND_OK` with assigned-anchor count `1`; a real reset restored epoch
  `3655541411`, slot `0`, and slot count `50` from NVS.
- A final BLE `Here I Am` command returned `COMMAND_OK`; the anchor received the
  priority channel-5 route advertisement and entered bounded flood forwarding.
- A sustained transmitter-to-anchor-to-gateway run delivered 298 decoded
  channel-9 frames in 95 seconds, including 187 965-byte and 54 974-byte frames.
  The anchor dropped 47 unacknowledged transit custody entries for origin retry
  without opening a route-reply listener; the previous multi-second route-search
  storm did not recur.
- An initial BLE notification attempt exposed a gateway watchdog reset while
  continuous RX remained inside one long-running work item. After crediting each
  bounded RX iteration, a 40-second concurrent BLE/mesh run completed with one
  gateway boot, 180 packet notifications, 30,989 log notifications, and 146
  received mesh frames, including 59 965-byte and 24 974-byte frames.
- Ordinary channel-9 SFD/CRC activity no longer triggers periodic full radio
  scrubs; the driver clears terminal status and the gateway immediately re-arms.

The transmitter remained powered but was not connected to either final probe,
so its final image was build-verified rather than reflashed in this pass.
Long-duration RF loss, BLE reconnect, power-cut persistence, stack low-water,
and multi-hop fault-injection gates remain open.

## Deployment acceptance gates

A multi-month deployment should not be accepted until all of the following are
demonstrated:

- No maintenance command can create permanent epoch divergence.
- Every pending route, result, ACK, report, and BLE record reaches success or a
  visible bounded terminal state.
- Fatal, SPI, radio, BLE, persistence, and workqueue stalls recover without a
  manual power cycle.
- Watchdog feeding depends on independent useful progress, not thread activity.
- Power-cut persistence tests are atomic and flash endurance is budgeted.
- Worst-case simultaneous traffic does not silently drop acknowledged data.
- All listener and receive windows have immutable cumulative bounds.
- Health evidence survives enough failures to explain the last recovery.
- Uptime rollover and saturating counters are proven.
- Stack low-water and RAM reserve are measured under concurrent worst-case load.
- Accelerated soak testing completes with no unrecovered stalls, unexplained
  resets, or silent data loss.
