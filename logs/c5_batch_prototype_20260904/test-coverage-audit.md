# C5 prototype coverage audit — 2026-09-05

The original green suites were not broad enough to establish that the production batch loop and enumeration radio handoff worked. Behavioral harnesses now execute those production functions. Source-order checks that required the regressed per-copy ownership scheme were removed. Mandatory deployment manifests, ledgers, promotion scripts, and their tests were also removed as requested.

## What changed

- C5 reports use bounded batches, receiver credit and exact acknowledgements, with retained retry custody and semantic identity independent of per-attempt batch hints. Host deduplication uses the same rule.
- Accepted pipelined Here-I-Am establishes the assignment epoch directly. The normal exchange sends no separate CLAIM; a lower epoch after gateway restart is valid authority. Duplicate HIA preserves the active table and deadline.
- Enumeration again retains its reservation across admission, activation and the complete short-copy burst. A bounded initial scanner handoff and failure on an expired required activation remain.
- Continuous receive returns PHY errors to the application again. These errors select wake probes or radio recovery; they are not merely logging. Hidden retries and their secondary accounting were removed.

## Automated evidence

The final native runs covered 165 mesh_integration cases and 157 hardware_models cases. Their only initial failures were two source-test executables requiring the discarded ownership scheme; both passed after removing those redundant assertions. Logs: /tmp/imec2-burst-integration.log, /tmp/imec2-burst-models.log, /tmp/imec2-burst-source-recheck.log. Labels overlap and must not be added as independent scenarios.

Production harnesses cover batch sender/receiver credit, ACK identity and partial ACKs, full-queue retry liveness, handoff cleanup and expired activation, lower-epoch HIA→TABLE→survey admission, and propagation of SFD/PHR/CRC failures after one physical receive. Hardware primitives and scheduling are mocked, so these are not full Zephyr concurrency or RF-interference proofs.

All four role builds pass. Static RAM margins are gateway 5,408 bytes, direct anchor 6,336, forced anchor 6,400 and clicker 33,328; all exceed 4 KiB. The latest GUI dedup/survey-timing check passed 33 tests; earlier protocol/dedup checks passed 56.

## Hardware evidence

- `restored-f1d-survey1.log` and `restored-f1d-survey2.log`: consecutive reset/repeat runs pass with two neighbor reports, one pair, five successful range samples and no partial flags. Enumeration identifies A at hop one and B at hop two through A.
- `restored-f1d-clicks-summary.json`: ten completed clicks, twenty six-sample reports and twenty exact host receipts. Gateway RTT explicitly identifies B's reports with A as previous hop.
- `restored-dd-survey.log`: two neighbor reports and a usable four-of-five-sample pair, with no partial flags. This meets the production minimum of three successful samples; the deliberately strict five-of-five HIL check returns false. `restored-dd-survey-repeat.log` passes that stricter check with five samples.
- `restored-dd-clicks-summary.json`: ten completed clicks, twenty six-sample reports and twenty exact host receipts. Both anchors were flashed as direct mesh anchors.
- Follow-up three-anchor runs passed F1F1D and F2F1D: each delivered thirty reports and thirty exact host receipts from ten completed clicks. F1F1D survey passed 15/15 samples across three pairs; F2F1D passed 10/10 across the two RF-visible pairs. See ../c5_batch_prototype_20260905/results.md for the actual cohort, route evidence, sample counts and setup recovery.

## What the failures established

The per-copy reservation releases allowed the scanner to interfere with relay wake/copy deadlines. Failed runs decoded the relay wake but missed typed advertisements; one trace places the sole observed advertisement inside a standard-PHR probe. Restoring receive-error propagation alone passed one of two surveys, so it was insufficient. With whole-burst reservation restored, the first reset run transmitted all three advertisements and the two subsequent survey checks both passed. This supports the combined restoration, without proving every intermittent failure had one cause.

## Remaining scope

The earlier `dd-backpressure-summary.json` run withheld host notifications for 35 seconds and demonstrated real two-frame batches on anchor→anchor and anchor→gateway links. All 36 reports from 18 completed clicks reached the host, but two of twenty click attempts timed out. That pressure run predates the final restoration and is not a twenty-click pass.

F1F1D/F2F1D are now covered by the three-anchor follow-up. Current forced-hop evidence uses enforced RF scope, not physical-distance isolation. Long-duration contention, reconnects, reset-during-custody and multi-month reliability are not established by these short prototype checks.
