cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT "${CMAKE_CURRENT_LIST_DIR}")

if(DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}" AND
   (NOT DEFINED IMEC_BUILD_PRESET OR IMEC_BUILD_PRESET STREQUAL ""))
    file(STRINGS "${SYSBUILD_CACHE}" imec_sysbuild_preset_line
         REGEX "^IMEC_BUILD_PRESET:")
    if(imec_sysbuild_preset_line)
        string(REGEX REPLACE "^[^=]*=" "" IMEC_BUILD_PRESET
               "${imec_sysbuild_preset_line}")
        set(IMEC_BUILD_PRESET "${IMEC_BUILD_PRESET}" CACHE STRING
            "High-debug preset name, for example tag_stage0_highdebug" FORCE)
    endif()
endif()

set(FIRMWARE_PRESET "" CACHE STRING "Compatibility alias for IMEC_BUILD_PRESET")
set_property(CACHE FIRMWARE_PRESET PROPERTY STRINGS
    ""
    tag_stage0_highdebug
    tag_stage1_highdebug
    tag_stage1_wake_spam
    tag_stage1_click_spam
    anchor_stage1_highdebug
    tag_stage2_highdebug
    anchor_stage2_highdebug
    tag_stage3_highdebug
    anchor_stage3_highdebug
    gateway_stage3_highdebug
    gateway_ble_connectivity_test
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
        "High-debug preset name, for example tag_stage0_highdebug" FORCE)
endif()
set(IMEC_BUILD_PRESET "${IMEC_BUILD_PRESET}" CACHE STRING "High-debug preset name, for example tag_stage0_highdebug")
set_property(CACHE IMEC_BUILD_PRESET PROPERTY STRINGS
    ""
    tag_stage0_highdebug
    tag_stage1_highdebug
    tag_stage1_wake_spam
    tag_stage1_click_spam
    anchor_stage1_highdebug
    tag_stage2_highdebug
    anchor_stage2_highdebug
    tag_stage3_highdebug
    anchor_stage3_highdebug
    gateway_stage3_highdebug
    gateway_ble_connectivity_test
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
set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile")
set(IMEC_GATEWAY_BLE_CONNECTIVITY_TEST OFF CACHE BOOL "Build only the gateway BLE connectivity test")
set(IMEC_GATEWAY_BLE_RANGE_SCANNER OFF CACHE BOOL "Build the gateway BLE connectivity test as a passive range scanner")
set(IMEC_ML_BUILD OFF CACHE BOOL "Build the ML data-collection firmware variant")
set(IMEC_MESH_ROUTE_TEST_BUILD OFF CACHE BOOL "Build the connected-routing mesh runtime")
set(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD OFF CACHE BOOL "Enable continuous synthetic mesh-route TX worker")
set(IMEC_STACK_DIAGNOSTICS_BUILD OFF CACHE BOOL "Enable exact-preset RTT stack diagnostics")
set(IMEC_STACK_STRESS_BUILD OFF CACHE BOOL
    "Enable opt-in stack canaries and retained fatal context for an exact production mesh preset")
set(IMEC_DEPLOYABLE_MESH_PRESET OFF)
set(IMEC_USE_HARDWARE_ANCHOR_ID OFF)
set(IMEC_DEVICE_ID "" CACHE STRING "Override DEVICE_ID, for example 0x1111222233334444ull")
set(IMEC_GATEWAY_ID "" CACHE STRING "Override GATEWAY_ID")
set(IMEC_NETWORK_ID "" CACHE STRING "Override NETWORK_ID")
set(IMEC_ML_ANCHOR_SLOT "" CACHE STRING "Override deterministic ML anchor discovery slot")
set(IMEC_HIGH_DEBUG_ANCHOR_SLOT "" CACHE STRING
    "Override deterministic flashed high-debug anchor slot")
set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "" CACHE STRING "BLE device name for mesh-route test builds")

