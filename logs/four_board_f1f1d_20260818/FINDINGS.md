# Four-Board F1F1D Survey Test Findings (2026-08-18)

## Test Configuration
- **Gateway**: Probe `E46070D247233537` (`mesh_gateway`)
- **Direct Anchor**: Probe `E4645C15CB365D30` (`mesh_anchor`, node `0x56da25fe4af6d141`)
- **Forced-Hop Anchor B**: Probe `E46070D247394D36` (`mesh_anchor_forcedhop`, depth 1, node `0x708bc0aab970300e`)
- **Forced-Hop Anchor C**: Probe `E4645C15CB0F3B37` (`mesh_anchor_forcedhop`, depth 1, node `0x191a9619c6c8cb07`)

## Protocol Sequence and Observations
1. **Survey Initiation**:
   - Gateway successfully broadcast the survey discovery command (`command_kind=2, command_id=256`).
2. **Discovery Uplinks**:
   - Direct anchor `0x56da25fe4af6d141` delivered its discovery report directly to the gateway.
   - Forced-hop Anchor B `0x708bc0aab970300e` negotiated Channel-9 timing with direct anchor `0x56da25fe4af6d141` and uploaded its discovery report via the relay.
3. **Cadence Bottleneck & Starvation**:
   - Anchor B retained the single downstream Channel-9 cadence on the direct anchor after its discovery report was ACKed.
   - Forced-hop Anchor C `0x191a9619c6c8cb07` continuously transmitted Channel-5 wake claims and route solicitations, but could not obtain a Channel-9 rhythm on direct anchor because the single downstream slot was occupied by Anchor B.
4. **Outcome**:
   - Gateway observed 2 out of 3 expected discovery reports (`DBG_SURVEY_COLLECTION_COUNT_MISMATCH expected=3 observed=2 missing=1`).
   - Discovery phase expired with timeout (`command_status=5, reason=6`).
