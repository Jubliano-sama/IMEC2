#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
APP = FIRMWARE / "app"
SOURCE = (APP / "src/app_durable_state.c").read_text()
HEADER = (APP / "src/app_durable_state.h").read_text()
CMAKE = (APP / "CMakeLists.txt").read_text()
KCONFIG = (APP / "Kconfig").read_text()
OVERLAY = (APP / "app.overlay").read_text()
MAIN = (APP / "src/main.c").read_text()
CLICK_SEQUENCE = (APP / "src/app_click_event_sequence.c").read_text()
CLICKER = (APP / "src/app_clicker.c").read_text()
ML_CLICKER = (APP / "src/app_ml.c").read_text()
GATEWAY_BLE = (APP / "src/app_gateway_ble.c").read_text()
GATEWAY_RESULT_RUNTIME = (
    APP / "src/app_gateway_result_runtime.inc"
).read_text()
GATEWAY_CONTROL_SEQUENCE = (
    APP / "src/app_gateway_control_sequence.c"
).read_text()
ANCHOR_INIT = (APP / "src/app_anchor_init.inc").read_text()
ANCHOR = (APP / "src/app_anchor.c").read_text()
ANCHOR_COMMANDS = (APP / "src/app_anchor_commands.inc").read_text()
GATEWAY_CONTROL = (APP / "src/app_anchor_gateway_control.inc").read_text()
APP_SURVEY = (APP / "src/app_survey.c").read_text()
MESH_REPORT = (APP / "src/app_mesh_report.c").read_text()
MESH_REPORT_RX = (APP / "src/app_mesh_report_rx.inc").read_text()
APP_STATE = (APP / "src/app_state.c").read_text()
ROUTE_REFRESH = (
    APP / "src/app_node_comm_gateway_route_refresh.c"
).read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function {name}")


