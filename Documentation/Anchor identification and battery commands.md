# Anchor identification and battery commands

Implemented in the GUI and firmware working tree, 2026-09-06. Recorded direct and software-forced depth-two GUI command/reply tests each passed six of six actions, including GPIO/RAM confirmation of RGB duration and radio sleep. The final production three-anchor cohort additionally passed nine of nine actions. Survey click-exclusion passed independently; concurrent same-channel click traffic still caused a 4/5 ranging sample result, retained as a failed all-samples gate in the dated review. This document records the latest user-approved scope. See the [Mesh contract](<Mesh Connected Routing Contract.md>) for existing transport and survey ownership.

## Enumeration prerequisite and waiting

Both actions require a successful enumeration in the current GUI connection session. The GUI retains the resulting stable anchor ID, discovery slot, observed hop depth and current assignment epoch in RAM. The stable 64-bit hardware-derived ID addresses the physical anchor; its slot is display/scheduling information. A selected anchor without that current enumeration entry cannot be queried. Disconnect, a changed gateway identity or the start of a new enumeration invalidates the prerequisite until enumeration succeeds again.

For saved hop depth `h`, the GUI waits at most `2 * h * 12,000 + 5,000` ms: 29 s for a direct anchor and 53 s at depth two. The gateway's outbound allowance is `h * 12,000` ms and its result-wait allowance is `2 * h * 12,000` ms. These reuse the existing per-hop safety allowance; they are deadlines, not intentional delays. An early correlated result completes immediately. It retains that command's identity and deadline through delivery; an old or mismatched response cannot complete a newer request. A successful enumeration establishes the bounded route context, not a guarantee that the anchor remains reachable forever. Missing replies end in an explicit timeout against the selected anchor.

A board restart can discard a survey RAM-only assignment and restore an older durable epoch. Its explicit `INVALID_STATE` rejection is terminal for that target; the GUI marks **Failed: enumerate again**, preserves any older cached reading, and continues the batch. Complete a fresh enumeration before retrying that anchor.

Each action uses the established path directly. It launches neither another enumeration nor a separate Here-I-Am preflight, and identification requires no battery/status pre-read solely to obtain a boot identity. These commands do not change assignment or write configuration.

## User actions

Right-click an anchor in **Click Location** or **Survey & Geometry**, including their fullscreen views, and choose **Blink RGB (10 s)**. Right-drag on empty space retains layout rotation. The menu checks command/survey availability both when opened and when invoked. The top **Anchor controls** also retain selected-anchor identification.

**Identify (10 s)** addresses one enumerated anchor and cycles its first RGB LED through red, green and blue for ten seconds, then releases the override. The interval is fixed, not a configurable protocol parameter. An exact retry must not restart or extend the original deadline; a new explicit user action may begin a new interval. Reset ends the volatile indication.

The board/status LED owner runs the override independently of radio work and restores the normal desired status on release. Low-power pin parking must respect the indication while allowing the DW3000 to return to its normal state after response delivery. The second LED's battery indication remains independent. Command success means the target accepted the indication; the GUI does not claim optical observation or equate host receipt time with its exact physical start.

**Read all batteries** requests a fresh conversion from every anchor in the current enumeration, in discovery-slot order. One GUI action serializes the existing targeted commands, with each saved hop depth setting that target's deadline. The next request starts after the previous exact result or timeout, so anchors do not send a simultaneous response burst. Individual failures remain visible and the batch continues to the remaining targets. Disconnect, route invalidation or survey ownership cancels pending batch work; a queued callback cannot revive an old batch. Other network operations remain disabled between individual requests as well as during them.

The separate voltage window opens on **Read all batteries** and can be reopened with **Show voltages** without radio work. Its scrollable table shows every enumerated anchor and its latest battery-request outcome. Hovering an anchor in either map displays its cached voltage and freshness without radio work. Identification and failed refreshes preserve the last valid battery reading; the displayed request outcome and age distinguish that older reading from a successful refresh. Cached readings and batch state are cleared with the connection's enumeration context.

Battery results display voltage in volts, observation freshness and an explicit error/unavailable state. Gateway stream v1 supplies queue age, so this transport displays elapsed time since receipt rather than claiming an end-to-end sample age. The source timestamp and boot counter remain available in the result. Voltage alone does not establish battery percentage. The existing battery helper owns divider settling, ADC conversion and cleanup; periodic battery indication and this action must serialize the complete transaction. Every exit restores the divider-off state or reports failure.

