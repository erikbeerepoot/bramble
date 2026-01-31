#pragma once

#include <cstdint>

/**
 * @brief Base class for mode-specific state machines
 *
 * Owns the common flags (initialized, error, RTC synced) and provides
 * shared behavior. Each mode defines its own state enum and transitions
 * on top via the updateState() hook.
 *
 * Flag-setting methods call updateState() so derived classes can
 * derive their mode-specific state from the flags.
 */
class ModeStateMachineBase {
public:
    virtual ~ModeStateMachineBase() = default;

    /**
     * @brief Mark hardware initialization complete
     *
     * Sets initialized flag and triggers state update.
     * Subsequent calls are no-ops.
     */
    void markInitialized()
    {
        if (initialized_) {
            return;
        }
        initialized_ = true;
        updateState();
    }

    /**
     * @brief Mark unrecoverable error
     *
     * Sets error flag and triggers state update.
     * Subsequent calls are no-ops.
     */
    void markError()
    {
        if (error_) {
            return;
        }
        error_ = true;
        updateState();
    }

    /**
     * @brief Report that RTC has been successfully synchronized
     *
     * Sets RTC synced flag and triggers state update.
     * Subsequent calls are no-ops.
     */
    void reportRtcSynced()
    {
        if (rtc_synced_) {
            return;
        }
        rtc_synced_ = true;
        updateState();
    }

    /**
     * @brief Report that RTC synchronization was lost
     *
     * Clears RTC synced flag and triggers state update.
     */
    void reportRtcLost()
    {
        if (!rtc_synced_) {
            return;
        }
        rtc_synced_ = false;
        updateState();
    }

protected:
    /**
     * @brief Derive mode-specific state from flags
     *
     * Called after any flag change. Derived classes override to
     * map the common flags to their mode-specific state enum.
     */
    virtual void updateState() = 0;

    bool initialized_ = false;
    bool error_ = false;
    bool rtc_synced_ = false;
};
