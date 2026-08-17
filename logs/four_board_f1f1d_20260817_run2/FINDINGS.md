# F1F1D survey run findings (2026-08-18)

Topology: gateway `E46070D247233537`, direct anchor `E4645C15CB365D30`
(node `0x56da25fe4af6d141`), forced1b `E46070D247394D36`
(node `0x708bc0aab970300e`), forced1c `E4645C15CB0F3B37`
(node `0x191a9619c6c8cb07`). Images built 22:40-ish local from the dirty
working tree (phase-complete close + waiter retry work) and staged through
`flash_verified_mesh.py`; direct anchor promoted with valid capture
`mesh_anchor-2e002534df8ed88f.json`. All four boards on current images.

## Runs

- Preflight `qualify-reachability` (interactive, not typescripted): failed
  with `expected hop-path evidence for 3 anchors, got 2; expected 2 multihop
  anchors, got 1`.
- Run A (`survey_runA.typescript`, 23:44-23:49): terminal
  `command_status=5` (timeout) `reason=6` (timeout). Host received 2 current-
  generation discovery reports (`0x56da`, `0x191a`) plus one stale report
  from `0x708b` carrying the previous generation id. Gateway:
  `DBG_SURVEY_COLLECTION_COUNT_MISMATCH expected=3 observed=2 missing=1`.
- Run B (`survey.typescript`, 00:00-00:03): same terminal (status=5
  reason=6). 2 current-generation reports (`0x56da`, `0x191a`), and again
  `0x708b`'s report arrived carrying the Run A generation id — exactly one
  generation stale, delivered at the start of the next host session.
  Gateway: discovery flood redriven 4x, `FLOOD_TERMINAL reason=0 attempts=4`,
  `REPORT_ACK_CONFIRM` for `0x56da` and `0x191a`,
  `COLLECTION_COUNT_MISMATCH expected=3 observed=2 missing=1`.
- Run C (`survey_c.typescript`, 00:10-00:12): terminal degraded to
  `command_status=8` (internal error) `reason=13` (internal). Only
  `0x56da`'s current-generation report arrived; `0x191a`'s did not;
  `0x708b`'s stale report now carries the Run B generation id. The number of
  current-generation reports per run went 2 -> 2 -> 1 across A/B/C, i.e.
  retained state appears to accumulate across failed surveys.

## Anchor-side evidence

- forced1b (`0x708b`) in Run B: `DBG_DISCOVERY_SLOT_RESPONSE phase=3
  hop=2 slot=0 attempt=9 ret=0` (its discovery response did transmit) but
  `DBG_DISCOVERY_SLOT_ACK_LOW_DUTY terminal_retries=9 state=ACK_PENDING`
  (never received the slot ACK). Its previous-generation report shows up one
  survey cycle later, so the report is not lost — it is stranded and
  redelivered after the generation it belongs to has ended.
- Direct anchor in Run B relayed `0x708b`'s seq=2 report via
  `DBG_CH9_TX_ACK_CORE_OWNER ... reason=retransmit-direct-gateway` and
  granted `0x708b` an rx-slot; the FCFS serial slot was still occupied by
  the other forced child's phase work.
- No `DBG_DISCOVERY_SLOT_CLAIM_RX` at all on the gateway in Run B despite two
  delivered reports.

## Assessment

The survey consistently cannot collect all three current-generation
discovery reports; the gateway then terminalizes the whole survey before the
pair phase, so no pair schedule, pair starts, or distances are ever produced.
The two forced children contend for the direct's single downstream Channel-9
slot, and the loser's report gets stranded across survey generations instead
of being admitted during the discovery window. Nothing here was fixed during
this session, per instructions.

## Non-protocol notes (self-inflicted, not firmware)

- A `capture_stack_evidence.py` run on the gateway during a live survey
  drops the host BLE session: the capture's pre/post flash readback halts
  the gateway. The first survey attempt at 23:21 died this way
  ("gateway disconnected during active command or monitoring"). Keep
  capture readbacks off the gateway while the provision host is connected.
- Gateway verified-capture remains blocked: the required
  `gateway_priority_control` workload only fires during the survey pair
  phase, which the protocol never reaches on this mix. The forced-hop bench
  captures are valid; their bench-completion topology is blocked on the
  missing gateway capture. Gateway journal left `awaiting_qualification`.
