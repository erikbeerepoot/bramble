#include "irrigation_mode.h"

#include <cstring>

#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "hardware/watchdog.h"

#include "../hal/logger.h"
#include "../hal/rtc_compat.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../util/time.h"
#include "../version.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;  // 60 seconds

// PMU UART configuration - selected by board version via board_pins.h
#include "../board/board_pins.h"

// Board version identifier stored in persisted state
#ifdef BOARD_V4
constexpr uint8_t IRRIGATION_BOARD_VERSION = 4;
#else
constexpr uint8_t IRRIGATION_BOARD_VERSION = 3;
#endif

// Timeout for wake notification after ClearToSend
constexpr uint32_t WAKE_NOTIFICATION_TIMEOUT_MS = 1000;

static Logger logger("IRRIG");
static Logger pmu_logger("PMU");

IrrigationMode::~IrrigationMode()
{
    delete reliable_pmu_;
    delete pmu_client_;
}

void IrrigationMode::onStart()
{
    logger.info("=== IRRIGATION MODE ACTIVE ===");
    logger.info("- 2 valve irrigation node");
    logger.info("- PMU power management integration");
    logger.info("- Orange LED blink (init) -> Blue short blink (operational)");

    // Cache device ID for heartbeat identification (big-endian, matching main.cpp registration)
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    device_id_ = 0;
    for (int i = 0; i < 8; i++) {
        device_id_ = (device_id_ << 8) | board_id.id[i];
    }

    // Check if we need to register (no saved address — may be overridden by PMU state restore)
    needs_registration_ = (messenger_.getNodeAddress() == ADDRESS_UNREGISTERED);
    if (needs_registration_) {
        logger.info("No saved address - will register after PMU wake");
    }

    // Initialize valve controller hardware (valve states remain UNKNOWN until restored or actuated)
    valve_controller_.initialize();

    // Set up state machine callback - drives all side effects
    irrigation_state_.setCallback([this](IrrigationState state) { this->onStateChange(state); });

    // Initialize PMU client at 9600 baud to match STM32 LPUART configuration
    pmu_client_ = new PmuClient(Board::PMU_UART_PORT, Board::PMU_UART_TX_PIN,
                                Board::PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        // Create reliable PMU client wrapper for automatic retry
        reliable_pmu_ = new PMU::ReliablePmuClient(pmu_client_);
        reliable_pmu_->init();

        // Allow UART to stabilize before first message
        sleep_ms(150);

        pmu_logger.info("PMU client initialized successfully");

        // Set up PMU callback handlers
        reliable_pmu_->onWake(
            [this](PMU::WakeReason reason, const PMU::ScheduleEntry *entry, bool state_valid,
                   const uint8_t *state) {
                this->handlePmuWake(reason, entry, state_valid, state);
            });

        reliable_pmu_->onScheduleComplete([this]() { this->handleScheduleComplete(); });

        // Send ClearToSend — PMU will respond with WakeNotification containing state blob
        pmu_logger.debug("Sending ClearToSend to PMU...");
        reliable_pmu_->clearToSend([this](bool success, PMU::ErrorCode error) {
            if (!success) {
                pmu_logger.error("ClearToSend failed: %d", static_cast<int>(error));
                // Proceed without PMU state — close all valves to establish known state
                valve_controller_.closeAllValves();
                irrigation_state_.markInitialized();
            } else {
                pmu_logger.info("ClearToSend ACK received - waiting for WakeNotification");
                // Start timeout — if WakeNotification doesn't arrive, proceed without state
                uint32_t now = to_ms_since_boot(get_absolute_time());
                wake_timeout_id_ = task_queue_.postDelayed(
                    [this](uint32_t) -> bool {
                        pmu_logger.warn(
                            "WakeNotification timeout - proceeding without PMU state");
                        valve_controller_.closeAllValves();
                        wake_timeout_id_ = 0;
                        irrigation_state_.markInitialized();
                        return true;
                    },
                    now, WAKE_NOTIFICATION_TIMEOUT_MS, TaskPriority::High);
            }
        });
    } else {
        logger.warn("PMU client not available - running without power management");
        // No PMU — close all valves to establish known state
        valve_controller_.closeAllValves();
        irrigation_state_.markInitialized();
    }

    // Start with orange blinking pattern while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 0, 255);

    // Add heartbeat task
    task_manager_.addTask(
        [this](uint32_t time) {
            logger.debug("Irrigation heartbeat");

            uint32_t uptime = time / 1000;
            uint8_t battery_level = 85;
            uint8_t signal_strength = 65;
            uint8_t error_flags = 0;
            uint8_t active_sensors = CAP_VALVE_CONTROL;

            uint8_t valve_mask = valve_controller_.getActiveValveMask();
            if (valve_mask != 0) {
                logger.info("Active valves: 0x%02X", valve_mask);
            }

            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, battery_level, signal_strength,
                                     active_sensors, error_flags, 0, device_id_);
        },
        HEARTBEAT_INTERVAL_MS, "Heartbeat");
}

