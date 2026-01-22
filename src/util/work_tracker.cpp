#include "work_tracker.h"

#include "../hal/logger.h"

static Logger logger("WORK");

void WorkTracker::setIdleCallback(IdleCallback callback)
{
    on_idle_ = callback;
}

void WorkTracker::addWork(WorkType type)
{
    uint8_t bit = 1 << static_cast<uint8_t>(type);

    if (active_work_ & bit) {
        // Already tracking this work type
        return;
    }

    active_work_ |= bit;
    idle_signaled_ = false;  // Reset - we have new work to do
    logger.info("Work added: %s", workTypeName(type));
    logState();
}

void WorkTracker::completeWork(WorkType type)
{
    uint8_t bit = 1 << static_cast<uint8_t>(type);

    if (!(active_work_ & bit)) {
        // Work wasn't pending
        logger.warn("Completed work that wasn't pending: %s", workTypeName(type));
        return;
    }

    active_work_ &= ~bit;
    logger.info("Work completed: %s", workTypeName(type));
    logState();

    // Note: checkIdle() is NOT called here automatically.
    // Caller should call checkIdle() explicitly when ready.
}

bool WorkTracker::hasWork() const
{
    return active_work_ != 0;
}

bool WorkTracker::hasWork(WorkType type) const
{
    uint8_t bit = 1 << static_cast<uint8_t>(type);
    return (active_work_ & bit) != 0;
}

void WorkTracker::checkIdle()
{
    if (isIdle() && on_idle_ && !idle_signaled_) {
        idle_signaled_ = true;  // Only signal once per idle period
        logger.info("All work complete - signaling idle");
        on_idle_();
    }
}

void WorkTracker::logState() const
{
    if (active_work_ == 0) {
        logger.debug("Work state: idle");
        return;
    }

    // Build list of active work types
    char buffer[64];
    int pos = 0;

    if (hasWork(WorkType::RtcSync)) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "RtcSync ");
    }
    if (hasWork(WorkType::BacklogTransmit)) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "BacklogTransmit ");
    }
    if (hasWork(WorkType::UpdatePull)) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "UpdatePull ");
    }
    if (hasWork(WorkType::Registration)) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Registration ");
    }

    logger.debug("Work state: %s", buffer);
}

const char *WorkTracker::workTypeName(WorkType type)
{
    switch (type) {
        case WorkType::RtcSync:
            return "RtcSync";
        case WorkType::BacklogTransmit:
            return "BacklogTransmit";
        case WorkType::UpdatePull:
            return "UpdatePull";
        case WorkType::Registration:
            return "Registration";
        default:
            return "Unknown";
    }
}
