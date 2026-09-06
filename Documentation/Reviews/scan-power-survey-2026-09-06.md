# Scan, click and survey qualification — 2026-09-06

This review records measurements from the four attached nRF52833/DWM3000 boards in `t3code-256367ad`. Raw captures, programming backups and test output are under `logs/scan_tuning_20260906/` in this checkout. It is dated evidence; the shared headers and current implementation remain authoritative. The route-probe click repair and direct/forced-route targeted anchor commands are qualified below. Survey ownership passed with injected clicks; its separate all-samples gate failed at 4/5 under concurrent same-channel traffic, as detailed below.

## Implemented changes

The shared SPI port now uses distinct immutable 2 MHz and 32 MHz configurations. The old port mutated one configuration object, but Zephyr's SPIM driver reused the configuration by pointer identity. Consequently the peripheral could stay at 2 MHz even when the application requested 32 MHz. Ninety original hardware samples read the 2 MHz peripheral setting; 1,269 samples after the fix read 32 MHz. Wake waits for the chip at safe speed, then selects the fast configuration before restoring retained radio registers. This applies to the common clicker, anchor, gateway and survey driver.

Profiling now uses unsigned 32-bit cycle differences on the nRF52833. Receive polling deadlines use Zephyr ticks and rounded-up durations, with a final status read at timeout. The native wake PHY model is aligned with the actual driver: 4096-symbol preamble, PAC16, 16-symbol SFD and SFD timeout 4097. Its receiver can acquire an ongoing preamble when enough symbols remain, but must still receive the complete subsequent frame without a collision.

The anchor uses **5 ms continuous acquisition followed by 380 ms idle delay**, without hardware SNIFF or pulsed acquisition. An observed Here-I-Am packet-start gap of 8,253 microseconds disproved the requested 3 ms all-phase coverage. At 1 microsecond phase resolution, 3 ms misses 1,118 phases at that measured gap and 1,902 at the conservative 9,037 microsecond bound. The 5 ms setting misses none at either bound, including acquisition and clipped-frame recovery constraints. Shortening setup while retaining this acquisition margin is the practical power choice.

Ordinary click/control wake is 445 ms, reliable uplink wake is 890 ms, and Here-I-Am activation remains a separate 500 ms budget. The wake envelope includes scheduling/rearm and completed-frame margin; 380 ms is the idle delay, not a measured full scan interval. The discovery-assignment propagation model explicitly retains the 500 ms activation budget instead of accidentally inheriting the shorter ordinary wake.

Active surveys now reject unrelated wake activations and commands before admission, relay or execution. Survey workers no longer switch to the click PHY or terminate their survey to service a click. Only validated START/PLAN/CANCEL controls for the exact active survey identity, and the survey's own ranging/results, remain eligible. Absolute survey deadlines remain unchanged by rejected traffic.

## Measured timing and power interpretation

| Measurement | Original port, 10 ms acquisition | Fixed port, 5 ms acquisition |
|---|---:|---:|
| Representative idle scan period | approximately 400.0 ms | 392.88–392.97 ms median across three anchors |
| Empty RX residency marker | approximately 10.0 ms median | 5.31–5.34 ms median |
| Wake/restore mean | 7.62–7.66 ms | 5.57–5.59 ms |
| Effective normal SPI | 2 MHz | 32 MHz |

The final idle capture contains 316 scan intervals across three anchors, with a maximum of 393.036 ms. A separate post-capture counter read recorded 511 wakes with zero SPI failures, wake failures or radio recoveries. The original capture also contains click work: its multi-second gaps are not idle scan periods and are not discarded as if they never happened.

