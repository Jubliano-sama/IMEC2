# F1DD ten-run enumeration result

Status: PASS (10/10)

The already flashed, verified F1DD cohort was exercised through ten fresh
RAM-only assignment operations over one BLE connection. Every operation
reported three anchors with the expected topology:

- `0x56da25fe4af6d141`: direct, hop 1, slot 0.
- `0x191a9619c6c8cb07`: direct, hop 1, slot 1.
- `0x708bc0aab970300e`: forced, hop 2, slot 2.

Per-run terminals:

- Runs 1, 2, 4, and 6: `anchors=3 direct=2 multihop=1 retries=0`.
- Runs 3, 5, 7, 8, 9, and 10: `anchors=3 direct=2 multihop=1 retries=1`.

The host terminalized normally with `BLE_COMPLETE packets=152` and exit code
zero. No operation failed, no topology changed, and no board rebooted during
the ten operations.

Pre-test caveat: the gateway's controlled RTT pre-reset exposed one retained
fatal breadcrumb from before this ten-run sequence. Address symbolication
identifies a Nordic SoftDevice Controller assertion (`reason=4`,
`sdc_assertion_handler` -> `assert_post_action`) on the idle thread. The
capture then contains one clean gateway boot before run 1 and no later boot or
fatal marker. This is separate BLE-controller crash evidence, not an
enumeration failure, and no firmware fix was applied.

Evidence SHA-256:

- `provision.log`: `98927c878fdc72542db9b00289464b2cdc54fb2711fb69cc70d6ad07385adc62`
- `gateway.log`: `042899fd722ad972431700cf1d06d7ad5584f72ddfbb82b3a9f2943e8324b677`
- `direct.log`: `2a4190fa3a249757e6660a33132c5861c5856a7a1cbd8477a02be843fa887361`
- `anchor_b.log`: `fb0b9654059445f6ff0ac2cb486ea5f5265b7b91100fc79043d888570015d4a6`
- `anchor_c.log`: `a91cc6a5cfdedaef0cf214fc95708a63c60ed6e92f36eed19e85c8e82fe07bdf`

This qualifies repeated F1DD enumeration and assignment only. It does not
claim DS-TWR survey qualification.
