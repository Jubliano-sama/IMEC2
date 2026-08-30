#!/usr/bin/env python3
from pathlib import Path
import re

from source_text import read_composed_source


ROOT = Path(__file__).resolve().parents[2]
DRIVER = read_composed_source(ROOT / "app/src/dwm3000_driver.c")

assert re.search(
    r"#define\s+DWM3000_PHY_RX_PAC\s+DWT_PAC32\b", DRIVER
), "the production channel-5 PHY default must use the documented PAC32"
assert re.search(
    r"#define\s+DWM3000_PHY_SFD_TIMEOUT\s+4073\b", DRIVER
), "the production channel-5 PHY default must use the documented SFD timeout"


def function_body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", DRIVER, re.DOTALL)
    assert match is not None, f"missing function {name}"
    brace = DRIVER.index("{", match.start())
    depth = 0
    for index in range(brace, len(DRIVER)):
        depth += DRIVER[index] == "{"
        depth -= DRIVER[index] == "}"
        if depth == 0:
            return DRIVER[match.start() : index + 1]
    raise AssertionError(f"unterminated function {name}")


def assert_order(source: str, *needles: str) -> None:
    cursor = 0
    for needle in needles:
        index = source.find(needle, cursor)
        assert index >= 0, f"missing or out-of-order source boundary: {needle}"
        cursor = index + len(needle)


read_frame = function_body("read_rx_frame")
receive = function_body("receive_frame_with_preamble_timeout")
receive_range_frame = function_body("receive_frame")
responder = function_body("responder_poll_once")
invalidate_radio_state = function_body("invalidate_radio_state_tagged")
mark_radio_awake_unconfigured = function_body(
    "mark_radio_awake_unconfigured_tagged"
)
check_device_fatal_status = function_body("check_device_fatal_status")
initialise = function_body("initialise_radio")
probe = function_body("dwm3000_driver_probe")
configure_default = function_body("dwm3000_driver_configure_default")
idle = function_body("dwm3000_driver_idle")
standby = function_body("dwm3000_driver_standby")
ensure_current = function_body("ensure_current_phy_or_range")
receive_response = function_body("receive_response")
continuous_activity = function_body(
    "receive_frame_continuous_extend_on_activity"
)
finish_abortible_continuous_receive = function_body(
    "finish_abortible_continuous_receive"
)
sniff = function_body("dwm3000_driver_sniff_activity")
wait_status = function_body("wait_status_internal")
send_range_frame = function_body("send_range_frame_until")
send_frame_tracked_until = function_body(
    "dwm3000_driver_send_frame_tracked_until"
)
start_prepared_range_frame = function_body("start_prepared_range_frame")
start_immediate_rx = function_body("start_immediate_rx")
write_tx_frame = function_body("write_tx_frame")
wait_tx_complete = function_body("wait_tx_complete_observed")
status_to_range_status = function_body("status_to_range_status")
status_to_rx_failure = function_body("status_to_rx_failure")
rx_status_has_activity = function_body("rx_status_has_activity")
ipatov_diagnostics_valid = function_body("ipatov_diagnostics_valid")
capture_rx_diag_raw = function_body("capture_rx_diag_raw")
read_cir_window = function_body("read_cir_window")
read_rx_diagnostics = function_body("read_rx_diagnostics")
capture_last_rx_cir = function_body("dwm3000_driver_capture_last_rx_cir")
send_clicker_diag = function_body("send_clicker_diag")
send_anchor_diag = function_body("send_anchor_diag")
send_anchor_diag_fragment_block = function_body(
    "send_anchor_diag_fragment_block"
)
range_initiator = function_body("dwm3000_driver_range_initiator")

