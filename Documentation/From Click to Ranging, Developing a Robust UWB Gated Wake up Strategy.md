# Introduction

Going from a button click to a reliable set of UWB ranges is not just a DS-TWR timing problem. Several clickers may be pressed near the same anchors at almost the same time, anchors must remain below a strict idle duty-cycle limit, and DWM3000 wake-up time is large enough that it must be included in the power budget.

This strategy therefore has four goals:

1. Make sure nearby anchors reliably hear a clicker's UWB wake-up request while the anchors are asleep most of the time.
2. Prevent several clickers from trying to use the same anchor at the same time.
3. Keep every DS-TWR sequence short, serialized, and timing-symmetric so range offsets are minimized.
4. Design the anchor idle schedule so normal operation remains below approximately 1% DWM3000 awake/radio duty cycle, including wake/start-up overhead and not merely RX airtime.

The protocol uses UWB for wake-up, discovery, arbitration, and ranging. The wake-up phase uses long-preamble UWB claim packets. The ranging phase uses one-anchor-at-a-time DS-TWR with constant reply delays.

# Constraints
The protocol must satisfy them; if a proposed optimization violates one of these constraints, that optimization is rejected.
- Anchor UWB activity must be designed to remain below approximately 1% DWM3000 awake/radio duty cycle in normal operation. The calculation must include oscillator start-up, PLL lock, RX preamble-detect time, TX time, and DS-TWR participation.
- Every idle scan cycle must include the DWM3000 wake/start-up cost. The design may not calculate duty cycle using RX airtime alone.
- The system must support installations with up to 50 anchors, while assuming only about 8 anchors are normally in range of any one clicker.
- up to 4 simultaneous clicks must not corrupt DS-TWR exchanges. A losing clicker may retry, but an anchor must never mix packets from different clickers inside one DS-TWR sequence.
- Every DS-TWR sequence must minimize the delta between the two DS-TWR reply times. The poll-to-response delay and response-to-final delay must be short, fixed, and as equal as practical.
- Once a DS-TWR sequence starts, it must finish immediately without interleaving another anchor or clicker. If a sequence is interrupted or delayed enough to violate the timing assumptions, the sequence must be discarded.

# Why This Strategy Works
During wake-up, the clicker spends energy, not the anchors. The clicker transmits a repeated long-preamble wake train long enough to intersect each anchor's short periodic receive aperture. The anchor does not need to catch a whole packet inside its 600-800 µs detect window. It only needs to detect enough preamble to justify staying awake briefly for SFD, PHR, payload, and CRC. This is why the scheme is much more reliable than trying to fit complete short packets into tiny anchor windows.

During claim and discovery, the system decides which clicker owns each anchor before any DS-TWR begins. This avoids the most dangerous simultaneous-click failure: an anchor responding to one clicker while another clicker believes the anchor belongs to its own session. Preamble collisions and garbled wake packets may still occur, but they do not corrupt ranging because a preamble alone is never accepted as a wake event. Only a CRC-valid `WAKE_CLAIM` can create protocol state. A garbled packet may still probe a longer listen in order to catch a possible non-garbled packet.

During ranging, the clicker polls anchors one by one. This is slower than a fully slotted multi-anchor DS-TWR exchange, but it avoids the practical range offsets caused by unequal reply delays. In ideal DS-TWR the unequal delays are compensated algebraically, but in real firmware and real radios, long staggered reply times interact with clock skew, delayed-TX quantization, timestamp handling, antenna-delay calibration, and implementation asymmetry. Keeping every DS-TWR exchange short, uninterrupted, and nearly identical is the safer design.

The design target of approximately 1% duty cycle is achieved by choosing the scan interval, detect aperture, and wake channel so the normal average is below the target. It is not treated as a hard real-time firmware limiter. That keeps the implementation simpler and avoids missing legitimate clicks just because a local accounting bucket is temporarily empty. Instead, the design should include enough margin that ordinary collision recovery and expected click rates still remain near the intended power envelope.

# Strategy
## Timing Constants
Recommended default values:

| Name                             |                                      Value | Meaning                                                                                               |
| -------------------------------- | -----------------------------------------: | ----------------------------------------------------------------------------------------------------- |
| `ANCHOR_SCAN_INTERVAL_MS`        |                                     300 ms | Idle interval between anchor UWB wake scans. Lower power than 250 ms while still giving good latency. |
| `ANCHOR_DWM_STARTUP_US`          |                                    1000 µs | DWM3000 sleep-to-usable start-up allowance.                                                           |
| `ANCHOR_PREAMBLE_DETECT_US`      | minimal that guarantees catching pre-amble | Normal RX preamble-detect aperture during idle scan.                                                  |
| `WAKE_PREAMBLE_SYMBOLS`          |                                       1024 | Long-preamble wake claim.                                                                             |
| `WAKE_TRAIN_MS`                  |          `ANCHOR_SCAN_INTERVAL_MS + 25 ms` | Duration for which the clicker repeats `WAKE_CLAIM` frames.                                           |
| `CLICKER_UWB_SNIFF_MS`           |                                        2ms | Clicker listens before transmitting wake claims.                                                      |
| `MAX_POLITENESS_WAIT_MS`         |                                     550 ms | Maximum time the clicker waits for quiet before trying anyway.                                        |
| `DISCOVERY_SLOT_COUNT`           |                                         50 | Static discovery slots for up to 50 anchors.                                                          |
| `DISCOVERY_SLOT_US`              |                    minimal + margin needed | Slot length for short discovery replies.                                                              |
| `ANCHOR_UWB_WAIT_MS`             |                                     400 ms | Maximum time a selected anchor waits for the clicker to start ranging.                                |
| `DS_TWR_REPLY_DELAY_US`          |                                900-1500 µs | Fixed poll-to-response and response-to-final delay target.                                            |
| `MAX_WAKE_ATTEMPTS`              |                                          6 | Maximum wake attempts per click before failure.                                                       |
| `MAX_RANGED_ANCHORS_PER_ATTEMPT` |                                          8 | Maximum anchors ranged in one attempt.                                                                |
| MAX_FAILED_RANGING_PER_ANCHOR    |                                          2 | Wait for 1 then 5ms. then consider anchor unresponsive. Doesn't include static discovery slots.       |
| RANGING_REQUESTS_PER_ANCHOR      |                                       1-15 | Configurable at compile time.                                                                         |

