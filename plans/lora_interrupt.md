# Interrupt-Driven LoRa Operation Plan

## Current Implementation (Polling)

The current implementation uses a polling approach where the main loop constantly checks for incoming messages:

```cpp
while (true) {
    // ... other tasks ...
    
    // Check for incoming messages (polling)
    uint8_t rx_buffer[MESSAGE_MAX_SIZE];
    int rx_len = lora.receive(rx_buffer, sizeof(rx_buffer));
    
    if (rx_len > 0) {
        processIncomingMessage(rx_buffer, rx_len, ...);
    }
    
    sleep_ms(100);  // Sleep between polls
}
```

### Problems with Polling:
1. **Power Consumption**: CPU wakes up every 100ms even if no messages
2. **Latency**: Up to 100ms delay before message is processed
3. **CPU Waste**: Constantly checking even when idle
4. **Missed Messages**: If multiple messages arrive quickly

## Current Hardware Configuration

The Bramble project uses the Adafruit Feather RP2040 with RFM95 LoRa module, which contains an SX1276 chip. The current pin connections are:

```cpp
#define PIN_CS   16  // SPI chip select
#define PIN_RST  17  // Reset pin
#define PIN_DIO0 21  // DIO0 interrupt pin (already connected!)
#define PIN_DIO1 22  // DIO1 (available but not currently used)
#define PIN_DIO2 23  // DIO2 (available but not currently used)
#define PIN_DIO3 19  // DIO3 (available but not currently used)
#define PIN_DIO4 20  // DIO4 (available but not currently used)
#define PIN_DIO5 18  // DIO5 (available but not currently used)
```

**Good news**: 
- DIO0 is already connected to GPIO 21, so we have the hardware support needed for interrupt-driven operation
- All other DIO pins are also available if we need more sophisticated interrupt handling later
- The current code already initializes DIO0 as an input with pull-down

## Proposed Interrupt-Driven Implementation

### 1. Hardware Setup
The SX1276 has 6 DIO (Digital I/O) pins that can signal various interrupt events. The specific function of each DIO pin is configurable through the DIO mapping registers (RegDioMapping1 0x40 and RegDioMapping2 0x41).

**Why DIO0?**
- **Most Common Choice**: DIO0 is the most commonly used interrupt pin in LoRa implementations
- **Dual Purpose**: Can be configured to signal both RxDone and TxDone based on operating mode
- **Simplified Hardware**: Using only DIO0 reduces the number of GPIO pins needed
- **Proven Approach**: Many implementations (Arduino-LoRa, LMIC) successfully use just DIO0

**DIO0 Mapping Options in LoRa Mode:**
- `00`: RxDone (in RX mode) / TxDone (in TX mode)
- `01`: CadDone
- `10`: FhssChangeChannel
- `11`: CadDetected

We'll configure DIO0 with mapping `00` to get RxDone/TxDone interrupts.

### 2. Architecture Changes

#### A. Add Interrupt Handler
```cpp
// Global interrupt flags for ISR to main communication
static volatile bool dio0_interrupt_fired = false;

// Interrupt Service Routine (minimal processing)
void sx1276_dio0_isr(uint gpio, uint32_t events) {
    // In ISR context - keep it minimal!
    if (gpio == dio0_pin && (events & GPIO_IRQ_EDGE_RISE)) {
        dio0_interrupt_fired = true;
    }
}
```

**Key Design Decisions:**
1. **No SPI in ISR**: SPI operations are too slow for ISR context
2. **Simple Flag**: Just set a volatile flag, process in main loop
3. **No Buffer Copying**: Defer all data handling to main context
4. **Edge Detection**: Use rising edge to detect DIO0 going high

#### B. Modified SX1276 Class
```cpp
class SX1276 {
public:
    // New methods for interrupt mode
    bool beginInterrupt(gpio_irq_callback_t callback);
    void enableRxInterrupt();
    void disableRxInterrupt();
    bool hasInterruptFired() const;
    void clearInterrupt();
    
    // Modified receive for interrupt mode
    int receiveFromISR(uint8_t* buffer, size_t max_length);
    
private:
    bool interrupt_mode_;
    volatile bool interrupt_fired_;
};
```