assert "DBG_DWM_CH5_RX_HUNT_EMPTY s=%u e=%u" in continuous_activity
assert_order(
    continuous_activity,
    "hunt_started_ms = k_uptime_get_32()",
    "ret = wait_status_internal(",
    "hunt_ended_ms = k_uptime_get_32()",
    "dwt_forcetrxoff()",
    "DBG_DWM_CH5_RX_HUNT_EMPTY",
)
for retained_observation in (
    "status",
    "active_phy_mode",
    "radio_configured",
    "radio_awake",
    "radio_state_unknown",
):
    assert retained_observation in continuous_activity[
        continuous_activity.index("DBG_DWM_CH5_RX_HUNT_EMPTY") :
    ], f"empty-hunt trace must retain {retained_observation}"

assert read_frame.count("dwt_read32bitreg(RX_FINFO_ID)") == 1
assert "last_rx_finfo_register = rx_finfo" in read_frame
assert_order(
    read_frame,
    "dwt_read32bitreg(RX_FINFO_ID)",
    'take_port_error("read-rx-frame-length")',
    'check_device_fatal_status("read-rx-frame-length")',
    "if (raw_frame_len <= UWB_RF_SCOPE_WIRE_LEN + FCS_LEN)",
    "dwt_readrxdata(&scope_wire",
    "uwb_rf_scope_decode(",
    "uwb_rf_scope_visible(",
    "return -EHOSTUNREACH",
    "frame_len = raw_frame_len - UWB_RF_SCOPE_WIRE_LEN",
    "if (frame_len > buffer_len)",
    "dwt_readrxdata(",
    'take_port_error("read-rx-frame-data")',
    'check_device_fatal_status("read-rx-frame-data")',
    "*frame_len_out = frame_len",
    "return 0",
)
assert "dwt_read32bitreg(RX_FINFO_ID)" not in receive
assert "ret == 0 ? last_rx_finfo_register : 0u" in receive
assert "unbounded diagnostic SPI transaction" in receive

for frame_consumer in (receive_range_frame, continuous_activity):
    frame_read = frame_consumer.index("ret = read_rx_frame(")
    scope_drop = frame_consumer.index("if (ret == -EHOSTUNREACH)", frame_read)
    scope_continue = frame_consumer.index("continue;", scope_drop)
    malformed_failure = frame_consumer.index("if (ret < 0)", scope_continue)
    assert frame_read < scope_drop < scope_continue < malformed_failure, (
        "RF-hidden frames must be discarded before malformed-frame handling"
    )

assert "ret = receive_frame(" in receive_response
assert "ret = read_rx_frame(" not in receive_response

poll_receive = responder.index("ret = receive_frame(")
poll_failure = responder.index("if (ret < 0)", poll_receive)
poll_timeout_status = responder.index(
    "result->status = ret == -ETIMEDOUT ? RANGE_RX_TIMEOUT", poll_failure
)
poll_timeout_return = responder.index("if (ret == -ETIMEDOUT)", poll_failure)
exchange_started = responder.index("result->exchange_started = true")
assert poll_failure < poll_timeout_status < poll_timeout_return < exchange_started, (
    "a pre-POLL receive timeout must be classified before returning and before "
    "the exchange is marked started"
)
assert "ret == -EMSGSIZE ? RANGE_BAD_FRAME" in responder[
    poll_failure:poll_timeout_return
], "a malformed pre-POLL frame must remain distinguishable from an RX error"
assert "RANGE_RX_ERROR" in responder[poll_failure:poll_timeout_return], (
    "a non-timeout pre-POLL receive failure must be reported as an RX error"
)

response_prestage = responder.index("ret = uwb_encode_response(")
response_write = responder.index("ret = write_tx_frame(", response_prestage)
response_receive = responder.index("ret = receive_frame(", response_write)
response_reencode = responder.index(
    "ret = uwb_encode_response(", response_receive
)
response_patch = responder.index("ret = patch_tx_frame(", response_reencode)
response_delayed_time = responder.index(
    "dwt_setdelayedtrxtime(resp_tx_time)", response_patch
)
response_start = responder.index(
    "ret = start_prepared_range_frame(", response_delayed_time
)
assert (
    response_prestage
    < response_write
    < response_receive
    < response_reencode
    < response_patch
    < response_delayed_time
    < response_start
), (
    "the responder must stage its invariant RESPONSE before POLL RX, then "
    "patch only timestamps before the delayed-TX command"
)
assert "2u * sizeof(uint32_t)" in responder[
    response_patch:response_delayed_time
], "the prepared RESPONSE path must patch exactly its two timestamp fields"
assert "send_range_frame(tx_buffer, tx_len" not in responder[
    response_receive:response_start
], "the post-POLL deadline must not contain a full RESPONSE frame write"