Current firmware uses a conservative measured-on-hardware baseline until final board timing is characterized:

| Name | Current firmware value | Reason |
| --- | ---: | --- |
| `ANCHOR_SCAN_INTERVAL_MS` | 400 ms | Keeps the idle wake-scan baseline under the approximate 1% DWM3000 awake-time target after including the larger startup allowance. |
| `ANCHOR_DWM_STARTUP_US` | 2500 us | Conservative sleep-to-usable allowance until DWM3000 wake timing is measured on the final board. |
| `ANCHOR_PREAMBLE_DETECT_US` | 1000 us | Gives the first status-polled implementation more wake margin; can be reduced after wake reliability measurements. |
| `WAKE_TRAIN_MS` | 430 ms | Covers one full 400 ms anchor scan period plus startup/RX margin. |
| `UWB_POLITE_SAMPLE_RX_MS` | 2 ms | UWB listen duration for each clicker politeness sample. |
| `UWB_POLITE_SAMPLE_PERIOD_MS` | 25 ms | Period between sampled politeness listens while waiting for quiet. |
| `UWB_POLITE_BUSY_SAMPLE_PERIOD_MS` | 75 ms | Lower-duty period used after UWB activity is detected. |
| `UWB_POLITE_REQUIRED_QUIET_SAMPLES` | 2 | Quiet samples required before the clicker starts contention/wake. |
| `MAX_POLITENESS_WAIT_MS` | 500 ms | Bounds click latency while still allowing sampled quiet-channel checks before wake claims. |
| `BLE_COURTESY_ADV_INTERVAL` | 20.0-20.625 ms | BLE 5.x non-connectable courtesy advertisement interval before controller advertising delay. |
| `BLE_COURTESY_SCAN_WINDOW` | 20 ms every 25 ms | Passive single-channel scan used only during clicker politeness. |
| `BLE_COURTESY_MIN_WINDOW_MS` | 75 ms | Minimum courtesy window before normal UWB contention/wake. |
| `BLE_COURTESY_DEFER_MS` | 75 ms | Same-attempt defer when a higher-precedence clicker is heard. |
| `UWB_CLICKER_CONTENTION_SLOT_MS` | 12 ms | Randomized contention slot width before each wake attempt. |
| `UWB_CLICKER_CONTENTION_ATTEMPT1_SLOTS` | 16 | Attempt-1 contention window: 0-180 ms. |
| `UWB_CLICKER_CONTENTION_ATTEMPT2_SLOTS` | 32 | Attempt-2 contention window: 0-372 ms. |
| `UWB_CLICKER_CONTENTION_ATTEMPT3_PLUS_SLOTS` | 64 | Attempt-3+ contention window: 0-756 ms. |
| `UWB_RETRY_BASE_DELAY_MS` | 150 ms | Base retry sleep before adding the randomized next-attempt contention window. |
| `UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US` | 400 us | Sub-millisecond jitter between repeated wake-claim frames; kept small to preserve long-preamble coverage. |
| `ANCHOR_CLAIM_COLLECTION_MS` | 15 ms | Anchor-side window for collecting nearly simultaneous valid claims before final selection. |
| `ANCHOR_UWB_WAIT_MS` | 500 ms | Responder safety window for the selected clicker/event. |
| `DS_TWR_REPLY_DELAY_US` | 900 us | Fixed equal delay accepted by schedule validation and DS-TWR timing checks. |
| `CLICK_UWB_TIMEOUT_MS` | 40 ms | Bounds the clicker response wait so a failing anchor cannot consume the next 50 ms scheduled anchor slot. |
| `RANGE_SCHEDULE_MIN_POLL_SPACING_MS` | 50 ms | Minimum accepted spacing between scheduled anchor polls; shorter schedules are rejected. |
| `UWB_MESH_ANCHOR_RX_INTERVAL_MS` | 6000 ms | Slow periodic anchor mesh receive cadence chosen so idle wake scan plus periodic mesh RX remains inside the approximate 1% DWM3000 awake-time budget. |
| `GATEWAY_COMMAND_RESULT_TIMEOUT_MS` | 12000 ms | Covers the slow periodic anchor mesh receive cadence, gateway ACK handling, and command-result margin. |
| `CMD_SET_SCAN_DUTY` accepted interval | derived from duty and wake-overlap guards | Runtime scan-duty changes must stay inside both the periodic idle duty budget and the maximum interval the 430 ms clicker wake train can overlap. |
| `RANGING_REQUESTS_PER_ANCHOR` | 2 | Default normal-click sample count, ordered round-robin across selected anchors. |

