# F1DD enumeration result

Status: PASS

The verified cohort `7c997c447c9234b17e42a69f38dbc79ee84057d6ab4f1f75de96e0eb5de8f4b8`
was flashed and read back on one gateway, two direct anchors, and one
`mesh_anchor_forcedhop` anchor. The successful host command used gateway BLE
address `E0:85:31:10:C4:17`.

Exact terminal:

`HERE_I_AM_REACHABILITY_QUALIFICATION_OK anchors=3 direct=2 multihop=1 retries=0`

Observed topology:

- `0x56da25fe4af6d141`: direct, hop 1, slot 0.
- `0x191a9619c6c8cb07`: direct, hop 1, slot 1.
- `0x708bc0aab970300e`: forced, hop 2 through `0x191a9619c6c8cb07`, slot 2.

The first host attempt used the gateway's pre-flash BLE address and failed
before submitting any radio command. It is retained as host-diagnostic
evidence and is not an enumeration failure.

Evidence SHA-256:

- `provision-retry.log`: `bdb50fd43d62fe7a1755564fde119ae34f9874d191ff0daf8bd3179d79a23860`
- `gateway.typescript`: `e7cf791ad38d55f57fe4a6a4fc928931b29ba4de311f2ff3bced3ff9f9a45217`
- `forced.typescript`: `fe269f2e8921f535e88fbd494f7a80b12cbac2cd8bf0ad1b0899e693f04b5cd3`
- `direct_b.typescript`: `b9ba118e11e6e3070604e6d97f2107f8b9cbc8a7f79490799a47d29f989f91e1`
- `direct_c.typescript`: `c27d62f9b3c02d3386013c5bd936559bbc2ba4707d5c4ad92d01911e5ce0bb63`

This result qualifies F1DD enumeration and assignment only. It does not claim
terminal DS-TWR survey qualification.
