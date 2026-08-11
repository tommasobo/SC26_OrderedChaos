cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# -------------------------------
#      Project Configuration
# -------------------------------
project(htsim_project LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------------------
#        Build Options
# -------------------------------
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Base compiler flags
set(BASE_COMPILE_OPTIONS
    -Wall
    -g
    -Wsign-compare
    -Wuninitialized
    -fPIE
)

# Optimization level
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND BASE_COMPILE_OPTIONS -O1)
else()
    list(APPEND BASE_COMPILE_OPTIONS -O3)
endif()

# Enable sanitizers if requested
if(ENABLE_SANITIZERS)
    message(STATUS "Sanitizers ENABLED")
    list(APPEND BASE_COMPILE_OPTIONS
        -fsanitize=address
        -fsanitize=undefined
        -fno-omit-frame-pointer
    )
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fsanitize=undefined")
endif()

add_compile_options(${BASE_COMPILE_OPTIONS})

# -------------------------------
# Set SPDLOG_ACTIVE_LEVEL globally based on build type
# -------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

# -------------------------------
#     Fetching Dependencies
# -------------------------------
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.1
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)
enable_testing()

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# -------------------------------
#       Helper Function
# -------------------------------
function(add_symlink_target EXECUTABLE TARGET_DIR)
    add_custom_command(
        TARGET ${EXECUTABLE}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove ${TARGET_DIR}/${EXECUTABLE}
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<TARGET_FILE:${EXECUTABLE}>
            ${TARGET_DIR}/${EXECUTABLE}
        COMMENT "Removing and recreating symlink for ${EXECUTABLE} in ${TARGET_DIR}"
    )
endfunction()

# -------------------------------
#          Libraries
# -------------------------------
set(SOURCE_FILES
    core/callback_pipe.cpp
    core/connection_matrix.cpp
    core/eventlist.cpp
    core/pipe.cpp
    core/route.cpp
    core/trigger.cpp
    host/oversubscribed_cc.cpp
    host/pcie_model.cpp
    host/uec_nic.cpp
    host/uec_sink.cpp
    host/rack_tlp.cpp
    host/uec_src.cpp
    host/uec_src_port.cpp
    host/uec_sink_port.cpp
    host/uec_pull_pacer.cpp
    logging/data_collector.cpp
    logging/logfile.cpp
    logging/loggers.cpp
    logging/metric.cpp
    logging/uec_logger.cpp
    packet/eth_pause_packet.cpp
    packet/packet_flow.cpp
    packet/packet.cpp
    packet/uec_packet.cpp
    queue/aeolusqueue.cpp
    queue/compositeprioqueue.cpp
    queue/compositequeue.cpp
    queue/cpqueue.cpp
    queue/ecnprioqueue.cpp
    queue/ecnqueue.cpp
    queue/exoqueue.cpp
    queue/fairpullqueue.cpp
    queue/priopullqueue.cpp
    queue/prioqueue.cpp
    queue/qos_scheduler.cpp
    queue/queue_lossless_input.cpp
    queue/queue_lossless_output.cpp
    queue/queue_lossless.cpp
    queue/queue.cpp
    queue/randomqueue.cpp
    queue/shared_queue.cpp
    switch/fat_tree_switch.cpp
    switch/route_table.cpp
    switch/switch_buffer.cpp
    switch/switch.cpp
    topology/fat_tree_topology.cpp
    utils/helpers.cpp
    logsim-interface.cpp
    compute_event.cpp
    null_event.cpp
    atlahs_htsim_api.cpp
)

add_library(htsim STATIC ${SOURCE_FILES})
target_include_directories(htsim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/datacenter
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/packet
    ${CMAKE_CURRENT_SOURCE_DIR}/queue
    ${CMAKE_CURRENT_SOURCE_DIR}/switch
    ${CMAKE_CURRENT_SOURCE_DIR}/topology
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)
target_link_libraries(htsim PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog)

# -------------------------------
#         Executables
# -------------------------------
add_executable(htsim_uec main_uec.cpp)
set(ALL_EXECUTABLES htsim_uec)

foreach(EXECUTABLE ${ALL_EXECUTABLES})
    target_link_libraries(${EXECUTABLE} PRIVATE htsim)
    add_symlink_target(${EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# -------------------------------
#          Unit Tests
# -------------------------------
set(UNIT_TEST_FILES
    core/callback_pipe_test
    core/pipe_test
    core/route_test
    host/rack_tlp_test
    logging/metric_test
    packet/uec_packet_test
    switch/switch_buffer_test
    queue/qos_scheduler_test
    queue/shared_queue_test
)

foreach(UNIT_TEST_FILE ${UNIT_TEST_FILES})
    get_filename_component(TEST_NAME ${UNIT_TEST_FILE} NAME)
    add_executable(${TEST_NAME} ${UNIT_TEST_FILE}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE htsim gtest_main gmock_main)
    gtest_discover_tests(${TEST_NAME} PROPERTIES LABELS "unit" PROPERTIES TIMEOUT 60)
endforeach()
