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

# Pull in Pico extras (if available)
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-extras)
if(EXISTS ${PICO_EXTRAS_PATH})
    include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)
    message(STATUS "Found pico-extras at ${PICO_EXTRAS_PATH}")
else()
    message(WARNING "pico-extras not found at ${PICO_EXTRAS_PATH}. Sleep functionality will be limited.")
endif()

project(bramble C CXX ASM)

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
    pico_set_program_version(bramble_test "0.1")
    pico_enable_stdio_uart(bramble_test 0)
    pico_enable_stdio_usb(bramble_test 1)
    
    # Test libraries
    target_link_libraries(bramble_test pico_stdlib)
    target_include_directories(bramble_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
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
    # Hardware variant selection
    set(HARDWARE_VARIANT "IRRIGATION" CACHE STRING "Hardware variant type")
    set_property(CACHE HARDWARE_VARIANT PROPERTY STRINGS
        GENERIC HUB CONTROLLER IRRIGATION SENSOR)
    
    # Common source files for all variants
    set(COMMON_SOURCES
        src/lora/sx1276.cpp
        src/lora/message.cpp
        src/lora/reliable_messenger.cpp
        src/lora/address_manager.cpp
        src/lora/hub_router.cpp
        src/lora/network_stats.cpp
        src/hal/neopixel.cpp
        src/hal/flash.cpp
        src/hal/logger.cpp
        src/hal/spi_device.cpp
        src/hal/gpio_interrupt_manager.cpp
        src/hal/pmu_protocol.cpp
        src/hal/pmu_client.cpp
        src/config/config_base.cpp
        src/config/node_config.cpp
        src/config/hub_config.cpp
        src/modes/application_mode.cpp
        src/modes/demo_mode.cpp
        src/modes/hub_mode.cpp
        src/modes/production_mode.cpp
    )
    
    # Build specialized executable based on hardware variant
    if(HARDWARE_VARIANT STREQUAL "IRRIGATION")
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
            DEFAULT_IS_HUB=0  # Irrigation nodes default to node role
        )
        set(TARGET_NAME bramble_irrigation)
        message(STATUS "Building IRRIGATION variant")
        
    elseif(HARDWARE_VARIANT STREQUAL "CONTROLLER")
        add_executable(bramble_controller
            main.cpp
            src/modes/controller_mode.cpp
            src/hal/controller_inputs.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_controller PRIVATE
            HARDWARE_CONTROLLER=1
            DEFAULT_IS_HUB=1  # Controllers default to hub role
        )
        set(TARGET_NAME bramble_controller)
        message(STATUS "Building CONTROLLER variant")

    elseif(HARDWARE_VARIANT STREQUAL "HUB")
        add_executable(bramble_hub
            main.cpp
            src/modes/hub_mode.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_hub PRIVATE
            HARDWARE_GENERIC=1  # Use generic hardware base
            DEFAULT_IS_HUB=1    # Always run as hub
        )
        set(TARGET_NAME bramble_hub)
        message(STATUS "Building HUB variant (pure hub mode)")

    elseif(HARDWARE_VARIANT STREQUAL "SENSOR")
        add_executable(bramble_sensor
            main.cpp
            src/modes/sensor_mode.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_sensor PRIVATE
            HARDWARE_SENSOR=1
            DEFAULT_IS_HUB=0  # Sensors default to node role
        )
        set(TARGET_NAME bramble_sensor)
        message(STATUS "Building SENSOR variant")
        
    else() # GENERIC
        add_executable(bramble_generic
            main.cpp
            src/modes/generic_mode.cpp
            ${COMMON_SOURCES}
        )
        target_compile_definitions(bramble_generic PRIVATE
            HARDWARE_GENERIC=1
            DEFAULT_IS_HUB=0  # Generic nodes default to node role
        )
        set(TARGET_NAME bramble_generic)
        message(STATUS "Building GENERIC variant")
    endif()
    
    # Add demo mode option
    option(DEMO_MODE "Build in demo mode with verbose output" OFF)
    if(DEMO_MODE)
        target_compile_definitions(${TARGET_NAME} PRIVATE DEMO_MODE=1)
        message(STATUS "Demo mode enabled")
    else()
        target_compile_definitions(${TARGET_NAME} PRIVATE DEMO_MODE=0)
        message(STATUS "Production mode enabled")
    endif()
endif()

# Configure specialized target if not building tests
if(NOT BUILD_TESTS)
    # Target configuration
    pico_set_program_name(${TARGET_NAME} "${TARGET_NAME}")
    pico_set_program_version(${TARGET_NAME} "0.1")
    pico_enable_stdio_uart(${TARGET_NAME} 0)
    pico_enable_stdio_usb(${TARGET_NAME} 1)
    
    # Libraries and configuration for the target
    target_link_libraries(${TARGET_NAME} pico_stdlib)
    
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
    )
    
    target_link_libraries(${TARGET_NAME}
        hardware_spi
        hardware_timer
        hardware_clocks
        hardware_pio
        hardware_flash
        hardware_sync
        hardware_rtc
        pico_multicore
        pico_util
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
endif()