fatal_mask_start = DRIVER.index("#define DWM3000_SYS_STATUS_HI_FATAL_MASK")
fatal_mask_end = DRIVER.index("#define RX_TERMINAL_STATUS_MASK", fatal_mask_start)
fatal_mask = DRIVER[fatal_mask_start:fatal_mask_end]
for fatal_bit in (
    "SYS_STATUS_HI_SPIERR_BIT_MASK",
    "SYS_STATUS_HI_SPI_UNF_BIT_MASK",
    "SYS_STATUS_HI_SPI_OVF_BIT_MASK",
    "SYS_STATUS_HI_CMD_ERR_BIT_MASK",
):
    assert fatal_bit in fatal_mask, (
        f"the fatal DW3000 high-status mask must include {fatal_bit}"
    )

for invalidated_claim in (
    "radio_configured = false",
    "radio_awake = false",
    "radio_restored_from_sleep = false",
    "active_phy_mode = DWM3000_PHY_NONE",
    "radio_state_unknown = true",
):
    assert invalidated_claim in invalidate_radio_state
for probed_claim in (
    "radio_configured = false",
    "radio_awake = true",
    "radio_restored_from_sleep = false",
    "active_phy_mode = DWM3000_PHY_NONE",
    "radio_state_unknown = false",
):
    assert probed_claim in mark_radio_awake_unconfigured

assert_order(
    check_device_fatal_status,
    "dwt_read32bitreg(SYS_STATUS_HI_ID)",
    "take_port_error(operation)",
    "fatal_status = status_hi & DWM3000_SYS_STATUS_HI_FATAL_MASK",
    "dwt_read32bitreg(FINT_STAT_ID)",
    "dwt_read16bitoffsetreg(SYS_STATUS_HI_ID, 0)",
    '"DBG_DWM_FATAL_CURRENT op=%s spi=%u first=%08x fatal=%04x panic=%u',
    '"DBG_DWM_FATAL_SLOW_REG fint=%08x low=%08x hi16=%04x hi32=%08x id=%08x',
    "dwt_write32bitreg(SYS_STATUS_HI_ID, fatal_status)",
    'take_port_error("device-fatal-status-clear")',
    '"DBG_DWM_FATAL operation=%s status_hi=0x%08x fatal=0x%08x clear_ret=%d\\n"',
    "invalidate_radio_state_tagged(__func__)",
)
assert "return ret < 0 ? ret : -EIO" in check_device_fatal_status, (
    "fatal device command/SPI status must fail the operation after being cleared"
)

slow_spi = initialise.index("ret = dwm3000_port_set_slow_spi()")
initialise_chip = initialise.index("dwt_initialise(mode)", slow_spi)
assert "if (ret < 0)" in initialise[slow_spi:initialise_chip], (
    "initialisation must stop if the mandatory slow-SPI transition fails"
)

slow_spi = probe.index("ret = dwm3000_port_set_slow_spi()")
read_id = probe.index("read_id = dwt_readdevid()", slow_spi)
assert probe.index("invalidate_radio_state_tagged(__func__)") < slow_spi, (
    "an out-of-band reset/probe must invalidate every prior PHY claim"
)
assert "if (ret < 0)" in probe[slow_spi:read_id], (
    "probing must not read the device identity after a failed slow-SPI transition"
)
assert "radio_configured = true" not in probe
assert "active_phy_mode =" not in probe, (
    "identity probing must leave the reset IC explicitly unconfigured"
)
assert_order(
    probe,
    "invalidate_radio_state_tagged(__func__)",
    "read_id = dwt_readdevid()",
    "dwm3000_port_dev_id_supported(read_id)",
    "*dev_id = read_id",
    "mark_radio_awake_unconfigured_tagged(__func__)",
)