void IrrigationMode::onStateChange(IrrigationState state)
{
    switch (state) {
        case IrrigationState::REGISTERING:
            if (needs_registration_) {
                attemptDeferredRegistration();
            } else {
                // Already registered, skip to time sync
                irrigation_state_.reportRegistrationComplete();
            }
            break;

        case IrrigationState::AWAITING_TIME:
            // Send heartbeat — hub response carries time and pending flags
            irrigation_state_.reportHeartbeatSending();
            break;

        case IrrigationState::SENDING_HEARTBEAT: {
            uint32_t uptime = to_ms_since_boot(get_absolute_time()) / 1000;
            messenger_.sendHeartbeat(HUB_ADDRESS, uptime, 85, 65, CAP_VALVE_CONTROL, 0, 0,
                                     device_id_);
            break;
        }

        case IrrigationState::AWAITING_REGISTRATION:
            // Base class callback already fired the registration request.
            // Wait for registration response via messenger callback.
            messenger_.setRegistrationSuccessCallback(
                [this](uint16_t new_address) {
                    logger.info("Re-registration assigned address 0x%04X", new_address);
                    messenger_.setRegistrationSuccessCallback(nullptr);
                    irrigation_state_.reportReregistrationComplete();
                });
            break;

        case IrrigationState::CHECKING_UPDATES:
            sendCheckUpdates();
            break;

        case IrrigationState::APPLYING_UPDATE:
            // Waiting for PMU callback - no action needed
            break;

        case IrrigationState::VALVE_ACTIVE:
            // Keep PMU awake while valve is open — periodic keepAlive every 5s
            if (pmu_available_ && reliable_pmu_) {
                reliable_pmu_->keepAwake(10);
                scheduleKeepAwake();
            }
            break;

        case IrrigationState::READY_FOR_SLEEP:
            switchToOperationalPattern();
            signalReadyForSleep();
            break;

        case IrrigationState::ERROR:
            led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 0, 0, 250, 250);
            break;

        case IrrigationState::INITIALIZING:
            // Should not re-enter INITIALIZING
            break;
    }
}

void IrrigationMode::scheduleKeepAwake()
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    keepawake_task_id_ = task_queue_.postDelayed(
        [this](uint32_t) -> bool {
            if (valve_controller_.getActiveValveMask() != 0 && reliable_pmu_) {
                reliable_pmu_->keepAwake(10);
                scheduleKeepAwake();
            }
            return true;
        },
        now, 5000, TaskPriority::High);
}

void IrrigationMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry,
                                   bool state_valid, const uint8_t *state)
{
    // Cancel the wake notification timeout — PMU responded
    if (wake_timeout_id_ != 0) {
        task_queue_.cancel(wake_timeout_id_);
        wake_timeout_id_ = 0;
    }

    // Restore state from PMU RAM if valid
    bool restored = false;
    if (state_valid && state != nullptr) {
        const IrrigationPersistedState *persisted =
            reinterpret_cast<const IrrigationPersistedState *>(state);
        restored = unpackState(persisted);
    }

    if (restored) {
        pmu_logger.info("State restored from PMU");
        // Re-evaluate registration need based on restored address
        needs_registration_ = (messenger_.getNodeAddress() == ADDRESS_UNREGISTERED);
    } else {
        pmu_logger.info("Cold start - no persisted state");
        valve_controller_.closeAllValves();
    }

    // For scheduled wakes, open the valve before reporting to state machine
    if (reason == PMU::WakeReason::Scheduled && entry) {
        logger.info("Scheduled wake - valve %d for %d seconds", entry->valveId, entry->duration);
        if (entry->valveId < ValveController::NUM_VALVES) {
            valve_controller_.openValve(entry->valveId);
        } else {
            logger.error("Invalid valve ID %d in schedule", entry->valveId);
        }
    }

    // First boot: kick off the state machine
    if (irrigation_state_.state() == IrrigationState::INITIALIZING) {
        irrigation_state_.markInitialized();
        return;
    }

    // Re-wake from sleep: let state machine handle the transition
    irrigation_state_.reportWakeFromSleep(reason);

    // For external wake with pending registration
    if (reason == PMU::WakeReason::External && needs_registration_) {
        logger.info("External wake + no address - registration will happen via state machine");
    }
}

void IrrigationMode::onLoop()
{
    if (sleep_pending_) {
        tight_loop_contents();
        return;
    }

    // Process reliable PMU client (handles retries, dedup, and raw UART)
    if (pmu_available_ && reliable_pmu_) {
        reliable_pmu_->update();
    }

    // Process deferred tasks (wake timeout, sleep signaling)
    task_queue_.process(to_ms_since_boot(get_absolute_time()));
}

void IrrigationMode::handleScheduleComplete()
{
    pmu_logger.warn("Schedule complete - power down imminent!");
    logger.info("Closing all valves before shutdown...");

    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        valve_controller_.closeValve(i);
    }

    sleep_ms(500);
    pmu_logger.info("Ready for power down");
}

void IrrigationMode::onActuatorCommand(const ActuatorPayload *payload)
{
    if (!payload) {
        logger.error("NULL actuator payload");
        return;
    }

    if (payload->actuator_type == ACTUATOR_VALVE) {
        if (payload->param_length < 1) {
            logger.error("Valve command missing valve ID parameter");
            return;
        }

        uint8_t valve_id = payload->params[0];

        if (valve_id >= ValveController::NUM_VALVES) {
            logger.error("Invalid valve ID %d (max %d)", valve_id, ValveController::NUM_VALVES - 1);
            return;
        }

        if (payload->command == CMD_TURN_ON) {
            logger.info("Opening valve %d", valve_id);
            valve_controller_.openValve(valve_id);
            irrigation_state_.reportValveOpened();
        } else if (payload->command == CMD_TURN_OFF) {
            logger.info("Closing valve %d", valve_id);
            valve_controller_.closeValve(valve_id);
            // Only report closed if no valves remain open
            if (valve_controller_.getActiveValveMask() == 0) {
                if (keepawake_task_id_ != 0) {
                    task_queue_.cancel(keepawake_task_id_);
                    keepawake_task_id_ = 0;
                }
                irrigation_state_.reportValveClosed();
            }
        } else {
            logger.error("Unknown valve command %d", payload->command);
        }
    } else {
        logger.warn("Unsupported actuator type %d", payload->actuator_type);
    }
}

