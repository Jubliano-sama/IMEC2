# Four-board survey mixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. HIL-first, inline in this session. Do not revive the sibling initiator START. Do not add a second downstream Channel-9 cadence. Do not commit unless the user asks.

**Goal:** On the four probed boards, DDD, F1DD, F1F1D, then F2F1D each pass `provision_mesh_anchor.py --command survey --require-survey-success --expected-anchors 3 --expected-pairs 3` without mid-survey route repair.

**Architecture:** One upstream and one downstream Channel-9 rhythm per anchor. F1F1D shares the production direct’s one downstream slot first-come, first-served: the occupant `EVENT_END`s after its current assignment/survey phase has no remaining local or transit uplink; the waiter retries `PROPOSE` on the same parent. Survey-generation gateway-ACK timeouts retry that parent through the operation deadline instead of hold-down or discovery. Pair control stays one tracked START identity per endpoint.

**Tech Stack:** Zephyr/nRF52833 firmware, native CTest, `mesh_integration` / `hardware_models`, `flash_verified_mesh.py`, `provision_mesh_anchor.py`, pyOCD RTT.

**Spec:** `docs/superpowers/specs/2026-08-17-four-board-survey-mixes-design.md`

---

## Boards and flashes

| Role | Probe |
| --- | --- |
| Gateway | `E46070D247233537` |
| Direct | `E4645C15CB365D30` |
| Anchor B | `E46070D247394D36` |
| Anchor C | `E4645C15CB0F3B37` |

Flash only through `firmware/scripts/flash_verified_mesh.py` at 4 MHz. RTT: `pyocd rtt -t nrf52833 -M pre-reset -u <probe-id>` under `script`, never redirected stdout.

HIL command:

```sh
.venv/bin/python firmware/scripts/provision_mesh_anchor.py \
  --gateway 'IMEC Mesh Test Gateway' \
  --command survey \
  --require-survey-success \
  --expected-anchors 3 \
  --expected-pairs 3 \
  --connect-timeout 30 \
  --route-refresh-timeout 90 \
  --assignment-timeout 180
```

Working-tree survey changes already in flight (keep, do not revert): staged batch load, `cleanup_required_mask` duplicate-safe complete, sample admit in `ARMED` as well as `OBSERVING`, sibling START age bypass removed.

---

### Task 1: Diagnose DDD from traces, then HIL

**Files:**
- Read: `logs/four_board_ddd_20260816_spaced/survey.typescript` (3 reports, pair `0x36e3`–`0x708b` five samples then status 5 reason 11, other two pairs OK)
- Read: `logs/four_board_ddd_20260816_startfix/survey3.typescript` (0 reports)
- Read: `logs/four_board_ddd_20260816_antdly/survey.typescript`
- Modify only after a current-tree cause is identified

- [ ] **Step 1: Classify the latest DDD miss**

Spaced DDD delivered all three discovery reports. Pair `0x36e3c2fe6cac46b2` / `0x708bc0aab970300e` uploaded five `MSG_SURVEY_PAIR_RESULT` frames and still terminalized as timeout/reason 11. The other two pairs succeeded.

`logs/four_board_ddd_20260816_antdly/survey.typescript` later passed: `SURVEY_QUALIFICATION_OK` 3 anchors / 3 pairs / 3 distances. That miss was the known negative-ToF calibration on `0x36e3`–`0x708b`, not pair START identity. Do not change antenna delay again unless a new DDD run on this tree fails the same edge.

The remaining DDD job is to prove the current working tree still passes, because survey START/sample/cleanup edits landed after that capture.

- [ ] **Step 2: Reproduce DDD on the current tree**

Three `mesh_anchor` + `mesh_gateway`. Capture gateway + three anchor RTT under `script`. Run the provision command above. Save under `logs/four_board_ddd_<date>/`.

- [ ] **Step 3: If protocol, write the failing native or scenario test first**

Likely homes:
- `firmware/tests/test_survey_pair_round_runtime.c` for sample/lane state
- `firmware/tests/test_app_gateway_survey_round.c` for START/sample/terminal
- `firmware/tests/mesh_integration/` survey scenarios if the bug is delivery/radio

Run the focused test and confirm it fails for the real reason.

- [ ] **Step 4: Fix the smallest current-tree cause**

Do not reintroduce sibling START. Do not stretch the 15 s start barrier into a route-repair horizon.

- [ ] **Step 5: Re-run the focused test, then DDD HIL until 3/3 or the only miss is calibration**

Also run:

```sh
ctest --test-dir firmware/build --output-on-failure -R 'survey_pair_round|app_gateway_survey_round'
```

If routing, Channel-9, ACK, or pair control changed:

```sh
ctest --test-dir firmware/build -L mesh_integration --output-on-failure
ctest --test-dir firmware/build -L hardware_models --output-on-failure
```

Those two labels must not run concurrently.

---

### Task 2: HIL `forced-1 direct direct`

**Files:** none until a new failure appears.

