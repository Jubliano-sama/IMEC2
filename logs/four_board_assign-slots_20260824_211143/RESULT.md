# F2F1D depth-aware listener ten-run result

Result: **PASS — 10/10 consecutive enumeration and assignment runs completed with the exact F2F1D topology.**

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
- Run 2: `ASSIGNMENT_QUALIFICATION_OK run=2/10 anchors=3 direct=1 multihop=2 retries=0`
- Run 3: `ASSIGNMENT_QUALIFICATION_OK run=3/10 anchors=3 direct=1 multihop=2 retries=0`
- Run 4: `ASSIGNMENT_QUALIFICATION_OK run=4/10 anchors=3 direct=1 multihop=2 retries=1`
- Run 5: `ASSIGNMENT_QUALIFICATION_OK run=5/10 anchors=3 direct=1 multihop=2 retries=1`
- Run 6: `ASSIGNMENT_QUALIFICATION_OK run=6/10 anchors=3 direct=1 multihop=2 retries=1`
- Run 7: `ASSIGNMENT_QUALIFICATION_OK run=7/10 anchors=3 direct=1 multihop=2 retries=0`
- Run 8: `ASSIGNMENT_QUALIFICATION_OK run=8/10 anchors=3 direct=1 multihop=2 retries=1`
- Run 9: `ASSIGNMENT_QUALIFICATION_OK run=9/10 anchors=3 direct=1 multihop=2 retries=1`
- Run 10: `ASSIGNMENT_QUALIFICATION_OK run=10/10 anchors=3 direct=1 multihop=2 retries=1`

The retries were bounded gateway backoffs and each affected run reached the required success terminal. The harness exited 0 after run 10.

## Depth and listener evidence

The first run began without a selected route on all three anchors, so each correctly used the fail-safe maximum window: `hop=0 listen=7530`. Here-I-Am then installed the real route depths. Runs 2-10 consistently logged:

- Direct anchor: `hop=1 listen=3750`.
- Forced-one-hop anchor: `hop=2 listen=4290`.
- Forced-two-hop anchor: `hop=3 listen=4830`.

This proves the listener is driven by the learned route depth in the running firmware, while missing topology lengthens rather than shortens it. Anchors still leave this receive path as soon as they accept the command, so these values are upper bounds for delayed or missing control traffic.

## Verification

- Native focused listener tests: 2/2 passed.
- Mesh source invariants: 92/92 passed.
- Mandatory `mesh_integration` gate: 138/138 passed.
- Mandatory `hardware_models` gate: 133/133 passed.
- Zephyr builds succeeded for gateway, direct anchor, forced-one-hop anchor, and forced-two-hop anchor.
- All four images were staged through `flash_verified_mesh.py` at 4 MHz with live readback before the run.
- The four RTT logs contain no fatal error, assertion, hard fault, watchdog reset, panic, or nonzero reset-reason marker.

This result qualifies F2F1D enumeration and assignment only. It does not claim survey or DS-TWR completion.

## Evidence hashes

- `gateway.log`: `e0445b8086e12976bf23328638bc4d0fe66b10b1421e460abd7ed2d759c33034`
- `direct.log`: `9111af8e3f8d326ec143ed59a74b93b024723d5bcd4a9feb76861d46870b473e`
- `anchor_b.log`: `32c87dda5ecd15ecdc6cd15792777f98e3570bc7e0e2ddeb2d2ac84aa44ef8d6`
- `anchor_c.log`: `c813c42c99cb9a6d263bfef82b39b6c023d4a288e351d8c748b8ba7e77c2ffba`
- `provision.log`: `0b80cc827b79fdccb4f8d9652b8c202c0169a64a2ded441969e3ebd8a06b1871`