assert "configure_radio_from_reset(DWM3000_PHY_RANGE)" in configure_default, (
    "default configuration must invalidate stale software state at its reset "
    "boundary and share the checked full-reset path"
)

for low_power_entry in (idle, standby):
    recovery_condition = (
        "radio_state_unknown || (radio_awake && !radio_configured)"
    )
    assert recovery_condition in low_power_entry
    assert_order(
        low_power_entry,
        recovery_condition,
        "configure_radio_from_reset(DWM3000_PHY_RANGE)",
        "if (!radio_awake)",
    )

enter_sleep = standby.index("dwt_entersleep(DWT_DW_IDLE_RC)")
mark_asleep = standby.index("radio_awake = false", enter_sleep)
sleep_error = standby.index('take_port_error("standby")', enter_sleep)
assert enter_sleep < mark_asleep < sleep_error, (
    "sleep state must fail closed before checking an ambiguous post-sleep port error"
)

assert "return ensure_phy_mode(" in ensure_current
assert "wake_configured_radio(" not in ensure_current, (
    "ordinary TX/RX must share ensure_phy_mode's full-reset recovery after a "
    "retained wake, SPI transition, or identity validation failure"
)

wait_failure = receive_response.index("if (ret < 0)")
wait_failure_end = receive_response.index("return ret", wait_failure)
assert "ret == -ETIMEDOUT" in receive_response[
    wait_failure:wait_failure_end
]
assert "RANGE_RX_ERROR" in receive_response[wait_failure:wait_failure_end], (
    "a response SPI/status failure must not be reported as an RF timeout"
)

first_wait = continuous_activity.index("ret = wait_status_internal(")
first_failure = continuous_activity.index(
    "if (ret < 0 && !rx_status_has_activity(status))", first_wait
)
first_failure_end = continuous_activity.index(
    "return finish_abortible_continuous_receive(-ETIMEDOUT)", first_failure
)
assert "if (ret != -ETIMEDOUT)" in continuous_activity[
    first_failure:first_failure_end
]
assert "return finish_abortible_continuous_receive(ret)" in continuous_activity[
    first_failure:first_failure_end
], "continuous RX acquisition must preserve port/status failures"

continuous_abort_enable = continuous_activity.index(
    "atomic_set(&receive_abort_enabled, 1)"
)
assert continuous_abort_enable < first_wait, (
    "continuous RX must enable existing abort-owner polling before its first wait"
)
assert_order(
    finish_abortible_continuous_receive,
    "atomic_set(&receive_abort_enabled, 0)",
    "return ret",
)
continuous_abort_scope = continuous_activity[continuous_abort_enable:]
raw_returns = [
    statement
    for statement in re.findall(r"return\s+[^;]+;", continuous_abort_scope)
    if "finish_abortible_continuous_receive(" not in statement
]
assert not raw_returns, (
    "every continuous RX exit after abort enable must disable polling: "
    + ", ".join(raw_returns)
)
assert continuous_abort_scope.count(
    "finish_abortible_continuous_receive("
) >= 10, "continuous RX success, timeout, abort, and rearm failures need cleanup"
continuous_waits = [
    match.start()
    for match in re.finditer(
        r"ret\s*=\s*wait_status_internal\s*\(", continuous_abort_scope
    )
]
assert len(continuous_waits) == 4, (
    "continuous RX must cover acquisition, scope-drop rearm, error rearm, "
    "and completion waits"
)
for wait_index, next_wait_index in zip(
    continuous_waits,
    continuous_waits[1:] + [len(continuous_abort_scope)],
):
    wait_phase = continuous_abort_scope[wait_index:next_wait_index]
    assert_order(
        wait_phase,
        "if (ret == -ECANCELED)",
        "dwt_forcetrxoff()",
        "return finish_abortible_continuous_receive(-ECANCELED)",
    )