- [ ] **Step 1: Flash** gateway `mesh_gateway`, one `mesh_anchor_forcedhop` hop-1, two `mesh_anchor`.
- [ ] **Step 2: Run the same provision survey bar.**
- [ ] **Step 3: If it fails, diagnose from RTT, add a regression, fix, re-HIL.** Same rules as Task 1.

This mix has one child of one direct. Do not implement sibling slot sharing yet.

---

### Task 3: Phase-complete close (needed before F1F1D)

**Files:**
- Modify: `firmware/app/src/app_mesh_report_event_tx.inc` (`mesh_close_channel9_connection`, ~2556)
- Modify: `firmware/app/src/app_mesh_report_route_control.inc` (ACK-complete close, ~1738)
- Modify: production EVENT_END receive path already clears timing; confirm `mesh_anchor` accepts END
- Test: `firmware/tests/test_app_mesh_ch9_ack.c`
- Test: `firmware/tests/mesh_integration/test_mesh_event_proposal_arbitration_source_invariants.py`
- Test: new focused native or scenario test for “phase work done ⇒ END; transit remaining ⇒ no END”

- [ ] **Step 1: Failing test — occupant with no remaining phase work sends `EVENT_END`**

Phase work means assignment response, discovery report, or current pair-batch result, including transit and ACK-confirm. Pair radio lease still owned ⇒ no close.

- [ ] **Step 2: Failing test — occupant with remaining transit or ACK-confirm does not close**

- [ ] **Step 3: Run those tests; they fail**

- [ ] **Step 4: Implement preferred close from the occupying child**

`CONFIG_IMEC_MESH_ROUTE_TEST` is already on production `mesh_anchor`. Change `app_mesh_ch9_ack_complete_should_close_timing()` (or a sibling helper) so it returns true when this node’s current phase has no remaining local or transit uplink. Send `EVENT_END` only to a cadence parent, never to the gateway. The parent that receives END drops that downstream rhythm. Do not enable a second downstream cadence.

- [ ] **Step 5: Confirm idle / missed-event close still frees the slot if END never goes out**

- [ ] **Step 6: Run the new tests plus `test_app_mesh_ch9_ack` and the event-proposal source invariants**

---

### Task 4: Waiter stays on the parent; survey does not discover

**Files:**
- Modify: `firmware/app/src/app_mesh_report_rx.inc` `mesh_handle_ch9_gateway_ack_timeout_route_failure` (~217)
- Modify: `firmware/src/mesh_relay_delivery.inc` `mesh_relay_note_delivery_failure_at` / `schedule_pending_parent_failure` (~1040)
- Modify: `firmware/src/route.c` only if a survey-safe action can be expressed without breaking the normal 3-retry / hold-down contract
- Test: `firmware/tests/test_route.c`
- Test: `firmware/tests/mesh_integration/test_mesh_route_recovery_scenarios.c`

- [ ] **Step 1: Failing test — rejected `PROPOSE` because parent already has a downstream rhythm does not increment gateway-ACK failure, does not hold-down, does not `ROUTE_DELIVERY_DISCOVER`**

- [ ] **Step 2: Failing test — while a survey generation is active, a gateway-ACK timeout on the selected parent returns retry-current through the operation deadline**

Assignment response window uses the same attach-retry rule.

- [ ] **Step 3: Run; they fail**

- [ ] **Step 4: Implement**

A rejected downstream-occupied `PROPOSE` is cadence wait, not route failure. Re-opening Channel-9 on the same parent after phase-complete close is cadence setup, not repair.

- [ ] **Step 5: Run `test_route`, route-recovery scenarios, and the new tests. If those files change, run `mesh_integration` then `hardware_models` serially.**

---

### Task 5: HIL `forced-1 forced-1 direct`

- [ ] **Step 1: Flash** gateway, two hop-1 `mesh_anchor_forcedhop`, one `mesh_anchor`.
- [ ] **Step 2: Confirm RTT shows FCFS:** one child END after its phase, the other PROPOSE after that close, both assignment responses and both discovery reports, then pair results, no `ROUTE_DELIVERY_DISCOVER` during the survey.
- [ ] **Step 3: Provision survey bar. On failure, regress and fix before Task 6.**

---

### Task 6: HIL `forced-2 forced-1 direct` last

- [ ] **Step 1: Flash** gateway, hop-2 forced, hop-1 forced, production direct (linear chain).
- [ ] **Step 2: Provision survey bar.**
- [ ] **Step 3: Depth-3 failures get a regression and a fix. Still no second cadence and no sibling START.**

---

### Task 7: Pair START identity guard (only if a new trace shows drift)

**Files:** `firmware/app/src/app_gateway_survey_round.c`, `firmware/src/survey_round_control.c`, `firmware/tests/test_app_gateway_survey_round.c`

- [ ] Current tree already removed the sibling age bypass and uses one tracked START per endpoint. Add a test only if HIL or review shows a second identity again.

---

## Out of scope

- Two simultaneous downstream cadences
- Antenna-delay / geometry solver (ask the user if DDD’s only miss is `0x36e3`–`0x708b` ToF)
- Reviving untracked sibling START
- NVS for hot retry state
