#pragma once

#include <cstdint>

/**
 * @brief Base class for state machines with PMU-driven sleep/wake cycles
 *
 * Provides default implementation for response tracking.
 * Derived classes implement state queries based on their state enum.
 *
 * Expected states in derived class:
 *   LISTENING        - Receive window open, awaiting hub responses
 *   READY_FOR_SLEEP  - All wake cycle work complete, PMU can sleep
 */
class SleepAware {
public:
    virtual ~SleepAware() = default;

    // Response tracking (default implementation)
    void expectResponse() { ++expected_responses_; }
    bool hasExpectedResponses() const { return expected_responses_ > 0; }

    // State queries (derived classes implement)
    virtual bool isReadyForSleep() const = 0;
    virtual bool isListening() const = 0;

    // Wake cycle management (derived classes implement)
    virtual bool reportWakeFromSleep() = 0;
    virtual void reportListenComplete() = 0;

protected:
    bool shouldListen() const { return expected_responses_ > 0; }
    void resetExpectedResponses() { expected_responses_ = 0; }

private:
    uint8_t expected_responses_ = 0;
};
