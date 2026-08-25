cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT "${CMAKE_CURRENT_LIST_DIR}")

if(DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
    if(NOT DEFINED IMEC_BUILD_PRESET OR IMEC_BUILD_PRESET STREQUAL "")
        file(STRINGS "${SYSBUILD_CACHE}" imec_sysbuild_preset_line
             REGEX "^(IMEC_BUILD_PRESET|FIRMWARE_PRESET):")
        if(imec_sysbuild_preset_line)
            string(REGEX REPLACE "^[^=]*=" "" IMEC_BUILD_PRESET
                   "${imec_sysbuild_preset_line}")
            set(IMEC_BUILD_PRESET "${IMEC_BUILD_PRESET}" CACHE STRING
                "Build preset name, for example mesh_anchor" FORCE)
        endif()
    endif()
    if(NOT DEFINED IMEC_FORCED_GATEWAY_RELAY_HOPS OR IMEC_FORCED_GATEWAY_RELAY_HOPS STREQUAL "")
        file(STRINGS "${SYSBUILD_CACHE}" imec_sysbuild_hops_line
             REGEX "^IMEC_FORCED_GATEWAY_RELAY_HOPS:")
        if(imec_sysbuild_hops_line)
            string(REGEX REPLACE "^[^=]*=" "" IMEC_FORCED_GATEWAY_RELAY_HOPS
                   "${imec_sysbuild_hops_line}")
            set(IMEC_FORCED_GATEWAY_RELAY_HOPS "${IMEC_FORCED_GATEWAY_RELAY_HOPS}" CACHE STRING
                "Exact gateway relay depth for mesh_anchor_forcedhop (1-8)" FORCE)
        endif()
    endif()
endif()

set(FIRMWARE_PRESET "" CACHE STRING "Compatibility alias for IMEC_BUILD_PRESET")
set_property(CACHE FIRMWARE_PRESET PROPERTY STRINGS
    ""
    ml_clicker
    mesh_clicker
    ml_anchor_1
    ml_anchor_2
    ml_anchor_3
    ml_anchor_4
    ml_anchor_5
    ml_anchor_6
    ml_anchor_7
    ml_anchor_8
    mesh_gateway
    mesh_transmitter
    mesh_transmitter_forcedhop
    mesh_anchor
    mesh_anchor_forcedhop
)
if(FIRMWARE_PRESET)
    if(IMEC_BUILD_PRESET AND NOT IMEC_BUILD_PRESET STREQUAL FIRMWARE_PRESET)
        message(FATAL_ERROR
            "FIRMWARE_PRESET (${FIRMWARE_PRESET}) conflicts with IMEC_BUILD_PRESET (${IMEC_BUILD_PRESET})")
    endif()
    set(IMEC_BUILD_PRESET "${FIRMWARE_PRESET}" CACHE STRING
        "Build preset name, for example mesh_anchor" FORCE)
endif()
set(IMEC_BUILD_PRESET "${IMEC_BUILD_PRESET}" CACHE STRING "Build preset name, for example mesh_anchor")
set_property(CACHE IMEC_BUILD_PRESET PROPERTY STRINGS
    ""
    ml_clicker
    mesh_clicker
    ml_anchor_1
    ml_anchor_2
    ml_anchor_3
    ml_anchor_4
    ml_anchor_5
    ml_anchor_6
    ml_anchor_7
    ml_anchor_8
    mesh_gateway
    mesh_transmitter
    mesh_transmitter_forcedhop
    mesh_anchor
    mesh_anchor_forcedhop
)
set(IMEC_ML_BUILD OFF CACHE BOOL "Build the ML data-collection firmware variant")
set(IMEC_MESH_ROUTE_TEST_BUILD OFF CACHE BOOL "Build the connected-routing mesh runtime")
set(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD OFF CACHE BOOL "Enable continuous synthetic mesh-route TX worker")
set(IMEC_STACK_DIAGNOSTICS_BUILD OFF CACHE BOOL "Enable exact-preset RTT stack diagnostics")
set(IMEC_STACK_STRESS_BUILD OFF CACHE BOOL
    "Enable opt-in stack canaries and retained fatal context for an exact production mesh preset")
set(IMEC_WATCHDOG_BYPASS_BUILD OFF CACHE BOOL
    "Disable watchdog reset policy for supervised bench captures")
set(IMEC_DEPLOYABLE_MESH_PRESET OFF)
set(IMEC_USE_HARDWARE_DEVICE_ID OFF)
set(IMEC_DEVICE_ID "" CACHE STRING "Override DEVICE_ID, for example 0x1111222233334444ull")
set(IMEC_GATEWAY_ID "" CACHE STRING "Override GATEWAY_ID")
set(IMEC_NETWORK_ID "" CACHE STRING "Override NETWORK_ID")
set(IMEC_ML_ANCHOR_SLOT "" CACHE STRING "Override deterministic ML anchor discovery slot")
set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "" CACHE STRING "BLE device name for mesh-route test builds")
set(IMEC_FORCED_GATEWAY_RELAY_HOPS "1" CACHE STRING
    "Exact gateway relay depth for mesh_anchor_forcedhop (1-8)")

