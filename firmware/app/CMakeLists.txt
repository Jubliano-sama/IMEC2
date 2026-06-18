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

set(IMEC_BUILD_PRESET "${IMEC_BUILD_PRESET}" CACHE STRING "High-debug preset name, for example tag_stage0_highdebug")
set_property(CACHE IMEC_BUILD_PRESET PROPERTY STRINGS
    ""
    tag_stage0_highdebug
    tag_stage1_highdebug
    tag_stage1_wake_spam
    anchor_stage1_highdebug
    tag_stage2_highdebug
    anchor_stage2_highdebug
    tag_stage3_highdebug
    anchor_stage3_highdebug
    gateway_stage3_highdebug
)
set(IMEC_HIGH_DEBUG OFF CACHE BOOL "Enable high-debug staged bring-up profile")
set(IMEC_USB_BOOTLOADER ON CACHE BOOL "Add MCUboot USB-C recovery config for high-debug builds")
set(IMEC_DEVICE_ID "" CACHE STRING "Override DEVICE_ID, for example 0x1111222233334444ull")
set(IMEC_GATEWAY_ID "" CACHE STRING "Override GATEWAY_ID")
set(IMEC_NETWORK_ID "" CACHE STRING "Override NETWORK_ID")

set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway")
set_property(CACHE FIRMWARE_ROLE PROPERTY STRINGS clicker tag anchor gateway)
set(DWM3000_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dwm3000 examples and sdk")

if(IMEC_BUILD_PRESET)
    set(IMEC_HIGH_DEBUG ON CACHE BOOL "Enable high-debug staged bring-up profile" FORCE)
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
    else()
        message(FATAL_ERROR "Unknown IMEC_BUILD_PRESET: ${IMEC_BUILD_PRESET}")
    endif()
endif()

if(FIRMWARE_ROLE STREQUAL "tag")
    set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker/tag, anchor, or gateway" FORCE)
endif()

if(FIRMWARE_ROLE STREQUAL "clicker")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/prj-clicker.conf")
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
    if(FIRMWARE_ROLE STREQUAL "gateway")
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/gateway-rtt.conf")
    endif()
    if(IMEC_USB_BOOTLOADER)
        list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/conf/usb-bootloader.conf")
        list(APPEND EXTRA_DTC_OVERLAY_FILE
            "${CMAKE_CURRENT_LIST_DIR}/overlays/bootloader-single-slot.overlay")
    endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_firmware)

if(FIRMWARE_ROLE STREQUAL "clicker")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=1)
elseif(FIRMWARE_ROLE STREQUAL "anchor")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=2)
elseif(FIRMWARE_ROLE STREQUAL "gateway")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=3)
else()
    message(FATAL_ERROR "FIRMWARE_ROLE must be clicker, anchor, or gateway")
endif()

# Operational wake, discovery, ranging, and relay traffic use UWB frames.
target_sources(app PRIVATE
    src/main.c
    src/dwm3000_driver.c
    src/dwm3000_port.c
    src/dwm3000_sdk_port.c
    "${DWM3000_SDK_DIR}/decadriver/deca_device.c"
    ../src/debug_log.c
    ../src/protocol.c
    ../src/gateway_command.c
    ../src/uwb.c
    ../src/uwb_ble_courtesy.c
    ../src/uwb_session.c
    ../src/mesh.c
    ../src/mesh_relay.c
    ../src/report.c
    ../src/serial_frame.c
    ../src/status.c
    ../src/route.c
    ../src/survey.c
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

target_compile_definitions(app PRIVATE
    IMEC_BUILD_PRESET_NAME="${IMEC_BUILD_PRESET}"
    IMEC_GIT_VERSION="${IMEC_GIT_VERSION}"
    IMEC_BUILD_TIMESTAMP="${IMEC_BUILD_TIMESTAMP}"
    IMEC_BOARD_TARGET="${BOARD}"
)
if(IMEC_DEVICE_ID)
    target_compile_definitions(app PRIVATE DEVICE_ID=${IMEC_DEVICE_ID})
endif()
if(IMEC_GATEWAY_ID)
    target_compile_definitions(app PRIVATE GATEWAY_ID=${IMEC_GATEWAY_ID})
endif()
if(IMEC_NETWORK_ID)
    target_compile_definitions(app PRIVATE NETWORK_ID=${IMEC_NETWORK_ID})
endif()
