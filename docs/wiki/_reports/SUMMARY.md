# Wiki Documentation Summary

Generated: 2026-07-26 13:34:47
Repository: IMEC2
Commit: `af15a7eb59b1ca8f75464506ffa97f980ffbfef7`
Wiki state SHA-256: `9518e5100f2995b4031f21ab8419eba438c3885b2a32e8650dbb5ea664b79e54`

## Generation Status

**Overall Status**: ✅ Complete

| Metric | Expected | Actual | Status |
|--------|----------|--------|--------|
| Pages | 14 | 14 | ✅ |
| Sections | 56 | 56 | ✅ |
| Citations | - | 991 | ✅ |
| Diagrams | 9 | 9 valid | ✅ |

## Page Details

| Page | Title | Sections | Citations | Diagrams | Status |
|------|-------|----------|-----------|----------|--------|
| README.md | Start Here: From the User Story to the System | 4/4 | 34 | 1 | ✅ |
| 01_product-roles-and-firmware-lines.md | Product Roles and Firmware Lines | 4/4 | 59 | 0 | ✅ |
| 02_one-click-end-to-end.md | One Click, End to End | 4/4 | 64 | 1 | ✅ |
| 03_uwb-wake-ranging-and-power.md | UWB Wake, Ranging, and Low-Power Radio | 4/4 | 108 | 1 | ✅ |
| 04_protocol-packets-and-data-contracts.md | Protocol, Packets, and Data Contracts | 4/4 | 125 | 0 | ✅ |
| 05_connected-routing-and-reliable-delivery.md | Connected Routing, Priority, and Reliable Delivery | 4/4 | 53 | 2 | ✅ |
| 06_anchor-identity-discovery-and-assignment.md | Anchor Identity, Discovery, and Assignment | 4/4 | 65 | 0 | ✅ |
| 07_anchor-self-setup-survey-and-geometry.md | Anchor Self-Setup: Survey and Geometry | 4/4 | 76 | 1 | ✅ |
| 08_data-custody-persistence-and-recovery.md | Data Custody, Persistence, and Recovery | 4/4 | 75 | 1 | ✅ |
| 09_gateway-host-tools-and-observability.md | Gateway, Host Tools, and Observability | 4/4 | 95 | 0 | ✅ |
| 10_build-presets-and-configuration.md | Build Presets, Configuration, and Repository Boundaries | 4/4 | 83 | 0 | ✅ |
| 11_verified-deployment-and-qualification.md | Verified Mesh Deployment and Hardware Qualification | 4/4 | 47 | 1 | ✅ |
| 12_testing-simulation-and-release-evidence.md | Testing, Simulation, and Release Evidence | 4/4 | 58 | 0 | ✅ |
| 13_hardware-bring-up-and-troubleshooting.md | Hardware Bring-Up and Troubleshooting | 4/4 | 49 | 1 | ✅ |

## Source Coverage

### Covered Files

