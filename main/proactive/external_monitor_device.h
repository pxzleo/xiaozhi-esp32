#ifndef EXTERNAL_MONITOR_DEVICE_H
#define EXTERNAL_MONITOR_DEVICE_H

#include <optional>
#include <string>

#include "external_monitor_probe.h"

namespace external_monitor {

struct ProbeResult {
    bool success = false;
    int retry_after_seconds = ProbeSchedule::kRegularIntervalSeconds;
    std::optional<PendingEvent> event;
    std::string error;
};

ProbeResult ProbePendingEvent();

}  // namespace external_monitor

#endif  // EXTERNAL_MONITOR_DEVICE_H