These differences are intentional deviations from the original 300 ms / 600 us planning profile. They trade a slower baseline scan cadence for more startup and preamble-detect margin while preserving the low-duty wake-scan budget. Periodic anchor UWB mesh receive windows are included in the firmware idle-budget guard. Active route/report traffic is included in the theoretical anchor budget under the normalized workload.

---
## Preamble-Detect Window Justification
The 600 µs anchor detect aperture is not chosen because a whole wake packet must fit inside it. It is chosen because the receiver only needs enough preamble to acquire the packet, after which it can stay awake for SFD, PHR, payload, and CRC.

The DWM3000 receiver works in PACs, or preamble acquisition chunks. The relevant PAC choices are:

| PAC Setting | Symbols per PAC | 2-PAC Minimum On-Time at 64 MHz PRF |
| ----------- | --------------: | ----------------------------------: |
| `DWT_PAC4`  |       4 symbols |                             8.14 µs |
| `DWT_PAC8`  |       8 symbols |                            16.28 µs |
| `DWT_PAC16` |      16 symbols |                            32.56 µs |


For a 1024-symbol wake preamble, use `DWT_PAC16`. Therefore the practical minimum continuous preamble exposure for a wake detect is:

```text
minimum_detect_time = 2 PAC × 16 symbols/PAC × 1.01763 µs/symbol
                    = 32.07 µs
```

The remaining requirement is that the clicker's wake train must not create too large a no-preamble gap between consecutive long-preamble frames. Define:

```text
H = max_no_preamble_gap
  = SFD time + PHR time + payload time + STS time, if any + host/driver turnaround gap

W = anchor detect aperture = 600 µs
D = practical minimum preamble exposure = 65 µs
```

For every arbitrary 600 µs anchor window to contain at least one usable preamble acquisition opportunity, the conservative condition is:

```text
H <= W - D
H <= 600 µs - 65 µs
H <= 535 µs
```

This is why the clicker implementation target should be stricter than the mathematical limit:

```text
measured max_no_preamble_gap < 300-400 µs
```

That leaves margin for receiver startup uncertainty, clock tolerance, firmware jitter, and imperfect preamble detection near the edge of the RX window.

With a 1024-symbol preamble, the preamble itself lasts:

```text
1024 × 1.01763 µs = 1042 µs
```

So the anchor's 600 µs aperture is much shorter than the transmitted preamble, but much longer than the acquisition requirement. The system is therefore designed around intersecting long preamble energy, not around fitting full packets into the anchor scan window.

---

## Wake Channel Choice: Channel 9 vs Channel 5

For this DWM3000 strategy, the practical wake-channel choice is channel 5 versus channel 9.

```text
Lower-power wake channel: channel 5
Higher-frequency wake channel: channel 9
```

Channel 9 has higher idle current and higher RX current. However, channel 9 also sits well above the 6 GHz Wi-Fi/Wi-Fi 6E band, while channel 5 sits inside or near that environment depending on the regulatory domain and local equipment. Therefore, the wake-channel choice is not simply a power question. It is a trade between lower anchor current on channel 5 and lower wake-failure probability on channel 9 in busy RF environments.

Recommended policy:

| Use Case                                       | Wake Channel | Ranging Channel | Reason                                                                                                                                                                                  |
| ---------------------------------------------- | ------------ | --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Lowest anchor power in clean RF                | Channel 5    | Channel 5       | Avoids channel switching and uses lower idle/RX current.                                                                                                                                |
| Most robust wake in busy Wi-Fi 6E environments | Channel 9    | Channel 9       | Avoids poisoning the wake phase with 6 GHz Wi-Fi activity. Higher current may be justified because failed wakes cause retries and worse user latency.                                   |
| Channel-9 system compatibility                 | Channel 9    | Channel 9       | Use if the rest of the UWB system is standardized on channel 9.                                                                                                                         |
| Split-channel optimization                     | Channel 9    | Channel 5 or 9  | Use channel 9 for robust wake, then optionally range on channel 5 for lower RX current. This adds channel-switch complexity and should only be used if measured power/latency improves. |

The cleanest implementation is to use the same channel for wake, discovery, and ranging. For pure current draw, that means channel 5. For robust wake-up in a dense Wi-Fi 6E environment, that may mean channel 9. A failed wake is expensive because it causes a full retry and increases latency, so a slightly higher-current wake channel can still be the better system-level power choice if it substantially reduces wake misses.

---

## Anchor Energy Calculation

The idle scan energy is dominated by RX preamble-detect time. Start-up time matters for duty cycle, but its current is much lower than RX current.

Channel 5 has the lower calculated idle-scan energy. Channel 9 has the more robust RF position for wake-up because it is outside the normal Wi-Fi 6E 6 GHz allocation. In a quiet lab, channel 5 may be the right answer. In a real deployment with nearby 6 GHz Wi-Fi APs, laptops, phones, and wide 80/160 MHz Wi-Fi channels, channel 9 may win at the system level because it avoids repeated wake failures and retries.

Assumptions for the baseline calculation:

