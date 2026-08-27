The system combines four roles around two UWB radio lanes. The clicker starts an interaction and performs ranging. Anchors provide fixed ranging points and relay traffic. The gateway receives mesh data and bridges it to the host over BLE. A transmitter role generates test traffic, but it never behaves as a ranging anchor. BLE is only used between the host and gateway for commands and visible results; discovery, routing, relay traffic, acknowledgements, and DS-TWR use UWB.

# Custom UWB Mesh Protocol
A custom independent UWB Mesh Communication protocol is a core part of this project. The connection layer is treated as a known-reliable transport once a route and schedule exist: it retains custody, acknowledges accepted packets, retries within explicit bounds, and reports terminal failure instead of silently losing data. Known-reliable does not mean infinite retries or unconditional eventual delivery.
## **Route formation happens before connection scheduling**
Where a node with gateway-bound traffic and no route needs to find a route, it first sends a short direct channel 9 gateway probe.

Otherwise the node sends a typed route wake train and route request on channel 5. Discovery expands as:

`Attempt 1: TTL 1 -> Attempt 2: TTL 2 -> Attempt 3: TTL 4 -> Attempt 4: TTL 6 -> Attempt 5+: TTL 8`

The retry base is `1000 ms -> 2000 ms -> 4000 ms -> 8000 keep doubling to a 60000 ms cap`, with fresh up to 10%, flatly distributed random jitter every time. Idle anchors answer when they have a usable path. Otherwise they probe the gateway and may rebroadcast when TTL and capacity allow. Replies return through protected channel 5 windows with per-hop ACKs and exact ancestry.

An anchor-to-anchor route becomes connected only after PROPOSE/ACCEPT negotiates one exact channel 9 timing rhythm. The successful PROPOSE transmission defines the phase. ACCEPT confirms that phase.

## **A relay interleaves two Channel 9 connection rhythms**

Each connected anchor can retain one upstream rhythm for its parent and one downstream rhythm for its child. Every rhythm belongs to one peer and keeps its own event counter. The PROPOSE owner transmits in the first event while the accepting peer receives; each following event advances that connection's counter and reverses its TX/RX direction. The two connections reverse independently, so the relay's overall radio sequence is not required to alternate globally between TX and RX.

The required production timing for one connection is:

`60 ms retune/early-RX guard -> 120 ms event window -> 60 ms trailing reservation -> repeat every 640 ms`

Because this rhythm is strict, the node farther from the gateway starts timing negotiation, but the connected parent owns the phase choice. If the child's phase already fits, the parent accepts it unchanged. If it conflicts only with the parent's one upstream rhythm, the parent returns the exact non-overlapping half-phase as a stable offset from that proposal attempt. Both peers apply that offset, so a repeated byte-identical ACCEPT cannot move the schedule. A relay with an existing downstream rhythm, exhausted capacity, or ambiguous timing ownership rejects the proposal.

A route reply may omit a conflicting proposed timing while still returning the valid route. Route formation remains separate from connection scheduling; the subsequent PROPOSE/ACCEPT exchange installs the parent-aligned cadence.

For click-report delivery, a failed PROPOSE/ACCEPT contact attempt does not invalidate that known parent. The sender makes the initial contact attempt plus three retries against the same parent, which lets four anchors sharing one relay establish contact in turn. Each retry is a fresh proposal identity whose relative delay is calculated immediately before that physical send; a delayed ACCEPT from an older attempt cannot move the current schedule. Only the fourth consecutive hard contact failure for that exact report and parent starts route repair; the existing contact cadence, receive windows, and retry delay remain unchanged.

`Connection A: T, T + 640 ms, T + 1280 ms, ...`

`Connection B: T + 320 ms, T + 960 ms, T + 1600 ms, ...`

The single 60 ms guard is the physical reservation used by admission, early receive preparation, and the late receive tail. It reserves 240 ms around each 120 ms window. Each 320 ms half-phase then leaves 80 ms: 60 ms for the Channel-9-to-Channel-5 retune boundary and 20 ms of actual Channel-5 receive time. Empty transmit turns may be skipped, so their matching empty receive windows advance parity without counting as failed transfers. Eight consecutive attempted-transfer failures make the timing stale; a completely silent peer is retired by the five-minute supervision timeout.

## **The connected cadence must remain regular**

The steady schedule is:

`Channel 9 TX/RX -> 60 ms retune reserve -> at least 20 ms Channel 5 RX -> Channel 9 TX/RX -> 60 ms retune reserve -> at least 20 ms Channel 5 RX -> Repeat`

