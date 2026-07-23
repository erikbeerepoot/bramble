#include "irrigation_mode.h"

#include <cstring>

#include "pico/stdlib.h"

#include "hardware/watchdog.h"

#include "../../main.h"
#include "../hal/logger.h"
#include "../hal/rtc_compat.h"
#include "../led_patterns.h"
#include "../lora/message.h"
#include "../lora/reliable_messenger.h"
#include "../util/time.h"
#include "../version.h"

constexpr uint16_t HUB_ADDRESS = ADDRESS_HUB;
constexpr uint32_t HEARTBEAT_INTERVAL_MS = 60000;  // 60 seconds

#ifdef HARDWARE_IRRIGATION_AC
// Always-awake AC node cadences.
constexpr uint32_t AC_UPDATE_POLL_INTERVAL_MS = 15000;    // poll hub for commands/updates
constexpr uint32_t AC_SCHEDULE_EVAL_INTERVAL_MS = 20000;  // evaluate schedules (< 60s)
constexpr uint32_t AC_VALVE_CLOSE_INTERVAL_MS = 2000;     // auto-close granularity
#endif

// PMU UART configuration - selected by board version via board_pins.h
#include "../board/board_pins.h"

// Board version identifier stored in persisted state
#if defined(BOARD_V5)
constexpr uint8_t IRRIGATION_BOARD_VERSION = 5;
#elif defined(BOARD_V4)
constexpr uint8_t IRRIGATION_BOARD_VERSION = 4;
#else
constexpr uint8_t IRRIGATION_BOARD_VERSION = 3;
#endif

// Timeout for wake notification after ClearToSend
constexpr uint32_t WAKE_NOTIFICATION_TIMEOUT_MS = 1000;

