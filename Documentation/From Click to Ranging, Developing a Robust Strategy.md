# Introduction 
Going from a button click to a set of reliable UWB ranges is not just a timing problem. Several clickers may be pressed near the same anchors at almost the same time, anchors only scan BLE at a low duty cycle while idle, and UWB ranging packets can collide if multiple devices start ranging without coordination.

This strategy therefore has three goals:

1. Make sure nearby anchors reliably hear a clicker's BLE wake-up request.
2. Prevent several clickers from trying to use the same anchor at the same time.
3. Keep each DS-TWR ranging sequence short, uninterrupted, and accurate.

The protocol uses BLE for discovery and arbitration, then UWB for the actual ranging measurements.

# Constraints 
- Calculating from [[Useful BLE Info]], the minimum scanning window must be 30ms to guarantee catching a packet immediately. With a 10% duty cycle, this means a 300ms scanning interval. Therefore the clicker must advertise its' BLE wake up advertisement continuously for at least 300ms, as fast as possible, aka 20ms adv interval.
- 4 simultaneous clicks must be supported.
- The maximum latency between click, and finished ranging, must be less than 15 seconds in all circumstances, including simultaneous clicks. 
- According to [[Minimizing Error in Firmware]], for any ds-twr sequence we must minimize the delta between the two ds-twr reply times. We must also minimize the time it takes to reply to a ds-twr poll itself. This means the UWB packets must have the least amount of data possible, and once a ds-twr sequence starts, it must be finished at once without interruption to minimize the detla between the ds-twr reply times, if this doesnt happen, the sequence must be discarded.
# Strategy 
## Timing Constants

| Name |   Value | Meaning |
| --- | ---: | --- |
| `WAKE_ADV_MS` |  330 ms | Clicker BLE wake-up advertisement duration |
| `WAKE_ADV_INTERVAL_MS` |   20 ms | BLE advertisement interval during wake-up |
| `READY_SCAN_MS` |  200 ms | Clicker full-duty scan window for anchor READY advertisements |
| `ANCHOR_READY_ADV_MS` |  180 ms | Anchor READY advertisement duration |
| `ANCHOR_READY_ADV_INTERVAL_MS` |   20 ms | Anchor READY advertisement interval |
| `ANCHOR_UWB_WAIT_MS` |  500 ms | Maximum time an anchor waits for the selected clicker to start UWB polling |
| `NO_ANCHOR_RETRY_DELAY_MS` |  700 ms | Delay before retrying the wake-up phase if too few anchors respond |
| `MAX_WAKE_ATTEMPTS` |       6 | Maximum number of BLE wake-up attempts per click |
| `MIN_UNIQUE_RANGED_ANCHORS` |       4 | Minimum number of unique anchors that must complete ranging |
| `DS_TWR_RETRY_BACKOFF_MS` | 4-10 ms | Random backoff after a failed DS-TWR sequence |

The politeness sniff duration still needs concrete firmware constants:

| Name | Meaning |
| --- | --- |
| `UWB_QUIET_TIME_MS` | Required quiet period before a clicker may start advertising |
| `MAX_POLITENESS_WAIT_MS` | Maximum time a clicker waits before advertising anyway |

These values must be chosen so that the worst-case click latency remains below 15 seconds.

---

## Wake-Up Packet Contents

Each clicker wake-up advertisement must contain enough timing information for anchors to align their READY reply with the clicker's listening window.

Recommended fields:

| Field                     | Purpose                                                                                  |
| ------------------------- | ---------------------------------------------------------------------------------------- |
| `clicker_id`              | Permanent clicker identity                                                               |
| `click_event_id`          | Unique event/session number for this button press                                        |
| `attempt_index`           | Wake-up attempt number for this click                                                    |
| `priority_id`             | Deterministic arbitration value; lower value wins, for example BLE MAC address           |
| `ready_scan_starts_in_ms` | Remaining time until the clicker stops advertising and starts scanning for READY replies |
| `ready_scan_duration_ms`  | Duration of the clicker's READY scan window, currently 480 ms                            |
| `min_anchor_count`        | Required unique ranged anchors, currently 4                                              |
| `diagnostic_flag`         | Marks self-test or diagnostic requests so they are not counted as normal clicks          |

