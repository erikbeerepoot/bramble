#ifndef PMU_CLIENT_H
#define PMU_CLIENT_H

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/structs/uart.h"
#include "pmu_protocol.h"

/**
 * @brief PMU Client - RP2040 interface to STM32 Power Management Unit
 *
 * Handles UART communication with the STM32 PMU using interrupt-driven receive
 * and blocking transmit. Uses UART0 at 9600 baud by default.
 */
class PmuClient {
public:
    /**
     * @brief Construct a new PMU Client
     *
     * @param uart_inst UART instance (uart0 or uart1)
     * @param tx_pin GPIO pin for TX
     * @param rx_pin GPIO pin for RX
     * @param baudrate Baud rate (default 9600 to match STM32)
     */
    PmuClient(uart_inst_t* uart_inst, uint tx_pin, uint rx_pin, uint baudrate = 9600);

    /**
     * @brief Initialize the UART hardware and protocol
     *
     * @return true if initialization successful
     * @return false if initialization failed
     */
    bool init();

    /**
     * @brief Process any pending received bytes (call from main loop)
     */
    void process();

    /**
     * @brief Get the PMU protocol instance
     *
     * @return PMU::Protocol& Reference to the protocol handler
     */
    PMU::Protocol& getProtocol() { return protocol_; }

    /**
     * @brief Check if UART is initialized
     *
     * @return true if initialized
     * @return false if not initialized
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief Send wake preamble to wake STM32 from STOP mode
     *
     * Sends dummy bytes to trigger LPUART wakeup, then waits for
     * the STM32 to reconfigure clocks and re-enable UART interrupt.
     * Call this before sending commands when STM32 may be sleeping.
     */
    void sendWakePreamble();

private:
    uart_inst_t* uart_;
    uint txPin_;
    uint rxPin_;
    uint baudrate_;
    bool initialized_;
    PMU::Protocol protocol_;

    // Ring buffer for IRQ -> main loop communication
    static constexpr size_t RX_BUFFER_SIZE = 128;
    volatile uint8_t rxBuffer_[RX_BUFFER_SIZE];
    volatile size_t rxHead_;
    volatile size_t rxTail_;

    // Error flags from last receive (for debugging)
    volatile uint8_t lastErrorFlags_;

    // UART send function (captured by lambda in protocol)
    void uartSend(const uint8_t* data, uint8_t length);

    // UART RX interrupt handler
    static void onUartRxIrq();

    // Static instance for IRQ callback
    static PmuClient* instance_;
};

#endif // PMU_CLIENT_H
