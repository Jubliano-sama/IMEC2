# Protocol timelines

- `four_board_qualification_status`: current end-to-end qualification map.
- `survey_depth3_upstream_retransmit_early`: exact four-probe reconstruction of B receiving C's report, then transmitting it toward A at the preparation boundary before A arms RX; the fixed retry waits for the negotiated send offset.
- `survey_three_pair_budget`: measured discovery plus the serialized pair-control schedule, showing why a 244 s failure-derived release cannot fit and how separate 5 s redrives and a 45 s shared execution barrier fit the same six-minute operation.
- `survey_depth3_uplink_route_wait_collision`: superseded first interpretation of the depth-3 uplink failure, retained as historical diagnosis evidence.
- `survey_depth3_deeper_relay_followup`: resolved first depth-3 survey failure; the 2-second deeper-relay listener is now live-proven.
- `survey_depth3_ack_overlap`: retained historical trace for the earlier Channel-5/Channel-9 overlap bug.
- `survey_parallel_deadline_feasibility`: real-time parallel proof that the 90 s survey start still covers all origin redrives, the legal four-hop propagation envelope, and physical preparation.
- `single_radio_parallel_feasibility`: real-time before/after proof that one 2 s Channel-5 contact cannot coexist with five Channel-9 turns, and that the documented control priority is feasible when Channel-9 custody is retained and rearmed after the exclusive contact.
- `relay_two_cadence_feasibility`: parallel radio proof that a 520 ms two-connection cadence leaves only 20 ms between 240 ms Channel-9 owners while the scanner needs 60 ms to retune plus 20 ms to receive; it shows the simple 640 ms cadence with two complete scan gaps.
- `click_three_anchor_feasibility`: real-time proof that a normal three-anchor click fits eight sequential 50 ms exchange reservations in its 400 ms burst while mesh custody waits without competing for the radio.
- `survey_wake_post_ch9_recovery`: four-probe trace showing the forced relay's 3 ms scan missing a live wake train after adjacent Channel-9 reservations, and the one-shot 20 ms recovery scan that removes the phase lock.

Render a bundle with:

```sh
PYTHONPATH=/home/tommie/Projects/TimeLineCreator \
python3 -m timeline_creator Documentation/protocol_timelines/<bundle> \
  --out Documentation/protocol_timelines/<bundle>/diagram.svg
```