set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway")
set_property(CACHE FIRMWARE_ROLE PROPERTY STRINGS clicker tag anchor gateway)
if(IMEC_BUILD_PRESET)
    set(IMEC_ML_BUILD OFF CACHE BOOL "Build the ML data-collection firmware variant" FORCE)
    set(IMEC_MESH_ROUTE_TEST_BUILD OFF CACHE BOOL "Build the connected-routing mesh runtime" FORCE)
    set(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD OFF CACHE BOOL "Enable continuous synthetic mesh-route TX worker" FORCE)
    if(IMEC_BUILD_PRESET STREQUAL "ml_clicker")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_ML_BUILD ON CACHE BOOL "Build the ML data-collection firmware variant" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "ml-clicker.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "mesh_clicker")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "mesh-clicker.conf")
        set(IMEC_DEVICE_ID "" CACHE STRING
            "Production mesh clickers always use the hardware-derived ID" FORCE)
        set(IMEC_USE_HARDWARE_DEVICE_ID ON)
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Clicker" CACHE STRING
            "Diagnostic name for connected-routing builds" FORCE)
        set(IMEC_STACK_DIAGNOSTICS_BUILD ON CACHE BOOL "Enable exact-preset RTT stack diagnostics" FORCE)
        set(IMEC_DEPLOYABLE_MESH_PRESET ON)
    elseif(IMEC_BUILD_PRESET MATCHES "^ml_anchor_([1-8])$")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_ML_BUILD ON CACHE BOOL "Build the ML data-collection firmware variant" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "ml-anchor.conf")
        set(IMEC_ML_ANCHOR_INDEX "${CMAKE_MATCH_1}")
        math(EXPR IMEC_ML_ANCHOR_SLOT_VALUE "${IMEC_ML_ANCHOR_INDEX} - 1")
        set(IMEC_DEVICE_ID "0x222222222222220${IMEC_ML_ANCHOR_INDEX}ull" CACHE STRING
            "Override DEVICE_ID, for example 0x1111222233334444ull" FORCE)
        set(IMEC_ML_ANCHOR_SLOT "${IMEC_ML_ANCHOR_SLOT_VALUE}" CACHE STRING
            "Override deterministic ML anchor discovery slot" FORCE)
    elseif(IMEC_BUILD_PRESET STREQUAL "mesh_gateway")
        set(FIRMWARE_ROLE "gateway" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Test Gateway" CACHE STRING
            "BLE device name for mesh-route test builds" FORCE)
        set(IMEC_STACK_DIAGNOSTICS_BUILD ON CACHE BOOL "Enable exact-preset RTT stack diagnostics" FORCE)
        set(IMEC_DEPLOYABLE_MESH_PRESET ON)
    elseif(IMEC_BUILD_PRESET STREQUAL "mesh_transmitter" OR
           IMEC_BUILD_PRESET STREQUAL "mesh_transmitter_forcedhop")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD ON CACHE BOOL
            "Enable continuous synthetic mesh-route TX worker" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Tx" CACHE STRING
            "BLE device name for mesh-route test builds" FORCE)
        set(IMEC_DEVICE_ID "0x3333333333333301ull" CACHE STRING
            "Override DEVICE_ID, for example 0x1111222233334444ull" FORCE)
    elseif(IMEC_BUILD_PRESET STREQUAL "mesh_anchor" OR
           IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "mesh-anchor.conf")
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Anchor" CACHE STRING
            "BLE device name for mesh-route test builds" FORCE)
        set(IMEC_DEVICE_ID "" CACHE STRING
            "Production mesh anchors always use the hardware-derived ID" FORCE)
        set(IMEC_USE_HARDWARE_DEVICE_ID ON)
        set(IMEC_STACK_DIAGNOSTICS_BUILD ON CACHE BOOL "Enable exact-preset RTT stack diagnostics" FORCE)
        if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor")
            set(IMEC_DEPLOYABLE_MESH_PRESET ON)
        endif()
    else()
        message(FATAL_ERROR "Unknown IMEC_BUILD_PRESET: ${IMEC_BUILD_PRESET}")
    endif()
