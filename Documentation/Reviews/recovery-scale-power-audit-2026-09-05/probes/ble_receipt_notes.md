> Historical review/evidence: findings, timings, probe mappings and test totals below belong to the recorded checkout and experiment. Revalidate against the current implementation; use the [documentation index](../../../README.md) for current contracts. Recommendations here are not user-approved requirements.

# BLE receipt blocked by command-result backpressure

The exact current BLE RX worker does not dispatch a receipt behind a command
when result admission returns `-ENOSPC`. Ten worker invocations leave both frames
queued, the receipt counter at one, and both dispatch counts at zero. Moving the
receipt to the front in the fixture restores progress on the next invocation.

Run:

```sh
python3 Documentation/Reviews/recovery-scale-power-audit-2026-09-05/probes/ble_receipt_runner.py
```

The runner extracts `gateway_ble_rx_work_handler()` verbatim from
`firmware/app/src/app_gateway_ble.c:2030` into `ble_receipt_worker.inc`, compiles
the fixture with `-Wall -Wextra -Werror`, and runs each case in a subprocess with
a one-second deadline. `ble_receipt_results.json` records commit, source hash,
worker hash, fixture hash, stdout, exit status, and elapsed time. Exit 1 is the
expected current regression result.

## Exact worker results

| Initial FIFO | Available result credits | Outcome after ten invocations | Elapsed |
| --- | --- | --- | --- |
| Receipt, command | 0 | Receipt and command dispatched; FIFO empty | 0.000653 s |
| Command, receipt | 1 | Command and receipt dispatched; FIFO empty | 0.000663 s |
| Command only | 0 | Command correctly retained; admission returns -28 | 0.000601 s |
| Command, receipt | 0 | Neither dispatched; FIFO still has both; receipt pending remains 1 | 0.000633 s |
| Command, receipt, then fixture reorders the two | 0 | Initially blocked; one subsequent invocation dispatches both after reordering | 0.000611 s |

The last case preserves both frames and changes only their ordering. It
demonstrates that allowing the receipt past the command is sufficient under the
fixture assumptions. It does not implement or validate a concurrent production
queue-reordering change.

The tested boundary assumptions are explicit: the transport is enabled; frame
tags represent valid decoded commands and receipts; queue operations are ordinary
FIFO peek/get; admission returns `-ENOSPC` until a credit is available; consuming
the matching receipt makes one credit available; a dispatched command consumes
that credit. Kernel scheduling, COBS parsing, receipt identity validation, RF,
BLE notification completion, and the production result/stream queues are stubbed.
The evidence proves this worker's dispatch behavior, rather than a complete
hardware deadlock scenario.

## Reachability from current application source

The saturated precondition is reachable by the admission rules in the current
application; this is source analysis, not an executed end-to-end workload:

1. The stream has six slots and 1,756 bytes
   (`app_gateway_ble_stream.h:25`). Locally synthesized command results carry
   `FLAG_GATEWAY_ACK_REQUIRED` (`firmware/src/gateway_command.c:1176`). Assignment
   publication events also use retained host custody. Once one of these heads
   has been notified, ATT completion keeps it for its receipt
   (`app_gateway_ble.c:1234`), preventing later stream records from advancing.
2. Completed or rejected commands can continue producing retained results.
   `gateway_host_command_emit_result()` binds and commits rejection results
   (`app_anchor_gateway_control.inc:4684`); its terminal commit path flushes into
   the stream (`app_gateway_result_runtime.inc:2236`). Thus an absent/delayed
   receipt does not itself stop admission while either output pool has room.
3. Five additional completed results can occupy the result pool after the stream
   fills (`app_gateway_command_result.h:23`). `gateway_flush_host_command_results`
   only pops one after stream admission succeeds; full retained stream capacity
   makes the flush stop without releasing a result credit
   (`app_gateway_result_runtime.inc:879`). Retained records cannot be evicted to
   make room (`app_gateway_ble_stream.c:370`). Result admission then returns
   `-ENOSPC` at occupancy five (`app_gateway_command_result.c:237`).
4. ATT intake reserves raw frame slots independently of result credits
   (`app_gateway_ble.c:529`). Its four-frame FIFO can therefore admit a command
   followed by the needed receipt even while both output pools are full. The
   worker peeks only the command, fails reservation, and stops before reaching
   that receipt (`app_gateway_ble.c:2057`, `2080`).

A command-result-only accumulation needs at most six small retained stream
results followed by five queued results to fill the slot capacities; existing
assignment or report custody can occupy stream capacity instead. The active GUI
may ordinarily serialize commands or send receipts promptly, so this evidence
does not establish how frequently the workload occurs. The firmware accepts its
ordering and has no invariant excluding it.

The separate exact classifier probe confirms that `MSG_SURVEY_EVENT` currently
returns false from `gateway_host_custody_supported`, even with ACK-required set.
TX completion consequently retires such an event at ATT completion. A survey
event alone is therefore not the correct retained-head example for this cycle;
use assignment publication, command results, or retained report custody.

## Existing recovery and smallest repair

Only `gateway_ble_rx_work_handler` removes frames from the static RX message
queue. New arrivals resubmit the same worker; repeating that work cannot bypass
the blocked head, as the fixture demonstrates. The one-second receipt timeout
rewinds and resends the same stream head (`app_gateway_ble.c:873`), and reconnect
resets partial-byte/TX state while preserving the RX FIFO and retained stream
(`app_gateway_ble.c:1638`, `1712`). Neither consumes the blocked receipt.

An independently freed result reservation or stream slot can restore progress,
so a temporarily full pool need not deadlock. The problematic case is the full
completed-result pool behind retained stream custody whose releasing receipt is
itself behind the command. No command-result expiry or alternative RX consumer
was found. A hardware watchdog reset was not modeled or measured here.

The smallest functional repair is to let already queued, validated receipts pass
commands that lack result credit while preserving command-to-command FIFO order.
A bounded extraction/priority path needs to remain safe against concurrent ATT
enqueue; merely resubmitting the worker does not help. Reserving receipt ingress
capacity within the existing bounded storage also prevents four blocked commands
from excluding the receipt before it reaches the FIFO. The fixture's reordering
case is evidence for the dispatch change, not a claim that unchecked get/put
rotation would be a safe implementation.