Each channel 5 window during a connection is 100 percent receive duty unless the node is itself originating a wake flood. An node may use the window for wake transmission, therefore skipping a connection turn to make a new connection. Unrelated, malformed, or route-class frames do not end a receive window. Between adjacent windows the DWM3000 stays idle or retune-ready rather than entering retained or deep sleep.

A route-request wake does not interrupt an active channel 9 rhythm. A valid click/ranging wake does. Once an anchor accepts a click claim, it stays on channel 5 continuously through the remaining wake train, the post-wake courtesy check, discovery, reply, schedule reception, all inter-sample gaps, and all DS-TWR exchanges. Until it accepts a valid discovery frame, it keeps arbitrating decoded click claims and moves its still-empty report reservation to a higher-precedence click. It abandons the existing channel 9 rhythm. Wake overlap is therefore checked against the worst-case channel 5-off gap of the complete two-connection schedule, including guards and late receive tails. Every wake train reaches a channel 5 receive window before it ends.

## **A wake flood repeats complete Channel 5 claims**

A wake flood is the wake train. Its ordinary production transmission budget is 500 ms. Throughout it, the sender starts complete, independently decodable Channel 5 standard-PHR packets. Each packet uses the 4096-symbol wake preamble, 16-symbol SFD, and 850 kbps data rate, followed by a versioned wake claim with its own CRC and the radio FCS. A clicker sends successful copies back-to-back. The first gateway-originated enumeration CLAIM deliberately uses the longer 1,000 ms activation train.

The on-air sequence is:

`20 ms quiet check -> repeat [preamble + wake claim + optional typed suffix] for 500 ms of wake transmission -> 20 ms quiet check`

The wake claim identifies the network, sender, event, attempt, priority, channels, required anchor count, and nonce. It also carries the remaining time until the flood ends, the follow-up begins after the 20 ms courtesy check, plus typed flags that distinguish click/ranging, route setup, and a separate control follow-up. Normal click claims reserve another 300 ms beyond the calculated discovery, schedule, and ranging path, while an anchor keeps its pre-discovery priority listener open for up to 1,000 ms after the advertised discovery start within a bounded two-second arbitration interval. Those countdowns are refreshed in every copy so a receiver that joins the flood late can still recover the same absolute schedule.

The first frame of every normal-click DS-TWR exchange is an extended POLL carrying the clicker's current button-event age in milliseconds. The responder subtracts that age from its local POLL reception time and uses the projected button instant as the click report timestamp; diagnostic and survey POLLs retain the compact header-only form. The normal-click deadline must remain below the age field's saturation limit, so a report never silently substitutes a saturated age for a precise click time.

## **Delivery keeps one owner and one identity**

One logical packet has one custody owner. The communication service owns routing, retries, ACKs, and terminal state. Pre-RF deferral consumes no attempt; an actual RF start does.

Normal deployments are expected to run continuously; frequent firmware restarts are not an operating assumption or a recovery mechanism. A restart requirement alone does not justify putting retry state, packet custody, duplicate history, click counters, or other hot state in persistent storage.

An exact hop ACK transfers custody to the next anchor: the child retires its copy, and that relay becomes the sole owner of every upstream retry. This repeats at every anchor-to-anchor edge. A gateway ACK proves final acceptance only to the immediate transmitting anchor and is never forwarded back through relays to the original child; that relay clears the gateway's exact receipt handshake itself. Direct-to-gateway sources retain their ACK_CONFIRM handshake. Relay ACKs use the sender’s next channel 9 transmit window, with gateway ACK ahead of hop ACK. One slot may carry several packets and one multi-packet ACK.

Every normal click report carries the complete sorted list of anchors selected for that click. For that packet, each anchor chooses the best of its three cached routes whose interior excludes all participating anchors; the reporter and gateway endpoints do not disqualify a route. If no cached route is provably clean, it immediately uses the ordinary best route without delay, route repair, or mutation of the normal route selection.

Direct-to-gateway traffic is batched. The sender reserves reply time, sends only what fits, marks the final packet, then switches to channel 9 receive. The gateway returns one batch ACK after the final marker. Missing entries retry in a later channel 9 window, without a new wake train while the connection remains alive.

The first three gateway-ACK failures retry the selected parent with bases of 1500 ms, 3000 ms, and 6000 ms. The fourth failure invalidates the active path and places that parent in a 60-second hold-down. An alternate current route is tried before new discovery. A connection can close  through explicit close, route invalidation, or sustained inactivity across several channel 9 cycles.

