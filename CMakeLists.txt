# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD adafruit_feather_rp2040 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Use W25Q boot2 - matches W25Q16JV flash chip
set(PICO_DEFAULT_BOOT_STAGE2_FILE ${PICO_SDK_PATH}/src/rp2040/boot_stage2/boot2_w25q080.S)

# Pull in Pico extras (if available)
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-extras)
if(EXISTS ${PICO_EXTRAS_PATH})
    include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)
    message(STATUS "Found pico-extras at ${PICO_EXTRAS_PATH}")
else()
    message(WARNING "pico-extras not found at ${PICO_EXTRAS_PATH}. Sleep functionality will be limited.")
endif()

project(bramble C CXX ASM)

# Read firmware version from VERSION file (major.minor.build)
file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" BRAMBLE_VERSION_RAW)
string(STRIP "${BRAMBLE_VERSION_RAW}" BRAMBLE_VERSION_STRING)
string(REPLACE "." ";" BRAMBLE_VERSION_PARTS "${BRAMBLE_VERSION_STRING}")
list(GET BRAMBLE_VERSION_PARTS 0 BRAMBLE_VERSION_MAJOR)
list(GET BRAMBLE_VERSION_PARTS 1 BRAMBLE_VERSION_MINOR)
list(GET BRAMBLE_VERSION_PARTS 2 BRAMBLE_VERSION_BUILD)

# Auto-increment build number once per build session.
# A guard file prevents multiple increments when building several
# hardware variants in parallel (each runs cmake configure separately).
set(VERSION_GUARD_FILE "${CMAKE_CURRENT_LIST_DIR}/.version_guard")
set(SHOULD_INCREMENT TRUE)

if(EXISTS "${VERSION_GUARD_FILE}")
    file(TIMESTAMP "${VERSION_GUARD_FILE}" _guard_ts "%s" UTC)
    string(TIMESTAMP _now_ts "%s" UTC)
    math(EXPR _elapsed "${_now_ts} - ${_guard_ts}")
    if(_elapsed LESS 30)
        set(SHOULD_INCREMENT FALSE)
    endif()
endif()

if(SHOULD_INCREMENT)
    math(EXPR BRAMBLE_VERSION_BUILD "${BRAMBLE_VERSION_BUILD} + 1")
    set(BRAMBLE_VERSION_STRING "${BRAMBLE_VERSION_MAJOR}.${BRAMBLE_VERSION_MINOR}.${BRAMBLE_VERSION_BUILD}")
    file(WRITE "${CMAKE_CURRENT_LIST_DIR}/VERSION" "${BRAMBLE_VERSION_STRING}\n")
    file(WRITE "${VERSION_GUARD_FILE}" "${BRAMBLE_VERSION_STRING}\n")
endif()

message(STATUS "Bramble firmware version: ${BRAMBLE_VERSION_STRING}")

set(PICO_CXX_ENABLE_EXCEPTIONS 1)

set(PICO_CXX_ENABLE_RTTI 1)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Build configuration: production or test
option(BUILD_TESTS "Build test version instead of production" OFF)

if(BUILD_TESTS)
    # Test executable
    add_executable(bramble_test 
        src/tests/test_main.cpp
        src/tests/test_framework.cpp
        src/tests/reliability_tests.cpp
        src/tests/mock_sx1276.cpp
        src/lora/message.cpp
        src/lora/reliable_messenger.cpp
        src/hal/neopixel.cpp
    )
    message(STATUS "Building test version")
    
    # Common test settings
    pico_set_program_name(bramble_test "bramble_test")
    pico_set_program_version(bramble_test "${BRAMBLE_VERSION_STRING}")
    pico_enable_stdio_uart(bramble_test 0)
    pico_enable_stdio_usb(bramble_test 1)

    # Test libraries
    target_link_libraries(bramble_test pico_stdlib)
    target_include_directories(bramble_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
    )
    target_compile_definitions(bramble_test PRIVATE
        BRAMBLE_VERSION_MAJOR=${BRAMBLE_VERSION_MAJOR}
        BRAMBLE_VERSION_MINOR=${BRAMBLE_VERSION_MINOR}
        BRAMBLE_VERSION_BUILD=${BRAMBLE_VERSION_BUILD}
    )
    
    target_link_libraries(bramble_test 
        hardware_spi
        hardware_timer
        hardware_clocks
        hardware_pio
        hardware_flash
        hardware_sync
    )
    
    pico_generate_pio_header(bramble_test ${CMAKE_CURRENT_LIST_DIR}/src/hal/ws2812.pio)
    pico_add_extra_outputs(bramble_test)
    
