# Mesh routing and radio ownership contract

Current production-candidate behavior, checked against the working tree on 2026-09-06. This document distinguishes explicit requirements from implemented mechanisms. The `mesh_` preset names survive the migration; production report delivery now uses Channel 5. Older Channel-9 cadence, PROPOSE/ACCEPT and ACK_CONFIRM descriptions are historical, although compatibility code and regression tests remain.

## Ownership requirements

**Survey exclusivity is an explicit user requirement confirmed on 2026-09-06.** From accepted START through all PLAN batches and result drains, an active survey owns every participating anchor exclusively. Only validated controls for that exact survey identity and its ranging/result traffic remain eligible. Completion, matching CANCEL, bounded expiry or terminal failure releases that ownership.

Participating anchors ignore clicks, enumeration/Here-I-Am activations, assignment changes and unrelated gateway commands, including identification and battery requests. Rejected traffic gets no command response and cannot change the survey identity, roster or deadlines. Admission applies both before RF side effects and before execution of already queued commands. Survey receive work does not probe the click PHY or cancel itself for a click. In each ranging receive phase, malformed, foreign and oversized frames are discarded while the original absolute receive deadline remains in force. Decoding an unrelated click must neither terminate that wait early nor grant it a fresh timeout. The gateway and GUI prevent unrelated operations while the survey is active.

A control listener gives each fresh per-source wake event its bounded payload window, capped by the existing maximum-depth listener plus one follow-up horizon. Duplicate or older wake events do not refresh that window, and per-source history is bounded without eviction during the listener.

Outside a survey, a valid click can take radio ownership from interruptible enumeration/control receive work. An accepted click keeps the radio through claim arbitration, discovery, schedule and ranging. A physical Here-I-Am activation transmission is atomic for its bounded train; future scheduled transmissions do not own the radio. Clicks overlapping that atomic transmission rely on the clicker's bounded retry/arbitration path.

One logical packet retains an exact custody identity through deferral, RF attempts, ACKs and terminal handling. A pre-RF wait does not count as an RF attempt. Bounded firmware RAM and GUI RAM hold frequently changing protocol state; NVS is for infrequent durable configuration, with a justified write-rate bound for any new writer. Normal deployment assumes continuous operation, not frequent resets.

## Wake and scan timing

The shared source is [`mesh_radio_timing.h`](../firmware/include/mesh_radio_timing.h), with application admission guards in [`app_config.h`](../firmware/app/src/app_config.h). Current defaults are:

| Operation | Timing |
|---|---|
| Idle anchor acquisition | 5 ms continuous RX, then 380 ms idle reschedule delay |
| Ordinary click/control wake | 445 ms |
| Reliable uplink wake | 890 ms, covering two receiver opportunities |
| Combined Here-I-Am activation | 500 ms |
| Ordinary wake courtesy checks | 20 ms before and after the train |
| Random gap after successful click wake copy | 0–400 microseconds |
| Random gap after Here-I-Am activation copy | 0–100 microseconds |

The idle delay is not the complete scan period: wake, configuration, RX, activity completion and release add elapsed time. The conservative wake envelope reserves 40 ms rearm, up to 10 ms configured RX and 15 ms complete-frame allowance. The accepted idle-delay command range is currently pinned to 380 ms by the minimum and overlap guards.

Each wake copy contains a complete standard-PHR frame with a 4096-symbol preamble, 16-symbol SFD, 850 kbps payload and a validated wake claim. Here-I-Am embeds its activation identity, depth, epoch, sequence and refreshed countdowns in that repeated frame. A receiver must acquire and finish a whole frame; partial overlap is not delivery proof. The measured 8,253 microsecond Here-I-Am start gap disproved a 3 ms acquisition window. The 5 ms setting passes the modeled 9,037 microsecond full-phase bound. Neither hardware SNIFF nor pulsed acquisition is allowed; continuous acquisition preserves sensitivity.

DWM3000 initialization and the wake handshake use 2 MHz SPI, then restore effective 32 MHz before normal transfers. The shared SPI configuration cache must reflect the configuration actually applied to the peripheral, including shared-structure mutation. Radio parking/recovery must succeed before ownership is released or watchdog progress is credited. These guarantees are tested separately from protocol scheduling.

## Routing and report delivery

Wake/contact, control, ranging and production mesh report delivery share Channel 5, with operation-specific standard/extended-PHR configurations. BLE GATT connects the gateway to the host; it does not carry anchor routing or DS-TWR.

Here-I-Am establishes observed upstream candidates in depth-separated activation/advertisement blocks. A sender chooses a randomized activation edge, sends its complete activation train and then independently positions short advertisements in their allotted strata. Relays preserve the original wave clock and packet age. A gateway-local command terminal does not prove every anchor received the wave, and a fresh wave must respect the previous wave's complete quiet boundary.

Candidates carry measured local link quality and gateway depth. Selection excludes unusable, expired, held or unreachable candidates. Correlated ACK feedback updates candidate depth and credit. A retained route is not proof of a listening peer or a successful exchange. The current application retains correlated route-request/reply recovery when no usable route exists; local `MSG_ROUTE_SOLICIT` helpers do not establish complete sleeping-anchor repair coverage. No unobserved direct-gateway fallback may certify success.

