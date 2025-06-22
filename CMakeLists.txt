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
    # Common source files for both hub and node
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
        src/config/config_base.cpp
        src/config/node_config.cpp
        src/config/hub_config.cpp
        src/application_mode.cpp
        src/demo_mode.cpp
        src/hub_mode.cpp
        src/production_mode.cpp
    )
    
    # Hub executable
    add_executable(bramble_hub 
        bramble.cpp 
        ${COMMON_SOURCES}
    )
    target_compile_definitions(bramble_hub PRIVATE IS_HUB=1)
    message(STATUS "Building hub version")
    
    # Node executable
    add_executable(bramble_node
        bramble.cpp 
        ${COMMON_SOURCES}
    )
    target_compile_definitions(bramble_node PRIVATE IS_HUB=0)
    message(STATUS "Building node version")
    
    # Add demo mode option
    option(DEMO_MODE "Build in demo mode with verbose output" ON)
    if(DEMO_MODE)
        target_compile_definitions(bramble_hub PRIVATE DEMO_MODE=1)
        target_compile_definitions(bramble_node PRIVATE DEMO_MODE=1)
        message(STATUS "Demo mode enabled")
    else()
        target_compile_definitions(bramble_hub PRIVATE DEMO_MODE=0)
        target_compile_definitions(bramble_node PRIVATE DEMO_MODE=0)
        message(STATUS "Production mode enabled")
    endif()
endif()

# Configure both hub and node targets if not building tests
if(NOT BUILD_TESTS)
    # Hub configuration
    pico_set_program_name(bramble_hub "bramble_hub")
    pico_set_program_version(bramble_hub "0.1")
    pico_enable_stdio_uart(bramble_hub 0)
    pico_enable_stdio_usb(bramble_hub 1)
    
    # Node configuration  
    pico_set_program_name(bramble_node "bramble_node")
    pico_set_program_version(bramble_node "0.1")
    pico_enable_stdio_uart(bramble_node 0)
    pico_enable_stdio_usb(bramble_node 1)
    
    # Common libraries for both targets
    foreach(target bramble_hub bramble_node)
        target_link_libraries(${target} pico_stdlib)
        
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_LIST_DIR}/src
        )
        
        target_link_libraries(${target} 
            hardware_spi
            hardware_timer
            hardware_clocks
            hardware_pio
            hardware_flash
            hardware_sync
            pico_multicore
        )
        
        # Add pico-extras libraries if available
        if(EXISTS ${PICO_EXTRAS_PATH})
            target_link_libraries(${target} 
                hardware_sleep
                hardware_rosc
            )
        endif()
        
        # Generate PIO headers
        pico_generate_pio_header(${target} ${CMAKE_CURRENT_LIST_DIR}/src/hal/ws2812.pio)
        
        pico_add_extra_outputs(${target})
    endforeach()
endif()