endif()

if(FIRMWARE_ROLE STREQUAL "tag")
    set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
endif()

if(FIRMWARE_ROLE STREQUAL "gateway")
    set(CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/prj-gateway.conf" CACHE STRING
        "Base Kconfig fragment for gateway builds" FORCE)
elseif(FIRMWARE_ROLE STREQUAL "clicker")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/prj-clicker.conf")
endif()

if(IMEC_ML_BUILD AND IMEC_PRESET_EXTRA_CONF)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/${IMEC_PRESET_EXTRA_CONF}")
elseif(IMEC_PRESET_EXTRA_CONF)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/${IMEC_PRESET_EXTRA_CONF}")
endif()

if(IMEC_MESH_ROUTE_TEST_BUILD)
    if(NOT IMEC_MESH_ROUTE_TEST_DEVICE_NAME)
        message(FATAL_ERROR "IMEC_MESH_ROUTE_TEST_DEVICE_NAME must be set for mesh-route test builds")
    endif()
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/mesh-route-test.conf")
    set(IMEC_MESH_ROUTE_TEST_NAME_CONF
        "${CMAKE_CURRENT_BINARY_DIR}/imec-mesh-route-test-name.conf")
    file(WRITE "${IMEC_MESH_ROUTE_TEST_NAME_CONF}" "")
    if(FIRMWARE_ROLE STREQUAL "gateway" AND NOT IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD)
        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
            "CONFIG_BT=y\n"
            "CONFIG_BT_PERIPHERAL=y\n"
            "CONFIG_BT_DEVICE_NAME=\"${IMEC_MESH_ROUTE_TEST_DEVICE_NAME}\"\n"
            "CONFIG_BT_MAX_CONN=1\n"
            "CONFIG_BT_BONDABLE=n\n"
            "CONFIG_BT_AUTO_PHY_UPDATE=y\n"
            "CONFIG_BT_DATA_LEN_UPDATE=y\n"
            "CONFIG_BT_L2CAP_TX_MTU=247\n"
            "CONFIG_BT_BUF_ACL_RX_SIZE=251\n"
            # Gateway notifications are explicitly serialized to one
            # application-owned in-flight packet. Four host TX buffers retain
            # controller/host slack without consuming the static RAM reserve.
            "CONFIG_BT_BUF_ACL_TX_COUNT=4\n"
            "CONFIG_BT_BUF_ACL_TX_SIZE=251\n"
            "CONFIG_BT_CTLR_DATA_LENGTH_MAX=251\n"
            "CONFIG_BT_LOG_LEVEL_WRN=y\n"
            "CONFIG_IMEC_GATEWAY_BLE=y\n")
        set(IMEC_GATEWAY_SYSTEM_WORKQUEUE_STACK_SIZE 4256)
        if(IMEC_BUILD_PRESET STREQUAL "mesh_gateway")
            # The exact production gateway does not sample ADC inputs and does
            # not request a secured or bonded BLE link. Keep controller crypto
            # available to the Bluetooth stack, but compile unused controller
            # key exchange/encryption and GATT cache/read-multiple surfaces out.
            # Assertions remain enabled; only their verbose controller-side
            # diagnostic strings are omitted from this flash-constrained role.
            # Service Changed remains explicit so the host can rediscover the
            # new attribute table on the same physical gateway identity.
            file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
                "CONFIG_ADC=n\n"
                "CONFIG_BT_CTLR_ECDH=n\n"
                "CONFIG_BT_CTLR_LE_ENC=n\n"
                "CONFIG_BT_ASSERT_VERBOSE=n\n"
                "CONFIG_BT_GATT_CACHING=n\n"
                "CONFIG_BT_GATT_READ_MULTIPLE=n\n"
                "CONFIG_BT_GATT_READ_MULT_VAR_LEN=n\n"
                "CONFIG_BT_GATT_SERVICE_CHANGED=y\n"
                "CONFIG_BT_CTLR_CRYPTO=y\n"
                # Keep fatal assertions, but omit source paths from their
                # messages in the flash-constrained gateway image. Typed RTT
                # evidence identifies the active protocol owner at runtime.
                "CONFIG_ASSERT_NO_FILE_INFO=y\n")
            set(IMEC_GATEWAY_SYSTEM_WORKQUEUE_STACK_SIZE 8512)
        endif()
        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
            # The gateway is the flash-constrained production role. Typed RTT
            # records retain protocol/stack evidence; compile ordinary Zephyr
            # logs above errors out of this exact preset so safety validators
            # do not compete with executable flash.
            "CONFIG_LOG_DEFAULT_LEVEL=1\n"
            "CONFIG_LOG_MAX_LEVEL=1\n"
            # Typed qualification records use their dedicated RTT channel;
            # keep the lossy deferred application log ring compact enough to
            # preserve the gateway's required static RAM reserve, including
            # the full broadcast-command replay commitment.
            "CONFIG_LOG_BUFFER_SIZE=1024\n"
            # Survey host-event custody reaches a 6776-byte synchronous chain.
            # 8512 retains the verifier's 20% free-space floor plus alignment
            # room. Complete platform closure remains a promotion gate;
            # generic gateway builds retain their prior 4256-byte setting.
            "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=${IMEC_GATEWAY_SYSTEM_WORKQUEUE_STACK_SIZE}\n")
    elseif(FIRMWARE_ROLE STREQUAL "anchor" AND
           NOT IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD)
        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
            # Typed stack/protocol evidence has its own RTT channel. Keep the
            # ordinary deferred warning/error ring bounded so the measured
            # scan stack leaves the required 9 KiB static RAM reserve.
            "CONFIG_LOG_BUFFER_SIZE=1920\n")
        if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor" OR
           IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")
            file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
                # Four-board enumeration qualification measured 2076/6144 bytes on
                # the system workqueue, while its current compiler chain is
                # 4088 bytes. 5376 retains the 20% policy floor plus compiler
                # margin, freeing RAM for the measured route worker without
                # reducing total static headroom.
                "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=5376\n")
        endif()
    endif()
    if(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD)
        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
            "CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER=y\n"
            "CONFIG_MESH_SMOKE_FAST_TX=y\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS=1000\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT=4\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES=900\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS=3000\n"
            "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=8192\n"
            "CONFIG_LOG=n\n")
        if(IMEC_BUILD_PRESET STREQUAL "mesh_transmitter_forcedhop")
            file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
                "CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ=y\n")
        endif()
    endif()
    if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")
        if(NOT IMEC_FORCED_GATEWAY_RELAY_HOPS MATCHES "^[1-8]$")
            message(FATAL_ERROR
                "IMEC_FORCED_GATEWAY_RELAY_HOPS must be an integer from 1 through 8")
        endif()
        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
            "CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ=y\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL=y\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_REQUIRED_GATEWAY_RELAY_HOPS=${IMEC_FORCED_GATEWAY_RELAY_HOPS}\n")
    endif()
    list(APPEND EXTRA_CONF_FILE "${IMEC_MESH_ROUTE_TEST_NAME_CONF}")