[Channel 5 Delivery Protocol](<Channel 5 Delivery Protocol.md>) describes the bounded report bank, batching, exact ACKs and backpressure. Legacy event constants and Channel-9 functions are implementation residue or compatibility paths, not a required 640 ms production report cadence.

## Enumeration and click ranging

The normal enumeration sequence is `Here-I-Am -> RESPONSE -> TABLE`; matching prearm avoids a second CLAIM wave. A fallback CLAIM can establish the same bounded operation when prearm is absent. The active operation binds its epoch, roster and deadlines; exact duplicates preserve them. Outside an active survey, an authoritative new enumeration can recover from an older or numerically lower gateway epoch without erasing every anchor.

The gateway freezes current responders and emits one authoritative immutable TABLE with explicit stable slots. Anchors validate and commit their own assignment; omitted anchors become unprovisioned. TABLE has no receipt/ACK_CONFIRM quorum and no terminal END wave. Gateway success follows local TABLE transmission and the bounded propagation hold, not verified reception by every physical board. Normal enumeration persists assignment; the GUI's survey enumeration requests temporary RAM-only assignment. Reset restores the saved configuration and permanent hardware ID. Fresh stable-ID identification/battery actions do not require the anchor to retain the temporary survey epoch; GUI/gateway enumeration admission remains required. Fresh surveys establish a new valid operation rather than resuming lost volatile state.

Normal click discovery uses the committed slot and advertised slot span. Invalid or absent assignment stays silent; identity-hash fallback must not introduce colliding normal-click replies. The clicker selects anchors, emits the ranging schedule and initiates DS-TWR. The normal 400 ms burst has twelve 33 ms reservations, giving four selected anchors three opportunities. Normal exchanges end at FINAL, and anchors retain measurements for gateway delivery. Diagnostic exchanges retain their report-bearing stride. The click POLL carries button-event age so anchors derive the report timestamp from the observed ranging exchange.

## Survey protocol

Implementation lives in [`app_survey.c`](../firmware/app/src/app_survey.c), [`survey.h`](../firmware/include/survey.h), [`survey_response_lane.h`](../firmware/include/survey_response_lane.h), their native modules, and the GUI's survey runtime/planning modules.

1. **Establish the roster.** The GUI performs fresh survey-specific RAM-only enumeration. Its authoritative TABLE retains an epoch-bound receive handoff. START must match the assignment epoch, table sequence, commitment and slot map and consume that handoff; it has no silent wake fallback. `SURVEY_ENUMERATION_HANDOFF_HOLD_MS` sums maximum TABLE propagation, the 5 s host START allowance, bounded control-origin submission and a radio guard. The separate 60 s host PLAN timeout does not extend this handoff.
2. **Collect neighbors.** Accepted START binds a survey generation and immutable roster. Each occupied stable slot receives a 1,000 ms neighbor interval with five spaced presence beacons; sparse slots remain quiet. Late work must not spill into another slot. Compact response records return through the depth/slot response lane using the established enumeration parent.
3. **Plan on the host.** The GUI owns the remaining pair queue and partitions it into at most 100 pairs per firmware PLAN. The firmware validates the exact survey/assignment identity and plan commitment. Noninterfering pairs can share a wave; conflicting pairs require separate waves. This is the current START/PLAN protocol, not per-endpoint PREPARE/START/GO transactions.
4. **Range and drain.** A range wave is 600 ms, with responder preparation and five attempts spaced 80 ms apart. Usable results require at least three successful ranges. Compact neighbor/range records are bundled and forwarded through the survey response lane. ACKs bind network, generation, endpoints, result kind, batch, sequence and the immutable bundle digest. Capacity pressure retains or defers bounded work; it must not silently truncate successful results.
5. **Finish or cancel.** Exact matching controls govern further batches and cancellation. Terminal publication distinguishes complete, partial, failed and aborted work; a partial graph is not complete geometry. Phase/depth estimates aid the UI, while the 30-minute hard cap and bounded local ownership remain terminal safety limits.

The GUI owns graph solving, NLOS processing, layout constraints and visualization. Its current solver produces a relative 2D layout. The broader 3D workplace self-setup objective still needs its separate height/frame/reflection contract; documentation must not present a 2D fit as proof of that product requirement.

## Qualification and power

Build/test success, flash/readback, enumeration, survey samples and click host receipts are separate evidence. Forced-hop decode-layer restrictions exercise routing but do not prove RF-distance isolation. Re-enumerate the live probes before bench work, preserve compatible durable state, back up and initialize incompatible state on role migration, and report final roles.

Power review includes the nRF CPU/clock/peripheral states, DW3000 sleep/idle/RX/TX, SPI, LEDs and regulator paths. RX duty alone is not battery current. Without a power instrument, report code/model duty and state residency as estimates; do not claim measured current or battery life. Preserve robustness and sensitivity before pursuing smaller timing values.