else()
    # Hardware variant selection (ALL builds every variant in parallel)
    set(HARDWARE_VARIANT "IRRIGATION" CACHE STRING "Hardware variant type")
    set_property(CACHE HARDWARE_VARIANT PROPERTY STRINGS
        ALL GENERIC HUB CONTROLLER IRRIGATION SENSOR)

    # Common source files for all variants
    set(COMMON_SOURCES
        src/lora/sx1276.cpp
        src/lora/message.cpp
        src/lora/reliable_messenger.cpp
        src/lora/address_manager.cpp
        src/lora/batch_transmitter.cpp
        src/lora/heartbeat_client.cpp
        src/lora/hub_router.cpp
        src/lora/network_stats.cpp
        src/hal/neopixel.cpp
        src/hal/flash.cpp
        src/hal/external_flash.cpp
        src/hal/logger.cpp
        src/hal/spi_device.cpp
        src/hal/gpio_interrupt_manager.cpp
        src/hal/pmu_protocol.cpp
        src/hal/pmu_client.cpp
        src/hal/pmu_reliability.cpp
        src/config/config_base.cpp
        src/config/node_config.cpp
        src/config/hub_config.cpp
        src/storage/sensor_flash_buffer.cpp
        src/util/work_tracker.cpp
        src/util/task_queue.cpp
        src/util/base_state_machine.cpp
        src/util/sensor_state_machine.cpp
        src/util/irrigation_state_machine.cpp
        src/modes/application_mode.cpp
        src/modes/hub_mode.cpp
    )

    # List of targets to configure
    set(BUILD_TARGETS "")

    # Build specialized executables (ALL builds every variant)
    if(HARDWARE_VARIANT STREQUAL "ALL" OR HARDWARE_VARIANT STREQUAL "IRRIGATION")
        add_executable(bramble_irrigation
            main.cpp
            src/modes/irrigation_mode.cpp
            src/hal/valve_controller.cpp
            src/hal/hbridge.cpp
            src/hal/valve_indexer.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_irrigation PRIVATE
            HARDWARE_IRRIGATION=1
            DEFAULT_IS_HUB=0
        )
        list(APPEND BUILD_TARGETS bramble_irrigation)
        message(STATUS "Building IRRIGATION variant")
    endif()

    if(HARDWARE_VARIANT STREQUAL "ALL" OR HARDWARE_VARIANT STREQUAL "CONTROLLER")
        add_executable(bramble_controller
            main.cpp
            src/modes/controller_mode.cpp
            src/hal/controller_inputs.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_controller PRIVATE
            HARDWARE_CONTROLLER=1
            DEFAULT_IS_HUB=1
        )
        list(APPEND BUILD_TARGETS bramble_controller)
        message(STATUS "Building CONTROLLER variant")
    endif()

    if(HARDWARE_VARIANT STREQUAL "ALL" OR HARDWARE_VARIANT STREQUAL "HUB")
        add_executable(bramble_hub
            main.cpp
            src/modes/hub_mode.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_hub PRIVATE
            HARDWARE_GENERIC=1
            DEFAULT_IS_HUB=1
        )
        list(APPEND BUILD_TARGETS bramble_hub)
        message(STATUS "Building HUB variant (pure hub mode)")
    endif()

    if(HARDWARE_VARIANT STREQUAL "ALL" OR HARDWARE_VARIANT STREQUAL "SENSOR")
        add_executable(bramble_sensor
            main.cpp
            src/modes/sensor_mode.cpp
            src/hal/cht832x.cpp
            src/util/sensor_pmu_manager.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_sensor PRIVATE
            HARDWARE_SENSOR=1
            DEFAULT_IS_HUB=0
        )
        list(APPEND BUILD_TARGETS bramble_sensor)
        message(STATUS "Building SENSOR variant")
    endif()

    if(HARDWARE_VARIANT STREQUAL "ALL" OR HARDWARE_VARIANT STREQUAL "GENERIC")
        add_executable(bramble_generic
            main.cpp
            src/modes/application_mode.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_generic PRIVATE
            HARDWARE_GENERIC=1
            DEFAULT_IS_HUB=0
        )
        list(APPEND BUILD_TARGETS bramble_generic)
        message(STATUS "Building GENERIC variant")
    endif()

endif()

# Configure all production targets
if(NOT BUILD_TESTS)
    foreach(TARGET_NAME IN LISTS BUILD_TARGETS)
        # Target configuration
        pico_set_program_name(${TARGET_NAME} "${TARGET_NAME}")
        pico_set_program_version(${TARGET_NAME} "${BRAMBLE_VERSION_STRING}")

        pico_enable_stdio_rtt(${TARGET_NAME} 0)

        # Configure stdio per-target:
        # - Hub: USB CDC for debug output (UART1 reserved for Raspberry Pi API)
        # - Other nodes: UART for debug output (no USB)
        if(TARGET_NAME STREQUAL "bramble_hub")
            pico_enable_stdio_usb(${TARGET_NAME} 1)
            pico_enable_stdio_uart(${TARGET_NAME} 0)
        else()
            pico_enable_stdio_usb(${TARGET_NAME} 0)
            pico_enable_stdio_uart(${TARGET_NAME} 1)
        endif()

        # UART1 pin configuration for custom board (all targets use same pins)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            PICO_DEFAULT_UART=1
            PICO_DEFAULT_UART_TX_PIN=24
            PICO_DEFAULT_UART_RX_PIN=-1
            PICO_XOSC_STARTUP_DELAY_MULTIPLIER=128
            BRAMBLE_VERSION_MAJOR=${BRAMBLE_VERSION_MAJOR}
            BRAMBLE_VERSION_MINOR=${BRAMBLE_VERSION_MINOR}
            BRAMBLE_VERSION_BUILD=${BRAMBLE_VERSION_BUILD}
        )

        # Suppress GCC ARM ABI notes (safe for standalone firmware)
        target_compile_options(${TARGET_NAME} PRIVATE -Wno-psabi)

        # Libraries and configuration for the target
        target_link_libraries(${TARGET_NAME} pico_stdlib)

        target_include_directories(${TARGET_NAME} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_LIST_DIR}/src
        )

        target_link_libraries(${TARGET_NAME}
            hardware_spi
            hardware_i2c
            hardware_timer
            hardware_clocks
            hardware_pio
            hardware_flash
            hardware_sync
            hardware_rtc
            hardware_watchdog
            pico_multicore
            pico_util
            pico_unique_id
        )

        # Add pico-extras libraries if available
        if(EXISTS ${PICO_EXTRAS_PATH})
            target_link_libraries(${TARGET_NAME}
                hardware_sleep
                hardware_rosc
            )
        endif()

        # Generate PIO headers
        pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/hal/ws2812.pio)

        pico_add_extra_outputs(${TARGET_NAME})
    endforeach()
endif()