// Maximum time to wait for LoRa TX queue to drain before sleeping
constexpr uint32_t LORA_DRAIN_TIMEOUT_MS = 5000;

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

    // Cache device ID for heartbeat identification
    device_id_ = ::getDeviceId();

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
    pmu_client_ =
        new PmuClient(Board::PMU_UART_PORT, Board::PMU_UART_TX_PIN, Board::PMU_UART_RX_PIN, 9600);
    pmu_available_ = pmu_client_->init();

    if (pmu_available_) {
        // Create reliable PMU client wrapper for automatic retry
        reliable_pmu_ = new PMU::ReliablePmuClient(pmu_client_);
        reliable_pmu_->init();

        // Allow UART to stabilize before first message
        sleep_ms(150);

        pmu_logger.info("PMU client initialized successfully");

        // Register PMU with base class for generic update handling
        setReliablePmu(reliable_pmu_);

        // Load any events that failed to transmit in the previous cycle
        {
            static uint8_t event_blob_buffer[140];
            reliable_pmu_->loadBlob(
                PMU::BLOB_SLOT_EVENT_LOG, event_blob_buffer, sizeof(event_blob_buffer),
                [this](bool success, uint16_t length) {
                    if (success && length > 0) {
                        event_log_.deserializeFromBlob(event_blob_buffer, length);
                        pmu_logger.info("Loaded %u persisted events from FRAM",
                                        event_log_.pendingCount());
                    }
                });
        }

        // Set up PMU callback handlers
        //
        // Fires BEFORE onWake when the PMU includes its current time in the wake
        // notification. On a duty-cycled node the RP2040 RTC is reset every wake,
        // so without this the wake handler runs with no wall time — and a valve
        // opened by a scheduled wake would get an auto-close deadline of
        // (0 + duration), i.e. 1970, which the next reconcile treats as already
        // expired and closes immediately. Setting the RTC here first makes
        // getUnixTimestamp() valid before any valve logic runs.
        reliable_pmu_->onWakeDateTime([this](bool valid, const PMU::DateTime &dt) {
            if (!valid || dt.month < 1 || dt.month > 12)
                return;
            datetime_t rtc_dt;
            rtc_dt.year = 2000 + dt.year;  // PMU year is years-since-2000
            rtc_dt.month = dt.month;
            rtc_dt.day = dt.day;
            rtc_dt.dotw = dt.weekday;
            rtc_dt.hour = dt.hour;
            rtc_dt.min = dt.minute;
            rtc_dt.sec = dt.second;
            if (rtc_set_datetime(&rtc_dt)) {
                sleep_us(64);  // let the RTC registers propagate before reads
                Logger::syncSubsecondCounter();
                uint32_t unix_ts = getUnixTimestamp();
                if (unix_ts > 0) {
                    event_log_.setTimeReference(to_ms_since_boot(get_absolute_time()), unix_ts);
                }
            }
        });

        reliable_pmu_->onWake([this](PMU::WakeReason reason, const PMU::ScheduleEntry *entry,
                                     bool state_valid, const uint8_t *state, bool valve_reset) {
            this->handlePmuWake(reason, entry, state_valid, state, valve_reset);
        });

        reliable_pmu_->onScheduleComplete([this]() { this->handleScheduleComplete(); });

        // Send ClearToSend — PMU will respond with WakeNotification containing state blob
        pmu_logger.debug("Sending ClearToSend to PMU...");
        reliable_pmu_->clearToSend([this](bool success, PMU::ErrorCode error) {
            if (!success) {
                pmu_logger.error("ClearToSend failed: %d", static_cast<int>(error));
                // Proceed without PMU state — force-close all valves so the
                // physical state matches firmware's assumed CLOSED, even if
                // the latching valves were left open before power loss.
                event_log_.record(EventType::BOOT_COLD, 0, 0);
                valve_controller_.forceCloseAllValves();
                irrigation_state_.markInitialized();
            } else {
                pmu_logger.info("ClearToSend ACK received - waiting for WakeNotification");
                // Start timeout — if WakeNotification doesn't arrive, proceed without state
                uint32_t now = to_ms_since_boot(get_absolute_time());
                wake_timeout_id_ = task_queue_.postDelayed(
                    [this](uint32_t) -> bool {
                        pmu_logger.warn("WakeNotification timeout - proceeding without PMU state");
                        event_log_.record(EventType::BOOT_COLD, 0, 0);
                        valve_controller_.forceCloseAllValves();
                        wake_timeout_id_ = 0;
                        irrigation_state_.markInitialized();
                        return true;
                    },
                    now, WAKE_NOTIFICATION_TIMEOUT_MS, TaskPriority::High);
            }
        });
    } else {
        logger.warn("PMU client not available - running without power management");
        // No PMU — force-close all valves to reconcile physical state.
        event_log_.record(EventType::BOOT_COLD, 0, 0);
        valve_controller_.forceCloseAllValves();
        irrigation_state_.markInitialized();
    }

    // Start with orange blinking pattern while waiting for RTC sync
    led_pattern_ = std::make_unique<BlinkingPattern>(led_, 255, 165, 0, 250, 250);
    operational_pattern_ = std::make_unique<ShortBlinkPattern>(led_, 0, 0, 255);

    // Initialize event log transmitter (in base class)
    initEventLogTransmitter();

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

#ifdef HARDWARE_IRRIGATION_AC
    startAlwaysAwakeTasks();
#endif
}