The important field is `ready_scan_starts_in_ms`. It prevents anchors from replying too early. An anchor that hears the clicker at the start, middle, or end of the 330 ms wake-up window can still schedule its READY advertisement inside the clicker's later READY scan window.

---

## Clicker Behaviour

### 1. Politeness Phase

When the button is pressed, the clicker does not immediately start BLE advertising.

First it:

1. Wakes the UWB radio.
2. Listens for ongoing ranging traffic.
3. Waits until the channel has been quiet for `UWB_QUIET_TIME_MS`, or until `MAX_POLITENESS_WAIT_MS` expires.

During this phase, the clicker may receive UWB packets but must not transmit UWB packets. Keeping the UWB radio on also gives the crystal time to warm up before ranging starts.

If the maximum wait expires, the clicker may continue anyway. This prevents a click from being delayed indefinitely by a noisy environment.

### 2. BLE Wake-Up Advertisement Phase

After the politeness phase, the clicker advertises `REQUEST_TO_RANGE` for `WAKE_ADV_MS = 330 ms` at a 20 ms advertisement interval.

Every advertisement includes `ready_scan_starts_in_ms`, counting down to the start of the clicker's READY scan phase.

The clicker keeps advertising until the full 330 ms window has elapsed. It does not stop early, even if one or more anchors have already heard it, because other anchors may still be inside their idle BLE scan interval.

### 3. Anchor READY Scan Phase

Immediately after the 330 ms wake-up advertisement window, the clicker stops advertising and starts full-duty BLE scanning for `READY_TO_RANGE` advertisements.

This scan lasts `READY_SCAN_MS = 200 ms`.

The clicker records all valid READY advertisements that match its own `clicker_id` and `click_event_id`. READY advertisements for other clickers or stale click events are ignored.

At the end of the READY scan, the clicker has a candidate anchor list for this attempt. If no anchors want to range, it will wait for 700ms, and try again. This can be repeated 6 times before the click is designated as failed. 

### 4. UWB Polling Phase

The clicker ranges with anchors sequentially. Up to 8 anchors, ordered by BLE RSSI(higher is better) are chosen to range with.

For each selected anchor:

1. Start a DS-TWR sequence with that anchor.
2. Finish the whole DS-TWR sequence before starting any other anchor.
3. As many measurements per anchor as possible within a time window of 50ms should be taken.
4. If a sequence fails because of CRC error, timeout, or missing response, retry after a random 4-10 ms backoff. Do not violate the 50ms time window.
5. If a sequence is interrupted or delayed in a way that violates DS-TWR timing assumptions, discard it and retry.

The clicker maintains a per-click set of anchors that have successfully completed UWB ranging. This is different from merely hearing a READY advertisement.

### 5. Success, Retry, or Failure

After each attempt:

- If at least 4 unique anchors have successfully completed UWB ranging, the click succeeds. The clicker indicates success, shuts down BLE/UWB activity, and returns to its low-power state.
- If fewer than 4 unique anchors have successfully completed UWB ranging, the clicker starts another wake-up attempt immediately.
- Previously successful anchors remain in the per-click result set, so retries only need to find additional unique anchors.
- If 6 wake-up attempts have completed and fewer than 4 unique anchors have successfully ranged, the click is marked as failed.

---

## Anchor Behaviour

### 1. Idle State

In idle state, anchors perform low-duty BLE scanning for clicker wake-up advertisements.

The UWB radio is normally off or in its lowest practical power state. It is only woken after a valid BLE wake-up request or another command schedules a UWB window.

### 2. Receiving a Wake-Up Advertisement

When an anchor receives a valid `REQUEST_TO_RANGE` advertisement, it:

1. Parses the clicker identity, click event ID, priority value, and `ready_scan_starts_in_ms`.
2. Wakes the UWB radio but does not transmit UWB.
3. Switches to full-duty BLE scanning for competing wake-up advertisements.
4. Builds a candidate list of clickers that are requesting ranging in the same local time window.

The anchor uses the advertised `ready_scan_starts_in_ms` to know when the clicker will start listening for READY replies. The anchor must not advertise READY before that point, because the clicker may still be advertising and not yet scanning.

### 3. Arbitration Between Clickers

If the anchor hears only one valid clicker request, that clicker is selected.