#### C. Main Loop Changes
```cpp
// Setup
gpio_set_irq_enabled_with_callback(dio0_pin, GPIO_IRQ_EDGE_RISE, true, &sx1276_dio0_isr);
lora.enableRxInterrupt();
lora.startReceive();  // Put in continuous RX mode

while (true) {
    // Check if interrupt fired
    if (rx_message_ready) {
        rx_message_ready = false;
        
        // Read the message outside ISR context
        uint8_t rx_buffer[MESSAGE_MAX_SIZE];
        int rx_len = lora.receiveFromISR(rx_buffer, sizeof(rx_buffer));
        
        if (rx_len > 0) {
            processIncomingMessage(rx_buffer, rx_len, ...);
        }
        
        // Re-enable for next message
        lora.startReceive();
    }
    
    // Do other tasks...
    
    // Can now sleep longer or use __wfi() (wait for interrupt)
    __wfi();  // Sleep until any interrupt occurs
}
```

### 3. Implementation Steps

1. **Update SX1276 driver**:
   - Add interrupt configuration methods
   - Add DIO0 mapping configuration
   - Add interrupt-safe receive method

2. **Add interrupt handler**:
   - Minimal ISR that just sets flags
   - Defer actual SPI operations to main loop

3. **Update main loops**:
   - Replace polling with interrupt checking
   - Use proper sleep modes between interrupts

4. **Add message queue** (optional for advanced):
   - Ring buffer for multiple messages
   - Producer (ISR) / Consumer (main) pattern

### 4. Benefits

1. **Power Efficiency**:
   - CPU can sleep until message arrives
   - No periodic wake-ups for polling
   - Ideal for battery-powered nodes

2. **Lower Latency**:
   - Immediate response to incoming messages
   - No polling delay

3. **Better Reliability**:
   - Won't miss messages during processing
   - Can queue multiple messages

4. **CPU Availability**:
   - More time for other tasks
   - Better overall system responsiveness

### 5. Example Implementation

Here's what the key changes would look like:

#### sx1276.h additions:
```cpp
class SX1276 {
public:
    // Interrupt support
    bool enableInterruptMode(uint dio0_pin);
    void handleInterrupt();
    bool isMessageReady() const { return message_ready_; }
    
private:
    volatile bool message_ready_;
    uint dio0_pin_;
};
```

#### sx1276.cpp additions:
```cpp
bool SX1276::enableInterruptMode(uint dio0_pin) {
    dio0_pin_ = dio0_pin;
    
    // Configure DIO0 mapping for RxDone/TxDone (mapping = 00)
    uint8_t dio_mapping1 = readRegister(SX1276_REG_DIO_MAPPING_1);
    dio_mapping1 &= 0x3F;  // Clear DIO0 mapping bits [7:6]
    dio_mapping1 |= 0x00;  // Set mapping to 00 (RxDone/TxDone)
    writeRegister(SX1276_REG_DIO_MAPPING_1, dio_mapping1);
    
    // Note: We don't mask interrupts in the SX1276 - we want DIO0 to fire
    // The interrupt flags register (RegIrqFlags) will tell us which event occurred
    
    return true;
}

void SX1276::handleInterrupt() {
    // Read and clear interrupt flags
    uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
    
    // Clear all flags by writing them back
    writeRegister(SX1276_REG_IRQ_FLAGS, irq_flags);
    
    // Determine what happened
    if (irq_flags & SX1276_IRQ_RX_DONE_MASK) {
        rx_done_ = true;
        // Could also check for CRC error here (bit 5)
        if (irq_flags & 0x20) {
            rx_crc_error_ = true;
        }
    }
    
    if (irq_flags & SX1276_IRQ_TX_DONE_MASK) {
        tx_done_ = true;
    }
}
```

### 6. Testing Strategy

1. **Functionality Test**:
   - Verify interrupts fire on message reception
   - Check no messages are lost
   - Verify proper cleanup after each message

2. **Power Measurement**:
   - Compare power consumption: polling vs interrupt
   - Measure wake-up latency

3. **Stress Test**:
   - Rapid message reception
   - Verify queue doesn't overflow
   - Check ISR execution time

4. **Edge Cases**:
   - Interrupt during message processing
   - Multiple messages arriving simultaneously
   - Error handling in interrupt context

### 7. Transmission Handling

The interrupt-driven approach needs to handle both RX and TX operations:

