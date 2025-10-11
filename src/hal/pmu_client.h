#ifndef PMU_CLIENT_H
#define PMU_CLIENT_H

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
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

private:
    uart_inst_t* uart_;
    uint txPin_;
    uint rxPin_;
    uint baudrate_;
    bool initialized_;
    PMU::Protocol protocol_;

    // UART send function (captured by lambda in protocol)
    void uartSend(const uint8_t* data, uint8_t length);

    // UART RX interrupt handler
    static void onUartRxIrq();

    // Static instance for IRQ callback
    static PmuClient* instance_;
};

#endif // PMU_CLIENT_H