endif()

if(IMEC_STACK_DIAGNOSTICS_BUILD)
    list(APPEND EXTRA_CONF_FILE
        "${CMAKE_CURRENT_LIST_DIR}/conf/mesh-stack-diagnostics.conf")
endif()

if(IMEC_STACK_STRESS_BUILD)
    if(NOT IMEC_DEPLOYABLE_MESH_PRESET)
        message(FATAL_ERROR
            "IMEC_STACK_STRESS_BUILD requires mesh_clicker, mesh_anchor, or mesh_gateway")
    endif()
    list(APPEND EXTRA_CONF_FILE
        "${CMAKE_CURRENT_LIST_DIR}/conf/mesh-stack-stress.conf")
endif()

if(IMEC_WATCHDOG_BYPASS_BUILD)
    if(NOT IMEC_DEPLOYABLE_MESH_PRESET AND
       NOT IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")
        message(FATAL_ERROR
            "IMEC_WATCHDOG_BYPASS_BUILD requires a deployable mesh preset or mesh_anchor_forcedhop")
    endif()
    list(APPEND EXTRA_CONF_FILE
        "${CMAKE_CURRENT_LIST_DIR}/conf/watchdog-bypass.conf")
endif()

if(IMEC_DEPLOYABLE_MESH_PRESET OR
   IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")
    set(IMEC_DURABLE_STATE_CONF
        "${CMAKE_CURRENT_BINARY_DIR}/imec-durable-state.conf")
    file(WRITE "${IMEC_DURABLE_STATE_CONF}"
        "CONFIG_FLASH=y\n"
        "CONFIG_FLASH_MAP=y\n"
        "CONFIG_FLASH_PAGE_LAYOUT=y\n"
        "CONFIG_FLASH_LOAD_SIZE=0x7a000\n"
        "CONFIG_NVS=y\n"
        "CONFIG_NVS_DATA_CRC=y\n"
        "CONFIG_IMEC_DURABLE_STATE=y\n")
    list(APPEND EXTRA_CONF_FILE "${IMEC_DURABLE_STATE_CONF}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_firmware)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/dwm3000_sdk.cmake")
imec_resolve_dwm3000_sdk(DWM3000_SDK_DIR)

# Five immutable priority records (or transmitter stress datagrams) may be
# admitted together while one protocol-response slot remains available.
# Keep the pure/native scheduler's wider capacity out of constrained images.
target_compile_definitions(app PRIVATE NODE_COMM_MAX_REQUESTS=6)

if(FIRMWARE_ROLE STREQUAL "clicker")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=1)
elseif(FIRMWARE_ROLE STREQUAL "anchor")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=2)
elseif(FIRMWARE_ROLE STREQUAL "gateway")
    target_compile_definitions(app PRIVATE
        DEVICE_ROLE=3
        APP_NODE_COMM_GATEWAY_ROLE=1
        IMEC_MESH_RELAY_GATEWAY_ONLY=1)