sniff_wait = sniff.index("ret = wait_status_internal(")
sniff_activity = sniff.index(
    "activity_seen = rx_status_has_activity(status)", sniff_wait
)
sniff_timeout = sniff.index("if (!activity_seen)", sniff_activity)
assert "if (ret < 0 && ret != -ETIMEDOUT)" in sniff[
    sniff_activity:sniff_timeout
]
assert "return ret" in sniff[
    sniff_activity:sniff_timeout
], "activity sniffing must preserve port/status failures"

timeout_read = wait_status.index(
    'take_port_error("status-timeout-read")'
)
timeout_accounting = wait_status.index(
    "driver_stats.sys_status_poll_timeouts++", timeout_read
)
assert "return ret" in wait_status[timeout_read:timeout_accounting], (
    "the final status read must preserve a boundary SPI failure"
)
assert "if ((read_status & mask) != 0u)" in wait_status[
    timeout_read:timeout_accounting
], "a hardware completion at the deadline boundary must beat timeout"
assert_order(
    wait_status,
    'check_device_fatal_status("status-entry")',
    "read_status = dwt_read32bitreg(SYS_STATUS_ID)",
    "sampled_at_ms = (uint64_t)k_uptime_get()",
    'take_port_error("status-read")',
    "if ((read_status & mask) != 0u)",
    'check_device_fatal_status("status-match")',
    "*observed_at_ms = sampled_at_ms",
)
boundary_status = wait_status[timeout_read:]
assert_order(
    boundary_status,
    "if ((read_status & mask) != 0u)",
    'check_device_fatal_status("status-boundary-match")',
    "*observed_at_ms = sampled_at_ms",
)
assert wait_status.index(
    'check_device_fatal_status("status-timeout")', timeout_read
) < timeout_accounting, (
    "a high-status command/SPI fault at the deadline must beat RF timeout "
    "accounting"
)

assert_order(
    receive_range_frame,
    "ret = wait_status_internal(",
    "&observed_at_ms",
    "ret = read_rx_frame(",
    "if (ret == -EHOSTUNREACH)",
    "last_rx_host_uptime_ms = (uint32_t)observed_at_ms",
    "read_rx_timestamp_u64()",
    "read_rx_diagnostics(",
)
assert_order(
    continuous_activity,
    "ret = wait_status_internal(",
    "&observed_at_ms",
    "ret = read_rx_frame(",
    "if (ret == -EHOSTUNREACH)",
    "last_rx_host_uptime_ms = (uint32_t)observed_at_ms",
    "read_rx_diagnostics(",
)
assert "&result.tx_completed_at_ms" in send_frame_tracked_until
assert "completed_at_ms" in wait_tx_complete
assert_order(
    wait_tx_complete,
    "wait_status_internal(",
    "completed_at_ms",
    "clear_status_checked(SYS_STATUS_TXFRS_BIT_MASK",
)

assert 'check_device_fatal_status("tx-preflight")' in send_range_frame
assert send_range_frame.index('check_device_fatal_status("tx-preflight")') < (
    send_range_frame.index("write_tx_frame(")
), "a stale device command/SPI fault must abort before TX buffer writes"
assert "radio_configured" in send_range_frame[
    send_range_frame.index('check_device_fatal_status("tx-preflight")'):
    send_range_frame.index("write_tx_frame(")
], "low-level TX must reject a PHY invalidated by a consumed port error"

assert_order(
    send_range_frame,
    "write_tx_frame(",
    "if (absolute_deadline_ms != 0u)",
    "device_now = dwt_readsystimestamphi32()",
    "host_now_ms = (uint64_t)k_uptime_get()",
    "host_now_ms >= absolute_deadline_ms",
    "dwt_setdelayedtrxtime(delayed_tx_time)",
    'take_port_error("deadline-tx-program")',
    'check_device_fatal_status("deadline-tx-program")',
    "effective_tx_mode |= DWT_START_TX_DELAYED",
)
deadline_program = send_range_frame[
    send_range_frame.index("if (absolute_deadline_ms != 0u)") :
    send_range_frame.index("} else {", send_range_frame.index(
        "if (absolute_deadline_ms != 0u)"
    ))
]
deadline_tx_lead = re.search(
    r"#define\s+DWM3000_DEADLINE_TX_LEAD_UUS\s+(\d+)u\b",
    DRIVER,
)
assert deadline_tx_lead is not None
assert int(deadline_tx_lead.group(1)) >= 5000, (
    "on-target gateway follow-up TX requires at least 5000 us of DW3000 "
    "delayed-start headroom"
)
assert "DWM3000_DEADLINE_TX_LEAD_UUS" in deadline_program
assert "DWM3000_UUS_TO_DWT_TIME" in deadline_program, (
    "absolute host deadlines must use a hardware-scheduled DW3000 TX target"
)