#### A. Current Transmission (Polling)
```cpp
// Current implementation
bool SX1276::send(const uint8_t* data, size_t length) {
    // ... write to FIFO ...
    setMode(SX1276_MODE_TX);  // Start transmission
    return true;
}

// Then poll for completion
while (!lora.isTxDone()) {
    sleep_ms(1);
}
```

#### B. Interrupt-Driven Transmission
```cpp
// New approach with interrupts
class SX1276 {
private:
    volatile bool tx_complete_;
    volatile bool tx_in_progress_;
    
public:
    bool sendAsync(const uint8_t* data, size_t length) {
        if (tx_in_progress_) return false;  // Already transmitting
        
        tx_in_progress_ = true;
        tx_complete_ = false;
        
        // Prepare for TX
        setMode(SX1276_MODE_STDBY);
        writePayload(data, length);
        
        // DIO0 will fire TxDone when transmission completes
        setMode(SX1276_MODE_TX);
        return true;
    }
    
    bool isTxComplete() const { return tx_complete_; }
    bool isTxInProgress() const { return tx_in_progress_; }
    
    void handleInterrupt() {
        uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
        
        if (irq_flags & SX1276_IRQ_TX_DONE_MASK) {
            tx_complete_ = true;
            tx_in_progress_ = false;
            // Clear TX done flag
            writeRegister(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_TX_DONE_MASK);
            
            // Automatically go back to RX mode
            startReceive();
        }
        
        if (irq_flags & SX1276_IRQ_RX_DONE_MASK) {
            rx_message_ready_ = true;
            // Clear RX done flag
            writeRegister(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_RX_DONE_MASK);
        }
    }
};
```

#### C. State Machine Approach
For robust operation, implement a state machine:

```cpp
enum RadioState {
    RADIO_IDLE,
    RADIO_RX,
    RADIO_TX,
    RADIO_CAD  // Channel Activity Detection
};

class SX1276 {
private:
    RadioState state_;
    
public:
    void handleInterrupt() {
        uint8_t irq_flags = readRegister(SX1276_REG_IRQ_FLAGS);
        writeRegister(SX1276_REG_IRQ_FLAGS, irq_flags);  // Clear flags
        
        switch (state_) {
            case RADIO_TX:
                if (irq_flags & SX1276_IRQ_TX_DONE_MASK) {
                    // TX complete, switch back to RX
                    state_ = RADIO_RX;
                    startReceive();
                    if (tx_done_callback_) tx_done_callback_();
                }
                break;
                
            case RADIO_RX:
                if (irq_flags & SX1276_IRQ_RX_DONE_MASK) {
                    // RX complete, process message
                    if (rx_done_callback_) rx_done_callback_();
                    // Stay in RX mode for next message
                }
                break;
        }
    }
};
```

#### D. Usage in ReliableMessenger
```cpp
bool ReliableMessenger::sendMessage(...) {
    // Build message...
    
    if (!lora_.sendAsync(tx_buffer, total_length)) {
        return false;  // Radio busy
    }
    
    // For critical messages, track pending ACK
    if (criticality >= CRITICAL_RELIABLE) {
        pending_ack_ = true;
        ack_timeout_ = current_time + ACK_TIMEOUT_MS;
    }
    
    return true;
}

// In main loop
void processRadioEvents() {
    if (lora.isTxComplete()) {
        // TX done, already back in RX mode
        logger.debug("TX complete");
    }
    
    if (lora.isMessageReady()) {
        // Process received message
        uint8_t buffer[256];
        int len = lora.receive(buffer, sizeof(buffer));
        // ... process ...
    }
}
```

#### E. Key Design Decisions for TX

1. **Automatic RX Return**: After TxDone, automatically return to RX mode
2. **Non-blocking Send**: `sendAsync()` returns immediately, check completion later
3. **Single DIO0**: Same pin handles both TxDone and RxDone based on mode
4. **State Tracking**: Prevent overlapping TX operations
5. **Callback Option**: Optional callbacks for TX completion notification

### 8. Considerations

1. **ISR Safety**:
   - No blocking operations in ISR
   - No dynamic memory allocation
   - Minimal processing time

2. **Shared State**:
   - Use volatile for ISR-shared variables
   - Consider memory barriers if needed
   - Atomic operations for flags

3. **Error Handling**:
   - Can't use logger in ISR context
   - Need safe error reporting mechanism

4. **Compatibility**:
   - Keep polling mode as fallback option
   - Allow runtime selection of mode