void IrrigationMode::onStateChange(IrrigationState state)
{
    // Re-arm (or clear) the per-state watchdog before running the state's side
    // effects, so a state that never gets its expected response recovers instead
    // of stranding the cycle.
    armStateWatchdog(state);

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
            // Send re-registration request (reset to unregistered so hub assigns a fresh address)
            messenger_.setNodeAddress(ADDRESS_UNREGISTERED);
            messenger_.sendRegistrationRequest(
                ADDRESS_HUB, device_id_, NODE_TYPE_HYBRID, CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE,
                BRAMBLE_FIRMWARE_VERSION, "Irrigation Node", ValveController::NUM_VALVES);
            // Set callback to advance SM when response arrives
            // Address is persisted via PMU state blob at sleep time (packState),
            // not flash — flash writes disrupt XIP cache on RP2350 and crash here.
            // Do NOT clear the callback from within itself — that destroys the
            // currently-executing std::function target, invalidating captured `this`
            // and causing UAF on any subsequent member access. The state machine's
            // state guard already rejects stale re-fires, and this callback is
            // overwritten next time AWAITING_REGISTRATION is entered.
            messenger_.setRegistrationSuccessCallback([this](uint16_t new_address) {
                logger.info("Re-registration assigned address 0x%04X", new_address);
                // Reset update sequence — hub starts fresh for new address
                update_state_.current_sequence = 0;
                irrigation_state_.reportReregistrationComplete();
            });
            break;

        case IrrigationState::CHECKING_UPDATES:
            sendCheckUpdates();
            break;

        case IrrigationState::APPLYING_UPDATE:
            // Waiting for PMU callback - no action needed
            break;

        case IrrigationState::VALVE_ACTIVE: {
#ifdef HARDWARE_IRRIGATION_AC
            // AC mains node: the valve GPIO is held because the node never
            // sleeps. Don't arm the PMU close-alarm; the periodic auto-close
            // task closes the valve at its deadline. Advance the SM out of
            // VALVE_ACTIVE (signalReadyForSleep is a no-op for AC).
            if (pmu_available_ && reliable_pmu_) {
                reliable_pmu_->keepAwake(KEEP_AWAKE_PROCESSING_SECONDS);
            }
            irrigation_state_.reportValveTimerSet();
            break;
#endif
            // If anything in the deadline queue is pending, arm the PMU's
            // RTC Alarm A for the soonest deadline. Otherwise fall back to
            // keepAlive (legacy: valve opened with no duration).
            uint32_t soonest_deadline = 0;
            uint8_t soonest_valve = soonestValveDeadline(soonest_deadline);
            if (soonest_valve != 0xFF && pmu_available_ && reliable_pmu_) {
                armNextValveTimer();
                irrigation_state_.reportValveTimerSet();
            } else {
                // No duration — legacy keepAlive behavior
                if (pmu_available_ && reliable_pmu_) {
                    reliable_pmu_->keepAwake(KEEP_AWAKE_PROCESSING_SECONDS);
                    scheduleKeepAwake();
                }
            }
            break;
        }

        case IrrigationState::TRANSMITTING_EVENTS: {
            // Enqueue any pending event log records into the messenger TX queue,
            // then poll until the messenger fully drains before signaling sleep.
            // This prevents the PMU from cutting power mid-LoRa-TX.
            onBeforeSleep();
            drain_start_ms_ = to_ms_since_boot(get_absolute_time());
            drain_task_id_ = task_queue_.post(
                [this](uint32_t now) -> bool {
                    if (messenger_.isFullyIdle()) {
                        logger.info("LoRa TX drained - proceeding to sleep");
                        drain_task_id_ = 0;
                        irrigation_state_.reportEventsTransmitted();
                        return true;
                    }
                    // Watchdog: don't drain forever
                    if (now - drain_start_ms_ > LORA_DRAIN_TIMEOUT_MS) {
                        logger.warn("LoRa drain timeout - sleeping anyway (event log lost)");
                        drain_task_id_ = 0;
                        irrigation_state_.reportEventsTransmitted();
                        return true;
                    }
                    return false;  // re-check on next tick
                },
                TaskPriority::High);
            break;
        }

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
                reliable_pmu_->keepAwake(KEEP_AWAKE_PROCESSING_SECONDS);
                scheduleKeepAwake();
            }
            return true;
        },
        now, 5000, TaskPriority::High);
}