rf_start_possible = send_range_frame.index("*rf_start_possible = true")
start_command = send_range_frame.index("dwt_starttx(effective_tx_mode)")
assert send_range_frame.index("dwt_setdelayedtrxtime(") < (
    rf_start_possible
) < start_command, (
    "RF accounting must become conservative after staging but before the "
    "ambiguous start-command transfer"
)
assert send_range_frame.index("*rf_start_at_ms = scheduled_host_ms") < (
    start_command
)
assert_order(
    send_range_frame[start_command:],
    "dwt_starttx(effective_tx_mode)",
    'take_port_error("tx-start")',
    'check_device_fatal_status("tx-start")',
)
assert_order(
    send_frame_tracked_until,
    "send_range_frame_until(",
    "&result.rf_started",
    "&result.rf_started_at_ms",
    "if (ret < 0)",
    "*observation = result",
    "return ret",
)

assert_order(
    start_prepared_range_frame,
    'check_device_fatal_status("prepared-tx-preflight")',
    "dwt_starttx(tx_mode)",
    'take_port_error("prepared-tx-start")',
    'check_device_fatal_status("prepared-tx-start")',
)
assert_order(
    start_immediate_rx,
    'check_device_fatal_status("rx-arm-preflight")',
    "dwt_rxenable(DWT_START_RX_IMMEDIATE)",
    'take_port_error("rx-arm")',
    'check_device_fatal_status("rx-arm")',
)

assert "config->phrMode == DWT_PHRMODE_EXT" in write_tx_frame
assert "UWB_PHY_EXTENDED_FRAME_MAX_LEN" in write_tx_frame
assert "DWM3000_STANDARD_FRAME_MAX_LEN" in write_tx_frame
assert_order(
    write_tx_frame,
    "max_frame_len = config->phrMode == DWT_PHRMODE_EXT",
    "if (frame_len > max_frame_len - FCS_LEN - UWB_RF_SCOPE_WIRE_LEN",
    "dwt_writetxdata(",
)
assert 'take_port_error("tx-frame-control")' in write_tx_frame, (
    "TX frame-control writes must be checked before starting RF"
)
assert "radio_configured" in start_immediate_rx[
    start_immediate_rx.index('check_device_fatal_status("rx-arm-preflight")'):
    start_immediate_rx.index("dwt_rxenable(")
], "low-level RX must reject a PHY invalidated by a consumed port error"
assert 'clear_status_checked(SYS_STATUS_TXFRS_BIT_MASK' in wait_tx_complete, (
    "successful TX must not return with an unchecked stale completion bit"
)

for phy_error_bit in (
    "SYS_STATUS_ARFE_BIT_MASK",
    "SYS_STATUS_CIAERR_BIT_MASK",
    "SYS_STATUS_RXOVRR_BIT_MASK",
):
    assert phy_error_bit in status_to_rx_failure, (
        f"{phy_error_bit} must classify as a CRC/PHY receive failure"
    )
    assert phy_error_bit in rx_status_has_activity, (
        f"{phy_error_bit} must count as observed RF activity"
    )