## The Role of the Gateway 
The Gateway does not use normal cadence connections in any way, connections to the gateway are therefore a special case, they do not follow a set cadence. A node can send to the gateway in batches, adding a flag to a packet when the batch has compeleted for that TX cycle, upon which the gateway will send a batch or single packet ACK ASAP. 

Furthermore, the gateway sends commands to anchors via a ch5 wake train followed by a ch5 packet. This is because most anchors at any time will be in low duty ch5 scanning mode. The gateway typically does not expect such a flooded packet to be ACKed in any way, it assumes correctly that the delivery mechanism is robust, because gateway originated ch5 packets pre-empt everything else.

# High Level Protocol Overview
## **The two radio lanes have separate jobs**

Channel 5 is the wake, control, discovery, ranging, and preemption lane. It carries click wake trains, route setup, gateway command floods, Here-I-Am advertisements, survey controls, and DS-TWR. Channel 9 is the scheduled data lane used for gateway-bound reports, relay packets, and their ACKs.

The gateway normally listens continuously on channel 9 and does not own a normal channel 9 connection. Each anchor can own at most one upstream channel 9 connection toward the gateway and one downstream connection toward a child. These are radio schedules, so an anchor with both directions occupied cannot promise another route.

## **The host controls operation policy**

Every ordinary GUI operation follows one visible host-side sequence:

`Here-I-Am -> wait for the gateway to stop transmitting -> send the command`

This is a gateway-local sequencing boundary: completion means that the gateway has finished the planned channel 5 wake train and advertisement transmissions and has released that radio work. Gateway-originated commands do not require an overall or end-to-end acknowledgement. Each upstream anchor that accepts a command for forwarding owns its downstream copy and retries locally if that delivery fails, while the gateway does not wait for aggregate confirmation from the anchors. The repeated flood gives each reachable anchor an opportunity to refresh its efficient route before the next command, reducing later route rediscovery.

Each host command carries one versioned runtime profile, and the same accepted values are forwarded to participating anchors. Firmware owns the safety limits for arithmetic, frame airtime, retuning, guards, PHY settings, antenna delays, and delayed transmission. The profile remains active in RAM, while reset restores compiled safe defaults.

## **Here-I-Am refreshes routes**

Here-I-Am is a gateway advertisement, it doesn't receive a response. The gateway first sends a channel 5 wake flood, then sends the Here-I-Am advertisement in separate typed follow-up frames. Anchors validate its identity, epoch, sequence, TTL, ancestry, age, and policy before installing or forwarding a path.

## **Enumeration assigns persistent discovery slots**

Enumeration uses three phases:

`CLAIM -> RESPONSE -> TABLE`

The gateway first sends a wake flood followed by separate CLAIM frames. Every eligible anchor that hears the CLAIM repeats it for more distant anchors, recursively within the TTL and local retry limits.

The anchors respond with their unique hardware-derived hash over their known route. Before stable slots exist, an anchor uses `hash % N` when the host supplied an expected anchor count and `hash % 50` otherwise; `N` spreads first contact but never caps membership. First contact is ordered by estimated RF hop depth and then temporary slot, and a deeper route discovered before the first RF attempt may only postpone that attempt. The preflight budget models the pessimistic `Fn--...--F1--D` chain from `N`; live responses refine the displayed depth without shortening the already-safe operation horizon.

The gateway freezes the current-run responders, sends another wake flood, then publishes one immutable table of stable identities, hashes, and explicit slots in separate TABLE frames. Retained sparse slots never move; newly seen anchors are ordered by observed depth and identity and occupy free slots. A late new anchor is deferred to the next enumeration, while an old retained anchor is required in this run only if it answered the current CLAIM.

Each current responder validates and durably stores the complete TABLE before acknowledging it. The first exact gateway ACK commits that pending table on the anchor, after which the anchor sends `ACK_CONFIRM` for the same epoch and table commitment. The gateway reports success only after every current responder confirms. A timeout reports an incomplete enumeration, retains the exact pending TABLE and confirmation debt, blocks survey or replacement publication, and resumes that same table after reset; it never rolls back to a different mapping or continues RF beyond the operation cap.

Normal click discovery replies use only that committed enumeration slot. An anchor whose committed slot is absent, invalid, or belongs to a different advertised slot span stays silent for that discovery attempt; it never substitutes an identity hash because two anchors could choose the same slot and interfere with each other.