A retained exact request/result identity returns the same sample on retry; a new user request takes a new sample. A failed conversion returns an error rather than a fabricated zero-voltage reading. Delayed results from completed or superseded GUI requests do not overwrite newer observations.

## Existing command transport

`CMD_IDENTIFY_ANCHOR = 0x0109` and `CMD_READ_ANCHOR_BATTERY = 0x010A` are allocated in `firmware/include/protocol.h`. They use the existing command/result envelopes, selected target ID, sequence/semantic correlation, expiry, bounded retries and result reservations. They add no bytes to repeated Here-I-Am activation packets.

The host sends a unicast command to the stable anchor ID with exactly 13 payload bytes: `TLV_COMMAND_ID` carrying a u16, `TLV_DISCOVERY_ASSIGNMENT_EPOCH` carrying a u32, and `TLV_HOP_COUNT` carrying a u8, including the three TLV headers. The gateway validates target, epoch and depth against its current successful enumeration roster. It uses the observed first hop retained in `anchor_previous_hop_ids`; roster sorting preserves alignment of that sidecar with each stable anchor ID. Absent evidence does not become a direct-route fallback.

Outbound commands use the existing bounded C5 wake/control sender. Its transport record is automatically reaped when transport completes; the correlated target result has separate ownership and is still required for action success. Intermediate anchors retain compact-enumeration descendant reverse edges in the existing downlink table through `mesh_relay_note_enumeration_downlink()` before acknowledging the response bundle. This closes the reverse-path learning gap left by compact responses bypassing ordinary mesh ingress; it adds no second inventory.

Like enumeration responses, these replies use hop-by-hop custody: exact parent acceptance retires the child's transport copy, and gateway acceptance retires the relay's copy. They do not create the legacy child-directed gateway-ACK forwarding tail. The GUI still requires the correlated result from the selected target.

A reply includes the stable target ID, assignment epoch, boot counter, timestamp and command status, plus battery millivolts for a successful battery conversion or `TLV_DURATION_MS = 10000` for identification. Errors omit the voltage. The boot counter is response information and does not require a pre-read. The fixed duration is shared as `ANCHOR_IDENTIFY_DURATION_MS` in `protocol.h`.

The RF validator in `firmware/src/mesh.c` recognizes the exact action-result schema. Here `TLV_NODE_BOOT_COUNTER` is observation metadata; its presence alone must not classify the reply as an incomplete collection result. Collection results retain their strict six-field identity plus collection-epoch requirements. This distinction uses existing TLVs and changes no wire layout.

Reuse the current RAM command admission and response transport. `anchor_commit_broadcast_command_replay()` copies and commits the RAM command-orchestrator state; it is not an NVS writer. `app_node_comm_freeze_delivery()` retains delivery in RAM, and the current application has no registered durable-attempt owner. These paths need no new NVS exception or parallel custody mechanism. LED timers, ADC samples and action retry state remain bounded RAM work.

The GUI owns the enumerated ID-to-depth/slot map and displayed results. Firmware needs no additional per-anchor inventory for these actions. A completed BLE write proves only transport completion; the correlated target response supplies the operation outcome. Disconnect/reset can leave an action's outcome unknown, so the GUI must not silently replay an old identification action after reconnect.

## Survey ownership

An active survey disables both actions. The gateway refuses their radio work, and each participating anchor independently ignores their commands before replay mutation, forwarding, result acknowledgement or side effects. The rule also applies to previously queued execution and delayed unrelated replies. Only controls/results belonging to the active survey remain eligible; a click or service action does not cancel it.

## Qualification

The native mesh, hardware-model and GUI gates pass. Direct and software-forced depth-two hardware runs each completed all six requested actions; each run also checked the physical GPIO outputs and absolute ten-second indication lifecycle. Use the [dated bench review](Reviews/scan-power-survey-2026-09-06.md) for the final experiment evidence; the evidence distinguishes programmed images, measured outcomes and remaining physical limitations.

Verify current-session enumeration gating, stable-ID selection, saved depth/slot handling, depth-derived bounded waits, disconnect invalidation, exact retries, expiry, malformed or stale responses, unavailable routes, backpressure and survey rejection. Confirm that an action emits no extra Here-I-Am or boot-identity pre-read and reuses the existing RAM reservation path.

Exercise the LED deadline under delayed work, ordinary status updates, low-power parking and reset. Verify ADC serialization and divider cleanup across failures. On the authorized four-board bench, query each enumerated anchor, including a routed anchor, and correlate the target event with the host result. Programming and software tests alone do not prove visible LED behavior or voltage calibration; use an external voltmeter for accuracy when available.