If the anchor hears multiple clicker requests that overlap in time, the anchor selects the clicker with the lowest `priority_id`. In the current strategy, this can be the lowest Bluetooth MAC address.

The arbitration rule must be deterministic so that all anchors that hear the same group of competing clickers are likely to choose the same winner.

The anchor should continue listening for competing requests until the selected clicker's READY scan is about to begin. This makes the arbitration window relative to the clicker's advertised timing, instead of relative to the moment the anchor happened to hear the first packet.

### 4. READY Advertisement

At the start of the selected clicker's READY scan window, or after a small implementation guard delay, the anchor advertises `READY_TO_RANGE` for `ANCHOR_READY_ADV_MS = 180 ms` at a 20 ms advertisement interval.

The READY advertisement must be tied to the selected clicker and click event.

Recommended fields:

| Field | Purpose |
| --- | --- |
| `anchor_id` | Permanent anchor identity |
| `selected_clicker_id` | Clicker this READY is intended for |
| `click_event_id` | Click event/session this READY belongs to |
| `attempt_index` | Wake-up attempt this READY belongs to |
| `priority_id_seen` | Optional debug field showing the winning priority value |
| `diagnostic_flag` | Mirrors diagnostic/self-test status when applicable |

Clickers ignore READY advertisements not addressed to their own current click event.

### 5. UWB Responder Window

After the READY advertisement, the anchor focuses on UWB and waits for the selected clicker to start polling.

The anchor waits up to `ANCHOR_UWB_WAIT_MS = 500 ms`.

During this responder window:

- The anchor only responds to the selected clicker and click event.
- It does not start unrelated UWB exchanges.
- Once a DS-TWR exchange starts, it finishes the exchange immediately if possible.
- If a DS-TWR exchange is interrupted or violates timing constraints, the sequence is discarded.

If no valid UWB poll arrives before the 500 ms timeout, the anchor returns to normal low-duty BLE scanning and powers down the UWB radio as appropriate.

---

## Example Single-Attempt Timeline

This is the intended timing relationship between the clicker and an anchor that hears the wake-up request early.

|             Time | Clicker                                                           | Anchor                                                                          |
| ---------------: | ----------------------------------------------------------------- | ------------------------------------------------------------------------------- |
|       `t = 0 ms` | Starts 330 ms `REQUEST_TO_RANGE` advertisement window             | May hear first wake-up advertisement                                            |
|   `t = 0-330 ms` | Keeps advertising; each packet includes `ready_scan_starts_in_ms` | Scans for competing clickers and schedules READY for this clicker's scan window |
|     `t = 330 ms` | Stops advertising and starts 480 ms full-duty READY scan          | Starts or prepares `READY_TO_RANGE` advertisement                               |
| `t = 330-530 ms` | Scans for anchor READY advertisements                             | Advertises READY for 180 ms                                                     |
|     `t ≈ 530 ms` | Starts sequential UWB polling                                     | Waits for selected clicker's UWB poll                                           |
|    `t ≈ 530 ms+` | Polls anchors one by one                                          | Completes DS-TWR exchanges when polled                                          |

## Collision-Avoidance Summary

The strategy avoids collisions in three layers:
1. **Before BLE wake-up:** clickers listen for ongoing UWB activity and wait for quietness before advertising.
2. **During BLE discovery:** anchors arbitrate between competing clickers and send READY only to the selected clicker/event.
3. **During UWB ranging:** the clicker polls anchors sequentially, and each DS-TWR sequence is completed without interleaving other ranging work.

This means BLE handles discovery and arbitration, while UWB is kept as simple and deterministic as possible.


---

## Implementation Notes

- READY advertisements should be explicitly addressed to a clicker and click event. Otherwise, losing clickers could mistake another clicker's READY for their own.
- The clicker should track two different sets: anchors that advertised READY, and anchors that successfully completed UWB ranging. The 4-anchor success condition should use the successful UWB ranging set.
- The anchor's READY timing should be based on `ready_scan_starts_in_ms`, not on a fixed delay after first reception. A fixed delay can make the anchor advertise READY while the clicker is still advertising and not yet listening.
- The deterministic priority rule works best if all anchors use the same `priority_id` field and compare it the same way.
- Diagnostic/self-test wake-up requests should use the same timing machinery but must be marked so the server does not count them as normal user clicks.