## **Survey discovers the graph and measures anchor pairs**

The survey discovery protocol creates an anchor-to-anchor distance graph. The server or GUI uses that graph to solve the anchor geometry; the geometry-solving algorithm is not part of the firmware. The product requirement remains a 3D self-setup result, but the current repository GUI is only a 2D diagnostic preview. The future host API/solver must define height or plane constraints, workplace-frame registration, reflection handling, and uncertainty for partial or non-rigid graphs before its output can satisfy the 3D requirement.

1. **Start the survey.** After the host-owned Here-I-Am and a fully confirmed assignment sequence proves the current routes, the host sends the survey command. The gateway binds the command to the exact assignment epoch, TABLE sequence, commitment, and occupied stable-slot span; an anchor participates only when all four match its committed assignment. The gateway sends a wake flood and then floods the discovery configuration in separate typed frames. It retains that exact delivered control owner and redrives the same generation at 2, 4, 6, and 8 seconds. The shared execution delay is derived from maximum observed depth: depths one through five keep 20,000 ms, depth six uses 21,104 ms, depth seven 23,104 ms, and depth eight or unknown 25,104 ms. An anchor may use the first newer-generation wave to retire obsolete survey custody; later waves remain idempotent.
2. **Discover neighbouring anchors.** Every survey uses four rounds. Each round contains the same occupied stable-slot span, and every slot is exactly 200 ms. An anchor transmits only in its own enumeration slot and listens in the others; sparse slot holes remain quiet, and a late transmission is skipped instead of spilling into the next slot.
3. **Report the discovered graph.** When discovery ends, each anchor creates one peer report and retains ownership of that exact report until the gateway acknowledges it. First contact is ordered by lower route depth and then stable slot using the 2270 ms admission allowance. A deeper route observed before first RF postpones the attempt. After the first attempt the child makes one randomized retry with the same parent, then tries known alternate parents, and uses route discovery only after those candidates are exhausted. The relay keeps no child roster and sends no BUSY/NACK; it owns at most one connection toward the gateway and one away from it. One directed observation is enough to create a candidate edge, although observations in both directions provide better link information.
4. **Select the ranging pairs.** The gateway combines the reports into a partial connectivity graph and selects useful anchor pairs. It prefers a bounded set of strong links that keeps the graph connected where possible. Pairs may range in parallel only when their endpoints and known neighbouring anchors do not overlap; all other pairs are serialized.
5. **Arm each pair.** The gateway chooses an initiator and responder and prepares both anchors in the fixed order: `PREPARE initiator -> PREPARE responder -> START responder -> START initiator`. The gateway does not send a separate go-command. The START controls share a 15-second execution barrier and are redriven every second over the already-proven routes. Each relay preserves the packet’s age, allowing every receiving anchor to subtract the time already spent in transit and schedule the same logical execution instant. Guard time covers relay delay, clock skew, radio retuning, and the complete DS-TWR receive window; a missed endpoint fails through the existing route-depth deadline instead of extending the execution barrier for route repair.
6. **Perform DS-TWR.** At the scheduled instant, each pair performs exactly five DS-TWR exchanges. The host reports the median distance so one outlier cannot dominate the pair result.
7. **Return the result.** The responder’s sends its result. Large diagnostic data such as the CIR is not needed for the normal geometry survey. All data should fit withing 1kB.
8. **Deliver and retry.** Each exact pair result is retained until gateway acknowledgement and uses the same reliable gateway-bound communication path as click reports and other protocol results. A strictly newer, durably admitted survey generation is the explicit repair boundary for obsolete discovery and pair-result custody: the anchor first abandons each exact communication handle, then retires only records from older generations without fabricating an acknowledgement. This prevents a gateway restart from pinning every later survey while preserving same-generation retries and all newer owners. Missing or unusable samples cause the complete PREPARE+START, and ranging sequence to rerun, with at most two reruns per pair, preferably also including new anchors in the new prepare command, and excluding successful pairs.
9. **Finish or report partial geometry.** The gateway publishes every successful distance. A disconnected or incomplete graph remains a valid partial result, but it cannot be reported as a complete survey. The GUI derives the estimate from anchor count, maximum observed hop depth, occupied slot span, and the pair plan, shows the active phase plus a countdown, and warns before start when depth makes more route redundancy useful. The estimate may be exceeded without aborting valid work; the 30-minute absolute cap remains terminal.