void IrrigationMode::onHeartbeatResponse(const HeartbeatResponsePayload *payload)
{
    // Call base class implementation to update RP2040 RTC
    ApplicationMode::onHeartbeatResponse(payload);

    if (!payload) {
        return;
    }

    if (payload->pending_update_flags != PENDING_FLAG_NONE) {
        logger.info("Pending updates flagged (0x%02X)", payload->pending_update_flags);
    }

    // Hub lost state — reset update sequence and wait for re-registration to complete
    bool needs_reregistration = (payload->pending_update_flags & PENDING_FLAG_REREGISTER) != 0;
    if (needs_reregistration) {
        update_state_.current_sequence = 0;
    }

    // Sync time to PMU if available
    if (pmu_available_ && reliable_pmu_) {
        PMU::DateTime datetime(payload->year % 100, payload->month, payload->day, payload->dotw,
                               payload->hour, payload->min, payload->sec);

        pmu_logger.info("Syncing time to PMU: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

        reliable_pmu_->setDateTime(
            datetime, [this, needs_reregistration](bool success, PMU::ErrorCode error) {
                if (success) {
                    pmu_logger.info("PMU time sync successful");
                } else {
                    pmu_logger.error("PMU time sync failed: error %d", static_cast<int>(error));
                }
                if (needs_reregistration) {
                    irrigation_state_.reportReregistrationRequired();
                } else {
                    irrigation_state_.reportHeartbeatResponseReceived();
                }
            });
    } else {
        if (needs_reregistration) {
            irrigation_state_.reportReregistrationRequired();
        } else {
            irrigation_state_.reportHeartbeatResponseReceived();
        }
    }
}

void IrrigationMode::sendCheckUpdates()
{
    logger.info("Sending CHECK_UPDATES (seq=%d)", update_state_.current_sequence);

    uint8_t seq_num = messenger_.sendCheckUpdates(HUB_ADDRESS, update_state_.current_sequence);

    if (seq_num != 0) {
        update_state_.pending_check_seq = seq_num;

        // Keep PMU awake while processing updates
        if (pmu_available_ && reliable_pmu_) {
            reliable_pmu_->keepAwake(10);
        }
    } else {
        logger.error("Failed to send CHECK_UPDATES");
        irrigation_state_.reportUpdateFailed();
    }
}

void IrrigationMode::onUpdateAvailable(const UpdateAvailablePayload *payload)
{
    if (!payload) {
        logger.error("NULL update payload");
        update_state_.reset();
        irrigation_state_.reportUpdateFailed();
        return;
    }

    // Cancel the pending CHECK_UPDATES message (UPDATE_AVAILABLE is the response)
    if (update_state_.pending_check_seq != 0) {
        messenger_.cancelPendingMessage(update_state_.pending_check_seq);
        update_state_.pending_check_seq = 0;
    }

    // Keep awake while processing
    if (pmu_available_ && reliable_pmu_) {
        reliable_pmu_->keepAwake(10);
    }

    // No more updates?
    if (payload->has_update == 0) {
        update_state_.reset();
        irrigation_state_.reportUpdateReceived(false);
        return;
    }

    // We have an update to apply
    UpdateType update_type = static_cast<UpdateType>(payload->update_type);
    uint8_t hub_sequence = payload->sequence;

    logger.info("Received update: type=%d, seq=%d", payload->update_type, hub_sequence);

    // Already processed?
    if (hub_sequence <= update_state_.current_sequence) {
        logger.info("  Already processed seq=%d (current=%d), re-checking", hub_sequence,
                    update_state_.current_sequence);
        sendCheckUpdates();
        return;
    }

    // Signal state machine that we have an update to apply
    irrigation_state_.reportUpdateReceived(true);

    // Apply the update via PMU
    switch (update_type) {
        case UpdateType::SET_SCHEDULE: {
            const uint8_t *data = payload->payload_data;
            uint8_t index = data[0];
            PMU::ScheduleEntry entry;
            entry.hour = data[1];
            entry.minute = data[2];
            entry.duration = data[3] | (data[4] << 8);
            entry.daysMask = static_cast<PMU::DayOfWeek>(data[5]);
            entry.valveId = data[6];
            entry.enabled = (data[7] != 0);

            logger.info("  SET_SCHEDULE[%d]: %02d:%02d, valve=%d, duration=%ds, days=0x%02X", index,
                        entry.hour, entry.minute, entry.valveId, entry.duration,
                        static_cast<uint8_t>(entry.daysMask));

            reliable_pmu_->setSchedule(
                entry, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  Schedule applied successfully");
                        update_state_.current_sequence = hub_sequence;
                        irrigation_state_.reportUpdateApplied();
                    } else {
                        logger.error("  Failed to apply schedule: error %d",
                                     static_cast<int>(error));
                        update_state_.reset();
                        irrigation_state_.reportUpdateFailed();
                    }
                });
            break;
        }

        case UpdateType::REMOVE_SCHEDULE: {
            uint8_t index = payload->payload_data[0];
            logger.info("  REMOVE_SCHEDULE[%d]", index);

            reliable_pmu_->clearSchedule(
                index, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  Schedule removed successfully");
                        update_state_.current_sequence = hub_sequence;
                        irrigation_state_.reportUpdateApplied();
                    } else {
                        logger.error("  Failed to remove schedule: error %d",
                                     static_cast<int>(error));
                        update_state_.reset();
                        irrigation_state_.reportUpdateFailed();
                    }
                });
            break;
        }

        case UpdateType::SET_DATETIME: {
            const uint8_t *data = payload->payload_data;
            PMU::DateTime datetime(data[0], data[1], data[2], data[3], data[4], data[5], data[6]);

            logger.info("  SET_DATETIME: 20%02d-%02d-%02d %02d:%02d:%02d", datetime.year,
                        datetime.month, datetime.day, datetime.hour, datetime.minute,
                        datetime.second);

            reliable_pmu_->setDateTime(
                datetime, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  DateTime set successfully");
                        update_state_.current_sequence = hub_sequence;
                        irrigation_state_.reportUpdateApplied();
                    } else {
                        logger.error("  Failed to set datetime: error %d",
                                     static_cast<int>(error));
                        update_state_.reset();
                        irrigation_state_.reportUpdateFailed();
                    }
                });
            break;
        }

        case UpdateType::SET_WAKE_INTERVAL: {
            uint16_t interval_seconds = payload->payload_data[0] | (payload->payload_data[1] << 8);
            logger.info("  SET_WAKE_INTERVAL: %d seconds", interval_seconds);

            reliable_pmu_->setWakeInterval(
                interval_seconds, [this, hub_sequence](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  Wake interval set successfully");
                        update_state_.current_sequence = hub_sequence;
                        irrigation_state_.reportUpdateApplied();
                    } else {
                        logger.error("  Failed to set wake interval: error %d",
                                     static_cast<int>(error));
                        update_state_.reset();
                        irrigation_state_.reportUpdateFailed();
                    }
                });
            break;
        }

        case UpdateType::ACTUATOR_COMMAND: {
            const uint8_t *data = payload->payload_data;
            ActuatorPayload actuator;
            actuator.actuator_type = data[0];
            actuator.command = data[1];
            actuator.param_length = data[2];
            memset(actuator.params, 0, sizeof(actuator.params));
            if (actuator.param_length > 0 && actuator.param_length <= sizeof(actuator.params)) {
                memcpy(actuator.params, &data[3], actuator.param_length);
            }

            logger.info("  ACTUATOR_COMMAND: type=%u, cmd=%u, params=%u", actuator.actuator_type,
                        actuator.command, actuator.param_length);

            onActuatorCommand(&actuator);
            update_state_.current_sequence = hub_sequence;
            // onActuatorCommand drives the state machine (e.g. reportValveOpened -> VALVE_ACTIVE).
            // For valve ON: state is now VALVE_ACTIVE, don't pull more updates — stay awake.
            // For valve OFF or non-valve: pull remaining updates.
            if (irrigation_state_.state() != IrrigationState::VALVE_ACTIVE) {
                irrigation_state_.reportUpdateApplied();
            }
            break;
        }

        default:
            logger.error("Unknown update type %d", payload->update_type);
            update_state_.reset();
            irrigation_state_.reportUpdateFailed();
            break;
    }
}

