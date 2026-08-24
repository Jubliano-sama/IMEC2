# F2F1D enumeration result

Status: PASS

Fresh artifacts from commit `0d35c23f4` were built and flashed through the
verified transaction wrapper. The four target code images were read back after
staging, and the successful enumeration transcript was then bound to a second
live readback for each board.

Exact terminal:

`HERE_I_AM_REACHABILITY_QUALIFICATION_OK anchors=3 direct=1 multihop=2 retries=0`

Observed topology:

- Gateway probe `E46070D247233537`: `mesh_gateway`, node
  `0x9999888877776666`.
- Direct probe `E4645C15CB365D30`: `mesh_anchor`, node
  `0x56da25fe4af6d141`, hop 1, slot 0.
- Forced-one-hop probe `E46070D247394D36`: `mesh_anchor_forcedhop` with
  `IMEC_FORCED_GATEWAY_RELAY_HOPS=1`, node `0x708bc0aab970300e`, hop 2 through
  `0x56da25fe4af6d141`, slot 1.
- Forced-two-hop probe `E4645C15CB0F3B37`: `mesh_anchor_forcedhop` with
  `IMEC_FORCED_GATEWAY_RELAY_HOPS=2`, node `0x191a9619c6c8cb07`, hop 3 through
  `0x708bc0aab970300e` and `0x56da25fe4af6d141`, slot 2.

The first flashed run passed. Here-I-Am needed one local flood attempt,
assignment needed one flood attempt, all three TABLE confirmations arrived,
and the host used zero whole-operation retries. The four RTT streams contain
no fatal, assertion, watchdog, hard-fault, reboot, or stack-overflow marker.

Artifact cohorts:

- Gateway, direct, and forced-one-hop:
  `ccac0f3c2cea91e4ef61343725829e6a1b0b85730e4b78637a1a9d29b7deea3c`.
- Forced-two-hop:
  `f341e3df8c1e4680c3b1e9eb8234fd036a3ee5e44bd89a5a82154e67790e607e`.

Evidence SHA-256:

- `provision.log`: `c5e9abede3a88c515d93bd1f3a58a6c98e15a281a795c23fafd882725d83d3e3`
- `gateway.log`: `c8ed503347792aa249665666e73ab87ce7fa3a69bfb9effcb2844c220b10b840`
- `direct.log`: `973d77644052d518dda347c8090258ff2cbd20ac4aa6c71e04d79c5e6d68031f`
- `anchor_b.log`: `7615217b3c5c0ef36630d616f51bc86bc495dbe27400604f30b68fb337e65b50`
- `anchor_c.log`: `6d91ed726347c4d22be5b69e762ee8068598d168a82715432f5d05298322851f`

This result qualifies F2F1D enumeration and assignment only. Survey code is
absent from this branch, and this run makes no DS-TWR survey claim.