class DurableStateSourceInvariants(unittest.TestCase):
    def test_survey_plan_is_the_single_execution_control(self) -> None:
        submit = function_body(APP_SURVEY, "app_survey_gateway_submit_plan")
        anchor = function_body(APP_SURVEY, "app_survey_anchor_apply_control")

        self.assertNotIn("APP_SURVEY_GATEWAY_SEND_START", APP_SURVEY)
        self.assertNotIn("SURVEY_PHASE_START", APP_SURVEY)
        self.assertIn(
            "execution_delay_ms = gateway_state.control_delivery_ms;",
            submit,
        )
        self.assertIn(
            "gateway_state.stage = APP_SURVEY_GATEWAY_EXECUTING;",
            submit,
        )
        self.assertIn(
            "anchor_state.action = APP_SURVEY_ANCHOR_ACTION_EXECUTE;",
            anchor,
        )
        request_fill = function_body(APP_SURVEY, "survey_range_request_fill")
        self.assertIn(
            "request->flags = FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;",
            request_fill,
        )

    def test_survey_roster_span_is_derived_from_occupied_slots(self) -> None:
        anchor_roster = function_body(
            APP_SURVEY, "app_survey_anchor_note_ram_roster"
        )
        gateway_table = function_body(
            GATEWAY_CONTROL,
            "gateway_discovery_assignment_prepare_durable_table_locked",
        )

        self.assertIn("entries[i].slot >= table_slot_count", anchor_roster)
        self.assertIn(
            "survey_slot_span_include(\n"
            "            anchor_state.slot_span, entries[i].slot)",
            anchor_roster,
        )
        self.assertNotIn("anchor_state.slot_span = table_slot_count",
                         anchor_roster)
        self.assertIn(
            "claimed_slot_span = survey_slot_span_include(", gateway_table
        )

        anchor_control = function_body(
            APP_SURVEY, "app_survey_anchor_apply_control"
        )
        self.assertIn(
            "table_slot_count == 0u || own_slot >= table_slot_count",
            anchor_control,
        )
        self.assertNotIn(
            "slot_count != control->identity.assignment.slot_span",
            anchor_control,
        )

    def test_clean_slate_assignment_identity_restores_unprovisioned(self) -> None:
        clear = function_body(
            ANCHOR_COMMANDS,
            "anchor_clear_committed_assignment_for_new_claim",
        )
        restore = function_body(
            APP_STATE, "local_anchor_restore_discovery_assignment"
        )
        validation = restore[:restore.index("k_spin_lock(")]

        # A newer clean-slate CLAIM intentionally keeps the finalized table
        # identity as its stale-packet barrier while removing the usable slot.
        # That exact historical, unprovisioned state must remain restorable.
        self.assertIn("snapshot.slot = 0u", clear)
        self.assertIn("snapshot.slot_count = 0u", clear)
        self.assertIn("snapshot.provisioned = 0u", clear)
        for retained_identity in (
            "snapshot.epoch",
            "snapshot.table_command_seq",
            "snapshot.table_commitment",
        ):
            self.assertNotRegex(clear, rf"{re.escape(retained_identity)}\s*=")

        # Slot bounds describe a usable assignment, so they apply only when
        # provisioned. Finalized identity validity remains independently
        # required for either a provisioned or historical snapshot.
        self.assertNotRegex(
            validation,
            r"finalized_valid\s*&&\s*\(\s*slot_count\s*==\s*0u",
        )
        self.assertRegex(
            validation,
            r"provisioned\s*&&\s*\(\s*!finalized_valid\s*\|\|\s*"
            r"slot_count\s*==\s*0u\s*\|\|\s*"
            r"slot_count\s*>\s*UWB_DISCOVERY_SLOT_COUNT\s*\|\|\s*"
            r"anchor_slot\s*>=\s*slot_count\s*\)",
        )

        def rejected(*, finalized: bool, provisioned: bool,
                     slot: int, slot_count: int) -> bool:
            return (
                not finalized
                or (
                    provisioned
                    and (
                        slot_count == 0
                        or slot_count > 50
                        or slot >= slot_count
                    )
                )
            )

        self.assertFalse(
            rejected(finalized=True, provisioned=False, slot=0, slot_count=0)
        )
        self.assertTrue(
            rejected(finalized=True, provisioned=True, slot=0, slot_count=0)
        )
        self.assertTrue(
            rejected(finalized=True, provisioned=True, slot=0, slot_count=51)
        )
        self.assertTrue(
            rejected(finalized=True, provisioned=True, slot=50, slot_count=50)
        )

    def test_one_module_owns_nvs(self) -> None:
        offenders: list[str] = []
        nvs_use = re.compile(r"\bnvs_(?:mount|read|write|delete|clear)\s*\(")
        for path in (APP / "src").glob("*"):
            if path.name in {"app_durable_state.c", "app_durable_state.h"}:
                continue
            if path.suffix not in {".c", ".h", ".inc"}:
                continue
            if nvs_use.search(path.read_text(errors="replace")):
                offenders.append(path.name)
        self.assertEqual(offenders, [])
        self.assertNotIn("struct nvs_fs", HEADER)
        self.assertNotIn("app_nvs_storage_fs", HEADER)

    def test_exact_mesh_roles_require_crc_backed_storage(self) -> None:
        for symbol in (
            "CONFIG_FLASH=y",
            "CONFIG_FLASH_MAP=y",
            "CONFIG_FLASH_PAGE_LAYOUT=y",
            "CONFIG_FLASH_LOAD_SIZE=0x7a000",
            "CONFIG_NVS=y",
            "CONFIG_NVS_DATA_CRC=y",
            "CONFIG_IMEC_DURABLE_STATE=y",
        ):
            self.assertIn(symbol, CMAKE)
        self.assertIn("IMEC_DEPLOYABLE_MESH_PRESET", CMAKE)
        self.assertIn('IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop"', CMAKE)
        self.assertRegex(
            KCONFIG,
            r"depends on FLASH && FLASH_MAP && FLASH_PAGE_LAYOUT && NVS && "
            r"NVS_DATA_CRC",
        )
        self.assertIn("IS_ENABLED(CONFIG_NVS_DATA_CRC)", SOURCE)
        self.assertIn("DT_NODELABEL(storage_partition)", SOURCE)
        self.assertIn("&storage_partition", OVERLAY)

    def test_gateway_fit_overrides_are_exact_preset_only(self) -> None:
        common_start = CMAKE.index(
            'if(FIRMWARE_ROLE STREQUAL "gateway" AND '
            'NOT IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD)'
        )
        common_end = CMAKE.index(
            'elseif(FIRMWARE_ROLE STREQUAL "anchor"', common_start
        )
        common = CMAKE[common_start:common_end]
        exact_start = common.index(
            'if(IMEC_BUILD_PRESET STREQUAL "mesh_gateway")'
        )
        exact_end = common.index("        endif()", exact_start)
        exact = common[exact_start:exact_end]

        self.assertLess(common.index('"CONFIG_BT=y\\n"'), exact_start)
        self.assertIn(
            "set(IMEC_GATEWAY_SYSTEM_WORKQUEUE_STACK_SIZE 4256)", common
        )
        self.assertIn(
            "set(IMEC_GATEWAY_SYSTEM_WORKQUEUE_STACK_SIZE 8512)", exact
        )
        self.assertIn(
            '"CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE='
            '${IMEC_GATEWAY_SYSTEM_WORKQUEUE_STACK_SIZE}\\n"',
            common,
        )
        for symbol in (
            "CONFIG_ADC=n",
            "CONFIG_BT_CTLR_ECDH=n",
            "CONFIG_BT_CTLR_LE_ENC=n",
            "CONFIG_BT_ASSERT_VERBOSE=n",
            "CONFIG_BT_GATT_CACHING=n",
            "CONFIG_BT_GATT_READ_MULTIPLE=n",
            "CONFIG_BT_GATT_READ_MULT_VAR_LEN=n",
            "CONFIG_BT_GATT_SERVICE_CHANGED=y",
            "CONFIG_BT_CTLR_CRYPTO=y",
        ):
            self.assertIn(symbol, exact)
            self.assertNotIn(symbol, common[:exact_start])
            self.assertNotIn(symbol, common[exact_end:])

    def test_owner_adds_no_scheduler_or_thread(self) -> None:
        for forbidden in (
            "k_work",
            "K_THREAD",
            "K_THREAD_STACK",
            "k_thread_create",
            "k_work_queue_start",
        ):
            self.assertNotIn(forbidden, SOURCE)
            self.assertNotIn(forbidden, HEADER)
        self.assertIn("K_MUTEX_DEFINE(durable_state_mutex)", SOURCE)

    def test_hot_runtime_state_is_out_of_contract(self) -> None:
        contract = HEADER.lower()
        for excluded in ("packet queues", "retry state", "ack history"):
            self.assertIn(excluded, contract)
        for forbidden_type in (
            "struct proto_packet",
            "struct mesh_outbound",
            "mesh_relay",
        ):
            self.assertNotIn(forbidden_type, HEADER)
            self.assertNotIn(forbidden_type, SOURCE)

    def test_gateway_assignment_receipts_are_owner_minted_capabilities(self) -> None:
        symbol = "app_durable_state_gateway_assignment_receipt_valid("

        self.assertNotIn(symbol, HEADER)
        self.assertNotIn(symbol, SOURCE)
        self.assertNotIn(symbol, GATEWAY_RESULT_RUNTIME)
        for function in (
            "app_durable_state_save_gateway_assignment_commit",
            "app_durable_state_restore_gateway_assignment_commit",
            "app_durable_state_retire_gateway_assignment_commit",
            "app_durable_state_delete_gateway_assignment",
        ):
            self.assertIn(function, HEADER)

    def test_boot_incarnation_has_one_reservation_owner(self) -> None:
        init = function_body(SOURCE, "app_durable_state_init")
        begin_boot = function_body(SOURCE, "app_durable_state_begin_boot")
        getter = function_body(
            SOURCE, "app_durable_state_boot_incarnation"
        )
        reserve = function_body(SOURCE, "app_durable_state_reserve")

        self.assertNotIn("durable_reserve_locked", init)
        self.assertIn("durable_read_locked(boot_spec", init)
        self.assertIn("durable_reserve_locked(boot_spec", begin_boot)
        self.assertLess(
            begin_boot.index("durable_reserve_locked(boot_spec"),
            begin_boot.index(
                "durable_owner.boot_incarnation = (uint32_t)boot.first"
            ),
        )
        self.assertIn("durable_owner.boot_incarnation", getter)
        self.assertNotIn("durable_reserve_locked", getter)
        self.assertIn(
            "type == APP_DURABLE_STATE_BOOT_INCARNATION", reserve
        )
        self.assertIn("return -ENOTSUP", reserve)

        reserve_locked = function_body(SOURCE, "durable_reserve_locked")
        self.assertIn("durable_counter_advance(", reserve_locked)
        self.assertNotIn("for (", reserve_locked)

    def test_main_binds_physical_ficr_before_runtime_admission(self) -> None:
        binding = function_body(MAIN, "durable_state_physical_device_id")
        self.assertIn("nrf_ficr_deviceid_get", binding)
        self.assertIn("device_identity_ficr_value", binding)
        self.assertNotIn("DEVICE_ID", binding)
        self.assertIn("device_id == 0u || device_id == UINT64_MAX", binding)

        durable = MAIN.index("app_durable_state_init(")
        begin_boot = MAIN.index("app_durable_state_begin_boot()")
        click = MAIN.index("app_click_event_sequence_init()")
        gateway = MAIN.index("gateway_broadcast_command_sequence_init()")
        node_comm = MAIN.index("app_node_comm_init(")
        anchor_init = MAIN.index("app_anchor_init()")
        radio = MAIN.index("dwm3000_port_init()")
        click_admission = MAIN.index("app_clicker_start_work_queue()")
        anchor_admission = MAIN.index("app_anchor_start_anchor_role()")
        gateway_admission = MAIN.index("app_anchor_start_gateway_role()")
        self.assertLess(durable, node_comm)
        self.assertLess(durable, anchor_init)
        self.assertLess(durable, radio)
        self.assertLess(radio, begin_boot)
        self.assertLess(begin_boot, click)
        self.assertLess(begin_boot, gateway)
        self.assertLess(radio, click)
        self.assertLess(radio, gateway)
        self.assertLess(click, click_admission)
        self.assertLess(radio, anchor_admission)
        self.assertLess(gateway, gateway_admission)
        self.assertNotIn(
            "gateway_broadcast_command_sequence_init", ANCHOR_INIT
        )
        startup = function_body(MAIN, "main")
        checkpoint_attempt = startup.index(
            "runtime_boot_checkpoint_attempted = true"
        )
        self.assertLess(checkpoint_attempt, begin_boot)
        self.assertIn(
            'runtime_start_fail_closed("durable state initialization", ret)',
            startup,
        )

        fail_closed = function_body(MAIN, "runtime_start_fail_closed")
        latch = fail_closed.index("if (runtime_boot_checkpoint_attempted)")
        manual = fail_closed.index("latched for manual reset", latch)
        keep_radio = fail_closed.index(
            "app_watchdog_note_radio_progress()", manual
        )
        reboot = fail_closed.index("sys_reboot(SYS_REBOOT_COLD)")
        self.assertLess(latch, manual)
        self.assertLess(manual, keep_radio)
        self.assertLess(keep_radio, reboot)

    def test_one_boot_incarnation_fans_out_to_runtime_identities(self) -> None:
        gateway_start = function_body(
            ANCHOR_INIT, "app_anchor_start_gateway_role"
        )
        anchor_start = function_body(
            ANCHOR_INIT, "app_anchor_start_anchor_role"
        )
        gateway_get = gateway_start.index(
            "app_durable_state_boot_incarnation(&boot_incarnation)"
        )
        gateway_error = gateway_start.index("if (ret < 0)", gateway_get)
        gateway_return = gateway_start.index("return ret", gateway_error)
        gateway_relay = gateway_start.index("mesh_relay_init(")
        self.assertLess(gateway_get, gateway_error)
        self.assertLess(gateway_error, gateway_return)
        self.assertLess(gateway_return, gateway_relay)
        self.assertRegex(
            gateway_start,
            r"mesh_relay_init\(&mesh_runtime,\s*"
            r"MESH_RELAY_ROLE_GATEWAY,\s*DEVICE_ID,\s*GATEWAY_ID,\s*"
            r"boot_incarnation\)",
        )
        self.assertRegex(
            anchor_start,
            r"mesh_relay_init\(&mesh_runtime,\s*"
            r"MESH_RELAY_ROLE_ANCHOR,\s*DEVICE_ID,\s*GATEWAY_ID,\s*1u\)",
        )
        self.assertNotIn("app_durable_state_boot_incarnation", anchor_start)

        collection_boot = function_body(
            ANCHOR_COMMANDS, "anchor_collection_node_boot_id"
        )
        collection_admit = function_body(
            ANCHOR_COMMANDS, "anchor_schedule_collection_command_result"
        )
        boot_read = collection_admit.index(
            "anchor_collection_node_boot_id(&node_boot_counter)"
        )
        boot_error = collection_admit.index("if (ret < 0)", boot_read)
        boot_return = collection_admit.index("return ret", boot_error)
        pending_mutation = collection_admit.index(
            "anchor_collection_result_pending.command = *command"
        )
        self.assertIn("app_durable_state_boot_incarnation", collection_boot)
        self.assertNotIn("sys_rand32_get", collection_boot)
        self.assertNotIn("k_uptime_get", collection_boot)
        self.assertLess(boot_read, boot_error)
        self.assertLess(boot_error, boot_return)
        self.assertLess(boot_return, pending_mutation)
        self.assertIn(
            "anchor_collection_result_pending.result_id.node_boot_counter =\n"
            "        node_boot_counter",
            collection_admit,
        )

        heartbeat = function_body(ANCHOR_COMMANDS, "anchor_send_heartbeat")
        durable_heartbeat = heartbeat[: heartbeat.index("#else")]
        heartbeat_get = durable_heartbeat.index(
            "app_durable_state_boot_incarnation(&boot_incarnation)"
        )
        heartbeat_error = durable_heartbeat.index(
            "if (ret < 0)", heartbeat_get
        )
        heartbeat_return = durable_heartbeat.index(
            "return ret", heartbeat_error
        )
        heartbeat_packet = heartbeat.index(
            "report_init_anchor_heartbeat_packet("
        )
        self.assertIn(
            "app_durable_state_boot_incarnation(&boot_incarnation)",
            durable_heartbeat,
        )
        self.assertNotIn("nonzero_uptime_session_id", durable_heartbeat)
        self.assertLess(heartbeat_get, heartbeat_error)
        self.assertLess(heartbeat_error, heartbeat_return)
        self.assertLess(heartbeat_return, heartbeat_packet)
        self.assertRegex(
            heartbeat,
            r"report_init_anchor_heartbeat_packet\([^;]*?"
            r"GATEWAY_ID,\s*boot_incarnation,\s*"
            r"anchor_next_heartbeat_seq\(\)",
        )

        event_nonce = function_body(APP_STATE, "mesh_event_boot_nonce")
        durable_event = event_nonce[: event_nonce.index("#else")]
        compatibility_event = event_nonce[event_nonce.index("#else") :]
        self.assertIn("app_durable_state_boot_incarnation", durable_event)
        self.assertNotIn("sys_rand32_get", durable_event)
        self.assertIn("sys_rand32_get", compatibility_event)

        for runtime_reader in (
            gateway_start,
            collection_boot,
            heartbeat,
            event_nonce,
        ):
            self.assertNotIn("app_durable_state_reserve(", runtime_reader)
            self.assertNotIn(
                "app_durable_state_advance_high_water(", runtime_reader
            )

    def test_gateway_route_refresh_uses_the_single_ram_owner(self) -> None:
        next_sequence = function_body(
            MESH_REPORT_RX, "mesh_route_refresh_next_sequence"
        )
        self.assertIn(
            "app_gateway_control_sequence_admission_available(",
            next_sequence,
        )
        self.assertIn(
            "APP_GATEWAY_CONTROL_SEQUENCE_FORCED_ROUTE_REFRESH_BUDGET",
            next_sequence,
        )
        self.assertIn("app_gateway_control_sequence_next(sequence)",
                      next_sequence)
        self.assertNotIn("app_durable_state_reserve(", next_sequence)
        self.assertNotIn("APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE",
                         next_sequence)
        self.assertNotIn("reserve_sequences", MESH_REPORT_RX)
        self.assertNotIn("ROUTE_REFRESH_SEQUENCE_BLOCK", ROUTE_REFRESH)
        self.assertNotIn("reserved_sequence", ROUTE_REFRESH)
        self.assertNotIn("APP_NODE_COMM_ROUTE_REFRESH_SEQUENCE_BLOCK_SIZE",
                         MESH_REPORT)
        next_sequence = function_body(
            ROUTE_REFRESH, "refresh_operation_next_sequence"
        )
        self.assertIn("operation->config->next_sequence(", next_sequence)
        self.assertNotIn("app_durable_state_reserve(", next_sequence)
        self.assertNotIn("refresh_sequence_increment", next_sequence)

    def test_assignment_epoch_is_the_exact_durable_claim_identity(self) -> None:
        start = function_body(
            GATEWAY_CONTROL, "gateway_start_discovery_assignment"
        )
        claim = start.index(
            "claim_command_seq = gateway_next_broadcast_command_seq()"
        )
        zero_guard = start.index("if (claim_command_seq == 0u)", claim)
        epoch = start.index("reserved_epoch = claim_command_seq", zero_guard)
        commit = start.index(
            "app_operation_policy_commit_prepared(&policy_candidate)", epoch
        )
        publish = start.index(
            "gateway_discovery_assignment_state.epoch = reserved_epoch",
            commit,
        )
        claim_publish = start.index(
            "gateway_discovery_assignment_state.claim_command_seq =",
            publish,
        )
        activate = start.index(
            "gateway_discovery_assignment_state.active = true",
            claim_publish,
        )
        self.assertLess(claim, zero_guard)
        self.assertLess(zero_guard, epoch)
        self.assertLess(epoch, commit)
        self.assertLess(commit, publish)
        self.assertLess(publish, claim_publish)
        self.assertLess(claim_publish, activate)
        for removed_owner in (
            "gateway_discovery_assignment_epoch_cursor",
            "gateway_discovery_assignment_epoch_ready",
            "gateway_discovery_assignment_reserve_epoch",
            "gateway_discovery_assignment_restore_epoch_cursor",
        ):
            self.assertNotIn(removed_owner, GATEWAY_CONTROL)

    def test_heartbeat_sequence_cannot_wrap_inside_gateway_retention(self) -> None:
        self.assertRegex(
            ANCHOR,
            r"BUILD_ASSERT\(\(uint64_t\)UINT16_MAX\s*\*\s*"
            r"\(uint64_t\)ANCHOR_HEARTBEAT_MIN_INTERVAL_MS\s*>\s*"
            r"\(uint64_t\)MESH_RELAY_GATEWAY_ACK_RETENTION_MS\s*\+\s*"
            r"\(uint64_t\)ANCHOR_HEARTBEAT_DELIVERY_TIMEOUT_MS",
        )

    def test_click_admission_uses_active_and_standby_ram_blocks(self) -> None:
        reserve = function_body(
            CLICK_SEQUENCE, "click_event_sequence_reserve_block_locked"
        )
        init = function_body(CLICK_SEQUENCE, "app_click_event_sequence_init")
        next_event = function_body(
            CLICK_SEQUENCE, "app_click_event_sequence_next"
        )
        maintain = function_body(
            CLICK_SEQUENCE, "app_click_event_sequence_maintain"
        )
        retained_idle = function_body(
            CLICKER, "clicker_enter_systemon_retained_idle"
        )

        self.assertRegex(
            HEADER,
            r"#define\s+APP_DURABLE_STATE_CLICK_BLOCK_SIZE\s+"
            r"UINT32_C\(65536\)",
        )
        self.assertIn("app_durable_state_reserve(", reserve)
        self.assertIn("APP_DURABLE_STATE_CLICK_BLOCK_SIZE", reserve)
        self.assertIn("click_event_sequence_reserve_block_locked(", init)
        self.assertIn("click_event_sequence_reserve_block_locked(", maintain)
        self.assertIn("CLICK_EVENT_SEQUENCE_PREFETCH_THRESHOLD", maintain)
        self.assertIn("APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_MAX_MS",
                      CLICK_SEQUENCE)
        self.assertIn("click_event_prefetch_error == -EOVERFLOW", maintain)
        self.assertIn("k_uptime_get() < click_event_prefetch_retry_at_ms",
                      maintain)
        self.assertNotIn("ret = click_event_prefetch_error", maintain)
        self.assertNotIn("app_durable_state_reserve(", next_event)
        self.assertNotIn("click_event_sequence_reserve_block_locked(",
                         next_event)
        self.assertIn(
            "click_event_active_block = click_event_standby_block",
            next_event,
        )
        self.assertIn("click_event_active_block.remaining--", next_event)
        self.assertIn("click_event_prefetch_error == -EOVERFLOW", next_event)
        self.assertNotIn("app_durable_state_reserve(", next_event)

        wake_arm = retained_idle.index("clicker_arm_retained_idle_wake()")
        maintenance = retained_idle.index(
            "app_click_event_sequence_maintain()"
        )
        radio_standby = retained_idle.index(
            "clicker_enter_radio_retained_standby()"
        )
        self.assertLess(radio_standby, wake_arm)
        self.assertLess(wake_arm, maintenance)

        self.assertIn(
            "APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_HORIZON_MS", CLICKER
        )
        self.assertRegex(
            CLICKER,
            r"BUILD_ASSERT\(\s*"
            r"\(uint64_t\)\(APP_DURABLE_STATE_CLICK_BLOCK_SIZE / 2u\)"
            r"\s*\*\s*\(uint64_t\)STATUS_PASS_DURATION_MS\s*>\s*"
            r"APP_CLICK_EVENT_SEQUENCE_PREFETCH_RETRY_HORIZON_MS\s*\+\s*"
            r"\(uint64_t\)STATUS_PASS_DURATION_MS",
        )
        action_handler = function_body(
            CLICKER, "app_clicker_handle_button_action"
        )
        normal = action_handler[
            action_handler.index("case BUTTON_ACTION_NORMAL_CLICK:"):
            action_handler.index("case BUTTON_ACTION_SELF_TEST_ARMED:")
        ]
        self_test = action_handler[
            action_handler.index("case BUTTON_ACTION_SELF_TEST_START:"):
            action_handler.index("case BUTTON_ACTION_SELF_TEST_CANCELLED:")
        ]
        self.assertLess(
            normal.index("app_clicker_run_normal_click(&normal_click_anchor_observed)"),
            normal.index("clicker_hold_terminal_status(ret)"),
        )
        self.assertLess(
            self_test.index("app_click_event_sequence_next("),
            self_test.index("clicker_hold_terminal_status("),
        )
        normal_run = function_body(CLICKER, "app_clicker_run_normal_click")
        action_worker = function_body(
            CLICKER, "clicker_action_work_handler"
        )
        self.assertEqual(CLICKER.count("app_click_event_sequence_next("), 2)
        self.assertEqual(normal_run.count("app_click_event_sequence_next("),
                         1)
        self.assertEqual(
            action_worker.count("app_clicker_handle_button_action(action)"),
            1,
        )

        ml_section = ML_CLICKER.index(
            "#if defined(CONFIG_IMEC_ML_CLICKER)",
            ML_CLICKER.index("uint8_t ml_clicker_discovery_slot_count_override"),
        )
        ml_section_end = ML_CLICKER.index(
            "\n#endif", ML_CLICKER.rfind("app_click_event_sequence_next(")
        )
        ml_calls = [
            match.start()
            for match in re.finditer(
                r"app_click_event_sequence_next\(", ML_CLICKER
            )
        ]
        self.assertEqual(len(ml_calls), 2)
        self.assertTrue(
            all(ml_section < call < ml_section_end for call in ml_calls)
        )

    def test_gateway_uses_one_nonwrapping_ram_only_command_owner(self) -> None:
        reserve = function_body(
            GATEWAY_CONTROL_SEQUENCE,
            "gateway_control_sequence_reserve_block",
        )
        self.assertIn("APP_DURABLE_STATE_GATEWAY_COMMAND_SEQUENCE", reserve)
        self.assertIn("app_durable_state_reserve(", reserve)
        self.assertIn("reservation.first == 0u", reserve)
        self.assertIn("reservation.reserved_through < reservation.first",
                      reserve)
        self.assertIn("APP_DURABLE_STATE_COMMAND_MAX_BLOCK_RESERVATIONS",
                      GATEWAY_CONTROL_SEQUENCE)
        self.assertIn("GATEWAY_CONTROL_SEQUENCE_PREFETCH_THRESHOLD",
                      GATEWAY_CONTROL_SEQUENCE)
        self.assertIn("APP_GATEWAY_CONTROL_SEQUENCE_PROTECTED_FLOOR",
                      GATEWAY_CONTROL_SEQUENCE)
        next_command = function_body(
            GATEWAY_CONTROL_SEQUENCE, "app_gateway_control_sequence_next"
        )
        self.assertNotIn("app_durable_state_reserve(", next_command)
        self.assertNotIn("app_gateway_control_sequence_admission_available(",
                         next_command)
        self.assertIn("gateway_control_sequence_active.remaining--",
                      next_command)
        availability = function_body(
            GATEWAY_CONTROL_SEQUENCE,
            "app_gateway_control_sequence_admission_available",
        )
        self.assertIn("gateway_control_sequence_available_locked", availability)
        self.assertIn("requested_ids +", GATEWAY_CONTROL_SEQUENCE)
        ble_init = function_body(
            GATEWAY_BLE, "gateway_broadcast_command_sequence_init"
        )
        ble_next = function_body(
            GATEWAY_BLE, "gateway_next_broadcast_command_seq"
        )
        self.assertIn("app_gateway_control_sequence_init()", ble_init)
        self.assertIn("app_gateway_control_sequence_next(&sequence)", ble_next)
        self.assertNotIn("app_durable_state_reserve(", GATEWAY_BLE)


if __name__ == "__main__":
    unittest.main()