assert_order(
    status_to_rx_failure,
    "SYS_STATUS_ARFE_BIT_MASK",
    "DWM3000_RX_FAILURE_CRC_OR_PHY",
    "SYS_STATUS_RXPTO_BIT_MASK",
)
assert "DWM3000_RX_ERROR_STATUS_MASK" in status_to_range_status
rx_error_mask_start = DRIVER.index("#define DWM3000_RX_ERROR_STATUS_MASK")
rx_error_mask_end = DRIVER.index(
    "#define DWM3000_SYS_STATUS_HI_FATAL_MASK", rx_error_mask_start
)
assert "SYS_STATUS_RXOVRR_BIT_MASK" in DRIVER[
    rx_error_mask_start:rx_error_mask_end
], "RX overrun must be terminal even when the SDK aggregate mask omits it"

assert "diagnostics->ipatovAccumCount == 0u" in ipatov_diagnostics_valid
assert "diagnostics->ipatovPower != 0u" in ipatov_diagnostics_valid
assert "first_path_index >= DWM3000_CIR_ACCUM_SAMPLE_COUNT" in (
    ipatov_diagnostics_valid
), "zero or out-of-range CIA diagnostics must never become valid samples"

diagnostics_read = capture_rx_diag_raw.index(
    "dwt_readdiagnostics(&diagnostics)"
)
diagnostics_publish = capture_rx_diag_raw.index(
    "pack_rx_diag_raw(", diagnostics_read
)
assert 'take_port_error("rx-diagnostics-raw")' in capture_rx_diag_raw[
    diagnostics_read:diagnostics_publish
], "raw diagnostics must not publish bytes from a failed SPI read"
assert capture_rx_diag_raw.index(
    "ipatov_diagnostics_valid(", diagnostics_read
) < diagnostics_publish, (
    "raw zero-CIA diagnostics must be rejected before publication"
)

cir_read = read_cir_window.index("dwt_readaccdata(")
cir_publish = read_cir_window.index("memcpy(", cir_read)
assert 'take_port_error("rx-cir-window")' in read_cir_window[
    cir_read:cir_publish
], "a partial CIR SPI failure must not publish a complete diagnostic window"
assert "memset(buffer, 0, total_len)" in read_cir_window[
    cir_read:cir_publish
]

assert 'take_port_error("rx-diagnostics")' in read_rx_diagnostics
assert "return false" in read_rx_diagnostics, (
    "optional RX diagnostics must expose a failed SPI sample"
)
assert_order(
    read_rx_diagnostics,
    "dwt_readdiagnostics(&diagnostics)",
    "ipatov_diagnostics_valid(&diagnostics",
    "estimate_ipatov_rsl_dbm(&diagnostics)",
)
assert "if (!result->anchor_rx_diag_sampled)" in capture_last_rx_cir
assert capture_last_rx_cir.index(
    "if (!result->anchor_rx_diag_sampled)"
) < capture_last_rx_cir.index("read_cir_window("), (
    "full CIR capture must not derive a window from an invalid first-path read"
)

for diagnostic_sender in (
    send_clicker_diag,
    send_anchor_diag,
):
    assert diagnostic_sender.index("ensure_current_phy_or_range()") < (
        diagnostic_sender.index("send_range_frame(")
    ), "post-ranging diagnostic TX must recover an invalidated PHY first"

assert 'clear_status_checked(SYS_STATUS_TXFRS_BIT_MASK' in (
    send_clicker_diag
)
assert 'clear_status_checked(SYS_STATUS_TXFRS_BIT_MASK' in (
    send_anchor_diag
)
assert 'clear_status_checked(SYS_STATUS_TXFRS_BIT_MASK' in (
    send_anchor_diag_fragment_block
), "diagnostic TX must not start after an unchecked status-clear failure"

diagnostic_failure = range_initiator.index(
    'take_port_error("post-arm-diagnostics-abort")'
)
final_wait = range_initiator.index(
    "if (request->skip_responder_report)", diagnostic_failure
)
assert "result->status = RANGE_INTERNAL_ERROR" in range_initiator[
    diagnostic_failure:final_wait
]
assert "return -EIO" in range_initiator[
    diagnostic_failure:final_wait
], (
    "a diagnostic port failure after delayed-FINAL arm must abort the exchange "
    "instead of continuing with an invalidated PHY"
)

print("DWM3000 receive source invariants passed")
