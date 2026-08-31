# Protocol timelines

- `four_board_qualification_status`: current end-to-end qualification map.
- `enumeration_slot_contract`: implemented depth/slot-ordered CLAIM responses and exact TABLE ACK_CONFIRM quorum, shown in parallel for D-F1-F2.
- `survey_enumeration_slot_proposal`: implemented enumeration-aware survey model with exact 200 ms Channel-5 probe slots, child-owned depth/slot-ordered first contact, one randomized same-parent retry before route fallback, and a topology-scaled GUI countdown.
- `survey_depth3_upstream_retransmit_early`: exact four-probe reconstruction of B receiving C's report, then transmitting it toward A at the preparation boundary before A arms RX; the fixed retry waits for the negotiated send offset.
- `survey_three_pair_budget`: measured discovery plus the serialized pair-control schedule, showing why a 244 s failure-derived release cannot fit and how separate 5 s redrives and a 45 s shared execution barrier fit the same six-minute operation.
- `survey_depth3_uplink_route_wait_collision`: superseded first interpretation of the depth-3 uplink failure, retained as historical diagnosis evidence.
- `survey_depth3_deeper_relay_followup`: resolved first depth-3 survey failure; the 2-second deeper-relay listener is now live-proven.
- `survey_depth3_ack_overlap`: retained historical trace for the earlier Channel-5/Channel-9 overlap bug.
- `survey_parallel_deadline_feasibility`: real-time parallel proof that the 90 s survey start still covers all origin redrives, the legal four-hop propagation envelope, and physical preparation.
- `single_radio_parallel_feasibility`: real-time before/after proof that one 2 s Channel-5 contact cannot coexist with five Channel-9 turns, and that the documented control priority is feasible when Channel-9 custody is retained and rearmed after the exclusive contact.
- `relay_two_cadence_feasibility`: parallel radio proof that a 520 ms two-connection cadence leaves only 20 ms between 240 ms Channel-9 owners while the scanner needs 60 ms to retune plus 20 ms to receive; it shows the simple 640 ms cadence with two complete scan gaps.
- `click_three_anchor_feasibility`: retained path name for the current normal-click timing proof; four anchors receive three sequential 33 ms exchanges each inside the 400 ms burst while mesh custody waits without competing for the radio. The opt-in RTT clicker injection path is also hardware-proven end to end across DDD, F1DD, F1F1D, and F2F1D: each three-anchor topology accepted three RTT clicks, completed 36/36 DS-TWR exchanges (12 per click), and delivered 9/9 anchor reports through the externally powered gateway to the host. Normal battery images keep RTT injection disabled.
- `survey_wake_post_ch9_recovery`: four-probe trace showing the forced relay's 3 ms scan missing a live wake train after adjacent Channel-9 reservations, and the one-shot 20 ms recovery scan that removes the phase lock.

These CSV bundles use the legacy renderer removed by TimeLineCreator commit
`13bec663c`; the current `timeline_creator` command starts the studio instead.
Render a bundle from a detached worktree at that commit's parent:

```sh
git -C /home/tommie/Projects/TimeLineCreator worktree add --detach \
  /tmp/timeline-csv-renderer 13bec663c^
PYTHONPATH=/tmp/timeline-csv-renderer python3 -m timeline_creator \
  Documentation/protocol_timelines/<bundle> \
  --out Documentation/protocol_timelines/<bundle>/diagram.svg
git -C /home/tommie/Projects/TimeLineCreator worktree remove \
  /tmp/timeline-csv-renderer
```
