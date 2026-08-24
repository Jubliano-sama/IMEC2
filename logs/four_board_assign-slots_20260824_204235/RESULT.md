# F2F1D ten-run enumeration result

Result: **FAIL — 2 consecutive successes, then failure on run 3; the harness stopped and runs 4-10 were not attempted.**

Command:

```sh
PATH=$PWD/.venv/bin:$PATH .venv/bin/python firmware/scripts/run_four_board_bench.py \
  --command assign-slots \
  --expected-anchors 3 \
  --repeat 10 \
  --deepest-hop 3 \
  --timeout 900 \
  --post-capture-seconds 5
```

## Run terminals

- Run 1: `ASSIGNMENT_QUALIFICATION_OK run=1/10 anchors=3 direct=1 multihop=2 retries=0`
- Run 2: `ASSIGNMENT_QUALIFICATION_OK run=2/10 anchors=3 direct=1 multihop=2 retries=1`
- Run 3: failed at epoch `59244611`; the gateway received two claims, published no table, transmitted ABORT, and reported `expected=3 received=2`.

Run 2's retry was a bounded gateway backoff (`stage=5 status=3 reason=2`) and ended in the required terminal success.

## Diagnosis

The direct anchor (`0x56da25fe4af6d141`) accepted run 3 and the forced-one-hop anchor (`0x708bc0aab970300e`) accepted it through that direct anchor. The forced-two-hop anchor (`0x191a9619c6c8cb07`) heard the gateway activation wake but never accepted epoch `59244611`.

Its RTT trace shows that it opened the fixed 2,000 ms gateway-control listener, decoded all three gateway copies and all three copies relayed by the direct anchor, rejected those copies because they had not yet traversed the required two relays, and then timed out. The forced-one-hop parent subsequently reports three successful physical transmissions, but the deepest listener had no remaining lifetime in which to receive them.

The timing model already allows one enumeration relay wave to consume up to 540 ms and gives a two-wave propagation hold of 1,230 ms. The deepest listener starts during the separate 1,000 ms gateway activation train, so a fixed 2,000 ms lifetime does not reliably cover the root payload and both forwarding steps. Runs 1 and 2 fit because their packet- and node-specific delays were shorter; run 3 exposed the upper tail. The existing listener renews after a later typed relay wake, but enumeration deliberately suppresses those extra wake trains, and it does not renew after decoding a valid control copy that has progressed through one relay but still needs another.

There was no fatal error, assertion, watchdog reset, hard fault, reboot, stack overflow, flash failure, or tooling failure in the four captures.

## Approved correction applied after diagnosis

The user rejected semantic-progress renewal because it depended on forced-hop rejection rather than production topology. The approved correction instead derives the initial listener lifetime from the anchor's selected Here-I-Am route depth and adds a redundant 2,000 ms safety margin. The resulting windows are 3,750 ms at depth 1, 4,290 ms at depth 2, and 4,830 ms at depth 3; an unknown route fails safe to the 7,530 ms maximum-depth window.

This correction was subsequently flashed and passed ten consecutive F2F1D runs. The successful follow-up evidence is in `../four_board_assign-slots_20260824_211143/RESULT.md`.

## Evidence hashes

- `gateway.log`: `df27dbb9f7c52b5a4d92f97c01fa88c637ab1ac0c626da71749b0e9590d2579e`
- `direct.log`: `7f9654ff034fdf179c4014de69dadb98770777dae598b97be7659993b08886e3`
- `anchor_b.log`: `e7b7a4e26c63f448d85f364cffe649d0e7cc6395afa0eb2cafac9b3c218a88dc`
- `anchor_c.log`: `d2f58c92e5b1db0322df1f58da94a756bf405a8632022ca587866a38cc020f6d`
- `provision.log`: `63dc1eee80c840e48dbeaea6334856510c5be35580b25c5a6181e419071a19de`
