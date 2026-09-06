# Reset, survey recovery and sequential control qualification

2026-09-06, follow-up to 0ba9e719e. User explicitly authorized fresh operations after reset when permanent identity survives, including blink and battery without another enumeration. Active survey exclusivity remains required.

## Reproduced failures

1. Starting another survey after the preceding run's recovery deadline sent unscoped GET_STATUS during Here-I-Am. The gateway published an old survey event, which the failed GUI refused to receipt. The resulting BLE FIFO blockage made later progress appear to loop. The fixed GUI drained the existing gateway backlog without resetting any board (gui-recover.log). That run recovered a two-anchor survey, but failed its strict three-anchor qualification because A was missing; this is not counted as a three-anchor pass.
2. Gateway status publication returned retryable backpressure but consumed the terminal reservation as INTERNAL_ERROR. Subsequent worker retries produced custody -EINVAL. The compiled production-handler regression verifies retained reservation through 30 retries, one terminal, generation matching, and malformed input rejection. Mutation checks caught the old behavior.
3. After a three-anchor survey, resetting A restored its durable assignment epoch. Its old firmware rejected blink/battery with INVALID_STATE although the permanent ID and saved slot survived. Current actions treat observed local epoch as metadata; GUI/gateway enumeration and exact target/session/sequence validation remain mandatory. Zero epoch metadata is covered natively and in GUI tests. No new NVS writes were introduced.
4. The extended baseline caught A hearing successive blink wake trains but closing its control listener before the final payload. The capture has listener closure at 222524.969 and gateway payload transmission at 222525.139. A one-renewal boolean ignored the next fresh wake identity. The new per-source watermark permits bounded renewal for fresh events; duplicates, stale/ambiguous serials, malformed claims and full source history do not renew, and an immutable maximum-depth horizon prevents indefinite retention. The regression catches the original one-renewal policy.

## Hardware and limits

| Probe | Final role | Permanent ID |
| --- | --- | --- |
| E46070D247233537 | Anchor A | c1c2306a5138ab2d |
| E46070D247394D36 | Anchor B | 0f6d3a3bdac0f858 |
| E4645C15CB365D30 | Anchor C | 9699122bd60a64e3 |
| E4645C15CB0F3B37 | Gateway | 9999888877776666 |

All four images were flashed with west/pyOCD at 4 MHz using normal sector erase. Full code-segment readback matches the current anchor/gateway HEX files (code-readback-final.json). Gateway RAM use is 125824/131072 bytes; anchor RAM use is 124928/131072 bytes. Final RAM headroom is 5248 and 6144 bytes respectively.

Resets in these tests are debugger-triggered board resets, not physical power cuts. No charging cable transition or electrical current trace was automated. The initial missing-anchor state and the later missed command are preserved separately; successful later tests do not establish an electrical root cause for charging-related RF loss. Anchor A's initial unavailable debug probe was explained by the user briefly disconnecting it.

The first one-second post-reset RAM snapshot was taken before assignment restore completed and is preserved as a timing artifact. The final run waits five seconds after reset before comparing assignments: all three restore slots 0/1/2 and span 50; epoch changes from 39059459 to saved 29360185. The permanent IDs continue to address the same boards.

## Validation

- 342 GUI tests passed; mypy passed 40 source files.
- 177 mesh integration tests and 169 hardware-model tests passed after all production edits, including the new native regressions and existing survey exclusivity checks.
- Raw RTT logs are preserved as lossless gzip files. Initial failed captures and qualifications remain alongside final evidence. The scripts are one-off bench drivers with explicit probe mappings; inspect/remove stale stop/control files before reuse.

## Final live result

The final run (gui-qualified.log) passed in 195.26 seconds: two complete three-anchor surveys, all three pairs at 5/5 samples in each survey (30/30 total); 36 successful RGB commands across the three anchors; nine successful battery reads. The middle 12 RGB commands and three battery reads occur after resetting all three anchors and before another enumeration. The second survey begins more than 65 seconds after the first terminal, reproducing the previous stale-timer trigger interval without sending GET_STATUS during enumeration. The last RGB overrides complete at about 10000 ms in each anchor RTT log. All test GUI processes were closed and the bench capture stopped; final-probe-state.json records the final running/sleeping states. The user's pre-existing GUI window was left open and disconnected to preserve its local state; it needs restarting to load the fixes.
