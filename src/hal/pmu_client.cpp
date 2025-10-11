#include "pmu_client.h"

// Static instance for IRQ callback
PmuClient* PmuClient::instance_ = nullptr;

PmuClient::PmuClient(uart_inst_t* uart_inst, uint tx_pin, uint rx_pin, uint baudrate)
    : uart_(uart_inst),
      txPin_(tx_pin),
      rxPin_(rx_pin),
      baudrate_(baudrate),
      initialized_(false),
      protocol_([this](const uint8_t* data, uint8_t length) {
          this->uartSend(data, length);
      }) {

    // Set static instance for IRQ handling
    instance_ = this;
}

bool PmuClient::init() {
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

    // Set UART format: 8 data bits, 1 stop bit, no parity
    uart_set_format(uart_, 8, 1, UART_PARITY_NONE);

    // Enable FIFO
    uart_set_fifo_enabled(uart_, true);

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

void PmuClient::uartSend(const uint8_t* data, uint8_t length) {
    if (!initialized_ || !data || length == 0) {
        return;
    }

    // Blocking transmit
    uart_write_blocking(uart_, data, length);
}

void PmuClient::onUartRxIrq() {
    if (!instance_ || !instance_->uart_) {
        return;
    }

    // Read all available bytes from FIFO
    while (uart_is_readable(instance_->uart_)) {
        uint8_t byte = uart_getc(instance_->uart_);
        instance_->protocol_.processReceivedByte(byte);
    }
}
