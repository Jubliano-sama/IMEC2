# Atomic survey DDD qualification

Date: 2026-08-25

## Outcome

The four-board DDD setup completed 10 of 10 surveys on one firmware boot and one BLE connection. Every survey ran a fresh enumeration first, collected all three expected neighbor reports, accepted one three-pair/three-wave ranging plan, and produced three usable medians with five successful DS-TWR samples per pair.

- Surveys: 10/10 complete
- Neighbor reports: 30/30 received
- Pair medians: 30/30 usable
- DS-TWR exchanges: 150/150 successful
- Endpoint calls: 300/300 returned `ret=0 status=0`
- Partial results: 0
- Skipped pairs: 0
- Survey timeouts: 0

The gateway emitted 20 logical survey controls across the 10 generations: one phase-1 neighbor-collection control and one phase-2 atomic plan/start control per survey. It emitted no separate START control.

Median ranges across the ten runs were 531-576 mm for pair 0, 643-697 mm for pair 1, and 1278-1310 mm for pair 2.

## Static and simulated gates

- Focused native survey, survey-protocol, and durable-source-invariant checks passed.
- `ctest --test-dir firmware/build -L mesh_integration --output-on-failure`: 141/141 passed.
- `ctest --test-dir firmware/build -L hardware_models --output-on-failure`: 136/136 passed.
- Production `mesh_gateway` and `mesh_anchor` builds passed.
- Gateway RAM: 125792/131072 bytes, 95.97%.
- Anchor RAM: 114952/131072 bytes, 87.70%.

## Proof boundary

The runner deliberately left 10 seconds between enumeration completion and survey admission, plus 10 seconds between completed surveys. This qualifies repeated operation with that handoff margin; it does not qualify an immediate zero-gap enumeration-to-survey handoff.

The known self-addressed `SURVEY_EVENT` host-receipt rejection occurred 80 times. Host deduplication handled the resulting replayed events and every survey still completed without lost results, but this checkpoint does not claim that receipt issue is fixed.

The four verified-flash journals remain `awaiting_qualification`; this result is live DDD survey evidence and does not mutate those journals into a broader deployment qualification.
