cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT "${CMAKE_CURRENT_LIST_DIR}")

set(FIRMWARE_ROLE "clicker" CACHE STRING "Firmware role: clicker, anchor, or gateway")
set_property(CACHE FIRMWARE_ROLE PROPERTY STRINGS clicker anchor gateway)
set(DWM3000_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dwm3000 examples and sdk")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_ble_firmware)

if(FIRMWARE_ROLE STREQUAL "clicker")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=1)
elseif(FIRMWARE_ROLE STREQUAL "anchor")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=2)
elseif(FIRMWARE_ROLE STREQUAL "gateway")
    target_compile_definitions(app PRIVATE DEVICE_ROLE=3)
else()
    message(FATAL_ERROR "FIRMWARE_ROLE must be clicker, anchor, or gateway")
endif()

target_sources(app PRIVATE
    src/main.c
    src/dwm3000_driver.c
    src/dwm3000_port.c
    src/dwm3000_sdk_port.c
    "${DWM3000_SDK_DIR}/decadriver/deca_device.c"
    ../src/protocol.c
    ../src/discovery.c
    ../src/uwb.c
    ../src/mesh.c
    ../src/report.c
    ../src/status.c
    ../src/route.c
    ../src/survey.c
)

target_include_directories(app PRIVATE
    ../include
    "${DWM3000_SDK_DIR}/decadriver"
)
