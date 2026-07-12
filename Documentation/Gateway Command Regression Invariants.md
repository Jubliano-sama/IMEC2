# Gateway Command Regression Invariants

The `gateway_gui_cross_layer` and `gateway_command_lifecycle_invariants` tests
are labeled `mesh_integration`, `hardware_models`, and `deployment`. Together
with the generated GUI property suite, failures report these contract
invariants rather than one historical trace:

- `command_lifecycle_single_owner_eventual_release`
- `telemetry_dedup_totals_and_order_converge`
- `topology_is_set_algebra_and_explicitly_persisted`
- `survey_builder_accepts_exact_valid_partition_only`
- `local_report_custody_survives_route_absence_and_transit_pressure`
- `stack_and_queue_capacity_are_compile_time_bounded`

Historical five-command bursts and exact GUI byte captures remain deterministic
seeds within these broader exhaustive and generated checks.
