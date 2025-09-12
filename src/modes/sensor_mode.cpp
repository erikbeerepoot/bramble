#include "sensor_mode.h"
#include <cstdio>

void SensorMode::onStart() {
    printf("=== SENSOR MODE ACTIVE (STUB) ===\n");
    printf("- Sensor-only functionality not yet implemented\n");
    printf("- Using base ApplicationMode behavior\n");
    
    // For now, just use the base class behavior
    ApplicationMode::onStart();
}