else()
    message(FATAL_ERROR "FIRMWARE_ROLE must be clicker, anchor, or gateway")
endif()

set(IMEC_APP_LOCAL_SOURCES
    src/app_anchor_click_event_runtime.c
    src/app_click_event_sequence.c
    src/app_clicker_event_runtime.c
    src/app_state.c
    src/app_radio_guard.c
    src/app_device_identity.c
    src/app_board.c
    src/app_gateway_control_sequence.c
    src/app_gateway_ble.c
    src/app_gateway_ble_stream.c
    src/app_gateway_command_result.c
    src/app_gateway_command_observability.c
    src/app_gateway_assignment_publisher.c
    src/app_gateway_collection_eack.c
    src/app_gateway_collection_recovery.c
    src/app_gateway_eack_retry.c
    src/app_gateway_eack_policy.c
    src/app_clicker.c
    src/app_radio_recovery.c
    src/app_ml.c
    src/app_mesh_test.c
    src/app_mesh_smoke_fast.c
    src/app_mesh_c5_priority.c
    src/app_mesh_command_orchestrator.c
    src/app_mesh_arbitration_zephyr.c
    src/app_node_comm_gateway_control.c
    src/app_node_comm_sync.c
    src/app_node_comm_gateway_route_refresh.c
    src/app_operation_policy.c
    src/app_gateway_command_ingress.c
    src/app_gateway_command_lifecycle.c
    src/app_mesh_gateway_command_flow.c
    src/app_mesh_rx_policy.c
    src/app_mesh_preemption.c
    src/app_mesh_flood.c
    src/app_mesh_ch9_ack.c
    src/app_mesh_direct_probe_diag.c
    src/app_mesh_direct_gateway_retry.c
    src/app_mesh_collection_deferral.c
    src/app_mesh_tx_handoff_gate.c
    src/app_mesh_route_reply_ack.c
    src/app_mesh_route_ready_handoff.c
    src/app_mesh_event_retry.c
    src/app_mesh_async_route_request.c
    src/app_mesh_route_request_policy.c
    src/app_mesh_route_wait_tx.c
    src/app_mesh_rf_retry.c
    src/app_mesh_gateway_ack_policy.c
    src/app_mesh_result_handoff.c
    src/app_wake_train_politeness.c
    src/app_node_comm.c
    src/app_mesh_report.c
    src/app_mesh_report_encode.c
    src/app_stack_diag_record.c
    src/app_stack_diag.c
    src/app_stack_workload_diag.c
    src/app_survey.c
    src/app_anchor.c
    src/app_anchor_command_completion.c
    src/app_anchor_ranging.c
    src/app_watchdog.c
    src/dwm3000_driver_handler_contract.c
)

target_sources_ifdef(CONFIG_IMEC_DURABLE_STATE app PRIVATE
    src/app_durable_state.c)

target_sources_ifdef(CONFIG_IMEC_CLICKER_RTT_CONTROL app PRIVATE
    src/app_clicker_rtt_control.c
    ../src/clicker_rtt_command.c)

target_sources(app PRIVATE
    ../src/node_comm.c
    ../src/node_transaction.c
    ../src/semantic_digest.c
    ../src/stack_diag_transport.c
    ../src/watchdog_adoption.c
)

