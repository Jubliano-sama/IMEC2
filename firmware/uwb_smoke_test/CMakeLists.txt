cmake_minimum_required(VERSION 3.20.0)

set(IMEC_APP_DIR "${CMAKE_CURRENT_LIST_DIR}/../app")
set(DWM3000_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../dwm3000 examples and sdk")
list(APPEND DTS_ROOT "${IMEC_APP_DIR}")

set(UWB_SMOKE_ROLE "tx" CACHE STRING "UWB smoke-test role: tx or rx")
set_property(CACHE UWB_SMOKE_ROLE PROPERTY STRINGS tx rx)

if(UWB_SMOKE_ROLE STREQUAL "tx")
    set(UWB_SMOKE_DEVICE_ID "0x1111222233334444ull" CACHE STRING "Smoke-test transmitter device ID")
elseif(UWB_SMOKE_ROLE STREQUAL "rx")
    set(UWB_SMOKE_DEVICE_ID "0x5555666677778888ull" CACHE STRING "Smoke-test receiver device ID")
else()
    message(FATAL_ERROR "UWB_SMOKE_ROLE must be tx or rx")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imec_uwb_smoke_test)

target_sources(app PRIVATE
    src/main.c
    "${IMEC_APP_DIR}/src/dwm3000_driver.c"
    "${IMEC_APP_DIR}/src/dwm3000_port.c"
    "${IMEC_APP_DIR}/src/dwm3000_sdk_port.c"
    "${DWM3000_SDK_DIR}/decadriver/deca_device.c"
    ../src/debug_log.c
    ../src/protocol.c
    ../src/uwb.c
    ../src/uwb_session.c
)

target_include_directories(app PRIVATE
    ../include
    "${IMEC_APP_DIR}/src"
    "${DWM3000_SDK_DIR}/decadriver"
)

target_compile_definitions(app PRIVATE
    DEVICE_ID=${UWB_SMOKE_DEVICE_ID}
    IMEC_BUILD_PRESET_NAME="uwb_smoke_${UWB_SMOKE_ROLE}"
    IMEC_GIT_VERSION="custom"
    IMEC_BUILD_TIMESTAMP="custom"
    IMEC_BOARD_TARGET="${BOARD}"
)

if(UWB_SMOKE_ROLE STREQUAL "tx")
    target_compile_definitions(app PRIVATE UWB_SMOKE_TX=1 DEVICE_ROLE=1)
else()
    target_compile_definitions(app PRIVATE UWB_SMOKE_RX=1 DEVICE_ROLE=2)
endif()