There is no current-measurement instrument in this bench, so these are elapsed-time and peripheral-state measurements, not measured board current. The [DWM3000 datasheet, table 5](https://www.decawave.com/wp-content/uploads/2021/01/DWM3000-Datasheet-1.pdf) gives typical channel-5 RX at 50 mA, IDLE at 12 mA, INIT at 6 mA, sleep at 850 nA and deep sleep at 260 nA. The [nRF52833 product specification](https://docs-be.nordicsemi.com/bundle/nRF52-Series-PS/raw/resource/enus/nRF52833_PS_v1.2.pdf) gives approximately 3.1–3.3 mA for the cited 64 MHz flash/CoreMark CPU conditions with DC/DC enabled. Live samples confirmed DC/DC enabled and the anchor SPI peripheral disabled between scans; that existing SPI parking is not a new power saving.

A rough scan-only model is `average current = sum(state current × state residency) / scan period`. Using representative periods of 400.04 and 392.92 ms, the RX contribution alone falls from about 1.25 to 0.68 mA. Assigning the remaining non-idle setup time a 6–12 mA DW3000 envelope, and assigning the nRF 3.1–3.3 mA while active except for its existing 2 ms wake sleep, gives approximately 1.54–1.70 mA before and 0.88–1.00 mA after. With matching assumptions that is roughly 0.66–0.70 mA less scan-related current. The setup-state partition and CPU activity are estimates; LEDs, regulators, other peripherals, traffic and debug-probe effects are not measured, so this is not a whole-board saving or battery-life prediction.

## Survey and enumeration evidence

The production three-anchor cohort completed a normal GUI survey and a second survey split into three separate PLAN batches. Both returned all three pairs, all five expected samples per pair, three result signals, no partial pairs and `GUI_FINAL ... success=true`. The first used one PLAN; the second exercised batch advancement and final cleanup. Full GUI captures are `survey3-default-gui.log` and `survey3-batches-gui.log`. The second run outlived the first continuous RTT capture; its GUI evidence is complete, while its final RTT tail is in a later capture.

Typical current survey radio configuration takes about 19 ms, individual DS-TWR attempts take 22–23 ms, and five-attempt waves finish around 442–443 ms inside the 600 ms reservation. These are observations, not authorization to silently shrink the ranging reservation. Anchor SPI/wake failures and recoveries remained zero after both surveys. The gateway recorded one radio recovery amid incompatible RF/decode activity; successful surveys must not be described as proof that no recovery occurred anywhere.

A subsequent durable enumeration reported three direct anchors, zero multihop anchors and zero retries. After reset, their committed slots were 0, 1 and 2. This is direct-bench evidence, not physical-hop isolation or a large-network qualification.

## Three-anchor click miss and retained delivery

For production minimum-three click discovery, the fourth board was temporarily the clicker and the other three were anchors; no gateway was present during these clicks. Three is the full expected reply count in that topology. All three button-path events eventually completed and generated nine distinct retained reports, which reached the host with receipts after the gateway role and its exact durable state were restored.

The third event needed a second attempt. The missing first-attempt responder was anchor `9699122bd60a64e3`. Its RTT positively records the accepted wake with 245 ms remaining until discovery, followed by repeated local-busy direct-gateway probe retries on the route worker. That loop did not observe the existing click-preemption wake token, so the queued custody transfer could not run before discovery. The scanner left without a reply and route probing resumed. RTT also records dropped debug messages; absence of a log line by itself is not evidence of missing RF.

The repair checks click preemption at direct-probe boundaries and uses the existing interruptible backoff. Callers unwind their ownership before custody transfer; a canceled rebroadcast retains its original packet identity and reply deadline, and the ordinary route owner retains its retry. A concise rejection marker now records event, attempt, wait time, deadline and error if direct-scan custody admission fails.

The regression compiles the actual probe loop, semaphore backoff, rebroadcast executor and worker. Seven scenarios include 116 busy-backoff arrival phases, uptime wrap, no-click retry accounting, stale wake tokens, original-deadline expiry and release of every scratch/radio owner before custody service. Both the old probe loop and a deliberately incorrect unlock order compile and fail behaviorally in the harness. The 16 click-priority source checks also pass. Mutation output is retained in `probe-yield-mutations/`.

The initial three-click latencies were approximately 1.7, 3.1 and 4.8 seconds, including courtesy deferral and absent-gateway route traffic. Nine eventual host receipts prove retained delivery; they do not make that run a latency pass.

After the repair, six further button-path events all discovered all three anchors on their first attempt. Latencies were 1.763, 1.705, 1.735, 1.746, 1.748 and 1.748 seconds (median 1.747). After restoring the gateway, all 18 distinct retained reports reached the host with 18 matching receipts, six per anchor. These captures are `probe-yield-clicks/` and `probe-yield-drain-summary.json`. Background route probing remained active, but no explicit hardware `CLICK_YIELD` marker was captured; branch-level preemption coverage comes from the production-source regression, while the hardware proves the complete observed click outcomes.

## Software validation and remaining evidence

The final source passed all 212 native tests. The required separate gates passed all 176 mesh-integration and 168 hardware-model tests; GUI validation passed 305 tests. Final production anchor, gateway and clicker builds passed without warnings. Static RAM margins are 6,144 bytes for the anchor, 5,248 for the gateway and 33,200 for the clicker. Evidence: `final-native.log`, `final-mesh.log`, `final-models.log`, `actions-final-gui.log` and `final-build-*.log`.

The SPI regression compiles the actual port against a pointer-caching hardware model, covering all four transfer APIs and repeated reset/sleep/wake/error transitions; the old port fails it. Survey tests compile extracted production admission/listener code and exercise identity mutations, unrelated commands, active RF ownership, unchanged state/deadlines, matching controls and expiry. A previously inert Python source-test file now actually invokes its tests, with removed-guard mutations proving failures.

The GUI identification and battery protocols are implemented as described in [Anchor identification and battery commands](<../Anchor identification and battery commands.md>), with current-session enumeration and saved per-anchor hop depth. Direct and forced-route command qualification passed at the checkpoints recorded below; the preceding integration failures are retained separately. The documentation refresh uses current source behavior and labels older plans/reviews as historical.


## Targeted commands: direct hardware qualification

After successful current-session enumeration, the actual GUI queried each of two direct anchors twice and identified each once. All six correlated target results succeeded in 0.65–0.74 seconds. The second battery conversion advanced the source timestamp on each anchor. Readings were approximately 2.95 V and 3.74 V; there was no independent voltmeter calibration in this test.

Non-halting GPIO/RAM samples observed 9.932 and 9.965 seconds of the fixed ten-second indication, including red, green, blue and off states. Each anchor retained one indication start timestamp, the other never blinked concurrently, and DW3000 sleep was observed in 243/245 and 247/254 samples during indication. Evidence: `anchor-actions-hil-direct-pass/`, `anchor-actions-bench-v8.log`, and `actions-v8-readback.log`. This diagnostic gateway build disabled typed stack workload output to retain protocol traces; the subsequent routed qualification below restores ordinary production diagnostics.

Integration testing found and repaired missing compact-enumeration downlinks, a NULL automatic-delivery-handle mismatch, sorting of anchor IDs without their first-hop sidecar, and rejection of a service reply's observation boot counter as an incomplete collection identity. The explicit service-result schema keeps collection identity validation strict. The clearer capture showed a 13 ms receive rearm and repeated correctly decoded but semantically rejected replies; previous missing RTT lines did not prove a 1.1-second receiver blackout. Temporary timing instrumentation was removed after locating the rejection.

Gateway stream v1 reports gateway queue age rather than end-to-end sample age. The action view now labels freshness as time since receipt on that transport; it keeps the anchor boot counter and source timestamp rather than presenting queue age as sample age.


## Routed command qualification

With B configured to require one relay, enumeration saved A at depth one and B at depth two. The ordinary gateway diagnostics were enabled. Three consecutive service actions on B returned exact results in 2.658, 2.762 and 2.564 seconds; the two battery samples had increasing source timestamps. The GUI used its saved depth-two allowance (53 seconds) and emitted no new enumeration or Here-I-Am preflight between actions. A's three direct actions also succeeded, giving six successes in this run.

A and B showed 9.938 and 9.969 seconds of sampled RGB activity, each with one start timestamp and all RGB/off states. Radio sleep occurred in 248/255 and 219/245 indication samples. The parent received child results and forwarded them to the gateway; this is an explicitly forced software topology on co-located hardware, not physical RF-isolation proof. Evidence: `anchor-actions-hil-forced-pass/`, `anchor-actions-bench-forced-v10.log` and `actions-v10-readback-forced.log`.

The preceding routed run was a failure despite its first successful result. The relay attempted the legacy gateway-ACK forwarding tail after accepting service-result custody, then blocked the next request. Service results now use the same hop-by-hop custody classification as enumeration. A regression sends consecutive battery/identify results without reinitializing either holder, proves release on exact ACKs, and retains ownership on corrupt digests. Restoring only the old transit branch reproduces the forbidden ACK-forward state. Mutation evidence is `/tmp/imec2-action-custody-mutations-3fzz71x0`.

A new build also caught tracked-default drift: cached builds selected 5 ms but `prj.conf` still explicitly selected 10 ms. Both tracked defaults are now 5 ms, and the forced-anchor build has no CLI scan overrides. Tests compare both `prj.conf` entries and both Kconfig defaults with the shared acquisition constant; mutating any one to 10 ms fails.


## Survey receive interference repair

A two-anchor survey test injected real clicker button-path traffic after both anchors accepted START, then again during the PLAN/range-result lane. RAM ownership samples and clicker discovery counters showed no anchor click replies while the survey was active. Nevertheless, the third test returned only four of five ranging samples: the responder decoded an unrelated 51-byte click frame and returned from its first POLL receive window roughly 77 ms before the expected survey poll. This was an early software exit, not evidence of a lost poll caused by RF overlap. The failed capture is retained in `survey-click-interference-foreign-frame-failure/`.

The shared DS-TWR driver now discards foreign, malformed and oversized frames during POLL, RESPONSE, FINAL and REPORT waits, explicitly rearms RX and recomputes only the time remaining until the original absolute deadline. Noise cannot grant another full timeout. SPI, CRC/PHY and other hard receive failures remain visible. FINAL diagnostics are kept local until the frame identity is accepted, so a foreign frame cannot leave measurement diagnostics in a failed result. The new production-source regression passes all four receive phases, wrong identity fields, malformed/oversized frames, sustained noise, unchanged delayed RESPONSE timestamps, hard RX/rearm errors, FINAL diagnostic isolation and uptime wrap. The fixed harness also passed UBSan; restoring the old foreign-POLL exit or refreshing the receive deadline each time both fail behaviorally (`/tmp/imec2-range-filter-mutations-9mz6gg2b`). The subsequent live run retained the complete 125 ms POLL window while repeatedly discarding click frames. It still returned four of five samples while the clicker was transmitting on the same RF channel; the strict GUI all-samples gate remains a failure, not a qualification pass. Protocol ownership passed separately: nine discovery attempts had no decoded anchor replies, neither anchor changed its PLAN deadline, and click replies resumed 1.009/1.049 seconds after the sampled inactive transition. One clicker receive contained four malformed discovery frames during survey ranging; raw RX count is not an anchor click-reply count. Non-halting SWD reads also straddled one START publication, so an initial zero generation is retained in the ownership interval rather than mistaken for a second survey. Evidence: `survey-click-interference/`, `survey-click-interference-v4.log`, `survey-click-ownership-v4.log`. This proves command exclusion and fixed receive deadlines, not immunity to RF collisions from another transmitter.


## Final production cohort

All four probes were flashed with the final production images and their complete programmed flash segments were read back successfully against those images, with FICR identity checks. A, B and C are `mesh_anchor`; G is `mesh_gateway`. C's preserved anchor NVS was restored from its checked SHA-256 backup after saving its temporary clicker state. The final GUI enumeration recovered all three stable anchors with slots 0, 1 and 2 and depth one. The clicker image was also built but is not a role in this final four-board cohort. Programming evidence is `final-flash-cohort.log`, `final-readback.log` and `final-cohort-readback.json`.

On these final images, all nine actual GUI actions succeeded: two fresh battery readings and one fixed-duration identification per anchor, with responses in 0.652–0.753 seconds. Source sample timestamps advanced on each second battery read. GPIO/RAM sampling observed 9.963, 9.927 and 9.990 seconds of indication on A, B and C, with one start time each, all RGB/off states and no other anchor identifying simultaneously. The DW3000 slept in 240/248, 241/248 and 242/249 active-indication samples. Evidence: `anchor-actions-final-production.log` and `anchor-actions-hil/`.

A final reset and normal actual-GUI survey on the same production images returned all three pairs with five successful samples each, three signal measurements, zero partial reasons and `success=true` in 60.532 seconds. All 30 logged initiator/responder attempt records reported `ret=0 st=0`. This confirms normal operation on the final images; it does not override the failed all-samples gate under deliberately concurrent click transmissions. Evidence: `final-survey-bench.log` and `final-survey/result.json`.