uint32_t IrrigationMode::stateWatchdogMs(IrrigationState state)
{
    // Budgets must exceed the messenger's full delivery window for the message
    // each state waits on, or the watchdog aborts a cycle that would still have
    // succeeded — and a late reply then lands out of state (the warning we're
    // trying to remove). RELIABLE = 3 tries, backoff 2/4/8s, gives up ~16s.
    switch (state) {
        case IrrigationState::REGISTERING:
        case IrrigationState::AWAITING_REGISTRATION:
            return 18000;  // Registration request is RELIABLE (~16s give-up)
        case IrrigationState::AWAITING_TIME:
            return 3000;  // No I/O — transitions to SENDING_HEARTBEAT immediately
        case IrrigationState::SENDING_HEARTBEAT:
            return 5000;  // Heartbeat is BEST_EFFORT (one shot); 5s to see the response
        case IrrigationState::CHECKING_UPDATES:
            return 18000;  // CHECK_UPDATES is RELIABLE (~16s give-up, no failure callback)
        case IrrigationState::APPLYING_UPDATE:
            return 10000;  // Local PMU schedule/datetime write plus its callback
        default:
            // Parked or self-healing states get no watchdog:
            // READY_FOR_SLEEP / VALVE_ACTIVE are intentionally long-lived, and
            // TRANSMITTING_EVENTS already self-advances via its own drain timeout
            // (reportWatchdogTimeout can't recover it — it targets that state).
            return 0;
    }
}

