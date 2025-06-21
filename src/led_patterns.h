#pragma once
#include <cstdint>
#include "hal/neopixel.h"

/**
 * @brief Base class for LED animation patterns
 */
class LEDPattern {
protected:
    NeoPixel& led_;
    
public:
    explicit LEDPattern(NeoPixel& led) : led_(led) {}
    virtual ~LEDPattern() = default;
    
    /**
     * @brief Update the LED based on current time
     * @param current_time Current system time in milliseconds
     */
    virtual void update(uint32_t current_time) = 0;
};

/**
 * @brief Breathing pattern - smooth fade in/out
 */
class BreathingPattern : public LEDPattern {
private:
    uint8_t r_, g_, b_;
    uint32_t period_ms_;
    uint32_t last_update_ = 0;
    uint8_t phase_ = 0;
    
public:
    BreathingPattern(NeoPixel& led, uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms = 2000)
        : LEDPattern(led), r_(r), g_(g), b_(b), period_ms_(period_ms) {}
    
    void update(uint32_t current_time) override {
        // Update approximately every 15ms for smooth animation
        if (current_time - last_update_ < 15) return;
        last_update_ = current_time;
        
        // Calculate brightness using sine approximation
        uint8_t brightness;
        if (phase_ < 64) {
            brightness = phase_ * 2;
        } else {
            brightness = (128 - phase_) * 2;
        }
        
        // Apply brightness to color
        uint8_t r = (r_ * brightness) / 255;
        uint8_t g = (g_ * brightness) / 255;
        uint8_t b = (b_ * brightness) / 255;
        
        led_.setPixel(0, r, g, b);
        led_.show();
        
        // Advance phase
        phase_ = (phase_ + 1) % 128;
    }
};

/**
 * @brief Heartbeat pattern - quick double pulse
 */
class HeartbeatPattern : public LEDPattern {
private:
    uint8_t r_, g_, b_;
    uint32_t last_beat_ = 0;
    uint8_t beat_phase_ = 0;
    
    static constexpr uint32_t BEAT_INTERVAL = 1000;  // 1 second between beats
    static constexpr uint8_t BEAT_PATTERN[] = {
        0, 10, 20, 30, 40, 50, 40, 30, 20, 10,  // First pulse
        0, 0, 0, 0, 0,                           // Short pause
        0, 10, 20, 30, 40, 50, 40, 30, 20, 10,  // Second pulse
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0            // Long pause
    };
    static constexpr size_t PATTERN_LENGTH = sizeof(BEAT_PATTERN);
    
public:
    HeartbeatPattern(NeoPixel& led, uint8_t r, uint8_t g, uint8_t b)
        : LEDPattern(led), r_(r), g_(g), b_(b) {}
    
    void update(uint32_t current_time) override {
        // Update pattern every 25ms
        if (current_time - last_beat_ < 25) return;
        last_beat_ = current_time;
        
        uint8_t brightness = BEAT_PATTERN[beat_phase_];
        
        // Apply brightness to color
        uint8_t r = (r_ * brightness) / 50;  // Max brightness is 50
        uint8_t g = (g_ * brightness) / 50;
        uint8_t b = (b_ * brightness) / 50;
        
        led_.setPixel(0, r, g, b);
        led_.show();
        
        // Advance phase
        beat_phase_ = (beat_phase_ + 1) % PATTERN_LENGTH;
    }
};

/**
 * @brief Static color pattern - no animation
 */
class StaticPattern : public LEDPattern {
private:
    uint8_t r_, g_, b_;
    bool initialized_ = false;
    
public:
    StaticPattern(NeoPixel& led, uint8_t r, uint8_t g, uint8_t b)
        : LEDPattern(led), r_(r), g_(g), b_(b) {}
    
    void update(uint32_t current_time) override {
        // Only update once
        if (!initialized_) {
            led_.setPixel(0, r_, g_, b_);
            led_.show();
            initialized_ = true;
        }
    }
};