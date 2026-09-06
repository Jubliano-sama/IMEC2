# Command recovery qualification, 2026-09-06

GUI-only follow-up to 704fbb1f0. No firmware changes or flashes, and no changes to scan timing, radio policy, or survey ownership.

## Findings and results

- Initial enumeration found two anchors although all three were powered. Passive RTT captures preserve the failure. All three firmware code readbacks matched the production anchor image, and sampled SPI frequency was 32 MHz. Resetting only anchor C restored a three-anchor roster. The underlying reception failure remains unresolved; recovery after reset is not proof of a firmware repair.
- After RAM-only enumeration, resetting anchor A restored a different durable assignment epoch. Its battery command returned INVALID_STATE. The GUI now identifies the stale assignment, asks for re-enumeration, completes the other two reads, and releases command ownership. See gui-reproduce-v2.log.
- A subsequent fresh three-anchor survey passed all three pairs with 5/5 samples each, followed by three successful battery reads. See gui-final.log.
- Deliberately halting anchor A after a successful baseline batch produced gateway TIMEOUT about 24.34 seconds after dispatch, within the 29-second host deadline. Both remaining reads succeeded and controls returned to Ready. See gui-loss.log. The script resumed A, and the bench cleanup resumed all targets. final-probe-state.json confirms all anchors sleeping normally and the gateway running afterward.
- Shared host regression tests cover sustained event traffic, malformed event recovery, survey setup cleanup, late receipts/results, and preserving bounded survey recovery after START. A pre-fix run failed as expected (recovery-before.log). The complete GUI suite passed 337 tests; the final typed stale-assignment flag passed 47 focused tests. Mypy passed all 40 source files.

## Final probe roles

| Probe | Role | Anchor ID |
| --- | --- | --- |
| E46070D247233537 | Anchor A | c1c2306a5138ab2d |
| E46070D247394D36 | Anchor B | 0f6d3a3bdac0f858 |
| E4645C15CB365D30 | Anchor C | 9699122bd60a64e3 |
| E4645C15CB0F3B37 | Gateway | 9999888877776666 |

## Evidence notes

Raw RTT logs are losslessly gzip-compressed. live/ contains an early interrupted attachment attempt; reproduce/ and reproduce-v2/ preserve subsequent observations and resets. loss/ captures the missing-reply test. The bench scripts are one-off qualification drivers with explicit probe mappings; inspect and remove stale control/stop files before any reuse. In loss.log the legacy label RESET denotes the requested halt and resume actions respectively, as implemented in bench_loss.py. The initial strict three-anchor enumeration failure is preserved in gui-reproduce.log rather than treated as a passing gate. No claim is made that the GUI regression suite proves every firmware failure mode.