void IrrigationMode::attemptDeferredRegistration()
{
    logger.info("Sending deferred registration (device_id=0x%016llX)", device_id_);

    uint8_t registration_seq = messenger_.sendRegistrationRequest(
        ADDRESS_HUB, device_id_, NODE_TYPE_HYBRID, CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE,
        BRAMBLE_FIRMWARE_VERSION, "Irrigation Node");

    if (registration_seq != 0) {
        logger.info("Registration request sent (seq=%d)", registration_seq);
        needs_registration_ = false;
        irrigation_state_.reportRegistrationComplete();
    } else {
        logger.error("Failed to send registration request");
        irrigation_state_.reportRegistrationComplete();
    }
}

void IrrigationMode::signalReadyForSleep()
{
    if (!pmu_available_ || !reliable_pmu_) {
        logger.debug("PMU not available, skipping ready for sleep signal");
        return;
    }

    task_queue_.post(
        [this](uint32_t) -> bool {
            // Pack state fresh at send time
            IrrigationPersistedState state;
            packState(state);

            pmu_logger.info("Sending ReadyForSleep with state (seq=%u, addr=0x%04X, update_seq=%u)",
                            state.next_seq_num, state.assigned_address, state.update_sequence);

            reliable_pmu_->readyForSleep(
                reinterpret_cast<const uint8_t *>(&state),
                [this](bool success, PMU::ErrorCode error) {
                    if (success) {
                        pmu_logger.info("Ready for sleep acknowledged - halting");
                        sleep_pending_ = true;
                    } else {
                        pmu_logger.error("Ready for sleep failed: error %d",
                                         static_cast<int>(error));
                    }
                });
            return true;
        },
        TaskPriority::Low);
}