| Parameter   | Channel 5 | Channel 9 | Meaning                      |
| ----------- | --------: | --------: | ---------------------------- |
| `I_STARTUP` |    1.5 mA |    1.5 mA | Oscillator/start-up current  |
| `I_IDLE`    |     18 mA |     32 mA | Idle/PLL current             |
| `I_RX`      |     72 mA |     88 mA | Continuous RX current        |
| `T_STARTUP` |    1.0 ms |    1.0 ms | Sleep-to-awake start-up time |
| `T_PLL`     |   0.17 ms |   0.17 ms | PLL/channel-ready allowance  |
| `T_DETECT`  |    0.6 ms |    0.6 ms | Preamble-detect aperture     |

Per-scan charge:

```text
Qscan = I_STARTUP × T_STARTUP
      + I_IDLE × T_PLL
      + I_RX × T_DETECT
```

For a 600 µs detect aperture:

```text
Channel 5:
Qscan = 1.5 mA × 1.0 ms + 18 mA × 0.17 ms + 72 mA × 0.6 ms
      = 47.76 mA·ms

Channel 9:
Qscan = 1.5 mA × 1.0 ms + 32 mA × 0.17 ms + 88 mA × 0.6 ms
      = 59.74 mA·ms
```

Daily DWM3000-only idle consumption:

|Scan Profile|Channel 5 Avg|Channel 5 Per Day|Channel 9 Avg|Channel 9 Per Day|Ch9 Penalty|
|---|--:|--:|--:|--:|--:|
|250 ms, 600 µs detect|0.191 mA|4.59 mAh/day|0.239 mA|5.73 mAh/day|+25%|
|300 ms, 600 µs detect|0.159 mA|3.82 mAh/day|0.199 mA|4.78 mAh/day|+25%|
|500 ms, 600 µs detect|0.096 mA|2.29 mAh/day|0.119 mA|2.87 mAh/day|+25%|
|250 ms, 800 µs detect|0.249 mA|5.97 mAh/day|0.309 mA|7.43 mAh/day|+24%|

These numbers are only for the DWM3000. They do not include MCU sleep current, regulator quiescent current, LEDs, sensors, battery self-discharge, or server/backhaul electronics.

The channel-9 penalty is therefore approximately 25% in the idle-scan calculation. That penalty should be compared against the cost of missed wake attempts. For example, if channel 5 wake attempts are occasionally corrupted by nearby 6 GHz Wi-Fi activity, the system may spend extra clicker transmit time, extra anchor false-detect time, and extra user-visible retry latency. In that environment, channel 9 can be the better design even though its nominal RX current is higher.

The table above is a planning-only DWM3000 idle-scan comparison. It is useful for channel and aperture tradeoffs, but it is not the current firmware acceptance budget because the implemented firmware deliberately uses a larger 2.5 ms startup allowance, 1.0 ms RX aperture, periodic UWB mesh receive windows, scheduled discovery/schedule windows, and report TX windows.

The current theoretical radio budget is maintained in `Documentation/UWB+BLE Architecture 0.5.52.md`. Under the normalized 1000 selected-anchor-events/day workload, the current UWB-gated anchor budget is:

```text
UWB wake scan baseline        10.91 mAh/day
Periodic UWB mesh RX baseline  0.93 mAh/day
UWB discovery/schedule         1.82 mAh/day
UWB responder windows          1.94 mAh/day
UWB mesh report TX             0.31 mAh/day
Total before margin           15.92 mAh/day
Total with 1.5x margin        23.88 mAh/day
```

That keeps the power design dominated by the periodic scan plus mesh-RX baseline, while still making active event traffic explicit in the theoretical workload. A normalized previous BLE-gated anchor estimate is 85.23 mAh/day before margin, or 127.85 mAh/day with the same 1.5x margin, so the current architecture is theoretically about 5.4x lower power.

The power design is therefore dominated by idle scan settings and periodic mesh RX, not by the normal DS-TWR exchange alone.

---

## Sniff Mode Position
DWM3000 sniff mode should not be the primary low-power mechanism. It should be treated as an optional optimization inside the preamble-detect aperture.

Recommended first implementation:

```text
Use normal RX during the 600-800 µs detect aperture.
Do not use continuous sniff mode.
Do not leave the DWM3000 awake between scan windows.
```

Reasoning:

- Continuous sniff is still far too expensive for a battery anchor.
- Sniff mode can reduce preamble-hunt energy, but it also reduces sensitivity and introduces aliasing risk with the clicker's packet gaps.
- The anchor's detect aperture is already short. Normal RX for 600 µs every 250-500 ms is predictable and keeps power acceptable.
- After the normal-RX design works, sniff mode can be tested as a second-stage optimization.

If sniff mode is later used, the design requirement is:

```text
The clicker's measured maximum no-preamble gap must be less than the anchor's effective sniff-on capture opportunity.
```

This must be measured on the real board. It should not be assumed from nominal SPI speed alone.

A useful measurement target:

|Mode|Channel 5 Target|Channel 9 Target|Comment|
|---|--:|--:|---|
|Normal RX, 300 ms scan, 600 µs detect|3.8 mAh/day|4.8 mAh/day|Planning baseline only; current firmware acceptance uses the 400 ms / 1 ms budget above|
|Sniff-optimized, same scan|2-3 mAh/day|2.5-4 mAh/day|Only if sensitivity and wake reliability remain good|