set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway")
set_property(CACHE FIRMWARE_ROLE PROPERTY STRINGS clicker tag anchor gateway)
set(DWM3000_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dwm3000 examples and sdk")

if(IMEC_BUILD_PRESET)
    set(IMEC_HIGH_DEBUG ON CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
    set(IMEC_GATEWAY_BLE_CONNECTIVITY_TEST OFF CACHE BOOL "Build only the gateway BLE connectivity test" FORCE)
    set(IMEC_ML_BUILD OFF CACHE BOOL "Build the ML data-collection firmware variant" FORCE)
    set(IMEC_MESH_ROUTE_TEST_BUILD OFF CACHE BOOL "Build the connected-routing mesh runtime" FORCE)
    set(IMEC_MESH_ROUTE_TEST_TRANSMITTER_BUILD OFF CACHE BOOL "Enable continuous synthetic mesh-route TX worker" FORCE)
    set(IMEC_PRESET_ROLE_CONF "")
    if(IMEC_BUILD_PRESET STREQUAL "tag_stage0_highdebug")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 0)
        set(IMEC_PRESET_ROLE_CONF "role-tag.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "tag_stage1_highdebug")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 1)
        set(IMEC_PRESET_ROLE_CONF "role-tag.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "tag_stage1_wake_spam")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 1)
        set(IMEC_PRESET_ROLE_CONF "role-tag.conf")
        set(IMEC_PRESET_EXTRA_CONF "tag-wake-spam.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "tag_stage1_click_spam")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 1)
        set(IMEC_PRESET_ROLE_CONF "role-tag.conf")
        set(IMEC_PRESET_EXTRA_CONF "tag-click-spam.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "anchor_stage1_highdebug")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 1)
        set(IMEC_PRESET_ROLE_CONF "role-anchor.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "tag_stage2_highdebug")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 2)
        set(IMEC_PRESET_ROLE_CONF "role-tag.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "anchor_stage2_highdebug")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 2)
        set(IMEC_PRESET_ROLE_CONF "role-anchor.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "tag_stage3_highdebug")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 3)
        set(IMEC_PRESET_ROLE_CONF "role-tag.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "anchor_stage3_highdebug")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 3)
        set(IMEC_PRESET_ROLE_CONF "role-anchor.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "gateway_stage3_highdebug")
        set(FIRMWARE_ROLE "gateway" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_BENCH_STAGE 3)
        set(IMEC_PRESET_ROLE_CONF "role-gateway.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "gateway_ble_connectivity_test")
        set(FIRMWARE_ROLE "gateway" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
        set(IMEC_GATEWAY_BLE_CONNECTIVITY_TEST ON CACHE BOOL "Build only the gateway BLE connectivity test" FORCE)
    elseif(IMEC_BUILD_PRESET STREQUAL "ml_clicker")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
        set(IMEC_ML_BUILD ON CACHE BOOL "Build the ML data-collection firmware variant" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "ml-clicker.conf")
    elseif(IMEC_BUILD_PRESET STREQUAL "mesh_clicker")
        set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "mesh-clicker.conf")
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Clicker" CACHE STRING
            "Diagnostic name for connected-routing builds" FORCE)
        set(IMEC_STACK_DIAGNOSTICS_BUILD ON CACHE BOOL "Enable exact-preset RTT stack diagnostics" FORCE)
        set(IMEC_DEPLOYABLE_MESH_PRESET ON)
    elseif(IMEC_BUILD_PRESET MATCHES "^ml_anchor_([1-8])$")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
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
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
        set(IMEC_HIGH_DEBUG_ANCHOR_SLOT "" CACHE STRING
            "Override deterministic flashed high-debug anchor slot" FORCE)
        set(IMEC_BENCH_STAGE 3)
        set(IMEC_PRESET_ROLE_CONF "role-gateway.conf")
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Test Gateway" CACHE STRING
            "BLE device name for mesh-route test builds" FORCE)
        set(IMEC_STACK_DIAGNOSTICS_BUILD ON CACHE BOOL "Enable exact-preset RTT stack diagnostics" FORCE)
        set(IMEC_DEPLOYABLE_MESH_PRESET ON)
    elseif(IMEC_BUILD_PRESET STREQUAL "mesh_transmitter" OR
           IMEC_BUILD_PRESET STREQUAL "mesh_transmitter_forcedhop")
        set(FIRMWARE_ROLE "anchor" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
        set(IMEC_HIGH_DEBUG_ANCHOR_SLOT "" CACHE STRING
            "Override deterministic flashed high-debug anchor slot" FORCE)
        set(IMEC_BENCH_STAGE 3)
        set(IMEC_PRESET_ROLE_CONF "role-anchor.conf")
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
        set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
        set(IMEC_PRESET_EXTRA_CONF "mesh-anchor.conf")
        set(IMEC_HIGH_DEBUG_ANCHOR_SLOT "" CACHE STRING
            "Override deterministic flashed high-debug anchor slot" FORCE)
        set(IMEC_BENCH_STAGE 3)
        set(IMEC_PRESET_ROLE_CONF "role-anchor.conf")
        set(IMEC_MESH_ROUTE_TEST_BUILD ON CACHE BOOL
            "Build the connected-routing mesh runtime" FORCE)
        set(IMEC_MESH_ROUTE_TEST_DEVICE_NAME "IMEC Mesh Anchor" CACHE STRING
            "BLE device name for mesh-route test builds" FORCE)
        set(IMEC_DEVICE_ID "" CACHE STRING
            "Production mesh anchors always use the hardware-derived ID" FORCE)
        set(IMEC_USE_HARDWARE_ANCHOR_ID ON)
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

if(FIRMWARE_ROLE STREQUAL "anchor" OR FIRMWARE_ROLE STREQUAL "gateway")
    set(IMEC_MESH_PERSISTENCE_CONF
        "${CMAKE_CURRENT_BINARY_DIR}/imec-mesh-persistence.conf")
    file(WRITE "${IMEC_MESH_PERSISTENCE_CONF}"
        "CONFIG_FLASH=y\n"
        "CONFIG_FLASH_MAP=y\n"
        "CONFIG_FLASH_PAGE_LAYOUT=y\n"
        "CONFIG_NVS=y\n")
    list(FIND EXTRA_CONF_FILE "${IMEC_MESH_PERSISTENCE_CONF}"
         IMEC_MESH_PERSISTENCE_CONF_INDEX)
    if(IMEC_MESH_PERSISTENCE_CONF_INDEX EQUAL -1)
        list(APPEND EXTRA_CONF_FILE "${IMEC_MESH_PERSISTENCE_CONF}")
    endif()
endif()

if(IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/gateway-ble-connectivity-test.conf")
    list(APPEND EXTRA_DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_LIST_DIR}/overlays/gateway-ble-connectivity-test.overlay")
    if(IMEC_GATEWAY_BLE_RANGE_SCANNER)
        set(IMEC_GATEWAY_BLE_RANGE_SCANNER_CONF
            "${CMAKE_CURRENT_BINARY_DIR}/imec-gateway-ble-range-scanner.conf")
        file(WRITE "${IMEC_GATEWAY_BLE_RANGE_SCANNER_CONF}"
            "CONFIG_IMEC_GATEWAY_BLE_RANGE_SCANNER=y\n"
            "CONFIG_BT_DEVICE_NAME=\"IMEC BLE Range Scanner\"\n")
        list(APPEND EXTRA_CONF_FILE "${IMEC_GATEWAY_BLE_RANGE_SCANNER_CONF}")
    endif()
elseif(IMEC_GATEWAY_BLE_RANGE_SCANNER)
    message(FATAL_ERROR
        "IMEC_GATEWAY_BLE_RANGE_SCANNER is only valid with gateway_ble_connectivity_test")
endif()

if(IMEC_ML_BUILD AND IMEC_PRESET_EXTRA_CONF)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/${IMEC_PRESET_EXTRA_CONF}")
elseif(IMEC_PRESET_EXTRA_CONF AND NOT IMEC_HIGH_DEBUG)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/${IMEC_PRESET_EXTRA_CONF}")
endif()

if(IMEC_HIGH_DEBUG)
    if(NOT DEFINED IMEC_BENCH_STAGE)
        set(IMEC_BENCH_STAGE 0)
    endif()
    list(APPEND EXTRA_CONF_FILE
        "${CMAKE_CURRENT_LIST_DIR}/conf/high-debug.conf"
        "${CMAKE_CURRENT_LIST_DIR}/conf/stage${IMEC_BENCH_STAGE}.conf"
    )
    if(IMEC_PRESET_ROLE_CONF)
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/${IMEC_PRESET_ROLE_CONF}")
    elseif(FIRMWARE_ROLE STREQUAL "clicker")
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/role-clicker.conf")
    elseif(FIRMWARE_ROLE STREQUAL "anchor")
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/role-anchor.conf")
    elseif(FIRMWARE_ROLE STREQUAL "gateway")
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/role-gateway.conf")
    endif()
    if(IMEC_PRESET_EXTRA_CONF)
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/${IMEC_PRESET_EXTRA_CONF}")
    endif()
    if(FIRMWARE_ROLE STREQUAL "gateway" AND NOT IMEC_MESH_ROUTE_TEST_BUILD)
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/gateway-rtt.conf")
    endif()
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
            "CONFIG_BT_BUF_ACL_TX_COUNT=10\n"
            "CONFIG_BT_BUF_ACL_TX_SIZE=251\n"
            "CONFIG_BT_CTLR_DATA_LENGTH_MAX=251\n"
            "CONFIG_BT_LOG_LEVEL_WRN=y\n"
            "CONFIG_IMEC_GATEWAY_BLE=y\n"
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
            "CONFIG_LOG_BUFFER_SIZE=1536\n"
            # The exact compiler chain for gateway assignment-result encoding
            # is 3152 bytes; retain the required 1024-byte runtime margin plus
            # one alignment step without weakening the global RAM-headroom gate.
            "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4256\n")
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
        file(APPEND "${IMEC_MESH_ROUTE_TEST_NAME_CONF}"
            "CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ=y\n"
            "CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_GATEWAY_CONTROL=y\n")
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

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_firmware)

# Four immutable survey samples (or transmitter stress datagrams) may be
# admitted together while one protocol-response slot remains available.
# Keep the pure/native scheduler's wider capacity out of constrained images.
target_compile_definitions(app PRIVATE NODE_COMM_MAX_REQUESTS=5)

if(FIRMWARE_ROLE STREQUAL "clicker")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=1)
elseif(FIRMWARE_ROLE STREQUAL "anchor")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=2)
elseif(FIRMWARE_ROLE STREQUAL "gateway")
    target_compile_definitions(app PRIVATE
        DEVICE_ROLE=3
        APP_NODE_COMM_GATEWAY_ROLE=1)
else()
    message(FATAL_ERROR "FIRMWARE_ROLE must be clicker, anchor, or gateway")
endif()

set(IMEC_APP_LOCAL_SOURCES
    src/app_nvs_storage.c
    src/app_mesh_route_state_persistence.c
    src/app_click_event_sequence.c
    src/app_state.c
    src/app_device_identity.c
    src/app_board.c
    src/app_gateway_ble.c
    src/app_gateway_ble_stream.c
    src/app_gateway_command_result.c
    src/app_gateway_command_observability.c
    src/app_gateway_assignment_publisher.c
    src/app_gateway_survey_observability.c
    src/app_gateway_survey_round.c
    src/app_gateway_collection_eack.c
    src/app_gateway_eack_retry.c
    src/app_gateway_eack_policy.c
    src/app_clicker.c
    src/app_radio_recovery.c
    src/app_ml.c
    src/app_mesh_test.c
    src/app_mesh_smoke_fast.c
    src/app_mesh_c5_priority.c
    src/app_mesh_coordinator.c
    src/app_mesh_coordinator_runtime.c
    src/app_mesh_command_orchestrator.c
    src/app_mesh_arbitration_zephyr.c
    src/app_node_comm_gateway_control.c
    src/app_node_comm_sync.c
    src/app_node_comm_gateway_route_refresh.c
    src/app_operation_policy.c
    src/app_gateway_command_ingress.c
    src/app_gateway_command_lifecycle.c
    src/app_mesh_gateway_command_flow.c
    src/app_mesh_local_delivery.c
    src/app_mesh_rx_policy.c
    src/app_mesh_persistence.c
    src/app_mesh_preemption.c
    src/app_mesh_flood.c
    src/app_mesh_ch9_ack.c
    src/app_mesh_direct_probe_diag.c
    src/app_mesh_direct_gateway_retry.c
    src/app_mesh_gateway_command_priority.c
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
    src/app_anchor.c
    src/app_anchor_command_completion.c
    src/app_anchor_ranging.c
    src/app_anchor_survey_discovery.c
    src/app_anchor_survey_result_delivery.c
    src/app_anchor_survey_runtime.c
    src/app_high_debug.c
    src/app_watchdog.c
    src/dwm3000_driver_handler_contract.c
)

if(FIRMWARE_ROLE STREQUAL "gateway")
    list(APPEND IMEC_APP_LOCAL_SOURCES
        src/app_gateway_collection_receipts.c
        src/app_gateway_terminal_receipts.c)
endif()

target_sources(app PRIVATE
    ../src/anchor_range_fragment_policy.c
    ../src/anchor_range_journal.c
    ../src/node_comm.c
    ../src/node_transaction.c
    ../src/semantic_digest.c
    ../src/stack_diag_transport.c
    ../src/watchdog_adoption.c
)

if(IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    target_sources(app PRIVATE
        src/main.c
        ${IMEC_APP_LOCAL_SOURCES}
        ../src/gateway_membership.c
        ../src/gateway_ble_transport.c
        ../src/protocol.c
        ../src/serial_frame.c
    )
else()
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
        ../src/gateway_command.c
        ../src/gateway_collection_journal.c
        ../src/gateway_ble_transport.c
        ../src/discovery_assignment.c
        ../src/dwm3000_timing.c
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
        ../src/route.c
        ../src/survey.c
        ../src/survey_pair_planner.c
        ../src/survey_pair_round_runtime.c
        ../src/survey_round_control.c
        ../src/survey_gateway_transaction.c
        ../src/survey_pair_lease.c
    )
endif()

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
string(SHA256 IMEC_STACK_DIAG_ID_HASH
    "${IMEC_BUILD_PRESET}|${IMEC_GIT_VERSION}|${IMEC_BUILD_TIMESTAMP}|${BOARD}|${CMAKE_C_COMPILER_ID}|${CMAKE_C_COMPILER_VERSION}")
set(IMEC_STACK_DIAG_BUILD_ID
    "imec-stack-v1:${IMEC_BUILD_PRESET}:${IMEC_STACK_DIAG_ID_HASH}")

target_compile_definitions(app PRIVATE
    IMEC_BUILD_PRESET_NAME="${IMEC_BUILD_PRESET}"
    IMEC_GIT_VERSION="${IMEC_GIT_VERSION}"
    IMEC_BUILD_TIMESTAMP="${IMEC_BUILD_TIMESTAMP}"
    IMEC_BOARD_TARGET="${BOARD}"
    IMEC_STACK_DIAG_BUILD_ID="${IMEC_STACK_DIAG_BUILD_ID}"
    IMEC_USE_HARDWARE_ANCHOR_ID=$<BOOL:${IMEC_USE_HARDWARE_ANCHOR_ID}>
)
# The verifier consumes both compiler frame records and IPA call graphs.  The
# latter is required for root-to-function attribution; missing graphs block a
# deployable mesh capture or flash.
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
if(NOT "${IMEC_HIGH_DEBUG_ANCHOR_SLOT}" STREQUAL "")
    if(NOT IMEC_HIGH_DEBUG)
        message(FATAL_ERROR "IMEC_HIGH_DEBUG_ANCHOR_SLOT is only valid for high-debug builds")
    endif()
    if(NOT FIRMWARE_ROLE STREQUAL "anchor")
        message(FATAL_ERROR "IMEC_HIGH_DEBUG_ANCHOR_SLOT is only valid for anchor builds")
    endif()
    if(DEFINED IMEC_BENCH_STAGE AND IMEC_BENCH_STAGE LESS 2)
        message(FATAL_ERROR "IMEC_HIGH_DEBUG_ANCHOR_SLOT is only valid for multi-anchor Stage 2/3 builds")
    endif()
    if(IMEC_HIGH_DEBUG_ANCHOR_SLOT LESS 0 OR IMEC_HIGH_DEBUG_ANCHOR_SLOT GREATER 49)
        message(FATAL_ERROR "IMEC_HIGH_DEBUG_ANCHOR_SLOT must be in the UWB discovery slot range 0..49")
    endif()
    target_compile_definitions(app PRIVATE
        IMEC_HIGH_DEBUG_ANCHOR_SLOT_ENABLED=1
        IMEC_HIGH_DEBUG_ANCHOR_SLOT=${IMEC_HIGH_DEBUG_ANCHOR_SLOT})
endif()
if(NOT "${IMEC_ML_ANCHOR_SLOT}" STREQUAL "")
    target_compile_definitions(app PRIVATE IMEC_ML_ANCHOR_SLOT=${IMEC_ML_ANCHOR_SLOT})
endif()
