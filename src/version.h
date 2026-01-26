#pragma once

#include <stdint.h>

// Bramble firmware version
// Major and minor are read from the VERSION file at the project root.
// Build number auto-increments on each cmake configure (stored in build dir).
// CMake passes all three as compile definitions.
//
// Encoding: (MAJOR << 24) | (MINOR << 16) | BUILD
// Major: 8 bits (0-255), Minor: 8 bits (0-255), Build: 16 bits (0-65535)

// Fallback defaults for IDE intellisense or non-CMake builds
#ifndef BRAMBLE_VERSION_MAJOR
#define BRAMBLE_VERSION_MAJOR 0
#endif

#ifndef BRAMBLE_VERSION_MINOR
#define BRAMBLE_VERSION_MINOR 0
#endif

#ifndef BRAMBLE_VERSION_BUILD
#define BRAMBLE_VERSION_BUILD 0
#endif

#define BRAMBLE_FIRMWARE_VERSION                                                                    \
    ((uint32_t)((BRAMBLE_VERSION_MAJOR << 24) | (BRAMBLE_VERSION_MINOR << 16) |                    \
                BRAMBLE_VERSION_BUILD))

// Extract version components from packed uint32_t
#define BRAMBLE_VERSION_GET_MAJOR(v) (((v) >> 24) & 0xFF)
#define BRAMBLE_VERSION_GET_MINOR(v) (((v) >> 16) & 0xFF)
#define BRAMBLE_VERSION_GET_BUILD(v) ((v) & 0xFFFF)
