#include "pmu_client.h"

#include "uart_rx.pio.h"

// Static instance for IRQ callback
PmuClient *PmuClient::instance_ = nullptr;

PmuClient::PmuClient(uart_inst_t *uart_inst, uint tx_pin, uint rx_pin, uint baudrate)
    : uart_(uart_inst), txPin_(tx_pin), rxPin_(rx_pin), baudrate_(baudrate), initialized_(false),
      protocol_([this](const uint8_t *data, uint8_t length) { this->uartSend(data, length); }),
      pioDev_(nullptr), pioSm_(0), pioOffset_(0), rxHead_(0), rxTail_(0)
{
    instance_ = this;
}

bool PmuClient::init()
{
    if (initialized_) {
        return true;
    }

    // Hardware UART for TX only — RX uses PIO because the v4 bodge wire lands on
    // GPIO21 which is not a valid UART1 RX pin (GPIO%4 mux rule). See bramble_v4_pins.h.
    uint actual_baudrate = uart_init(uart_, baudrate_);
    if (actual_baudrate == 0) {
        return false;
    }
    gpio_set_function(txPin_, GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart_, true);
    uart_set_format(uart_, 8, 2, UART_PARITY_NONE);

    // PIO RX on rxPin_. Use pio1 since pio0 SM0 is taken by the NeoPixel driver.
    pioDev_ = pio1;
    pioSm_ = pio_claim_unused_sm(pioDev_, true);
    pioOffset_ = pio_add_program(pioDev_, &uart_rx_program);
    uart_rx_program_init(pioDev_, pioSm_, pioOffset_, rxPin_, baudrate_);

    // Route PIO0 of pioDev_ to NVIC and enable RX-FIFO-not-empty for our SM.
    int pio_irq = (pioDev_ == pio0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
    irq_set_exclusive_handler(pio_irq, onUartRxIrq);
    irq_set_enabled(pio_irq, true);
    pio_set_irq0_source_enabled(
        pioDev_, (pio_interrupt_source_t)(pis_sm0_rx_fifo_not_empty + pioSm_), true);

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
    while (rxTail_ != rxHead_) {
        uint8_t byte = rxBuffer_[rxTail_];
        rxTail_ = (rxTail_ + 1) % RX_BUFFER_SIZE;
        protocol_.processReceivedByte(byte);
    }
}

void PmuClient::onUartRxIrq()
{
    if (!instance_ || !instance_->pioDev_) {
        return;
    }

    // The uart_rx program autopushes 8 bits with shift-right, so the received
    // byte sits in bits 31:24 of the 32-bit FIFO word.
    while (!pio_sm_is_rx_fifo_empty(instance_->pioDev_, instance_->pioSm_)) {
        uint32_t word = pio_sm_get(instance_->pioDev_, instance_->pioSm_);
        uint8_t byte = (uint8_t)(word >> 24);

        size_t next_head = (instance_->rxHead_ + 1) % RX_BUFFER_SIZE;
        if (next_head != instance_->rxTail_) {
            instance_->rxBuffer_[instance_->rxHead_] = byte;
            instance_->rxHead_ = next_head;
        }
    }
}
