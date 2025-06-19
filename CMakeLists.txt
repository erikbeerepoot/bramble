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
    add_executable(bramble 
        src/tests/test_main.cpp
        src/tests/test_framework.cpp
        src/tests/reliability_tests.cpp
        src/tests/mock_sx1276.cpp
        src/lora/message.cpp
        src/lora/reliable_messenger.cpp
        src/hal/neopixel.cpp
    )
    message(STATUS "Building test version")
else()
    # Production executable
    add_executable(bramble 
        bramble.cpp 
        src/lora/sx1276.cpp
        src/lora/message.cpp
        src/lora/reliable_messenger.cpp
        src/lora/address_manager.cpp
        src/lora/hub_router.cpp
        src/hal/neopixel.cpp
        src/hal/flash.cpp
        src/hal/logger.cpp
        src/hal/spi_device.cpp
        src/config/node_config.cpp
        src/config/hub_config.cpp
    )
    message(STATUS "Building production version")
endif()

pico_set_program_name(bramble "bramble")
pico_set_program_version(bramble "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(bramble 0)
pico_enable_stdio_usb(bramble 1)

# Add the standard library to the build
target_link_libraries(bramble
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(bramble PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
)

# Add any user requested libraries
target_link_libraries(bramble 
        hardware_spi
        hardware_timer
        hardware_clocks
        hardware_pio
        hardware_flash
        hardware_sync
        )

# Add pico-extras libraries if available
if(EXISTS ${PICO_EXTRAS_PATH})
    target_link_libraries(bramble 
            hardware_sleep
            hardware_rosc
            )
endif()

# Generate PIO headers
pico_generate_pio_header(bramble ${CMAKE_CURRENT_LIST_DIR}/src/hal/ws2812.pio)

pico_add_extra_outputs(bramble)

