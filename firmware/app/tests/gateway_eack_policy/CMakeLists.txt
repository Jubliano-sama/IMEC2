cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gateway_eack_policy_test)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../include
    ${CMAKE_CURRENT_LIST_DIR}/../../src
)

target_sources(app PRIVATE
    src/main.c
    ../../src/app_gateway_eack_policy.c
)
