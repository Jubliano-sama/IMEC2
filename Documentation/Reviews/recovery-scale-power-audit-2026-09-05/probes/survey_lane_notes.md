# Survey response lane regression evidence

The unmodified native core fails bounded scheduling, rejected-bundle atomicity,
and stale-ACK separation. The probes use only public production functions and
valid record codecs. They do not change firmware or exercise hardware.

Run from the repository root after building the native core:

```sh
python3 Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/survey_lane_runner.py --library /tmp/imec-recovery-scale-audit-c859cd2e/libcore.a --output Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/survey_lane_results.json
```

The runner compiles `survey_lane_probe.c` against that archive, runs each case
in a separate process with a one-second deadline, and returns 1 when a contract
fails. Timeouts kill and reap the affected child, so a production infinite loop
cannot hang the runner. Recorded durations include process launch and teardown;
they are native host observations, not MCU timing measurements. The JSON records
the exact commit, archive hash, probe hash, outputs, and elapsed times.

## Bounded scheduling

| Input | Records / bundles | Seed | Result | Elapsed |
| --- | --- | --- | --- | --- |
| 30 anchors, compact slots 0–29 | 75 / 4 | 265 | Returns `PROTO_OK` (0) | 0.000659 s |
| 30 anchors, sparse slots 20–49 | 118 / 6 | 8 | No return before deadline | 1.001825 s |
| 100 pair results, baseline seed | 100 / 5 | 456 | Returns `PROTO_OK` (0) | 0.000834 s |
| 100 pair results, failing seed | 100 / 5 | 265 | No return before deadline | 1.001634 s |
| Maximum supported 50-anchor record set | 162 / 9 | 0 | No return before deadline | 1.001109 s |

Sparse-slot records contain signal levels only for the 30 participating slots;
the remaining encoded levels are zero. Production creates signal chunks from the
assigned owner slot (`app_survey.c:851`), so compact node count alone does not
bound the number of chunks. These cases model all records reaching one relay;
the 30-node count by itself does not imply every topology will aggregate them.

`survey_response_lane_prepare_round()` (`firmware/src/survey_response_lane.c:243`)
greedily places bundles in offsets 0–70 and requires at least 10 ms between them.
It loops forever if no offset remains, without a retry bound, cancellation
check, or fallback. Five bundles with seed 265 jam after offsets 26, 43, 61, 7;
six with seed 8 jam after 6, 50, 68, 22, 40. Those earlier choices leave no legal
next offset even though a different arrangement could fit. Nine bundles cannot
fit any arrangement because the 71-offset span accommodates at most eight.

The runtime calls this function synchronously with `sys_rand32_get()` in
`firmware/app/src/app_survey.c:452`. Once it jams, that owner cannot reach later
deadline, cleanup, or cancellation code. The probe demonstrates the native hang;
whether a hardware watchdog resets the device is outside this probe's evidence.
The existing maximum-neighbor test checks the 162 records and 9 bundles but never
calls `prepare_round`; the existing 100-pair test uses only the passing seed 456.

Expected contract: every valid lane and random seed must settle in bounded work,
either scheduling safe offsets, leaving deferred bundles explicitly owned, or
returning an error. A one-second child deadline is a generous regression guard,
not a proposed radio timing change. This probe checks spacing on successful
returns and allows an explicit error to satisfy its completion check.

## Rejected bundle mutates already-ACKed custody

The baseline merges one new record followed by an exact duplicate into a
previously ACKed one-record lane. It returns 0, grows count 1→2, and correctly
clears the ACK and attempt masks (1→0), in 0.000788 s.

The conflicting case uses the same new first record followed by an individually
valid second record that conflicts with an existing pair result. Both records
pass production wire encode/decode. `merge_bundle` returns `PROTO_ERR_STALE`
(-9), but count grows 1→2 while both masks remain 1 and `all_acked` stays true.
The full probe returns failure in 0.000562 s. Retrying the valid prefix alone
returns 0 with `added=false`; the new record retains the old ACK, and preparing
the next round gives bundle 0 offset 255 (`SURVEY_RESPONSE_NO_OFFSET`).

The early return at `firmware/src/survey_response_lane.c:177` bypasses the
invalidation at lines 187–190 after earlier records already changed the lane.
`firmware/app/src/app_survey.c:499` treats this as a rejected receive and leaves
the mutated lane live. Expected contract: rejection leaves record custody,
ACK/attempt masks, and scheduling unchanged; successful admission invalidates
every affected upstream bundle. Full preflight or rollback can establish that
contract without changing RF timing.

## Prior lane ACK accepts changed data when generation repeats

The probe captures a valid ACK for sequence 0, reinitializes the lane, appends a
different range result, marks its sequence 0 attempted using the normal bundle
function, and feeds the captured ACK. A new generation (7→8) rejects it and leaves
`all_acked=false`, completing in 0.000651 s. Reusing generation 7 accepts it and
sets `all_acked=true`, completing with a failed contract in 0.000629 s.

`firmware/src/survey_response_lane.c:328` binds network, generation, peers, kind,
sequence, and the current attempt bit. It has no batch identity or payload
commitment to distinguish those two lane contents. The API-level replay is
proved; a claim about on-air occurrence requires the runtime's batch/restart
identity reuse and a stale frame entering its receive window. Expected contract:
an ACK for old contents cannot retire different current contents when an
operation identity repeats across batches or after restart.