void IrrigationMode::armStateWatchdog(IrrigationState state)
{
    if (state_watchdog_id_ != 0) {
        task_queue_.cancel(state_watchdog_id_);
        state_watchdog_id_ = 0;
    }
    const uint32_t watchdog_ms = stateWatchdogMs(state);
    if (watchdog_ms == 0) {
        return;
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    state_watchdog_id_ = task_queue_.postDelayed(
        [this](uint32_t) -> bool {
            state_watchdog_id_ = 0;
            irrigation_state_.reportWatchdogTimeout();
            return true;
        },
        now, watchdog_ms, TaskPriority::High);
}

#ifdef HARDWARE_IRRIGATION_AC
void IrrigationMode::startAlwaysAwakeTasks()
{
    logger.info("AC node: starting always-awake tasks (keepalive/poll/schedule/close)");

    // Keep the PMU powered so it never cuts the main rail (which would drop an
    // open SSR valve). KEEP_AWAKE is 10s; renew well inside that.
    task_manager_.addTask(
        [this](uint32_t) {
            if (pmu_available_ && reliable_pmu_) {
                reliable_pmu_->keepAwake(KEEP_AWAKE_PROCESSING_SECONDS);
            }
        },
        5000, "ACKeepalive");

    // Poll the hub for queued commands/updates (manual run/stop, schedule
    // changes, group-mirrored master opens) — low latency since we never sleep.
    // Drive the state machine through a full service cycle (heartbeat ->
    // CHECKING_UPDATES -> apply -> sleep) rather than calling sendCheckUpdates()
    // behind its back: that lets the SM own state and process updates in the
    // CHECKING_UPDATES/APPLYING_UPDATE states the rest of the code expects.
    // reportServiceTick() is a no-op unless the SM is parked, so it never
    // interrupts an open valve or an in-flight update. Skip until registered
    // and time-synced.
    task_manager_.addTask(
        [this](uint32_t) {
            if (messenger_.getNodeAddress() != ADDRESS_UNREGISTERED && getUnixTimestamp() != 0) {
                irrigation_state_.reportServiceTick();
            }
        },
        AC_UPDATE_POLL_INTERVAL_MS, "ACPoll");

    // Fire due schedules locally — the PMU only fires schedules by cold-waking a
    // sleeping node, which never happens here.
    task_manager_.addTask([this](uint32_t) { evaluateLocalSchedules(); },
                          AC_SCHEDULE_EVAL_INTERVAL_MS, "ACSchedule");

    // Auto-close valves whose deadlines have passed (no PMU RTC alarm involved).
    task_manager_.addTask([this](uint32_t) { processExpiredValveDeadlines(); },
                          AC_VALVE_CLOSE_INTERVAL_MS, "ACClose");
}

void IrrigationMode::evaluateLocalSchedules()
{
    uint32_t now = getUnixTimestamp();
    if (now == 0) {
        return;  // no wall-clock yet
    }

    const uint32_t now_min = now / 60;
    const uint8_t hour = (now % 86400) / 3600;
    const uint8_t minute = (now % 3600) / 60;
    // Unix epoch (1970-01-01) was a Thursday; day-of-week bit 0 = Sunday.
    const uint8_t dow = static_cast<uint8_t>(((now / 86400) + 4) % 7);

    for (uint8_t i = 0; i < AC_SCHEDULE_CACHE; i++) {
        if (!ac_schedule_valid_[i]) {
            continue;
        }
        const PMU::ScheduleEntry &s = ac_schedule_[i];
        // firesAt() handles both legacy one-shot (fires at hour:minute) and
        // interval entries (fires at each period boundary within the window).
        if (!s.firesAt(dow, hour, minute)) {
            continue;
        }
        if (ac_schedule_last_fired_min_[i] == now_min) {
            continue;  // already fired this occurrence (this minute)
        }
        if (s.valveId >= ValveController::NUM_VALVES) {
            continue;
        }

        ac_schedule_last_fired_min_[i] = now_min;
        logger.info("AC schedule[%u] fire: valve %u for %us", i, s.valveId, s.duration);
        valve_controller_.openValve(s.valveId);
        event_log_.record(EventType::VALVE_OPEN, 0, s.valveId);
        valve_close_deadlines_[s.valveId] = (s.duration > 0) ? (now + s.duration) : 0;
    }
}
#endif  // HARDWARE_IRRIGATION_AC

void IrrigationMode::handlePmuWake(PMU::WakeReason reason, const PMU::ScheduleEntry *entry,
                                   bool state_valid, const uint8_t *state, bool valve_reset)
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
        event_log_.record(EventType::WAKE, 0, 0);
        // Re-evaluate registration need based on restored address
        needs_registration_ = (messenger_.getNodeAddress() == ADDRESS_UNREGISTERED);
    } else {
        pmu_logger.info("Cold start - no persisted state");
        event_log_.record(EventType::BOOT_COLD, 0, 0);
    }

    // Reset valves to a known-closed state only on a genuine cold start (no valid
    // persisted state) or a deliberate reset (PMU power-on / NRST-button press,
    // signalled by valve_reset). On a warm reboot (RP2040 reset, watchdog, crash,
    // normal wake) we trust the restored state so in-progress irrigation survives.
    // The PMU's persisted state can claim valves are CLOSED while the latching
    // valves are mechanically OPEN (e.g. power loss mid-cycle); the deliberate
    // reset path is how the operator forces physical reconciliation.
    if (!restored || valve_reset) {
        valve_controller_.forceCloseAllValves();
        for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
            valve_close_deadlines_[i] = 0;
        }
    }

    // Process the per-valve deadline queue: close every valve whose
    // deadline has passed. This handles both ValveTimer wakes (the alarm
    // we armed expired) and periodic wakes that happen to coincide with a
    // deadline. The PMU's reported valve_id is ignored — the queue is the
    // source of truth.
    processExpiredValveDeadlines();

    // For scheduled wakes, open the valve and add its deadline to the queue.
    // VALVE_ACTIVE will then re-arm the PMU for the soonest pending close.
    if (reason == PMU::WakeReason::Scheduled && entry) {
        logger.info("Scheduled wake - valve %d for %d seconds", entry->valveId, entry->duration);
        if (entry->valveId < ValveController::NUM_VALVES) {
            valve_controller_.openValve(entry->valveId);
            event_log_.record(EventType::VALVE_OPEN, 0, entry->valveId);
            if (entry->duration > 0) {
                uint32_t now = getUnixTimestamp();
                valve_close_deadlines_[entry->valveId] = now + entry->duration;
            }
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
            // Extract duration if present: params = [valve_id, duration_lo, duration_hi].
            // Each valve has its own deadline slot, so a second open does not
            // clobber the first — both auto-close at their own times.
            if (payload->param_length >= 3) {
                uint16_t duration = payload->params[1] | (payload->params[2] << 8);
                uint32_t now = getUnixTimestamp();
                valve_close_deadlines_[valve_id] = (duration > 0) ? (now + duration) : 0;
                logger.info("Opening valve %d for %u seconds", valve_id, duration);
            } else {
                valve_close_deadlines_[valve_id] = 0;
                logger.info("Opening valve %d (no duration)", valve_id);
            }
            valve_controller_.openValve(valve_id);
            event_log_.record(EventType::VALVE_OPEN, 0, valve_id);
            irrigation_state_.reportValveOpened();
        } else if (payload->command == CMD_TURN_OFF) {
            logger.info("Closing valve %d", valve_id);
            valve_controller_.closeValve(valve_id);
            event_log_.record(EventType::VALVE_CLOSE, 0, valve_id);
            // Cancel this valve's pending auto-close and re-arm the PMU for
            // the next soonest deadline (or leave the existing alarm to fire
            // harmlessly if this valve wasn't the soonest — see followups).
            valve_close_deadlines_[valve_id] = 0;
            armNextValveTimer();
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

void IrrigationMode::onReregistrationRequested()
{
    // Suppress the base class callback — irrigation SM handles re-registration
    // after PMU time sync in onHeartbeatResponse().
    logger.info("Re-registration deferred to irrigation state machine");
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

void IrrigationMode::onModeSpecificUpdate(const UpdateAvailablePayload *payload,
                                          uint8_t hub_sequence)
{
    UpdateType update_type = static_cast<UpdateType>(payload->update_type);

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

#ifdef HARDWARE_IRRIGATION_AC
            // Cache locally so the always-awake node can fire it itself (the PMU
            // only fires schedules by cold-waking a sleeping node).
            if (index < AC_SCHEDULE_CACHE) {
                ac_schedule_[index] = entry;
                ac_schedule_valid_[index] = true;
                ac_schedule_last_fired_min_[index] = 0;
            } else {
                logger.warn("  Schedule index %d exceeds AC local cache (%d)", index,
                            AC_SCHEDULE_CACHE);
            }
#endif

            reliable_pmu_->setSchedule(
                index, entry, [this, hub_sequence, index](bool success, PMU::ErrorCode error) {
                    if (success) {
                        logger.info("  Schedule applied successfully");
                        event_log_.record(EventType::SCHEDULE_APPLIED, 0, index);
                    } else {
                        // Pack PMU error code into the high byte of the event detail
                        // so the dashboard can surface why the apply failed.
                        logger.error("  Failed to apply schedule: error %d",
                                     static_cast<int>(error));
                        const uint16_t detail = (static_cast<uint16_t>(error) << 8) | index;
                        event_log_.record(EventType::SCHEDULE_FAILED, 2, detail);
                    }
                    onUpdateApplied(hub_sequence);
                });
            break;
        }

        case UpdateType::REMOVE_SCHEDULE: {
            uint8_t index = payload->payload_data[0];
            logger.info("  REMOVE_SCHEDULE[%d]", index);

#ifdef HARDWARE_IRRIGATION_AC
            if (index < AC_SCHEDULE_CACHE) {
                ac_schedule_valid_[index] = false;
            }
#endif

            reliable_pmu_->clearSchedule(index, [this, hub_sequence, index](bool success,
                                                                            PMU::ErrorCode error) {
                if (success || error == PMU::ErrorCode::InvalidIndex) {
                    // Treat removing a non-existent schedule as success (idempotent)
                    if (!success) {
                        logger.info("  Schedule index %d already empty - treating as removed",
                                    index);
                    } else {
                        logger.info("  Schedule removed successfully");
                    }
                    event_log_.record(EventType::SCHEDULE_REMOVED, 0, index);
                } else {
                    // Pack PMU error code into the high byte of the event detail
                    // so the dashboard can surface why the remove failed.
                    logger.error("  Failed to remove schedule: error %d", static_cast<int>(error));
                    const uint16_t detail = (static_cast<uint16_t>(error) << 8) | index;
                    event_log_.record(EventType::SCHEDULE_FAILED, 2, detail);
                }
                onUpdateApplied(hub_sequence);
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
            ApplicationMode::onUpdateApplied(hub_sequence);
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
            onUpdateFailed();
            break;
    }
}

void IrrigationMode::onUpdateApplied(uint8_t hub_sequence)
{
    ApplicationMode::onUpdateApplied(hub_sequence);
    irrigation_state_.reportUpdateApplied();
}

void IrrigationMode::onUpdateFailed()
{
    ApplicationMode::onUpdateFailed();
    irrigation_state_.reportUpdateFailed();
}

void IrrigationMode::onUpdateReceived(bool has_updates)
{
    // If a scheduled wake opened a valve and added a deadline to the queue
    // without yet arming the PMU, detour through VALVE_ACTIVE to arm it
    // before sleeping. (Cold-boot path in handlePmuWake doesn't transition
    // via VALVE_ACTIVE on its own — reportWakeFromSleep is bypassed when
    // state==INITIALIZING.)
    if (!has_updates) {
        uint32_t deadline = 0;
        if (soonestValveDeadline(deadline) != 0xFF) {
            logger.info("Pending valve close needs PMU timer setup - entering VALVE_ACTIVE");
            irrigation_state_.reportValveOpened();
            return;
        }
    }
    irrigation_state_.reportUpdateReceived(has_updates);
}

void IrrigationMode::attemptDeferredRegistration()
{
    logger.info("Sending deferred registration (device_id=0x%016llX)", device_id_);

    uint8_t registration_seq = messenger_.sendRegistrationRequest(
        ADDRESS_HUB, device_id_, NODE_TYPE_HYBRID, CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE,
        BRAMBLE_FIRMWARE_VERSION, "Irrigation Node", ValveController::NUM_VALVES);

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

#ifdef HARDWARE_IRRIGATION_AC
    // Mains AC node: never sleep (deep sleep would drop the SSR GPIO and close
    // an open valve). Keep the PMU powered; the periodic always-awake tasks keep
    // the node serviced. The state machine parks here harmlessly until the next
    // valve/command event.
    reliable_pmu_->keepAwake(KEEP_AWAKE_PROCESSING_SECONDS);
    return;
#endif

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
    // Persist the per-valve close-deadline queue verbatim.
    static_assert(sizeof(out.valve_close_deadlines) >= sizeof(valve_close_deadlines_),
                  "persisted deadline array smaller than runtime array");
    memcpy(out.valve_close_deadlines, valve_close_deadlines_, sizeof(valve_close_deadlines_));
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

    // Restore assigned address from PMU RAM. The PMU state is the
    // authoritative record of the most recently re-registered address —
    // packState saves it on every sleep, so a non-UNREGISTERED value here
    // always reflects a more recent registration than what main.cpp loaded
    // from flash. Prefer it unconditionally; otherwise, after a hub-side
    // DELETE_NODE + re-register, the flash-loaded (stale) address would win
    // and force a re-register on every wake.
    if (persisted->assigned_address != ADDRESS_UNREGISTERED) {
        messenger_.setNodeAddress(persisted->assigned_address);
    }

    // Restore update pull sequence number
    update_state_.current_sequence = persisted->update_sequence;

    // Restore valve states without actuating (DC latching valves hold position during sleep)
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        valve_controller_.restoreValveState(i, static_cast<ValveState>(persisted->valve_states[i]));
    }

    // Restore the per-valve auto-close deadline queue.
    memcpy(valve_close_deadlines_, persisted->valve_close_deadlines,
           sizeof(valve_close_deadlines_));

    uint8_t pending_mask = 0;
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        if (valve_close_deadlines_[i] != 0) {
            pending_mask |= (1u << i);
        }
    }
    pmu_logger.info("Restored state: seq=%u, addr=0x%04X, update_seq=%u, pending_close_mask=0x%02X",
                    persisted->next_seq_num, persisted->assigned_address,
                    persisted->update_sequence, pending_mask);

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

// ============================================================================
// Per-valve auto-close deadline queue
// ============================================================================

uint8_t IrrigationMode::soonestValveDeadline(uint32_t &out_deadline) const
{
    uint8_t best = 0xFF;
    uint32_t best_deadline = UINT32_MAX;
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        uint32_t d = valve_close_deadlines_[i];
        if (d != 0 && d < best_deadline) {
            best_deadline = d;
            best = i;
        }
    }
    if (best != 0xFF) {
        out_deadline = best_deadline;
    }
    return best;
}

void IrrigationMode::armNextValveTimer()
{
    if (!pmu_available_ || !reliable_pmu_) {
        return;
    }

    // Process any expired deadlines first. handlePmuWake calls this too,
    // but at boot it runs before time sync (getUnixTimestamp() == 0) and
    // returns without clearing stale slots. By the time we get here we're
    // past time sync, so re-running it closes any expired deadlines and
    // empties their slots — preventing a 1-second re-arm loop on
    // restored-but-already-elapsed deadlines.
    processExpiredValveDeadlines();

    uint32_t deadline = 0;
    uint8_t valve = soonestValveDeadline(deadline);
    if (valve == 0xFF) {
        // Queue empty. A previously-armed Alarm A (if any) will still fire,
        // but the wake handler scans the queue and does nothing. Adding a
        // PMU ClearValveTimer opcode to disarm Alarm A is a follow-up.
        return;
    }

    uint32_t now = getUnixTimestamp();
    uint32_t seconds;
    if (now == 0 || deadline <= now) {
        // No valid wall time, or deadline already past — fire ASAP so the
        // queue processor on the next wake closes it. (The processExpired
        // call above will have closed it if time was valid, so this branch
        // only triggers in the truly-no-time-yet case.)
        seconds = 1;
    } else {
        uint32_t delta = deadline - now;
        seconds = (delta > 0xFFFFu) ? 0xFFFFu : delta;
    }

    logger.info("Arming valve timer: valve=%u seconds=%lu", valve,
                static_cast<unsigned long>(seconds));
    reliable_pmu_->setValveTimer(static_cast<uint16_t>(seconds), valve,
                                 [this, valve](bool success, PMU::ErrorCode error) {
                                     if (success) {
                                         event_log_.record(EventType::VALVE_TIMER_SET, 0, valve);
                                     } else {
                                         logger.error("Failed to arm valve timer for valve %u: %d",
                                                      valve, static_cast<int>(error));
                                     }
                                 });
}

void IrrigationMode::processExpiredValveDeadlines()
{
    uint32_t now = getUnixTimestamp();
    if (now == 0) {
        // No wall time yet; can't decide. The next wake after time sync
        // will process the queue.
        return;
    }
    // Deadlines below this are not real wall-clock times: they were computed as
    // (0 + duration) because the RTC was still unset when the valve opened (the
    // scheduled/manual-open paths). Treat such a value as a relative duration and
    // rebase it to an absolute deadline now that we have wall time, instead of
    // closing the valve as if it had already expired. (With a PMU that sends the
    // time on wake, opens see valid time and deadlines are always absolute, so
    // this rebase never triggers — it's the fallback for older PMU firmware.)
    static constexpr uint32_t MIN_PLAUSIBLE_UNIX = 1000000000u;  // 2001-09-09
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        uint32_t d = valve_close_deadlines_[i];
        if (d == 0)
            continue;
        if (d < MIN_PLAUSIBLE_UNIX) {
            valve_close_deadlines_[i] = now + d;
            logger.info("Rebased relative valve deadline: valve=%u duration=%lu now=%lu", i,
                        static_cast<unsigned long>(d), static_cast<unsigned long>(now));
            continue;
        }
        if (d <= now) {
            logger.info("Auto-close fired for valve %u (deadline %lu, now %lu)", i,
                        static_cast<unsigned long>(d), static_cast<unsigned long>(now));
            valve_controller_.closeValve(i);
            event_log_.record(EventType::VALVE_TIMER_CLOSE, 0, i);
            valve_close_deadlines_[i] = 0;
        }
    }
}