The protocol should be approved against the normal-RX baseline. Sniff mode should only be accepted if it improves measured battery life without creating missed wake-ups.

---

## Wake-Up Packet Contents

Each UWB wake-up frame is a long-preamble `WAKE_CLAIM` packet. It must contain enough timing information for anchors to decide whether to lock, whether to ignore it, and when to expect discovery/ranging.

Recommended fields:

|Field|Purpose|
|---|---|
|`network_id`|Rejects unrelated systems.|
|`clicker_id`|Permanent clicker identity.|
|`click_event_id`|Unique event/session number for this button press.|
|`attempt_index`|Wake-up attempt number for this click.|
|`priority_id`|Deterministic arbitration value. Lower value wins.|
|`wake_channel`|Channel used for wake/discovery. Usually channel 5.|
|`ranging_channel`|Channel used for DS-TWR. Usually the same as the wake channel.|
|`wake_train_ends_in_ms`|Remaining time until the clicker stops wake transmission.|
|`discovery_starts_in_ms`|Remaining time until the discovery phase starts.|
|`claimed_duration_ms`|How long other clickers should defer if they hear this claim.|
|`min_anchor_count`|Required unique ranged anchors, currently 4.|
|`max_anchor_count`|Maximum anchors to range this attempt, currently 8.|
|`nonce`|Random session value to distinguish repeated attempts and avoid stale acceptance.|
|`diagnostic_flag`|Marks self-test or diagnostic requests so they are not counted as normal clicks.|
|`crc`|Required. Preamble detect alone is not a valid wake.|

The important fields are `discovery_starts_in_ms` and `claimed_duration_ms`. An anchor that hears the clicker near the start, middle, or end of the wake train can still align itself with the later discovery phase.

---

## Clicker Behaviour

### 1. Politeness Phase

When the button is pressed, the clicker does not immediately transmit.

First it:

1. Wakes its UWB radio.
2. Takes 2 ms UWB activity samples every 25 ms while quiet, or every 75 ms after activity.
3. Waits until two quiet samples have been observed, or until `MAX_POLITENESS_WAIT_MS` expires.
During this phase, the clicker may receive UWB packets but must not transmit UWB packets.

At the same time, a normal clicker emits and scans a BLE courtesy advertisement on channel 37 only. The advertisement carries network, clicker, event, attempt, and priority identity. BLE does not wake anchors and does not authorize ranging; it only lets a clicker defer if it hears a higher-precedence simultaneous clicker. Channel 37 is used for both advertising and scanning so a short politeness window does not depend on BLE scan-channel rotation. The current implementation keeps this courtesy window open for at least 75 ms before UWB contention/wake.

If the maximum wait expires, the clicker may continue anyway. This prevents a click from being delayed indefinitely by a noisy environment. The anchor-side lock and retry behavior still protect the DS-TWR phase.

After politeness, the clicker applies randomized contention before the wake train. Attempt 1 chooses one of 16 12 ms slots, attempt 2 chooses one of 32 slots, and attempts 3+ choose one of 64 slots. Retries add a 150 ms base delay, then rerun politeness/BLE courtesy and the randomized window for the next attempt. This keeps exact simultaneous button presses from staying phase-locked across attempts without spending the politeness worst case in continuous RX.

### 2. UWB Wake-Claim Phase

After the politeness phase, the clicker repeatedly transmits `WAKE_CLAIM` frames for `WAKE_TRAIN_MS`.

The wake train should use:

|Parameter|Recommended Value|
|---|--:|
|Preamble length|1024 symbols|
|Payload|As short as possible|
|STS|Off for initial wake, unless the security requirement says otherwise|
|Data rate|850 kbps|
|Channel|Channel 5 for low power, channel 9 only if required|

The anchor does not need a whole packet to fall inside its 600 µs window. It only needs the detect window to intersect enough preamble to trigger preamble detection. Once preamble is detected, the anchor stays awake briefly to look for SFD, PHR, payload, and CRC.

The clicker firmware must measure and limit this value:

```text
max_no_preamble_gap = maximum time from the end of one wake preamble
                      to the start of the next wake preamble
```

For a 600 µs anchor detect aperture, the mathematical upper bound is:

```text
max_no_preamble_gap <= 535 µs
```

This comes from the 600 µs window minus the approximately 65 µs minimum 2-PAC acquisition opportunity when using PAC32. The implementation target should be stricter:

```text
max_no_preamble_gap < 300-400 µs
```

At 32 MHz SPI, the raw SPI time for a small wake packet is not the limiting factor. Firmware latency, driver overhead, status polling cadence, logging, and RTOS scheduling are the real risks. The clicker should preload the wake frame and retransmit it with the smallest possible host gap.

### 3. Anchor Discovery Phase

After the wake train, the clicker sends a short `DISCOVER` packet.

Anchors that accepted the selected clicker's `WAKE_CLAIM` respond in static discovery slots.

```text
slot = assigned_anchor_slot
```

Use assigned slots rather than random slots where possible. With 50 anchors total and only about 8 in range, a 50-slot discovery map is sufficient if slots are planned so nearby anchors do not share slots.

The discovery reply is not used for ranging. It only says that the anchor is present and willing to range.

Recommended discovery reply fields:

|Field|Purpose|
|---|---|
|`anchor_id`|Permanent anchor identity.|
|`selected_clicker_id`|Clicker this anchor selected.|
|`click_event_id`|Click event/session this reply belongs to.|
|`attempt_index`|Wake attempt this reply belongs to.|
|`anchor_slot`|Debug and collision diagnostics.|
|`battery/status`|Optional.|
|`rx_quality`|Optional wake-quality diagnostic.|

At the end of discovery, the clicker has a candidate anchor list. It chooses up to 8 anchors, preferably ordered by received signal quality.

### 4. UWB Polling Phase
The clicker ranges with anchors sequentially.

For each selected anchor:
1. Use the same poll-to-response delay for every anchor.
2. Use the same response-to-final delay for every anchor.
3. Keep DS-TWR frames as short as possible.
4. If a sequence fails because of CRC error, timeout, missing response, or timing violation, discard the sequence
5. Retry after a random `DS_TWR_RETRY_BACKOFF_MS` if enough time and anchor duty budget remain.

Recommended DS-TWR structure:

```text
POLL_i   clicker -> anchor Ai
RESP_i   anchor Ai -> clicker
FINAL_i  clicker -> anchor Ai
RESULT_i optional anchor Ai -> clicker
```

Only the addressed anchor participates. Other anchors ignore the exchange or sleep.

The goal is not just short timing, but equal timing. Equal and repeatable delays reduce the practical range offset that appears when reply delays differ by several milliseconds.

### 5. Success, Retry, or Failure
After each attempt:
- If at least 4 unique anchors have successfully completed UWB ranging, the click succeeds. The clicker indicates success, shuts down UWB activity, and returns to low power.
- If fewer than 4 unique anchors have successfully completed UWB ranging, the clicker backs off and starts another wake attempt.
- Previously successful anchors remain in the per-click result set, so retries only need to find additional unique anchors.
- If 6 wake attempts complete and fewer than 4 unique anchors have successfully ranged, the click is marked as failed.

The clicker must distinguish between:

```text
anchors heard during discovery
anchors successfully ranged by DS-TWR
```

Only successful DS-TWR results count toward the minimum-anchor success condition.

---

## Anchor Behaviour
### 1. Idle State

In idle state, an anchor sleeps almost all the time.

Every `ANCHOR_SCAN_INTERVAL_MS`, it performs one UWB wake scan:
1. Wake DWM3000.
2. Wait for oscillator/start-up.
3. Wait for PLL/channel readiness.
4. Enable RX preamble hunt for `ANCHOR_PREAMBLE_DETECT_US`.
5. If no preamble is detected, shut the DWM3000 down immediately.
6. If preamble is detected, extend RX only long enough to attempt SFD/PHR/payload/CRC.

### 2. Receiving a Wake-Claim
When an anchor receives a valid `WAKE_CLAIM`, it:
1. Checks `network_id`, CRC, `click_event_id`, and freshness.
2. Records `clicker_id`, `click_event_id`, `attempt_index`, `priority_id`, and `claimed_duration_ms`.
3. Optionally remains awake for `CLAIM_COLLECTION_MS` to hear competing valid claims.
4. Selects a clicker according to the arbitration rule.
5. Ignores other clickers until the selected epoch ends, unless the selected epoch fails before ranging begins.

The anchor should not enter a long RX state just because preamble was seen. It should only enter the claim/discovery/ranging state after a valid claim frame.

### 3. Arbitration Between Clickers
Clicker sampled sniff-before-send and randomized contention reduce collisions, but do not eliminate them.

The anchor arbitration rule is:

```text
Among valid overlapping WAKE_CLAIM frames heard by the anchor,
select higher attempt_index first,
then the lowest priority_id,
then the lowest clicker_id,
then the lowest click_event_id.
```

The priority may be derived from:

```text
priority_id = hash(clicker_id, click_event_id, attempt_index)
```

A hash is better than a permanent MAC-address priority because it avoids one clicker always winning every simultaneous event.

If the anchor hears only one valid clicker request, that clicker is selected.

If hidden clickers cause different anchors to select different winners, the system still remains safe:
- Each anchor responds only to its selected clicker and event.
- Each clicker ranges only anchors that discovered under its event.
- Missing anchors are recovered by retry.

This may add one retry, but it prevents corrupted mixed-clicker DS-TWR.

For two clickers pressed at the same instant, the probability that both choose the same first-attempt contention slot is `1/16`. If that attempt still collides and both retry, the next slot match probability is `1/32`, then `1/64` for later attempts. Repeated `WAKE_CLAIM` frames also use only 0-400 us jitter, which is enough to decorrelate exact transmit timing without intentionally adding millisecond-scale no-preamble holes to the wake train.

A 300,000-trial Monte Carlo estimate for the BLE courtesy phase used the implemented single-channel intervals: 20 ms scan in every 25 ms period, 20.0-20.625 ms advertising interval, uniform 0-10 ms controller advertising delay, random initial advertising and scan phases, and 1 ms channel-37 advertising events. That table is a first-order estimate: it does not explicitly subtract a clicker's own advertising TX events from that clicker's passive scan RX windows. Because the BLE radio is single-event, local channel-37 TX should be modeled as scan-RX blackout before treating the result as a proven interception probability.

