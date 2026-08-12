# Click-handoff production seam

The first native_sim ztest compiles and invokes the exact click-handoff block from
`app_anchor_radio.inc`. It runs the production priority relationship (repair
queue priority 0, click queue priority 1), the generation-bearing radio guard,
the six-record `app_node_comm` facade, and the digest-bound forwarded-ACK
authorization helpers. A barrier holds the click queue after publication, so
the repair is runnable before and during click RF; both attempts must retain
the token and fail at the real radio guard. After the transferred click lease
is released, the same token starts RF and is consumed exactly once.

This is deliberately labeled `composition` and `future_facade`: its
`mesh_range_report_batch_reserve()` adapter reserves the real six-slot
`app_node_comm` facade, while current production click reports use a separate
nine-slot report queue.

The `report_custody` ztest links the exact production `app_mesh_report.c`
reservation, fragment, journal, and immutable-head helpers. It proves that a
maximum nine-fragment click reserves all nine report slots, rejects a competing
batch, preserves the click identity in its journal, and returns a byte-identical
head after the same abort/reacquire operation used by the transient retry path.
Only committing that head exposes fragment two, so the test also guards FIFO
custody and exact-once removal.

The `gateway_result_actions` ztest builds the real gateway relay and
result-handoff helper. It proves that a direct anchor's large result offer is
granted (or gets an exact BUSY response under contention), that the accepted
result reaches local delivery and produces its gateway ACK only after commit,
and that non-anchor hop-ACK handling remains reachable. Its companion source
invariant prevents role-size gates from compiling those action dispatches out
of the gateway image.

The test intentionally exposes two remaining composed-link gaps instead of
claiming a full application proof:

- Production `mesh_range_report_batch_reserve()` owns the monolithic
  report queue and does not use an `app_node_comm_reservation_lease`. The test
  implements that existing callback boundary as a small adapter to the real
  six-slot facade, so it proves the two owners compose but does not prove that
  production click reports already reserve node-communication slots.
- The 27k-line `app_mesh_report` translation unit is not linked. The repair
  worker validates a real digest-bound capability and claims the real radio
  guard, but the outer `mesh_handoff_anchor_click_claim()` call is represented
  by publishing its click-window bit before invoking the exact anchor callback,
  and the monolithic coordinator/retry plumbing, node-communication RF backend,
  and DWM3000 effects are represented by the harness. Exact-role builds and the
  three-board workload remain the proof for those boundaries.

Run it with the host qualifier required by this workspace:

```sh
CCACHE_DISABLE=1 .venv/bin/west build --no-sysbuild -p always \
  -b native_sim/native/64 \
  -d /tmp/imec2-production-seam-click-handoff \
  firmware/tests/mesh_integration/production_seam_click_handoff
/tmp/imec2-production-seam-click-handoff/zephyr/zephyr.exe
```

Twister can discover the same scenario through `testcase.yaml`.

The top-level firmware CMake also registers the scenario under
`mesh_integration`, `hardware_models`, and `protocol_matrix`. Its CTest wrapper
locks the generated Zephyr directory, so independent label jobs cannot run
two pristine builds over the same files.

Run the exact production report-custody seam with:

```sh
CCACHE_DISABLE=1 .venv/bin/west build --no-sysbuild -p always \
  -b native_sim/native/64 \
  -d /tmp/imec2-production-seam-report-custody \
  firmware/tests/mesh_integration/production_seam_click_handoff/report_custody
/tmp/imec2-production-seam-report-custody/zephyr/zephyr.exe
```
