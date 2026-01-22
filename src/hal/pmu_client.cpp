#include "pmu_client.h"

#include "hal/logger.h"

static Logger pmu_client_log("PMU_RX");

// Static instance for IRQ callback
PmuClient *PmuClient::instance_ = nullptr;

PmuClient::PmuClient(uart_inst_t *uart_inst, uint tx_pin, uint rx_pin, uint baudrate)
    : uart_(uart_inst), txPin_(tx_pin), rxPin_(rx_pin), baudrate_(baudrate), initialized_(false),
      protocol_([this](const uint8_t *data, uint8_t length) { this->uartSend(data, length); }),
      rxHead_(0), rxTail_(0), lastErrorFlags_(0)
{
    // Set static instance for IRQ handling
    instance_ = this;
}

bool PmuClient::init()
{
    if (initialized_) {
        return true;
    }

    // Initialize UART
    uint actual_baudrate = uart_init(uart_, baudrate_);

    if (actual_baudrate == 0) {
        return false;
    }

    // Set TX and RX pins
    gpio_set_function(txPin_, GPIO_FUNC_UART);
    gpio_set_function(rxPin_, GPIO_FUNC_UART);

    // Enable pull-up on RX pin for better signal integrity
    // UART idle state is high, pull-up helps prevent noise
    gpio_pull_up(rxPin_);

    // Enable FIFO first
    uart_set_fifo_enabled(uart_, true);

    // Set UART format: 8 data bits, 2 stop bits, no parity
    // Must match STM32 LPUART configuration (set after FIFO to ensure it sticks)
    uart_set_format(uart_, 8, 2, UART_PARITY_NONE);

    // Get the IRQ number for this UART
    int uart_irq = uart_ == uart0 ? UART0_IRQ : UART1_IRQ;

    // Set up RX interrupt
    irq_set_exclusive_handler(uart_irq, onUartRxIrq);
    irq_set_enabled(uart_irq, true);

    // Enable UART RX interrupt
    uart_set_irq_enables(uart_, true, false);  // RX enabled, TX disabled

    initialized_ = true;
    return true;
}

void PmuClient::uartSend(const uint8_t *data, uint8_t length)
{
    if (!initialized_ || !data || length == 0) {
        return;
    }

    // Blocking transmit
    uart_write_blocking(uart_, data, length);
}

void PmuClient::sendWakePreamble()
{
    if (!initialized_) {
        return;
    }

    // Send dummy bytes to wake STM32 from STOP mode
    // These will be ignored by the protocol parser (not 0xAA start byte)
    // but will trigger LPUART wakeup interrupt
    //
    // The STM32 wakes on first byte, runs wakeupFromStopMode() to reconfigure
    // clocks, does a quick LED pulse, then may go back to STOP mode.
    // We need to keep it awake long enough to receive our actual command.
    //
    // Send multiple wake pulses with delays to ensure STM32 stays awake
    // through its main loop cycle and is ready when we send the real command.
    for (int i = 0; i < 3; i++) {
        const uint8_t wake_byte = 0x00;
        uart_write_blocking(uart_, &wake_byte, 1);
        sleep_ms(50);  // Small delay between bytes
    }

    // Final delay to ensure STM32 has completed its wake cycle
    sleep_ms(150);
}

void PmuClient::process()
{
    // Check for UART errors
    if (lastErrorFlags_ != 0) {
        pmu_client_log.warn("UART errors: FE=%d PE=%d BE=%d OE=%d", (lastErrorFlags_ & 1) ? 1 : 0,
                            (lastErrorFlags_ & 2) ? 1 : 0, (lastErrorFlags_ & 4) ? 1 : 0,
                            (lastErrorFlags_ & 8) ? 1 : 0);
        lastErrorFlags_ = 0;
    }

    // Process all bytes from the ring buffer
    while (rxTail_ != rxHead_) {
        uint8_t byte = rxBuffer_[rxTail_];
        rxTail_ = (rxTail_ + 1) % RX_BUFFER_SIZE;
        protocol_.processReceivedByte(byte);
    }
}

void PmuClient::onUartRxIrq()
{
    if (!instance_ || !instance_->uart_) {
        return;
    }

    // Read all available bytes from FIFO and store in ring buffer
    while (uart_is_readable(instance_->uart_)) {
        // Check for framing/parity errors before reading
        uint32_t dr = uart_get_hw(instance_->uart_)->dr;
        uint8_t byte = dr & 0xFF;
        bool frame_error = (dr & UART_UARTDR_FE_BITS) != 0;
        bool parity_error = (dr & UART_UARTDR_PE_BITS) != 0;
        bool break_error = (dr & UART_UARTDR_BE_BITS) != 0;
        bool overrun_error = (dr & UART_UARTDR_OE_BITS) != 0;

        // Log any errors (only in IRQ context, keep it brief)
        if (frame_error || parity_error || break_error || overrun_error) {
            // Set a flag to log later (can't log from IRQ)
            instance_->lastErrorFlags_ = (frame_error ? 1 : 0) | (parity_error ? 2 : 0) |
                                         (break_error ? 4 : 0) | (overrun_error ? 8 : 0);
        }

        size_t next_head = (instance_->rxHead_ + 1) % RX_BUFFER_SIZE;
        if (next_head != instance_->rxTail_) {
            instance_->rxBuffer_[instance_->rxHead_] = byte;
            instance_->rxHead_ = next_head;
        }
        // If buffer full, drop the byte
    }
}