| Courtesy window | Lower hears higher | At least one direction hears | Mutual detection |
| ---: | ---: | ---: | ---: |
| 10 ms | 27.4% | 47.3% | 7.5% |
| 15 ms | 41.2% | 65.4% | 16.9% |
| 20 ms | 54.8% | 79.6% | 29.9% |
| 25 ms | 69.0% | 90.3% | 47.6% |
| 27 ms | 74.9% | 93.7% | 56.0% |
| 30 ms | 83.8% | 97.3% | 70.0% |
| 40 ms | 88.5% | 98.7% | 78.3% |
| 50 ms | 91.9% | 99.3% | 84.5% |
| 60 ms | 94.8% | 99.7% | 89.9% |
| **75 ms** | **96.7%** | **99.9%** | **93.6%** |
| 90 ms | 98.1% | 100.0% | 96.3% |
| 100 ms | 98.7% | 100.0% | 97.4% |
| 125 ms | 99.5% | 100.0% | 99.0% |
| 150 ms | 99.8% | 100.0% | 99.6% |
| 200 ms | 100.0% | 100.0% | 99.9% |

The implemented value is 75 ms for now because the first-order model shows the curve flattening near that point. The exact single-radio probability is lower than the 96.7% table value by local TX blackout and controller scheduling effects, so it should be calibrated with a stricter model or hardware traces. A BLE miss is safe because the UWB contention and retry rules still apply.

### 4. Discovery Reply
At the discovery phase, the selected anchor replies in its assigned discovery slot.

The reply must be tied to the selected clicker and click event. Losing clickers must not be able to mistake another clicker's discovery replies for their own.

The discovery reply is intentionally short. It is not used to compute distance, so unequal discovery slots do not create range offsets.

### 5. UWB Responder Window
After discovery, the anchor waits for a `RANGE_SCHEDULE` from the selected clicker.

If the anchor is not listed in the schedule, it returns to idle.

If it is listed, it either:
1. Sleeps until shortly before its scheduled DS-TWR poll, then wakes the DWM3000, or
2. Remains in a short guarded wait if its poll is imminent.

During the responder window:
- The anchor only responds to the selected clicker and click event.
- It does not start unrelated UWB exchanges.
- Once a DS-TWR exchange starts, it finishes the exchange immediately if possible.
- If a DS-TWR exchange is interrupted or violates timing constraints, the sequence is discarded.

If no valid UWB poll arrives before `ANCHOR_UWB_WAIT_MS`, the anchor returns to idle.

### 6. False Wake and Collision Handling

Multiple long-preamble clicker wake trains can collide. A collided preamble can still cause preamble detect, but SFD, PHR, payload, or CRC may fail.

The anchor must use tight timeouts:

|Timeout|Purpose|
|---|---|
|Preamble-detect timeout|Ends the idle scan if no preamble is seen.|
|SFD timeout|Stops listening after preamble if no SFD arrives soon enough.|
|Frame-wait timeout|Stops listening if the frame does not complete.|
|CRC/filter rejection|Prevents garbled or unrelated frames from becoming wake events.|

Abuse limiter:

```text
If repeated failed wake decodes occur within a short interval,
enter a brief collision cooldown where preamble extensions are limited.
```

These cooldowns are not meant to enforce an exact 1% limit. They are only a practical way to avoid wasting power during pathological collisions or external UWB interference. The normal duty cycle should still be controlled primarily by the scan interval and detect aperture.

---

## Design-Level Duty-Cycle Budget
The 1% target is a design-level average, not a hard firmware cutoff.

Every DWM3000 active interval should still be included in the budget calculation:

- Oscillator start-up

- PLL lock

- Preamble-detect aperture

- Extended RX after preamble detect

- Discovery TX

- Schedule RX

- DS-TWR poll RX

- DS-TWR response TX

- DS-TWR final RX

- Optional result TX

- Periodic UWB mesh RX windows

- UWB mesh TX for reports, route discovery, commands, and gateway ACKs


The anchor firmware does not need to maintain a strict token bucket or refuse otherwise valid ranging opportunities because of local duty accounting. Instead, the protocol constants should be chosen so the expected average remains below the target under normal traffic.

A useful design equation is:

The chosen constants should satisfy:

```text
average_awake_time_per_second < 10 ms/s
```

with margin.

For example, the recommended 300 ms scan profile uses about:

```text
1.77 ms / 300 ms = 5.9 ms/s
```

This leaves about 4.1 ms/s for ordinary wake extensions, discovery, and DS-TWR participation before reaching the 1% design target. If field measurements show that anchors exceed the target in realistic use, the preferred fixes are:

The current 400 ms firmware scan profile uses the same design equation with the conservative startup/RX values:

```text
(2.5 ms startup + 0.17 ms PLL + 1.0 ms RX) / 400 ms = 9.175 ms/s
```

That is just under the 10 ms/s wake-scan baseline target before adding mesh RX and active traffic. The current firmware therefore uses a deliberately slow 6 s periodic anchor mesh RX cadence and guards the combined periodic idle budget at build time:

```text
scan baseline ≈ 9.1 ms/s
periodic anchor mesh RX estimate ≈ 0.8 ms/s
combined periodic idle estimate ≈ 9.9 ms/s
```

Active UWB mesh TX/RX, discovery, scheduled ranging, and the BLE courtesy simulation are included as calculated terms in the theoretical Architecture 0.5.51 workload.

1. Increase `ANCHOR_SCAN_INTERVAL_MS`.

2. Reduce `ANCHOR_PREAMBLE_DETECT_US` only if wake reliability remains acceptable.

3. Reduce false wake extensions with tighter SFD/frame timeouts.