void IrrigationMode::packState(IrrigationPersistedState &out) const
{
    memset(&out, 0, sizeof(out));
    out.version = IRRIGATION_STATE_VERSION;
    out.board_version = IRRIGATION_BOARD_VERSION;
    out.next_seq_num = messenger_.getNextSeqNum();
    out.update_sequence = update_state_.current_sequence;
    out.assigned_address = messenger_.getNodeAddress();
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        out.valve_states[i] = static_cast<uint8_t>(valve_controller_.getValveState(i));
    }
}

bool IrrigationMode::unpackState(const IrrigationPersistedState *persisted)
{
    if (!persisted) {
        return false;
    }

    // Check version compatibility
    if (persisted->version != IRRIGATION_STATE_VERSION) {
        pmu_logger.warn("State version mismatch (got %u, expected %u) - cold start",
                        persisted->version, IRRIGATION_STATE_VERSION);
        return false;
    }

    // Check board version — reject state from a different hardware revision
    if (persisted->board_version != IRRIGATION_BOARD_VERSION) {
        pmu_logger.warn("Board version mismatch (got %u, expected %u) - cold start",
                        persisted->board_version, IRRIGATION_BOARD_VERSION);
        return false;
    }

    // Restore LoRa sequence number
    messenger_.setNextSeqNum(persisted->next_seq_num);

    // Restore assigned address from PMU RAM — but don't overwrite a freshly registered address
    // (main.cpp registration runs before onStart, so the messenger may already have a valid address)
    uint16_t current_address = messenger_.getNodeAddress();
    if (current_address == ADDRESS_UNREGISTERED && persisted->assigned_address != ADDRESS_UNREGISTERED) {
        messenger_.setNodeAddress(persisted->assigned_address);
    }

    // Restore update pull sequence number
    update_state_.current_sequence = persisted->update_sequence;

    // Restore valve states without actuating (DC latching valves hold position during sleep)
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        valve_controller_.restoreValveState(i, static_cast<ValveState>(persisted->valve_states[i]));
    }

    pmu_logger.info("Restored state: seq=%u, addr=0x%04X, update_seq=%u",
                    persisted->next_seq_num, persisted->assigned_address,
                    persisted->update_sequence);

    return true;
}

void IrrigationMode::onRebootRequested()
{
    if (pmu_available_ && reliable_pmu_) {
        logger.warn("Requesting full system reset via PMU");
        reliable_pmu_->systemReset([](bool success, PMU::ErrorCode error) {
            (void)error;
            (void)success;
            watchdog_reboot(0, 0, 0);
        });
    } else {
        logger.warn("PMU not available - performing RP2040-only watchdog reboot");
    }
    watchdog_reboot(0, 0, 0);
}

void IrrigationMode::onFactoryResetRequested()
{
    if (pmu_available_ && reliable_pmu_) {
        logger.warn("Requesting factory reset via PMU (wipes FRAM)");
        reliable_pmu_->factoryReset([](bool success, PMU::ErrorCode error) {
            (void)error;
            (void)success;
            watchdog_reboot(0, 0, 0);
        });
    } else {
        logger.warn("PMU not available - performing RP2040-only watchdog reboot");
    }
    watchdog_reboot(0, 0, 0);
}