- `.github/workflows/firmware-verification.yml` - cited in 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `.github/workflows/mesh-deployment-policy.yml` - cited in 11_verified-deployment-and-qualification.md
- `AGENTS.md` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 10_build-presets-and-configuration.md, 11_verified-deployment-and-qualification.md, 12_testing-simulation-and-release-evidence.md, 13_hardware-bring-up-and-troubleshooting.md
- `AGENT_KNOWN_ISSUES.md` - cited in 12_testing-simulation-and-release-evidence.md, 13_hardware-bring-up-and-troubleshooting.md
- `CODEMAP.md` - cited in 01_product-roles-and-firmware-lines.md, 10_build-presets-and-configuration.md
- `Documentation/Architecture Reset Plan.md` - cited in 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `Documentation/Customer Needs.md` - cited in README.md
- `Documentation/Development and Deployment Guide.md` - cited in 10_build-presets-and-configuration.md, 11_verified-deployment-and-qualification.md, 13_hardware-bring-up-and-troubleshooting.md
- `Documentation/From Click to Ranging, Developing a Robust UWB Gated Wake up Strategy.md` - cited in 02_one-click-end-to-end.md
- `Documentation/Gateway BLE Streaming.md` - cited in 09_gateway-host-tools-and-observability.md
- `Documentation/Gateway Command Observability.md` - cited in 09_gateway-host-tools-and-observability.md
- `Documentation/Gateway Here-I-Am Stress Coverage.md` - cited in 06_anchor-identity-discovery-and-assignment.md
- `Documentation/HARDWARE_BRINGUP_DEBUG.md` - cited in 13_hardware-bring-up-and-troubleshooting.md
- `Documentation/Mesh Connected Routing Contract.md` - cited in 05_connected-routing-and-reliable-delivery.md, 06_anchor-identity-discovery-and-assignment.md
- `Documentation/Mesh Integration Coverage Matrix 2026-07-12.md` - cited in 12_testing-simulation-and-release-evidence.md
- `Documentation/Stakeholder Requirements.md` - cited in README.md
- `Documentation/UWB+BLE Architecture 0.6.6.2.md` - cited in 02_one-click-end-to-end.md, 03_uwb-wake-ranging-and-power.md
- `Documentation/UWB+BLE Protocols and Strategies 0.3.12.4.md` - cited in 04_protocol-packets-and-data-contracts.md, 07_anchor-self-setup-survey-and-geometry.md
- `Documentation/narrative(user story).md` - cited in 07_anchor-self-setup-survey-and-geometry.md, README.md
- `Documentation/user requirements.md` - cited in README.md
- `README.md` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 07_anchor-self-setup-survey-and-geometry.md, 09_gateway-host-tools-and-observability.md, 12_testing-simulation-and-release-evidence.md, 13_hardware-bring-up-and-troubleshooting.md, README.md
- `firmware/CMakeLists.txt` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `firmware/README.md` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 07_anchor-self-setup-survey-and-geometry.md, 09_gateway-host-tools-and-observability.md, 12_testing-simulation-and-release-evidence.md, 13_hardware-bring-up-and-troubleshooting.md, README.md
- `firmware/app/CMakeLists.txt` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `firmware/app/Kconfig` - cited in 03_uwb-wake-ranging-and-power.md, 10_build-presets-and-configuration.md
- `firmware/app/app.overlay` - cited in 10_build-presets-and-configuration.md
- `firmware/app/conf/mesh-anchor.conf` - cited in 01_product-roles-and-firmware-lines.md, 10_build-presets-and-configuration.md
- `firmware/app/conf/mesh-clicker.conf` - cited in 01_product-roles-and-firmware-lines.md, 10_build-presets-and-configuration.md
- `firmware/app/conf/role-gateway.conf` - cited in 01_product-roles-and-firmware-lines.md
- `firmware/app/prj-clicker.conf` - cited in 10_build-presets-and-configuration.md
- `firmware/app/prj-gateway.conf` - cited in 10_build-presets-and-configuration.md
- `firmware/app/prj.conf` - cited in 10_build-presets-and-configuration.md
- `firmware/app/src/app_anchor.c` - cited in 02_one-click-end-to-end.md
- `firmware/app/src/app_anchor_gateway_survey_round.inc` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/app/src/app_anchor_low_power_policy.h` - cited in 03_uwb-wake-ranging-and-power.md
- `firmware/app/src/app_anchor_survey_runtime.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/app/src/app_clicker.c` - cited in 02_one-click-end-to-end.md
- `firmware/app/src/app_config.h` - cited in 01_product-roles-and-firmware-lines.md, 02_one-click-end-to-end.md, 03_uwb-wake-ranging-and-power.md, 10_build-presets-and-configuration.md
- `firmware/app/src/app_device_identity.c` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/app/src/app_gateway_assignment_publisher.c` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/app/src/app_gateway_ble.c` - cited in 02_one-click-end-to-end.md, 08_data-custody-persistence-and-recovery.md, 09_gateway-host-tools-and-observability.md
- `firmware/app/src/app_gateway_ble_stream.c` - cited in 02_one-click-end-to-end.md, 08_data-custody-persistence-and-recovery.md, 09_gateway-host-tools-and-observability.md
- `firmware/app/src/app_gateway_command_observability.c` - cited in 09_gateway-host-tools-and-observability.md
- `firmware/app/src/app_gateway_survey_round.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/app/src/app_mesh_gateway_command_flow.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/app/src/app_mesh_persistence.c` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/app/src/app_mesh_report.c` - cited in 02_one-click-end-to-end.md
- `firmware/app/src/app_watchdog.c` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/app/src/dwm3000_driver.c` - cited in 03_uwb-wake-ranging-and-power.md
- `firmware/app/src/dwm3000_port.c` - cited in 13_hardware-bring-up-and-troubleshooting.md
- `firmware/app/src/main.c` - cited in 01_product-roles-and-firmware-lines.md, 03_uwb-wake-ranging-and-power.md
- `firmware/architecture_boundaries.json` - cited in 10_build-presets-and-configuration.md
- `firmware/include/device_identity.h` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/include/discovery_assignment.h` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/include/dwm3000_runtime.h` - cited in 03_uwb-wake-ranging-and-power.md
- `firmware/include/gateway_collection_journal.h` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/include/mesh_relay.h` - cited in 05_connected-routing-and-reliable-delivery.md
- `firmware/include/node_comm.h` - cited in 05_connected-routing-and-reliable-delivery.md
- `firmware/include/node_transaction.h` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/include/protocol.h` - cited in 04_protocol-packets-and-data-contracts.md
- `firmware/include/report.h` - cited in 04_protocol-packets-and-data-contracts.md
- `firmware/include/route.h` - cited in 05_connected-routing-and-reliable-delivery.md
- `firmware/include/serial_frame.h` - cited in 04_protocol-packets-and-data-contracts.md
- `firmware/include/stack_budget.h` - cited in 11_verified-deployment-and-qualification.md
- `firmware/include/survey.h` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/include/survey_gateway_transaction.h` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/include/survey_pair_lease.h` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/include/uwb.h` - cited in 02_one-click-end-to-end.md, 03_uwb-wake-ranging-and-power.md, 04_protocol-packets-and-data-contracts.md
- `firmware/scripts/capture_stack_evidence.py` - cited in 09_gateway-host-tools-and-observability.md, 11_verified-deployment-and-qualification.md, 13_hardware-bring-up-and-troubleshooting.md
- `firmware/scripts/check_architecture_boundaries.py` - cited in 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `firmware/scripts/check_mesh_deployment_policy.py` - cited in 11_verified-deployment-and-qualification.md
- `firmware/scripts/check_repository_truth.py` - cited in 12_testing-simulation-and-release-evidence.md
- `firmware/scripts/flash_verified_mesh.py` - cited in 11_verified-deployment-and-qualification.md
- `firmware/scripts/provision_mesh_anchor.py` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/scripts/verification_inputs.py` - cited in 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `firmware/scripts/verify_changes.py` - cited in 10_build-presets-and-configuration.md, 12_testing-simulation-and-release-evidence.md
- `firmware/scripts/verify_stack_evidence.py` - cited in 09_gateway-host-tools-and-observability.md, 11_verified-deployment-and-qualification.md
- `firmware/src/device_identity.c` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/src/discovery_assignment.c` - cited in 06_anchor-identity-discovery-and-assignment.md
- `firmware/src/dwm3000_runtime.c` - cited in 03_uwb-wake-ranging-and-power.md
- `firmware/src/gateway_collection_journal.c` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/src/gateway_membership.c` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/src/mesh_runtime.c` - cited in 05_connected-routing-and-reliable-delivery.md
- `firmware/src/node_comm.c` - cited in 05_connected-routing-and-reliable-delivery.md
- `firmware/src/node_transaction.c` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/src/protocol.c` - cited in 04_protocol-packets-and-data-contracts.md
- `firmware/src/report.c` - cited in 02_one-click-end-to-end.md, 04_protocol-packets-and-data-contracts.md
- `firmware/src/serial_frame.c` - cited in 04_protocol-packets-and-data-contracts.md
- `firmware/src/survey.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/src/survey_gateway_transaction.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/src/survey_pair_lease.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/src/uwb.c` - cited in 03_uwb-wake-ranging-and-power.md
- `firmware/src/uwb_session.c` - cited in 02_one-click-end-to-end.md
- `firmware/src/watchdog_adoption.c` - cited in 08_data-custody-persistence-and-recovery.md
- `firmware/tests/mesh_integration/README.md` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 07_anchor-self-setup-survey-and-geometry.md, 09_gateway-host-tools-and-observability.md, 12_testing-simulation-and-release-evidence.md, 13_hardware-bring-up-and-troubleshooting.md, README.md
- `firmware/tests/mesh_integration/test_dwm3000_models.c` - cited in 12_testing-simulation-and-release-evidence.md
- `firmware/tests/mesh_integration/test_gateway_ble_transport_model.c` - cited in 12_testing-simulation-and-release-evidence.md
- `firmware/tests/mesh_integration/test_mesh_click_report_delivery_sweep.c` - cited in 12_testing-simulation-and-release-evidence.md
- `firmware/tests/mesh_integration/test_mesh_production_scenarios.c` - cited in 12_testing-simulation-and-release-evidence.md
- `firmware/tests/mesh_integration/test_mesh_survey_topology_scenarios.c` - cited in 12_testing-simulation-and-release-evidence.md
- `firmware/tests/test_app_gateway_survey_round.c` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `firmware/west_projects.lock.json` - cited in 10_build-presets-and-configuration.md
- `tools/gateway_gui/README.md` - cited in 01_product-roles-and-firmware-lines.md, 06_anchor-identity-discovery-and-assignment.md, 07_anchor-self-setup-survey-and-geometry.md, 09_gateway-host-tools-and-observability.md, 12_testing-simulation-and-release-evidence.md, 13_hardware-bring-up-and-troubleshooting.md, README.md
- `tools/gateway_gui/anchor_geometry.py` - cited in 07_anchor-self-setup-survey-and-geometry.md
- `tools/gateway_gui/app.py` - cited in 09_gateway-host-tools-and-observability.md
- `tools/gateway_gui/ble_transport.py` - cited in 09_gateway-host-tools-and-observability.md
- `tools/gateway_gui/protocol.py` - cited in 09_gateway-host-tools-and-observability.md
- `tools/mesh_ble_route_monitor.py` - cited in 09_gateway-host-tools-and-observability.md, 13_hardware-bring-up-and-troubleshooting.md

### Uncovered Files

> Files listed in TOC but not cited in any documentation:

- `Documentation/Mesh Deployment Hardening Audit.md`
- `Documentation/Mesh Integration Simulator Adversarial Review 2026-07-11.md`
- `firmware/app/src/app_discovery_assignment_stack.h`
- `firmware/app/src/app_gateway_collection_eack.c`
- `firmware/app/src/app_high_debug.c`
- `firmware/app/src/app_mesh_coordinator.c`
- `firmware/app/src/app_stack_diag.c`
- `firmware/app/src/app_stack_workload_diag.c`
- `firmware/include/dwm3000_timing.h`
- `firmware/include/gateway_ble_transport.h`
- `firmware/include/gateway_membership.h`
- `firmware/include/mesh.h`
- `firmware/include/mesh_runtime.h`
- `firmware/include/stack_diag_transport.h`
- `firmware/include/status.h`
- `firmware/include/uwb_session.h`
- `firmware/include/watchdog_adoption.h`
- `firmware/scripts/check_agent_guidance.py`
- `firmware/sim/mesh_sim.c`
- `firmware/sim/mesh_sim.h`
- `firmware/src/gateway_ble_transport.c`
- `firmware/src/mesh.c`
- `firmware/src/mesh_relay.c`
- `firmware/src/route.c`
- `firmware/src/status.c`
- `firmware/tests/mesh_integration/test_gateway_survey_round_glue_source_invariants.py`
- `manifest/west.yml`

## Issues

### Errors

None

### Recommendations

- Add citations for 27 uncovered source files