4. Use channel 5 instead of channel 9 where possible.

5. Optimize the wake aperture with sniff mode only after measuring reliability.


The system should not depend on a firmware budget mechanism to be low-power. It should be low-power because the normal schedule is low-power.

---

## Example Single-Attempt Timeline

This is the intended timing relationship between the clicker and an anchor that catches the wake train.

|Time|Clicker|Anchor|
|--:|---|---|
|`t = -30 ms`|Takes sampled UWB politeness listens and runs BLE courtesy on channel 37|Sleeping|
|`t = 0-180 ms`|Sleeps for randomized first-attempt contention slot|Sleeping|
|`t = 0-430 ms after contention`|Sends repeated long-preamble `WAKE_CLAIM` frames with 0-400 us jitter|Wakes every 400 ms, detects preamble, attempts to decode claim|
|`t ≈ scan overlap`|Still transmitting wake claim|Receives valid `WAKE_CLAIM`, collects competing valid claims for 15 ms, then locks to the selected clicker/event|
|`t = wake train end`|Stops wake train|Locked to selected clicker/event|
|`t ≈ wake train end`|Sends `DISCOVER`|Prepares assigned discovery slot|
|`t ≈ +0-50 ms`|Receives discovery replies across 50 static slots|Sends short discovery reply in assigned slot|
|`t ≈ +50 ms`|Sends `RANGE_SCHEDULE`|Sleeps or waits until its scheduled poll|
|`t ≈ +55 ms and later`|Polls scheduled anchors one by one, round-robin across samples|Responds only when addressed|
|`t ≈ 1.3-1.5 s`|Finishes an 8-anchor, 2-sample attempt under normal conditions|Returns to idle after its own DS-TWR exchange|

The current 400 ms scan profile gives a typical click-to-finished-ranging latency around 0.6-1.4 s depending on where the nearest anchors are in their scan periods, how many anchors are selected, how many samples are configured, and whether there is a retry.

Worst-case latency remains bounded by `MAX_WAKE_ATTEMPTS`, retry backoff, and `ANCHOR_UWB_WAIT_MS`. With 6 attempts, the system remains comfortably below a 15-second failure bound if retry delays are kept below about 1 second.

---

## Collision-Avoidance Summary

The strategy avoids collisions in five layers:

1. **Before wake-up:** clickers sample UWB activity, wait for quiet, then apply randomized contention before transmitting.

2. **During wake-up:** clickers transmit valid `WAKE_CLAIM` frames with event IDs, priorities, claimed durations, and sub-millisecond inter-frame jitter.

3. **At the anchor:** anchors collect nearly simultaneous valid claims for a short window, then lock to one selected clicker/event and ignore all others until that epoch ends.

4. **During ranging:** the clicker polls anchors sequentially, and each DS-TWR sequence is completed without interleaving.

5. **After failed attempts:** retries use a 150 ms base sleep plus a larger randomized contention window.

This means UWB wake-up can be opportunistic and collision-recovered, while UWB ranging remains deterministic and timing-clean.

---

## Implementation Notes

- Use channel 5 as the low-current default in clean RF environments. Use channel 9 as the robust wake default in environments with significant Wi-Fi 6E activity, because wake reliability may matter more than the approximately 25% idle-scan current penalty.

- Do not rely on preamble detect as proof of a clicker. Only CRC-valid `WAKE_CLAIM` frames can start an anchor epoch.

- Measure `max_no_preamble_gap` on the clicker with real firmware, the real SPI driver, the status-polling path, and logging disabled. Do not infer it only from 32 MHz SPI bandwidth.

- Preload the wake frame and retransmit it with minimal host intervention. If a nonce or countdown must change, rewrite only the necessary bytes.

- Use tight SFD and frame-wait timeouts after preamble detection to avoid power loss from collided wake trains.

- Keep the wake payload small. Long wake reliability should come from preamble length, not from a large frame.

- Use normal RX for the first implementation. Add sniff mode only after measuring that it does not reduce wake reliability.

- Assign discovery slots deliberately so anchors that can be in range of the same clicker do not share the same slot.

- Discovery slots are not DS-TWR response slots. They are only presence announcements.

- DS-TWR must be one anchor at a time. Do not use staggered anchor slots for the ranging response packet.

- Use delayed TX for the anchor response and clicker final wherever possible.

- Use equal fixed delays for all anchors. Avoid per-anchor reply delays that differ by milliseconds.

- Keep DS-TWR frames minimal. Extra payload belongs in discovery or backend messages, not in the timing-critical ranging exchange.

- Track two different sets on the clicker: anchors discovered and anchors successfully ranged.

- Ranging success must be based only on completed DS-TWR results.

- The 1% target should be met by the chosen scan interval, detect aperture, wake channel, and expected click rate. Do not rely on a hard firmware budget as the main power-control mechanism.

- The DWM3000 IRQ pin is not directly available to the MCU. Firmware must not require `irq-gpios`; TX/RX completion is detected through bounded `SYS_STATUS` polling over SPI.

- Include periodic UWB mesh RX and UWB mesh TX in anchor awake-time measurements. The wake-scan baseline alone is not the complete anchor power budget.

- Log per-anchor counters: scans attempted, preambles detected, SFD timeouts, CRC failures, valid claims, discovery replies, DS-TWR successes, DS-TWR failures, false-wake cooldowns, and measured awake time.
