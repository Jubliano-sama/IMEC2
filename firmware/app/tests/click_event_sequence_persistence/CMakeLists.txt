cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(click_event_sequence_persistence_test)

target_compile_definitions(app PRIVATE
    CONFIG_IMEC_CLICK_EVENT_SEQUENCE_PERSISTENCE=1
    DEVICE_ROLE=1
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

target_sources(app PRIVATE
    src/main.c
    ../../src/app_nvs_storage.c
    ../../src/app_click_event_sequence.c
)