# Operational wake, discovery, ranging, and relay traffic use UWB frames.
target_sources(app PRIVATE
    src/main.c
    ${IMEC_APP_LOCAL_SOURCES}
    src/dwm3000_driver.c
    src/dwm3000_port.c
    src/dwm3000_sdk_port.c
    "${DWM3000_SDK_DIR}/decadriver/deca_device.c"
    ../src/debug_log.c
    ../src/device_identity.c
    ../src/protocol.c
    ../src/protocol_rx_lifecycle.c
    ../src/gateway_command.c
    ../src/gateway_ble_transport.c
    ../src/discovery_assignment.c
    ../src/dwm3000_timing.c
    ../src/enumeration_response_lane.c
    ../src/firmware_events.c
    ../src/firmware_delivery_loss.c
    ../src/firmware_state_click.c
    ../src/firmware_state_control.c
    ../src/firmware_state_mesh.c
    ../src/firmware_state_radio.c
    ../src/gateway_membership.c
    ../src/uwb.c
    ../src/uwb_ble_courtesy.c
    ../src/uwb_session.c
    ../src/mesh.c
    ../src/mesh_event_owner.c
    ../src/mesh_event_owner_registry.c
    ../src/mesh_preemption.c
    ../src/mesh_relay.c
    ../src/mesh_relay_channel9.c
    ../src/mesh_route_path.c
    ../src/operation_policy.c
    ../src/report.c
    ../src/serial_frame.c
    ../src/status.c
    ../src/survey.c
    ../src/survey_protocol.c
    ../src/survey_response_lane.c
    ../src/route.c
)

target_include_directories(app PRIVATE
    ../include
    "${DWM3000_SDK_DIR}/decadriver"
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
    OUTPUT_VARIABLE IMEC_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT IMEC_GIT_VERSION)
    set(IMEC_GIT_VERSION "unknown")
endif()
string(TIMESTAMP IMEC_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%S")
set(IMEC_STACK_DIAG_ID_INPUT
    "${IMEC_BUILD_PRESET}|${IMEC_GIT_VERSION}|${IMEC_BUILD_TIMESTAMP}|${BOARD}|${CMAKE_C_COMPILER_ID}|${CMAKE_C_COMPILER_VERSION}")
if(IMEC_BUILD_PRESET STREQUAL "mesh_anchor_forcedhop")
    string(APPEND IMEC_STACK_DIAG_ID_INPUT
        "|forced_gateway_relay_hops=${IMEC_FORCED_GATEWAY_RELAY_HOPS}")
endif()
string(SHA256 IMEC_STACK_DIAG_ID_HASH
    "${IMEC_STACK_DIAG_ID_INPUT}")
set(IMEC_STACK_DIAG_BUILD_ID
    "imec-stack-v1:${IMEC_BUILD_PRESET}:${IMEC_STACK_DIAG_ID_HASH}")

target_compile_definitions(app PRIVATE
    IMEC_BUILD_PRESET_NAME="${IMEC_BUILD_PRESET}"
    IMEC_GIT_VERSION="${IMEC_GIT_VERSION}"
    IMEC_BUILD_TIMESTAMP="${IMEC_BUILD_TIMESTAMP}"
    IMEC_BOARD_TARGET="${BOARD}"
    IMEC_STACK_DIAG_BUILD_ID="${IMEC_STACK_DIAG_BUILD_ID}"
    IMEC_USE_HARDWARE_DEVICE_ID=$<BOOL:${IMEC_USE_HARDWARE_DEVICE_ID}>
)
# The verifier closes every application-owned frame and synchronous call edge.
# Platform and controller code keeps its SDK-owned stack contract and is
# covered by the required on-target whole-thread high-watermark capture.
target_compile_options(app PRIVATE -fstack-usage -fdump-ipa-cgraph)
if(IMEC_DEVICE_ID)
    target_compile_definitions(app PRIVATE DEVICE_ID=${IMEC_DEVICE_ID})
endif()
if(IMEC_GATEWAY_ID)
    target_compile_definitions(app PRIVATE GATEWAY_ID=${IMEC_GATEWAY_ID})
endif()
if(IMEC_NETWORK_ID)
    target_compile_definitions(app PRIVATE NETWORK_ID=${IMEC_NETWORK_ID})
endif()
if(NOT "${IMEC_ML_ANCHOR_SLOT}" STREQUAL "")
    target_compile_definitions(app PRIVATE IMEC_ML_ANCHOR_SLOT=${IMEC_ML_ANCHOR_SLOT})
endif()
