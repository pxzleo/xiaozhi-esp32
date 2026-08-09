#ifndef EXTERNAL_MONITOR_PROBE_H
#define EXTERNAL_MONITOR_PROBE_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>

namespace external_monitor {

struct PendingEvent {
    std::string event_id;
    std::string topic;
    std::string priority;
    std::time_t created_at = 0;
    std::time_t expires_at = 0;
};

class ProbeSchedule {
public:
    static constexpr int kRegularIntervalSeconds = 5 * 60;
    static constexpr int kMaximumFailureDelaySeconds = 30 * 60;
    static constexpr int kMaximumInitialJitterSeconds = 30;

    void Initialize(const std::string& device_id, int64_t now_us);
    bool Ready(int64_t now_us) const;
    void OnSuccess(int64_t now_us, int retry_after_seconds);
    void OnFailure(int64_t now_us);

    int jitter_seconds() const { return jitter_seconds_; }
    int failure_delay_seconds() const { return failure_delay_seconds_; }
    int64_t next_probe_us() const { return next_probe_us_; }

private:
    int jitter_seconds_ = 0;
    int failure_delay_seconds_ = kRegularIntervalSeconds;
    int64_t next_probe_us_ = 0;
};

bool IsSupportedTopic(const std::string& topic);
bool IsSupportedPriority(const std::string& priority);
bool ParseManagerDateTime(const std::string& value, std::time_t& timestamp);
bool ReadBoundedResponseBody(size_t declared_length, size_t maximum_length,
                             const std::function<int(char*, size_t)>& read,
                             std::string& body, std::string& error);

}  // namespace external_monitor

#endif  // EXTERNAL_MONITOR_PROBE_H
